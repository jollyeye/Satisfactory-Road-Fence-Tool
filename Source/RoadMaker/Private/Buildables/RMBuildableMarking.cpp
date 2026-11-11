#include "Buildables/RMBuildableMarking.h"
#include "Buildables/RMBuildableRoad.h"
#include "FGFactoryColoringTypes.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Buildables/RMSwatchGroupRoads.h"
#include "RMRoadUtils.h"
#include "FGRecipe.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RotationMatrix.h"
#include "RoadMakerModule.h"

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
	// Like the road's lines: asphalt and concrete paint in the primary colour (the middle lines' colour),
	// cyberpunk glows in the secondary colour (its lines' glow). A marking starts with a copy of its road's paint.
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	mMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
	mMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
}

// ---------------------------------------------------------------------------------------------
// Shapes. Drawn in units (a lane-width-independent design) with X forward and Y right, then scaled
// so the marking is half the width given across and stretched 1.8 times along.
// ---------------------------------------------------------------------------------------------

namespace
{
	struct FShapeBuilder
	{
		FRMMesh& Mesh;
		float Scale;
		float Z;

		/** Markings are stretched along the road, like painted ones: seen from a driving car they would be squashed. */
		static constexpr float Stretch = 1.8f;
		FVector P(float X, float Y) const { return FVector(X * Scale * Stretch, Y * Scale, Z); }

		/** A point in real proportions (no stretch: the layout already has it). */
		FVector Real(float X, float Y) const { return FVector(X * Scale, Y * Scale, Z); }

		void RectReal(float X0, float X1, float Y0, float Y1)
		{
			Mesh.AddQuad(Real(X0, Y0), Real(X1, Y0), Real(X1, Y1), Real(X0, Y1), FVector::UpVector,
				FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1));
		}

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

	// Arrows are laid out in real proportions (units across = units along, the stretch is already in the
	// numbers): every arrow fits the straight arrow's box, 1 wide and 3.96 long; only the turn-and-straight
	// and left-or-right arrows reach out a little to the side. Same layout as Tools/IconSource/arrow_layout.py.
	constexpr float ArrowTop = 1.98f, ArrowBottom = -1.98f;
	constexpr float ArrowShaft = 0.14f;      // shaft half width
	constexpr float ArrowHeadHalf = 0.5f;    // forward head half width
	constexpr float ArrowHeadLen = 1.53f;    // forward head length
	constexpr float ArrowSideLen = 0.5f;     // a side head's length (across)
	constexpr float ArrowSideBase = 1.3f;    // a side head's base (along)
	constexpr float ArrowBarThick = 0.4f;    // the bar from the shaft to a side head (along)
	constexpr float ArrowBranchBar = 0.16f;  // that bar's length on the branching arrows (across)

