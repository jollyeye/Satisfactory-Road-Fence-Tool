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
#include "EngineUtils.h"
#include "FGDismantleInterface.h"

ARMBuildableMarking::ARMBuildableMarking(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Close: cut into slices lying on the road; further: a flat copy; far: not drawn.
	mLODDistance = DrapedDistance;
	mFarMesh->SetCullDistance(MaxDrawDistance);
	mFarMesh->SetCastShadow(false);

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
	DOREPLIFETIME(ARMBuildableMarking, mDrapeStart);
	DOREPLIFETIME(ARMBuildableMarking, mDrapeStep);
	DOREPLIFETIME(ARMBuildableMarking, mDrapePos);
	DOREPLIFETIME(ARMBuildableMarking, mDrapeRight);
	DOREPLIFETIME(ARMBuildableMarking, mDrapeUp);
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
	FRMMarkingDrape Drape;
	Drape.Start = mDrapeStart;
	Drape.Step = mDrapeStep;
	Drape.Pos = mDrapePos;
	Drape.Right = mDrapeRight;
	Drape.Up = mDrapeUp;
	// The cyberpunk look is the second entry (index look - 1); asphalt and concrete share the plain paint.
	UMaterialInterface* Material = LookMaterial(mLookTopMaterials, mMarkingMaterial);
	FRMMesh Paint;
	BuildMarkingMesh(mKind, mVariant, mLaneWidth, Paint, Drape.IsValid() ? &Drape : nullptr);
	Paint.ApplyToSection(mMesh, 0, true);
	ApplyMaterial(0, Material);
	// Far copy: flat, no slices.
	FRMMesh Flat;
	BuildMarkingMesh(mKind, mVariant, mLaneWidth, Flat);
	ApplyFarSection(Flat, 0, Material);
	ApplyMarkingColors(mCustomizationData);
	UpdateLODDistances();
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
	for (UProceduralMeshComponent* M : { mMesh, mFarMesh })
	{
		if (M)
		{
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		}
	}
}

