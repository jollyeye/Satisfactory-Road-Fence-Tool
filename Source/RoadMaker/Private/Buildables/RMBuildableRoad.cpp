#include "Buildables/RMBuildableRoad.h"
#include "FGRecipe.h"
#include "RoadMakerModule.h"
#include "FGDismantleInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "Components/SplineComponent.h"
#include "ProceduralMeshComponent.h"
#include "Math/RotationMatrix.h"
#include "Net/UnrealNetwork.h"
#include "FGFactoryColoringTypes.h"
#include "Buildables/RMSwatchGroupRoads.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

ARMBuildableRoad::ARMBuildableRoad(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Paintable with the customizer, with its own default-swatch group ("Roads").
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();

	mStartAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("StartAnchor"));
	mStartAnchor->SetupAttachment(mMesh);
	mStartAnchor->ComponentTags.Add(TEXT("RMRoadAnchor"));

	mEndAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("EndAnchor"));
	mEndAnchor->SetupAttachment(mMesh);
	mEndAnchor->ComponentTags.Add(TEXT("RMRoadAnchor"));
}

void ARMBuildableRoad::RebuildMesh()
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	if (Frames.Num() < 2)
	{
		mMesh->ClearAllMeshSections();
		ClearFarMesh();
		return;
	}

	FRMMesh Top, Body;
	RMGeometry::BuildRoad(Frames, GetRoadWidth(), mRoadHeight, mUVScale, mPreviewMarkings.Get(mMarkings), mEdgeLineInset, Top, Body);
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyMaterial(0, TopMat);
	ApplyMaterial(1, BodyMat);

	// Far version: the same road sampled every 4 m instead of every 1 m.
	ClearFarMesh();
	TArray<FRMFrame> FarFrames;
	GetLocalFrames(FarFrames, 400.f);
	if (FarFrames.Num() >= 2)
	{
		FRMMesh FarTop, FarBody;
		RMGeometry::BuildRoad(FarFrames, GetRoadWidth(), mRoadHeight, mUVScale, mPreviewMarkings.Get(mMarkings), mEdgeLineInset, FarTop, FarBody);
		ApplyFarSection(FarTop, 0, TopMat);
		ApplyFarSection(FarBody, 1, BodyMat);
	}
	ApplyMarkingColors(mCustomizationData);

	// Anchors sit on the top-centre of each end, facing outward.
	const FRMFrame& First = Frames[0];
	const FRMFrame& Last = Frames.Last();
	mStartAnchor->SetRelativeLocationAndRotation(First.Pos, FRotationMatrix::MakeFromXZ(-First.Fwd, FVector::UpVector).Rotator());
	mEndAnchor->SetRelativeLocationAndRotation(Last.Pos, FRotationMatrix::MakeFromXZ(Last.Fwd, FVector::UpVector).Rotator());
}

void ARMBuildableRoad::GetEndWorld(int32 EndIndex, FVector& OutLocation, FVector& OutOutward) const
{
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 200.f);
	if (Frames.Num() < 2)
	{
		OutLocation = GetActorLocation();
		OutOutward = GetActorForwardVector();
		return;
	}
	const FRMFrame& F = EndIndex == 0 ? Frames[0] : Frames.Last();
	OutLocation = F.Pos;
	OutOutward = EndIndex == 0 ? -F.Fwd : F.Fwd;
}

void ARMBuildableRoad::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableRoad, mMarkings);
}

