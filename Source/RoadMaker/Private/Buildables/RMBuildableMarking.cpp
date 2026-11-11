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
	DOREPLIFETIME(ARMBuildableMarking, mColour);
	DOREPLIFETIME(ARMBuildableMarking, mDataPrimary);
	DOREPLIFETIME(ARMBuildableMarking, mHasColour);
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

void ARMBuildableMarking::SetPlacementColour(const FLinearColor& Colour, const FLinearColor& DataPrimary)
{
	mColour = Colour;
	mDataPrimary = DataPrimary;
	mHasColour = true;
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
	// The paint is one colour (primary of the paint data); the cyberpunk look glows in it. When built it
	// takes the road's line colour; if the paint data resolves to something else later, it was repainted.
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	FLinearColor P = Resolved.PrimaryColor;
	if (mHasColour && P.Equals(mDataPrimary, 0.003f))
	{
		P = mColour;
	}
	else if (HasAuthority() && (!mHasColour || !P.Equals(mDataPrimary, 0.003f)))
	{
		mColour = P;
		mDataPrimary = P;
		mHasColour = true;
	}
	// Both slots: the plain paint reads the first, the cyberpunk one the second.
	mMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
	mMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
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

	constexpr float Shaft = 0.14f;     // half width of the arrow shaft: 0.28 units across
	constexpr float HeadHalf = 0.5f;   // arrow head half width
	constexpr float HeadLen = 0.85f;   // forward head length (units along, stretched like the rest)
	constexpr float SideBar = 0.15f;   // a turn's bar beyond the shaft (across, units)
	constexpr float SideHead = 0.6f;   // a turn's head length (across, units)
	constexpr float SideTip = Shaft + SideBar + SideHead;

	void BuildArrow(FShapeBuilder& B, uint8 Variant)
	{
		// Along X everything is stretched, so thicknesses measured along X are divided by the stretch to
		// match the same thickness across: the turn arms look like the straight arrow's shaft and head.
		const float ShX = Shaft / FShapeBuilder::Stretch;
		const float HeadX = 0.5f / FShapeBuilder::Stretch;
		// A side arm at X = At leaving the shaft toward Side (-1 left, +1 right); Y shifted by Shift.
		auto SideArm = [&B, ShX, HeadX](float Side, float At, float Shift)
		{
			B.Rect(At - ShX, At + ShX, Side * Shaft + Shift, Side * (Shaft + SideBar) + Shift);
			B.HeadSide(Side, Side * (Shaft + SideBar) + Shift, Side * SideTip + Shift, HeadX, At);
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
			const float Shift = -S * (SideTip - Shaft) * 0.5f; // centre the bend
			B.Rect(-1.1f, ShX, -Shaft + Shift, Shaft + Shift);
			SideArm(S, 0.f, Shift);
			break;
		}
		case 3: // straight or left
		case 4: // straight or right
		{
			const float S = Variant == 3 ? -1.f : 1.f;
			const float Shift = -S * (SideTip - HeadHalf) * 0.5f;
			B.Rect(-1.1f, 1.1f - HeadLen, -Shaft + Shift, Shaft + Shift);
			B.HeadForward(1.1f - HeadLen, 1.1f, HeadHalf, Shift);
			SideArm(S, -0.4f, Shift); // leaves the shaft below the head
			break;
		}
		default: // left or right
			B.Rect(-1.1f, ShX, -Shaft, Shaft);
			SideArm(-1.f, 0.f, 0.f);
			SideArm(1.f, 0.f, 0.f);
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
	FLinearColor Primary, Secondary;
	Road->GetLineColours(Primary, Secondary);
	New->SetPlacementColour(Road->GetLook() == 2 ? Secondary : Primary, Primary);
	UGameplayStatics::FinishSpawningActor(New, T);
	return New;
}