void ARMBuildableMarking::SetDrape(const FRMMarkingDrape& Drape)
{
	mDrapeStart = Drape.Start;
	mDrapeStep = Drape.Step;
	mDrapePos = Drape.Pos;
	mDrapeRight = Drape.Right;
	mDrapeUp = Drape.Up;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

FVector FRMMarkingDrape::Map(float X, float Y, float Z, FVector* OutNormal) const
{
	// Between the two samples around X (straight on past the ends).
	const float T = (X - Start) / Step;
	const int32 I = FMath::Clamp(FMath::FloorToInt(T), 0, Pos.Num() - 2);
	const float A = T - I;
	const FVector P = FMath::Lerp(Pos[I], Pos[I + 1], A);
	const FVector R = FMath::Lerp(Right[I], Right[I + 1], FMath::Clamp(A, 0.f, 1.f)).GetSafeNormal();
	const FVector U = FMath::Lerp(Up[I], Up[I + 1], FMath::Clamp(A, 0.f, 1.f)).GetSafeNormal();
	if (OutNormal)
	{
		*OutNormal = U;
	}
	return P + R * Y + U * Z;
}

// ---------------------------------------------------------------------------------------------
// Shapes. Drawn in units (a lane-width-independent design) with X forward and Y right, then scaled
// so the marking is half the width given across and stretched 1.8 times along.
// ---------------------------------------------------------------------------------------------

namespace
{
	/** Collects the marking as convex polygons in cm (X along, Y across); they are cut and laid on the road after. */
	struct FShapeBuilder
	{
		TArray<TArray<FVector2D>>& Polys;
		float Scale;

		/** Digits are stretched along the road, like painted ones: seen from a driving car they would be squashed. */
		static constexpr float Stretch = 1.8f;
		FVector2D P(float X, float Y) const { return FVector2D(X * Scale * Stretch, Y * Scale); }

		/** A point in real proportions (no stretch: the arrow layout already has it). */
		FVector2D Real(float X, float Y) const { return FVector2D(X * Scale, Y * Scale); }

		void RectReal(float X0, float X1, float Y0, float Y1)
		{
			Polys.Add({ Real(X0, Y0), Real(X1, Y0), Real(X1, Y1), Real(X0, Y1) });
		}

		/** Rectangle from (X0, Y0) to (X1, Y1) in stretched units. */
		void Rect(float X0, float X1, float Y0, float Y1)
		{
			Polys.Add({ P(X0, Y0), P(X1, Y0), P(X1, Y1), P(X0, Y1) });
		}

		void Tri(const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			Polys.Add({ A, B, C });
		}
	};

	/** The part of a convex polygon on one side of X = At (Keep +1: X >= At, -1: X <= At). */
	TArray<FVector2D> ClipX(const TArray<FVector2D>& In, float At, float Keep)
	{
		TArray<FVector2D> Out;
		for (int32 I = 0; I < In.Num(); ++I)
		{
			const FVector2D& A = In[I];
			const FVector2D& B = In[(I + 1) % In.Num()];
			const float DA = (A.X - At) * Keep;
			const float DB = (B.X - At) * Keep;
			if (DA >= 0.f)
			{
				Out.Add(A);
			}
			if ((DA >= 0.f) != (DB >= 0.f))
			{
				const float T = DA / (DA - DB);
				Out.Add(A + (B - A) * T);
			}
		}
		return Out;
	}

	/** Fan of a convex polygon, each corner placed by Place (position and normal). */
	template <typename TPlace>
	void EmitPolygon(const TArray<FVector2D>& Poly, FRMMesh& Out, TPlace Place)
	{
		if (Poly.Num() < 3)
		{
			return;
		}
		TArray<int32> Ids;
		FVector Normal = FVector::UpVector;
		for (const FVector2D& V : Poly)
		{
			FVector N;
			const FVector Pos = Place(V, N);
			Normal = N;
			Ids.Add(Out.AddVertex(Pos, N, FVector2D(V.X / 100.f, V.Y / 100.f), FVector::ForwardVector));
		}
		for (int32 I = 1; I + 1 < Ids.Num(); ++I)
		{
			Out.AddTriangle(Ids[0], Ids[I], Ids[I + 1], Normal);
		}
	}

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

float ARMBuildableMarking::HalfLength(ERMMarkingKind Kind, float LaneWidth)
{
	const float Scale = LaneWidth * 0.5f;
	return Kind == ERMMarkingKind::Arrow ? ArrowTop * Scale : 0.9f * FShapeBuilder::Stretch * Scale;
}

void ARMBuildableMarking::BuildMarkingMesh(ERMMarkingKind Kind, uint8 Variant, float LaneWidth, FRMMesh& Out, const FRMMarkingDrape* Drape)
{
	Variant = static_cast<uint8>(FMath::Clamp<int32>(Variant, 0, VariantCount(Kind) - 1));
	// One size for every shape, so the turn arrows match the straight one: its head is half the width given.
	TArray<TArray<FVector2D>> Polys;
	FShapeBuilder B{ Polys, LaneWidth * 0.5f };
	if (Kind == ERMMarkingKind::Arrow)
	{
		BuildArrow(B, Variant);
	}
	else
	{
		BuildDigit(B, Variant);
	}

	if (!Drape || !Drape->IsValid())
	{
		for (const TArray<FVector2D>& Poly : Polys)
		{
			EmitPolygon(Poly, Out, [](const FVector2D& V, FVector& N) { N = FVector::UpVector; return FVector(V.X, V.Y, Lift); });
		}
		return;
	}
	// Cut every piece at the samples, so each slice lies on the road between two of them.
	auto Place = [Drape](const FVector2D& V, FVector& N) { return Drape->Map(V.X, V.Y, Lift, &N); };
	for (const TArray<FVector2D>& Poly : Polys)
	{
		float MinX = TNumericLimits<float>::Max(), MaxX = -TNumericLimits<float>::Max();
		for (const FVector2D& V : Poly)
		{
			MinX = FMath::Min(MinX, V.X);
			MaxX = FMath::Max(MaxX, V.X);
		}
		const int32 K0 = FMath::FloorToInt((MinX - Drape->Start) / Drape->Step);
		const int32 K1 = FMath::CeilToInt((MaxX - Drape->Start) / Drape->Step);
		for (int32 K = K0; K < K1; ++K)
		{
			const float X0 = Drape->Start + K * Drape->Step;
			const float X1 = X0 + Drape->Step;
			EmitPolygon(ClipX(ClipX(Poly, X0, 1.f), X1, -1.f), Out, Place);
		}
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

namespace
{
	/** Where on the road: the frame index along it, the lane centre across, which way it faces. */
	struct FMarkingSpot
	{
		float Dist = 0.f;
		float Offset = 0.f;
		float DirSign = 1.f;
		float LaneWidth = 200.f;
		FTransform Transform;
	};

	bool FindMarkingSpot(const ARMBuildableRoad* Road, const TArray<FRMFrame>& Frames, const FVector& Aim, const FVector& LookDir, FMarkingSpot& Out)
	{
		if (!Road || Frames.Num() < 2)
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
		Road->GetLaneCentres(Offsets, Out.LaneWidth);
		const float U = FVector::DotProduct(Aim - Near->Pos, Near->Right);
		float BestOff = TNumericLimits<float>::Max();
		for (float O : Offsets)
		{
			if (FMath::Abs(O - U) < BestOff)
			{
				BestOff = FMath::Abs(O - U);
				Out.Offset = O;
			}
		}
		Out.Dist = Near->Dist;
		Out.DirSign = FVector::DotProduct(LookDir, Near->Fwd) >= 0.f ? 1.f : -1.f;
		Out.Transform = FTransform(FRotationMatrix::MakeFromXZ(Near->Fwd * Out.DirSign, Near->Up).ToQuat(), Near->Pos + Near->Right * Out.Offset);
		return true;
	}

	/** The road's frame at a distance along it, straight on past its ends. */
	FRMFrame MarkingFrameAtDist(const TArray<FRMFrame>& Frames, float Dist)
	{
		if (Dist <= 0.f)
		{
			FRMFrame F = Frames[0];
			F.Pos += F.Fwd * Dist;
			return F;
		}
		if (Dist >= Frames.Last().Dist)
		{
			FRMFrame F = Frames.Last();
			F.Pos += F.Fwd * (Dist - F.Dist);
			return F;
		}
		for (int32 I = 1; I < Frames.Num(); ++I)
		{
			if (Frames[I].Dist >= Dist)
			{
				const FRMFrame& A = Frames[I - 1];
				const FRMFrame& B = Frames[I];
				const float T = FMath::Clamp((Dist - A.Dist) / FMath::Max(B.Dist - A.Dist, KINDA_SMALL_NUMBER), 0.f, 1.f);
				FRMFrame F = A;
				F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
				F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
				F.Right = FMath::Lerp(A.Right, B.Right, T).GetSafeNormal();
				F.Up = FMath::Lerp(A.Up, B.Up, T).GetSafeNormal();
				F.Dist = Dist;
				return F;
			}
		}
		return Frames.Last();
	}
}

bool ARMBuildableMarking::FindPlacement(const ARMBuildableRoad* Road, const FVector& Aim, const FVector& LookDir, FTransform& OutTransform, float& OutLaneWidth)
{
	TArray<FRMFrame> Frames;
	if (Road)
	{
		Road->GetWorldFrames(Frames, 25.f);
	}
	FMarkingSpot Spot;
	if (!FindMarkingSpot(Road, Frames, Aim, LookDir, Spot))
	{
		return false;
	}
	OutTransform = Spot.Transform;
	OutLaneWidth = FMath::Min(Spot.LaneWidth, MaxLaneWidth);
	return true;
}

bool ARMBuildableMarking::PlanOn(const ARMBuildableRoad* Road, const FVector& Aim, const FVector& LookDir, ERMMarkingKind Kind, float SizeMultiplier,
	FTransform& OutTransform, float& OutWidth, FRMMarkingDrape& OutDrape)
{
	TArray<FRMFrame> Frames;
	if (Road)
	{
		Road->GetWorldFrames(Frames, 25.f);
	}
	FMarkingSpot Spot;
	if (!FindMarkingSpot(Road, Frames, Aim, LookDir, Spot))
	{
		return false;
	}
	OutTransform = Spot.Transform;
	OutWidth = FMath::Min(Spot.LaneWidth, MaxLaneWidth) * SizeMultiplier;

	// The road surface along the lane, every 50 cm over the marking's length (plus a little), in its local space.
	const float Half = HalfLength(Kind, OutWidth) + 25.f;
	const int32 Count = FMath::Max(2, FMath::CeilToInt(2.f * Half / 50.f) + 1);
	OutDrape = FRMMarkingDrape();
	OutDrape.Step = 2.f * Half / (Count - 1);
	OutDrape.Start = -Half;
	for (int32 K = 0; K < Count; ++K)
	{
		const float X = OutDrape.Start + K * OutDrape.Step;
		const FRMFrame F = MarkingFrameAtDist(Frames, Spot.Dist + Spot.DirSign * X);
		OutDrape.Pos.Add(OutTransform.InverseTransformPosition(F.Pos + F.Right * Spot.Offset));
		OutDrape.Right.Add(OutTransform.InverseTransformVectorNoScale(F.Right * Spot.DirSign));
		OutDrape.Up.Add(OutTransform.InverseTransformVectorNoScale(F.Up));
	}
	return true;
}

UMaterialInterface* ARMBuildableMarking::GetPaintMaterial(uint8 Look) const
{
	return Look > 0 && mLookTopMaterials.IsValidIndex(Look - 1) && mLookTopMaterials[Look - 1] ? mLookTopMaterials[Look - 1] : mMarkingMaterial;
}

ARMBuildableMarking* ARMBuildableMarking::SpawnOn(TSubclassOf<ARMBuildableMarking> Class, TSubclassOf<UFGRecipe> Recipe, ARMBuildableRoad* Road,
	uint8 Variant, float SizeMultiplier, const FVector& Aim, const FVector& LookDir, const FFactoryCustomizationData* Paint, int32 Look)
{
	UWorld* World = Road ? Road->GetWorld() : nullptr;
	const ARMBuildableMarking* Default = Class ? Class->GetDefaultObject<ARMBuildableMarking>() : nullptr;
	FTransform T;
	float Width = 200.f;
	FRMMarkingDrape Drape;
	if (!World || !Default || !PlanOn(Road, Aim, LookDir, Default->mKind, SizeMultiplier, T, Width, Drape))
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
	New->SetShape(Variant % Count, Width);
	New->SetDrape(Drape);
	// Painted like the road (its look and paint), unless it keeps its own (a moved marking).
	New->SetLook(Look >= 0 ? static_cast<uint8>(Look) : Road->GetLook());
	New->SetCustomizationData_Native(Paint ? *Paint : Road->GetCustomizationData_Native());
	UGameplayStatics::FinishSpawningActor(New, T);
	return New;
}

int32 ARMBuildableMarking::MoveMarkings(ARMBuildableRoad* From, ARMBuildableRoad* To)
{
	UWorld* World = From ? From->GetWorld() : nullptr;
	UProceduralMeshComponent* FromMesh = From ? From->GetProceduralMesh() : nullptr;
	if (!World || !To || !FromMesh || !From->HasAuthority())
	{
		return 0;
	}
	TArray<FRMFrame> OldFrames, NewFrames;
	From->GetWorldFrames(OldFrames, 25.f);
	To->GetWorldFrames(NewFrames, 25.f);
	if (OldFrames.Num() < 2 || NewFrames.Num() < 2)
	{
		return 0;
	}
	const float OldLength = FMath::Max(OldFrames.Last().Dist, 1.f);
	const float NewLength = NewFrames.Last().Dist;

	TArray<ARMBuildableMarking*> OnRoad;
	for (TActorIterator<ARMBuildableMarking> It(World); It; ++It)
	{
		ARMBuildableMarking* M = *It;
		if (!M || !IsValid(M) || M->GetIsDismantled() || M->IsActorBeingDestroyed())
		{
			continue;
		}
		// On this road = over its surface: across within its width, just above it, along within its ends.
		const FVector P = M->GetActorLocation();
		if (!FromMesh->Bounds.GetBox().ExpandBy(100.f).IsInside(P))
		{
			continue;
		}
		const FRMFrame* Near = &OldFrames[0];
		float Best = TNumericLimits<float>::Max();
		for (const FRMFrame& F : OldFrames)
		{
			const float D = FVector::DistSquared(F.Pos, P);
			if (D < Best)
			{
				Best = D;
				Near = &F;
			}
		}
		const FVector Rel = P - Near->Pos;
		if (FMath::Abs(FVector::DotProduct(Rel, Near->Right)) <= From->GetRoadWidth() * 0.5f
			&& FMath::Abs(FVector::DotProduct(Rel, Near->Up)) < 60.f && FMath::Abs(FVector::DotProduct(Rel, Near->Fwd)) < 30.f)
		{
			OnRoad.Add(M);
		}
	}

	int32 Moved = 0;
	for (ARMBuildableMarking* M : OnRoad)
	{
		// Where it is on the old road: share of the length, offset across, direction.
		const FVector P = M->GetActorLocation();
		const FRMFrame* Near = &OldFrames[0];
		float Best = TNumericLimits<float>::Max();
		for (const FRMFrame& F : OldFrames)
		{
			const float D = FVector::DistSquared(F.Pos, P);
			if (D < Best)
			{
				Best = D;
				Near = &F;
			}
		}
		const float Share = Near->Dist / OldLength;
		const float Across = FVector::DotProduct(P - Near->Pos, Near->Right);
		const float Dir = FVector::DotProduct(M->GetActorForwardVector(), Near->Fwd) >= 0.f ? 1.f : -1.f;

		const FRMFrame F = MarkingFrameAtDist(NewFrames, Share * NewLength);
		const FVector Aim = F.Pos + F.Right * Across;
		const FFactoryCustomizationData Paint = M->GetCustomizationData_Native();
		ARMBuildableMarking* New = SpawnOn(M->GetClass(), M->GetBuiltWithRecipe(), To, M->GetVariant(), 1.f, Aim, F.Fwd * Dir, &Paint, M->GetLook());
		if (New)
		{
			++Moved;
		}
		UE_LOG(LogRoadMaker, Verbose, TEXT("Marking move: %s at %s (share %.2f, across %.0f) -> %s at %s"), *M->GetName(), *P.ToCompactString(),
			Share, Across, New ? *New->GetName() : TEXT("none"), New ? *New->GetActorLocation().ToCompactString() : TEXT("-"));
		// Out of the way at once (like a replaced road), then dismantled.
		M->SetActorEnableCollision(false);
		M->SetActorHiddenInGame(true);
		IFGDismantleInterface::Execute_Dismantle(M);
	}
	if (Moved > 0)
	{
		UE_LOG(LogRoadMaker, Display, TEXT("Moved %d lane marking(s) from %s to %s"), Moved, *From->GetName(), *To->GetName());
	}
	return Moved;
}