void ARMBuildableRoad::SetMarkings(const FRMMarkings& InMarkings)
{
	FRMMarkings Clean = InMarkings;
	Clean.Lanes = FMath::Clamp<uint8>(Clean.Lanes, 1, 4);
	if (Clean == mMarkings)
	{
		return;
	}
	mMarkings = Clean;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableRoad::GetLaneCentres(TArray<float>& OutOffsets, float& OutLaneWidth) const
{
	OutOffsets.Reset();
	const float Width = GetRoadWidth();
	const int32 Lanes = FMath::Clamp<int32>(mMarkings.Lanes, 1, 4);
	if (Lanes <= 1)
	{
		const int32 Count = FMath::FloorToInt(Width * 0.5f / 200.f);
		for (int32 K = -Count; K <= Count; ++K)
		{
			OutOffsets.Add(K * 200.f);
		}
		OutLaneWidth = 200.f;
		return;
	}
	const float Inset = mMarkings.bEdgeLines ? mEdgeLineInset : 0.f; // no side lines: full width
	const float Inner = FMath::Max(Width - 2.f * Inset, 1.f);
	for (int32 I = 0; I < Lanes; ++I)
	{
		OutOffsets.Add(-Width * 0.5f + Inset + Inner * (I + 0.5f) / Lanes);
	}
	OutLaneWidth = Inner / Lanes;
}

void ARMBuildableRoad::SetPreviewMarkings(const TOptional<FRMMarkings>& Preview)
{
	const bool bSame = Preview.IsSet() == mPreviewMarkings.IsSet()
		&& (!Preview.IsSet() || Preview.GetValue() == mPreviewMarkings.GetValue());
	if (bSame)
	{
		return;
	}
	mPreviewMarkings = Preview;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableRoad::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableRoad::GetLineColours(FLinearColor& OutPrimary, FLinearColor& OutSecondary) const
{
	const FResolvedFactoryCustomizationData Resolved(mCustomizationData, mColorSlot);
	OutPrimary = Resolved.PrimaryColor;
	OutSecondary = Resolved.SecondaryColor;
}

void ARMBuildableRoad::ApplyMarkingColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh)
	{
		return;
	}
	// Resolves swatch / colour slot / custom colours into the actual primary and secondary colour.
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	FLinearColor P = Resolved.PrimaryColor;
	FLinearColor S = Resolved.SecondaryColor;
	AdjustColorsForLook(Data, P, S);
	for (UProceduralMeshComponent* M : { mMesh, mFarMesh })
	{
		if (M)
		{
			M->SetCustomPrimitiveDataVector4(PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
			M->SetCustomPrimitiveDataVector4(SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		}
	}
}

void ARMBuildableRoad::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyMarkingColors(customizationData);
}

namespace
{
	/**
	 * Where a track / path spline crosses the cut: the plane through the cut point facing along the
	 * road, within the road's width (plus 1 m) and 4 m above / below.
	 */
	bool FindCutCrossing(const USplineComponent* Spline, const FRMFrame& Cut, float HalfWidth, float& OutDist)
	{
		const float Length = Spline->GetSplineLength();
		auto Side = [&](float D)
		{
			return FVector::DotProduct(Spline->GetLocationAtDistanceAlongSpline(D, ESplineCoordinateSpace::World) - Cut.Pos, Cut.Fwd);
		};
		float PrevD = 0.f;
		float PrevS = Side(0.f);
		for (float D = 20.f; PrevD < Length; D += 20.f)
		{
			D = FMath::Min(D, Length);
			const float S = Side(D);
			if ((PrevS <= 0.f) != (S <= 0.f))
			{
				// Linear estimate between the two samples on either side of the plane.
				const float Cross = FMath::Clamp(FMath::IsNearlyEqual(PrevS, S) ? PrevD : PrevD + (D - PrevD) * PrevS / (PrevS - S), PrevD, D);
				const FVector P = Spline->GetLocationAtDistanceAlongSpline(Cross, ESplineCoordinateSpace::World) - Cut.Pos;
				if (FMath::Abs(FVector::DotProduct(P, Cut.Right)) <= HalfWidth + 100.f && FMath::Abs(FVector::DotProduct(P, Cut.Up)) <= 400.f)
				{
					OutDist = Cross;
					return true;
				}
			}
			PrevD = D;
			PrevS = S;
		}
		return false;
	}

	/**
	 * Shortest piece the game builds for this track / path: its hologram's mMinLength (tracks) or
	 * mMinPathLength (vehicle paths), read from the build class's hologram; 1 m if not found.
	 */
	float RiderMinLength(const AActor* Rider)
	{
		static TMap<TWeakObjectPtr<UClass>, float> Cache;
		UClass* Class = Rider->GetClass();
		if (const float* Found = Cache.Find(Class))
		{
			return *Found;
		}
		float Min = 100.f;
		const FClassProperty* HoloProp = FindFProperty<FClassProperty>(AFGBuildable::StaticClass(), TEXT("mHologramClass"));
		UClass* Holo = HoloProp ? Cast<UClass>(HoloProp->GetObjectPropertyValue_InContainer(Class->GetDefaultObject())) : nullptr;
		if (Holo)
		{
			for (const TCHAR* Name : { TEXT("mMinLength"), TEXT("mMinPathLength") })
			{
				if (const FFloatProperty* P = FindFProperty<FFloatProperty>(Holo, Name))
				{
					Min = FMath::Max(Min, P->GetPropertyValue_InContainer(Holo->GetDefaultObject()));
					break;
				}
			}
		}
		UE_LOG(LogRoadMaker, Display, TEXT("Cut Road: shortest %s piece = %.0f cm"), *Class->GetName(), Min);
		Cache.Add(Class, Min);
		return Min;
	}
}

int32 ARMBuildableRoad::PlanRiderCuts(float Dist, TArray<TPair<AActor*, float>>& OutSplits) const
{
	OutSplits.Reset();
	FRMFrame Cut;
	if (!GetFrameAt(Dist, Cut))
	{
		return 0;
	}
	TArray<AActor*> Riders;
	IFGDismantleInterface::Execute_GetChildDismantleActors(const_cast<ARMBuildableRoad*>(this), Riders);
	const float HalfWidth = GetRoadWidth() * 0.5f;
	int32 TooShort = 0;
	for (AActor* Rider : Riders)
	{
		const USplineComponent* Spline = nullptr;
		if (const AFGBuildableRailroadTrack* Track = Cast<AFGBuildableRailroadTrack>(Rider))
		{
			Spline = Track->GetSplineComponent();
		}
		else if (const AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(Rider))
		{
			Spline = Path->GetSplineComponent();
		}
		float At = 0.f;
		if (!Spline || !FindCutCrossing(Spline, Cut, HalfWidth, At))
		{
			continue;
		}
		const float Min = RiderMinLength(Rider);
		if (At < Min || Spline->GetSplineLength() - At < Min)
		{
			++TooShort;
			continue;
		}
		OutSplits.Add(TPair<AActor*, float>(Rider, At));
	}
	return TooShort;
}

bool ARMBuildableRoad::CanSplitLengthwise() const
{
	if (mWidthInFoundations < 2)
	{
		return false;
	}
	return true;
}

void ARMBuildableRoad::DismantleReplaced()
{
	mBeingCut = true;
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	IFGDismantleInterface::Execute_Dismantle(this);
}

ARMBuildableRoad::EMergeCheck ARMBuildableRoad::CheckMerge(const ARMBuildableRoad* Other, TArray<FRMFrame>* OutCentre, float* OutWidth) const
{
	if (!Other || Other == this)
	{
		return EMergeCheck::NotBeside;
	}
	if (mWidthInFoundations + Other->GetWidthInFoundations() > 4)
	{
		return EMergeCheck::TooWide;
	}
	TArray<FRMFrame> A, B;
	GetWorldFrames(A, 100.f);
	Other->GetWorldFrames(B, 100.f);
	if (A.Num() < 2 || B.Num() < 2)
	{
		return EMergeCheck::NotBeside;
	}
	// The other road may run the other way.
	const bool bReversed = FVector::DotProduct(FVector(A[0].Fwd.X, A[0].Fwd.Y, 0.f), FVector(B[0].Fwd.X, B[0].Fwd.Y, 0.f)) < 0.f;
	const float LenB = B.Last().Dist;
	auto BAt = [&B, LenB, bReversed](float Fraction) -> FVector
	{
		const float D = (bReversed ? 1.f - Fraction : Fraction) * LenB;
		for (int32 I = 1; I < B.Num(); ++I)
		{
			if (B[I].Dist >= D)
			{
				const float Alpha = (D - B[I - 1].Dist) / FMath::Max(B[I].Dist - B[I - 1].Dist, KINDA_SMALL_NUMBER);
				return FMath::Lerp(B[I - 1].Pos, B[I].Pos, Alpha);
			}
		}
		return B.Last().Pos;
	};

	const float Tolerance = 50.f;
	const float Expected = (GetRoadWidth() + Other->GetRoadWidth()) * 0.5f;
	// Both ends in line with this road's ends (not longer or shorter).
	for (const int32 End : { 0, 1 })
	{
		const FRMFrame& F = End == 0 ? A[0] : A.Last();
		const FVector P = BAt(End == 0 ? 0.f : 1.f);
		if (FMath::Abs(FVector::DotProduct(P - F.Pos, F.Fwd)) > 100.f)
		{
			return EMergeCheck::LengthMismatch;
		}
	}
	// Beside it all the way, on one side, edge to edge and level (checked every tenth of the length).
	float Side = 0.f;
	const float LenA = A.Last().Dist;
	for (int32 K = 0; K <= 10; ++K)
	{
		const float Fraction = K / 10.f;
		FRMFrame F;
		const float D = Fraction * LenA;
		F = A.Last();
		for (int32 I = 1; I < A.Num(); ++I)
		{
			if (A[I].Dist >= D)
			{
				F = A[I - 1];
				break;
			}
		}
		// The other road's nearest centre point (its curve runs parallel, a different length on bends).
		FVector Nearest = B[0].Pos;
		float BestSq = TNumericLimits<float>::Max();
		for (const FRMFrame& G : B)
		{
			const float Sq = FVector::DistSquared2D(G.Pos, F.Pos);
			if (Sq < BestSq)
			{
				BestSq = Sq;
				Nearest = G.Pos;
			}
		}
		const FVector Delta = Nearest - F.Pos;
		const float Across = FVector::DotProduct(Delta, F.Right);
		if (FMath::Abs(FMath::Abs(Across) - Expected) > Tolerance || FMath::Abs(Delta.Z) > Tolerance)
		{
			return EMergeCheck::NotBeside;
		}
		const float S = FMath::Sign(Across);
		if (Side != 0.f && S != Side)
		{
			return EMergeCheck::NotBeside;
		}
		Side = S;
	}
	if (OutCentre)
	{
		// This road's curve moved sideways by half the other road's width, towards it.
		TArray<FRMSplinePoint> Points;
		RMGeometry::OffsetCurve(A, Side * Other->GetRoadWidth() * 0.5f, 200.f, Points);
		RMGeometry::SampleSpline(Points, 100.f, *OutCentre);
	}
	if (OutWidth)
	{
		*OutWidth = GetRoadWidth() + Other->GetRoadWidth();
	}
	return EMergeCheck::Ok;
}

ARMBuildableRoad* ARMBuildableRoad::MergeWith(ARMBuildableRoad* Other)
{
	if (!HasAuthority() || !Other || GetIsDismantled() || Other->GetIsDismantled() || IsActorBeingDestroyed() || Other->IsActorBeingDestroyed())
	{
		return nullptr;
	}
	TArray<FRMFrame> Centre;
	float Width = 0.f;
	if (CheckMerge(Other, &Centre, &Width) != EMergeCheck::Ok || Centre.Num() < 2)
	{
		return nullptr;
	}
	TArray<FRMSplinePoint> Points;
	RMGeometry::OffsetCurve(Centre, 0.f, 200.f, Points);
	const int32 Foundations = mWidthInFoundations + Other->GetWidthInFoundations();
	UClass* Class = LoadClass<ARMBuildableRoad>(nullptr, *FString::Printf(TEXT("/RoadMaker/Buildables/Build_Road_%dx.Build_Road_%dx_C"), Foundations, Foundations));
	UClass* Recipe = LoadClass<UFGRecipe>(nullptr, *FString::Printf(TEXT("/RoadMaker/Unlocks/Recipe_Road_%dx.Recipe_Road_%dx_C"), Foundations, Foundations));
	if (!Class || Points.Num() < 2)
	{
		return nullptr;
	}
	// This road's lanes, look and paint (CopyCutSettingsTo); the build animation from the end nearer the player.
	ARMBuildableRoad* Merged = Cast<ARMBuildableRoad>(SpawnLike(Class, Recipe, Points,
		FMath::Max(1, GetCostMultiplier() + Other->GetCostMultiplier()), ERMRevealFrom::NearestPlayer));
	if (!Merged)
	{
		return nullptr;
	}
	FRMMarkings Markings = mMarkings;
	Markings.bEdgeLines |= Other->GetMarkings().bEdgeLines;
	Merged->SetMarkings(Markings);
	// The old roads go, leaving what is built on them (markings, paths, tracks) where it is.
	DismantleReplaced();
	Other->DismantleReplaced();
	return Merged;
}

ARMBuildableRoad* ARMBuildableRoad::SplitLengthwise(int32 LeftFoundations, ARMBuildableRoad** OutRight)
{
	if (OutRight)
	{
		*OutRight = nullptr;
	}
	const int32 RightFoundations = mWidthInFoundations - LeftFoundations;
	if (!HasAuthority() || GetIsDismantled() || IsActorBeingDestroyed() || LeftFoundations < 1 || RightFoundations < 1 || !CanSplitLengthwise())
	{
		return nullptr;
	}
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 25.f);
	if (Frames.Num() < 3)
	{
		return nullptr;
	}
	// Each half is this road's curve moved sideways to the centre of its half.
	const float Half = GetRoadWidth() * 0.5f;
	const float LeftW = LeftFoundations * RM::FoundationSize;
	const float RightW = RightFoundations * RM::FoundationSize;
	TArray<FRMSplinePoint> LeftPoints, RightPoints;
	RMGeometry::OffsetCurve(Frames, -Half + LeftW * 0.5f, 200.f, LeftPoints);
	RMGeometry::OffsetCurve(Frames, Half - RightW * 0.5f, 200.f, RightPoints);
	if (LeftPoints.Num() < 2 || RightPoints.Num() < 2)
	{
		return nullptr;
	}
	auto ClassFor = [](int32 Foundations, UClass*& OutClass, UClass*& OutRecipe)
	{
		OutClass = LoadClass<ARMBuildableRoad>(nullptr, *FString::Printf(TEXT("/RoadMaker/Buildables/Build_Road_%dx.Build_Road_%dx_C"), Foundations, Foundations));
		OutRecipe = LoadClass<UFGRecipe>(nullptr, *FString::Printf(TEXT("/RoadMaker/Unlocks/Recipe_Road_%dx.Recipe_Road_%dx_C"), Foundations, Foundations));
	};
	UClass *LeftClass = nullptr, *LeftRecipe = nullptr, *RightClass = nullptr, *RightRecipe = nullptr;
	ClassFor(LeftFoundations, LeftClass, LeftRecipe);
	ClassFor(RightFoundations, RightClass, RightRecipe);
	if (!LeftClass || !RightClass)
	{
		return nullptr;
	}
	const int32 Cost = FMath::Max(1, GetCostMultiplier());
	// Build animations in opposite directions: two new roads, not the same one again.
	ARMBuildableSpline* Left = SpawnLike(LeftClass, LeftRecipe, LeftPoints, Cost, ERMRevealFrom::Start);
	ARMBuildableSpline* Right = SpawnLike(RightClass, RightRecipe, RightPoints, Cost, ERMRevealFrom::End);
	if (!Left || !Right)
	{
		if (Left)
		{
			Left->Destroy();
		}
		if (Right)
		{
			Right->Destroy();
		}
		return nullptr;
	}
	mBeingCut = true;
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	IFGDismantleInterface::Execute_Dismantle(this);
	if (OutRight)
	{
		*OutRight = Cast<ARMBuildableRoad>(Right);
	}
	return Cast<ARMBuildableRoad>(Left);
}

