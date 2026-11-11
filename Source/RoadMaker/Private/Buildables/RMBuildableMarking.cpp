#include "Buildables/RMBuildableMarking.h"
#include "Buildables/RMBuildableRoad.h"
#include "FGFactoryColoringTypes.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Buildables/RMSwatchGroupRoads.h"

ARMBuildableMarking::ARMBuildableMarking(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mLODDistance = 0.f; // a few dozen triangles: always drawn in full

	// Paintable on its own (the paint gun, with the roads' swatch group); it starts in the road's line colours.
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();

	// Seen by the build gun (aim to dismantle, paint) but never in the way of pioneers, vehicles or the camera.
	mMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	mMesh->SetCollisionResponseToAllChannels(ECR_Block);
	mMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	mMesh->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
	mMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	mMesh->SetCastShadow(false);
}

void ARMBuildableMarking::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableMarking, mVariant);
	DOREPLIFETIME(ARMBuildableMarking, mLaneWidth);
}

void ARMBuildableMarking::SetShape(uint8 Variant, float LaneWidth)
{
	mVariant = Variant;
	mLaneWidth = LaneWidth;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableMarking::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Paint;
	BuildMarkingMesh(mKind, mVariant, mLaneWidth, Paint);
	Paint.ApplyToSection(mMesh, 0, true);
	// The cyberpunk look is the second entry (index look - 1); asphalt and concrete share the plain paint.
	ApplyMaterial(0, LookMaterial(mLookTopMaterials, mMarkingMaterial));
	ApplyMarkingColors(mCustomizationData);
}

void ARMBuildableMarking::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableMarking::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableMarking::ApplyMarkingColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh)
	{
		return;
	}
	// Same slots as the road's lines: M_RoadMarking reads the primary, the cyberpunk one the secondary.
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	mMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
	mMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
}

// ---------------------------------------------------------------------------------------------
// Shapes. Drawn in units (a lane-width-independent design) with X forward and Y right, then scaled
// so the marking fits its lane: 80% of the lane across, at most 3 m along.
// ---------------------------------------------------------------------------------------------

namespace
{
	struct FShapeBuilder
	{
		FRMMesh& Mesh;
		float Scale;
		float Z;

		FVector P(float X, float Y) const { return FVector(X * Scale, Y * Scale, Z); }

		/** Rectangle from (X0, Y0) to (X1, Y1) in units. */
		void Rect(float X0, float X1, float Y0, float Y1)
		{
			Mesh.AddQuad(P(X0, Y0), P(X1, Y0), P(X1, Y1), P(X0, Y1), FVector::UpVector,
				FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1));
		}

		void Tri(const FVector& A, const FVector& B, const FVector& C)
		{
			const FVector T = (B - A).GetSafeNormal();
			const int32 IA = Mesh.AddVertex(A, FVector::UpVector, FVector2D(0, 0), T);
			const int32 IB = Mesh.AddVertex(B, FVector::UpVector, FVector2D(1, 0), T);
			const int32 IC = Mesh.AddVertex(C, FVector::UpVector, FVector2D(0, 1), T);
			Mesh.AddTriangle(IA, IB, IC, FVector::UpVector);
		}