	void BuildArrow(FShapeBuilder& B, uint8 Variant)
	{
		auto Rect = [&B](float X0, float X1, float Y0, float Y1)
		{
			B.RectReal(X0, X1, FMath::Min(Y0, Y1), FMath::Max(Y0, Y1));
		};
		auto Forward = [&B](float Y)
		{
			B.Tri(B.Real(ArrowTop - ArrowHeadLen, Y - ArrowHeadHalf), B.Real(ArrowTop, Y), B.Real(ArrowTop - ArrowHeadLen, Y + ArrowHeadHalf));
		};
		// Side head pointing to Side (-1 left, +1 right), its base at BaseY, centred along at Xc.
		auto SideHead = [&B](float Side, float Xc, float BaseY)
		{
			B.Tri(B.Real(Xc - ArrowSideBase * 0.5f, BaseY), B.Real(Xc + ArrowSideBase * 0.5f, BaseY), B.Real(Xc, BaseY + Side * ArrowSideLen));
		};

		switch (Variant)
		{
		case 0: // straight
			Rect(ArrowBottom, ArrowTop - ArrowHeadLen, -ArrowShaft, ArrowShaft);
			Forward(0.f);
			break;
		case 1: // left: the shaft runs up the far side of the box and turns at the top
		case 2: // right
		{
			const float S = Variant == 1 ? -1.f : 1.f;
			const float Xc = ArrowTop - ArrowSideBase * 0.5f;
			const float ShaftY = -S * (ArrowHeadHalf - ArrowShaft);
			const float BaseY = S * (ArrowHeadHalf - ArrowSideLen);
			Rect(ArrowBottom, Xc + ArrowBarThick * 0.5f, ShaftY - ArrowShaft, ShaftY + ArrowShaft);
			Rect(Xc - ArrowBarThick * 0.5f, Xc + ArrowBarThick * 0.5f, ShaftY + S * ArrowShaft, BaseY);
			SideHead(S, Xc, BaseY);
			break;
		}
		case 3: // straight or left: the branch leaves the shaft just below the head
		case 4: // straight or right
		{
			const float S = Variant == 3 ? -1.f : 1.f;
			const float Tip = ArrowShaft + ArrowBranchBar + ArrowSideLen;
			const float Shift = -S * (Tip - ArrowHeadHalf) * 0.5f;
			Rect(ArrowBottom, ArrowTop - ArrowHeadLen, Shift - ArrowShaft, Shift + ArrowShaft);
			Forward(Shift);
			const float Xc = ArrowTop - ArrowHeadLen - 0.15f - ArrowSideBase * 0.5f;
			const float BaseY = Shift + S * (ArrowShaft + ArrowBranchBar);
			Rect(Xc - ArrowBarThick * 0.5f, Xc + ArrowBarThick * 0.5f, Shift + S * ArrowShaft, BaseY);
			SideHead(S, Xc, BaseY);
			break;
		}
		default: // left or right
		{
			const float Xc = ArrowTop - ArrowSideBase * 0.5f;
			Rect(ArrowBottom, Xc + ArrowBarThick * 0.5f, -ArrowShaft, ArrowShaft);
			for (const float S : { -1.f, 1.f })
			{
				const float BaseY = S * (ArrowShaft + ArrowBranchBar);
				Rect(Xc - ArrowBarThick * 0.5f, Xc + ArrowBarThick * 0.5f, S * ArrowShaft, BaseY);
				SideHead(S, Xc, BaseY);
			}
			break;
		}
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
	// One size for every shape, so the turn arrows match the straight one: its head is half the width given.
	const float Scale = LaneWidth * 0.5f;
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

// ---------------------------------------------------------------------------------------------
// Finding the road and the lane, and painting a marking
// ---------------------------------------------------------------------------------------------

ARMBuildableRoad* ARMBuildableMarking::FindRoadAt(UWorld* World, const FVector& Location)
{
	TArray<ARMBuildableRoad*> Roads;
	RMRoadUtils::GatherNearby(World, Location, 150.f, Roads);
	ARMBuildableRoad* Best = nullptr;
	float BestDist = TNumericLimits<float>::Max();
	for (ARMBuildableRoad* Road : Roads)
	{
		if (!Road || Road->GetIsDismantled() || Road->IsActorBeingDestroyed())
		{
			continue;
		}
		TArray<FRMFrame> Frames;
		Road->GetWorldFrames(Frames, 50.f);
		for (const FRMFrame& F : Frames)
		{
			const FVector Rel = Location - F.Pos;
			// Within the road's width.
			if (FMath::Abs(FVector::DotProduct(Rel, F.Right)) > Road->GetRoadWidth() * 0.5f + 20.f)
			{
				continue;
			}
			const float D = Rel.SizeSquared();
			if (D < BestDist)
			{
				BestDist = D;
				Best = Road;
			}
		}
	}
	return Best;
}

bool ARMBuildableMarking::FindPlacement(const ARMBuildableRoad* Road, const FVector& Aim, const FVector& LookDir, FTransform& OutTransform, float& OutLaneWidth)
{
	TArray<FRMFrame> Frames;
	if (Road)
	{
		Road->GetWorldFrames(Frames, 25.f);
	}
	if (Frames.Num() < 2)
	{
		return false;
	}
	// The spot along the road: the frame nearest the aim; the lane: the centre nearest across.
	const FRMFrame* Near = &Frames[0];
	float Best = TNumericLimits<float>::Max();
	for (const FRMFrame& F : Frames)
	{
		const float D = FVector::DistSquared(F.Pos, Aim);
		if (D < Best)
		{
			Best = D;
			Near = &F;
		}
	}
	TArray<float> Offsets;
	Road->GetLaneCentres(Offsets, OutLaneWidth);
	const float U = FVector::DotProduct(Aim - Near->Pos, Near->Right);
	float Off = 0.f;
	float BestOff = TNumericLimits<float>::Max();
	for (float O : Offsets)
	{
		if (FMath::Abs(O - U) < BestOff)
		{
			BestOff = FMath::Abs(O - U);
			Off = O;
		}
	}
	const FVector Dir = FVector::DotProduct(LookDir, Near->Fwd) >= 0.f ? Near->Fwd : -Near->Fwd;
	OutTransform = FTransform(FRotationMatrix::MakeFromXZ(Dir, Near->Up).ToQuat(), Near->Pos + Near->Right * Off);
	return true;
}

UMaterialInterface* ARMBuildableMarking::GetPaintMaterial(uint8 Look) const
{
	return Look > 0 && mLookTopMaterials.IsValidIndex(Look - 1) && mLookTopMaterials[Look - 1] ? mLookTopMaterials[Look - 1] : mMarkingMaterial;
}

ARMBuildableMarking* ARMBuildableMarking::SpawnOn(TSubclassOf<ARMBuildableMarking> Class, TSubclassOf<UFGRecipe> Recipe, ARMBuildableRoad* Road,
	uint8 Variant, float SizeMultiplier, const FVector& Aim, const FVector& LookDir)
{
	UWorld* World = Road ? Road->GetWorld() : nullptr;
	FTransform T;
	float LaneWidth = 200.f;
	if (!World || !Class || !FindPlacement(Road, Aim, LookDir, T, LaneWidth))
	{
		return nullptr;
	}
	ARMBuildableMarking* New = World->SpawnActorDeferred<ARMBuildableMarking>(Class, T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!New)
	{
		return nullptr;
	}
	const uint8 Count = static_cast<uint8>(VariantCount(New->mKind));
	New->SetBuiltWithRecipe(Recipe);
	New->SetShape(Variant % Count, LaneWidth * SizeMultiplier);
	New->SetLook(Road->GetLook());
	New->SetCustomizationData_Native(Road->GetCustomizationData_Native());
	UGameplayStatics::FinishSpawningActor(New, T);
	return New;
}