bool ARMBuildableRoad::CanCutAt(float Dist) const
{
	// Tracks / paths on the road that the cut crosses: refused if one would get too short a piece.
	TArray<TPair<AActor*, float>> Splits;
	return PlanRiderCuts(Dist, Splits) == 0;
}

void ARMBuildableRoad::CopyCutSettingsTo(ARMBuildableSpline* New) const
{
	if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(New))
	{
		Road->SetMarkings(mMarkings);
		Road->SetLook(mLook);
	}
}

void ARMBuildableRoad::BeforeCutDismantle(float Dist)
{
	// Game rail tracks and vehicle paths built on this road are split at the same place (the game's
	// own split, which keeps the pieces connected); the dismantle below leaves them alone.
	TArray<TPair<AActor*, float>> Splits;
	PlanRiderCuts(Dist, Splits);
	for (const TPair<AActor*, float>& Split : Splits)
	{
		if (AFGBuildableRailroadTrack* Track = Cast<AFGBuildableRailroadTrack>(Split.Key))
		{
			AFGBuildableRailroadTrack::Split(Track, Split.Value, true);
		}
		else if (AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(Split.Key))
		{
			Path->Split(Split.Value);
		}
	}
	mBeingCut = true;
}

void ARMBuildableRoad::GetCutSection(float& OutHalfWidth, float& OutBottom, float& OutTop) const
{
	OutHalfWidth = GetRoadWidth() * 0.5f;
	OutBottom = -FMath::Max(mRoadHeight, 10.f);
	OutTop = 0.f;
}