		/** Arrow head pointing along +X (Forward) or +Y / -Y (Side = +1 right, -1 left). */
		void HeadForward(float BaseX, float TipX, float HalfWidth, float Y)
		{
			Tri(P(BaseX, Y - HalfWidth), P(TipX, Y), P(BaseX, Y + HalfWidth));
		}
		void HeadSide(float Side, float BaseY, float TipY, float HalfLength, float X)
		{
			Tri(P(X - HalfLength, BaseY), P(X + HalfLength, BaseY), P(X, TipY));
			(void)Side;
		}
	};

	constexpr float Shaft = 0.14f;   // half width of the arrow shaft: 0.28 units
	constexpr float HeadHalf = 0.5f; // arrow head half width
	constexpr float HeadLen = 0.55f; // arrow head length

	/** Half length (along) and half width (across) of a shape in units, to scale it into its lane. */
	void Extent(ERMMarkingKind Kind, uint8 Variant, float& OutHalfLength, float& OutHalfWidth)
	{
		if (Kind == ERMMarkingKind::Digit)
		{
			OutHalfLength = 0.9f;
			OutHalfWidth = 0.5f;
			return;
		}
		OutHalfLength = 1.1f;
		OutHalfWidth = Variant == 5 ? 0.95f : (Variant >= 3 ? 0.7f : (Variant == 0 ? 0.5f : 0.55f));
	}

	void BuildArrow(FShapeBuilder& B, uint8 Variant)
	{
		// Arms leave the shaft sideways: the arm (a short bar) and its head. Y shifts keep the whole arrow centred.
		auto Arm = [&B](float Side, float AtX, float ArmEnd, float HeadTip, float HeadHalfLength, float ShiftY)
		{
			// Bar from the shaft edge to ArmEnd, head beyond it.
			if (Side < 0.f)
			{
				B.Rect(AtX - Shaft, AtX + Shaft, -ArmEnd + ShiftY, -Shaft + ShiftY);
				B.HeadSide(-1.f, -ArmEnd + ShiftY, -HeadTip + ShiftY, HeadHalfLength, AtX);
			}
			else
			{
				B.Rect(AtX - Shaft, AtX + Shaft, Shaft + ShiftY, ArmEnd + ShiftY);
				B.HeadSide(1.f, ArmEnd + ShiftY, HeadTip + ShiftY, HeadHalfLength, AtX);
			}
		};

		switch (Variant)
		{
		case 0: // straight
			B.Rect(-1.1f, 1.1f - HeadLen, -Shaft, Shaft);
			B.HeadForward(1.1f - HeadLen, 1.1f, HeadHalf, 0.f);
			break;
		case 1: // left (the shaft turns left at the top)
		case 2: // right
		{
			const float S = Variant == 1 ? -1.f : 1.f;
			const float Shift = -S * 0.4f; // centre the bend
			B.Rect(-1.1f, 0.f + Shaft, -Shaft + Shift, Shaft + Shift);
			Arm(S, 0.f, 0.45f, 0.95f, 0.45f, Shift);
			break;
		}
		case 3: // straight or left
		case 4: // straight or right
		{
			const float S = Variant == 3 ? -1.f : 1.f;
			const float Shift = -S * 0.2f;
			B.Rect(-1.1f, 1.1f - HeadLen, -Shaft + Shift, Shaft + Shift);
			B.HeadForward(1.1f - HeadLen, 1.1f, HeadHalf, Shift);
			// The branch leaves the shaft below the head, at 0.3 units back from the middle.
			const float At = -0.3f;
			if (S < 0.f)
			{
				B.Rect(At - Shaft, At + Shaft, -0.5f + Shift, -Shaft + Shift);
				B.HeadSide(-1.f, -0.5f + Shift, -0.9f + Shift, 0.4f, At);
			}
			else
			{
				B.Rect(At - Shaft, At + Shaft, Shaft + Shift, 0.5f + Shift);
				B.HeadSide(1.f, 0.5f + Shift, 0.9f + Shift, 0.4f, At);
			}
			break;
		}
		default: // left or right
			B.Rect(-1.1f, 0.f + Shaft, -Shaft, Shaft);
			Arm(-1.f, 0.f, 0.45f, 0.95f, 0.45f, 0.f);
			Arm(1.f, 0.f, 0.45f, 0.95f, 0.45f, 0.f);
			break;
		}
	}

	void BuildDigit(FShapeBuilder& B, uint8 Digit)
	{
		// Seven segments in a box 1 wide and 1.8 tall (X is up: the top of the digit points forward).
		const float T = 0.24f;       // stroke
		const float L = -0.9f, H = 0.9f; // bottom / top (X)
		const float Lo = -0.5f, Hi = 0.5f; // left / right (Y)
		const float Mid = 0.f;
		auto Top = [&]() { B.Rect(H - T, H, Lo, Hi); };
		auto Bottom = [&]() { B.Rect(L, L + T, Lo, Hi); };
		auto Middle = [&]() { B.Rect(Mid - T * 0.5f, Mid + T * 0.5f, Lo, Hi); };
		// Verticals run between the horizontals, so no two pieces overlap.
		auto UpperRight = [&]() { B.Rect(Mid + T * 0.5f, H - T, Hi - T, Hi); };
		auto LowerRight = [&]() { B.Rect(L + T, Mid - T * 0.5f, Hi - T, Hi); };
		auto UpperLeft = [&]() { B.Rect(Mid + T * 0.5f, H - T, Lo, Lo + T); };
		auto LowerLeft = [&]() { B.Rect(L + T, Mid - T * 0.5f, Lo, Lo + T); };

		switch (Digit)
		{
		case 0: Top(); Bottom(); UpperRight(); LowerRight(); UpperLeft(); LowerLeft(); break;
		case 1:
			B.Rect(L, H, -T * 0.5f, T * 0.5f);              // the bar
			B.Rect(H - T, H, -T * 0.5f - 0.25f, -T * 0.5f); // flag at the top
			B.Rect(L, L + T, -0.3f, -T * 0.5f);             // foot, left half
			B.Rect(L, L + T, T * 0.5f, 0.3f);               // foot, right half
			break;
		case 2: Top(); Middle(); Bottom(); UpperRight(); LowerLeft(); break;
		case 3: Top(); Middle(); Bottom(); UpperRight(); LowerRight(); break;
		case 4: Middle(); UpperLeft(); UpperRight(); LowerRight(); B.Rect(H - T, H, Lo, Lo + T); B.Rect(H - T, H, Hi - T, Hi); break;
		case 5: Top(); Middle(); Bottom(); UpperLeft(); LowerRight(); break;
		case 6: Top(); Middle(); Bottom(); UpperLeft(); LowerLeft(); LowerRight(); break;
		case 7: Top(); UpperRight(); LowerRight(); break;
		case 8: Top(); Middle(); Bottom(); UpperLeft(); UpperRight(); LowerLeft(); LowerRight(); break;
		default: Top(); Middle(); Bottom(); UpperLeft(); UpperRight(); LowerRight(); break; // 9
		}
	}
}

void ARMBuildableMarking::BuildMarkingMesh(ERMMarkingKind Kind, uint8 Variant, float LaneWidth, FRMMesh& Out)
{
	Variant = static_cast<uint8>(FMath::Clamp<int32>(Variant, 0, VariantCount(Kind) - 1));
	float HalfL = 1.f, HalfW = 0.5f;
	Extent(Kind, Variant, HalfL, HalfW);
	const float Scale = FMath::Min(LaneWidth * 0.8f / (2.f * HalfW), 300.f / (2.f * HalfL));
	FShapeBuilder B{ Out, Scale, Lift };
	if (Kind == ERMMarkingKind::Arrow)
	{
		BuildArrow(B, Variant);
	}
	else
	{
		BuildDigit(B, Variant);
	}
}
