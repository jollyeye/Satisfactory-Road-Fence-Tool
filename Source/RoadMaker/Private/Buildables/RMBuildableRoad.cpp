#include "Buildables/RMBuildableRoad.h"
#include "FGRecipe.h"
#include "Algo/Reverse.h"
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
#include "Buildables/RMBuildableIntersection.h"
#include "RMRoadUtils.h"
#include "EngineUtils.h"

namespace
{
	/** The road piece class (and its recipe) of a width: roads by foundations, walking paths by metres. */
	void RoadClassFor(bool bWalking, float WidthCm, UClass*& OutClass, UClass*& OutRecipe)
	{
		if (bWalking)
		{
			const int32 M = FMath::RoundToInt(WidthCm / 100.f);
			OutClass = LoadClass<ARMBuildableRoad>(nullptr, *FString::Printf(TEXT("/RoadMaker/Buildables/Build_WalkingPath_%dm.Build_WalkingPath_%dm_C"), M, M));
			OutRecipe = LoadClass<UFGRecipe>(nullptr, *FString::Printf(TEXT("/RoadMaker/Unlocks/Recipe_WalkingPath_%dm.Recipe_WalkingPath_%dm_C"), M, M));
			return;
		}
		const int32 F = FMath::RoundToInt(WidthCm / RM::FoundationSize);
		OutClass = LoadClass<ARMBuildableRoad>(nullptr, *FString::Printf(TEXT("/RoadMaker/Buildables/Build_Road_%dx.Build_Road_%dx_C"), F, F));
		OutRecipe = LoadClass<UFGRecipe>(nullptr, *FString::Printf(TEXT("/RoadMaker/Unlocks/Recipe_Road_%dx.Recipe_Road_%dx_C"), F, F));
	}
}

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
	// Long roads are drawn in sections (each switches detail on its own); every detail level gets a
	// ring exactly on each section boundary, so the sections meet without a gap.
	UpdateChunkLayout();
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	RMGeometry::ReduceRoadFrames(Frames, IsWalkingPath()); // fewer quads (a flat strip: the curve stays round enough)
	if (Frames.Num() < 2)
	{
		mMesh->ClearAllMeshSections();
		ClearFarMesh();
		return;
	}
	AddChunkBoundaries(Frames);
	PrepareChunks();
	FRMMesh Top, Body;
	TArray<bool> CoveredL, CoveredR;
	SlopeCovered(Frames, CoveredL, CoveredR);
	RMGeometry::BuildRoad(Frames, GetRoadWidth(), mRoadHeight, mUVScale, LimitMarkings(mPreviewMarkings.Get(mMarkings)), mEdgeLineInset, Top, Body,
		!SlopeCapAt(0), !SlopeCapAt(1), GetShownLook() == 2, &CoveredL, &CoveredR);
	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyNearSection(Top, 0, true, TopMat);
	ApplyNearSection(Body, 1, true, BodyMat);
	// Side slopes (1.6.0): the banks in the body's concrete, solid.
	FRMMesh Slopes;
	RequestSlopeMeasure();
	if (mSideSlopes)
	{
		BuildSlopeMesh(mSlopeLeft, mSlopeRight, Frames, Slopes);
	}
	ApplyNearSection(Slopes, 2, true, BodyMat);

	// Far version: the same road sampled every 4 m instead of every 1 m.
	ClearFarMesh();
	TArray<FRMFrame> FarFrames;
	GetLocalFrames(FarFrames, 400.f);
	AddChunkBoundaries(FarFrames);
	if (FarFrames.Num() >= 2)
	{
		FRMMesh FarTop, FarBody;
		TArray<bool> FarCoveredL, FarCoveredR;
		SlopeCovered(FarFrames, FarCoveredL, FarCoveredR);
		RMGeometry::BuildRoad(FarFrames, GetRoadWidth(), mRoadHeight, mUVScale, LimitMarkings(mPreviewMarkings.Get(mMarkings)), mEdgeLineInset, FarTop, FarBody,
			!SlopeCapAt(0), !SlopeCapAt(1), GetShownLook() == 2, &FarCoveredL, &FarCoveredR);
		ApplyFarSection(FarTop, 0, TopMat);
		ApplyFarSection(FarBody, 1, BodyMat);
		if (!Slopes.IsEmpty())
		{
			ApplyFarSection(Slopes, 2, BodyMat);
		}
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
	DOREPLIFETIME(ARMBuildableRoad, mSideSlopes);
	DOREPLIFETIME(ARMBuildableRoad, mSlopeLeft);
	DOREPLIFETIME(ARMBuildableRoad, mSlopeRight);
}

FRMMarkings ARMBuildableRoad::LimitMarkings(const FRMMarkings& In) const
{
	FRMMarkings Out = In;
	Out.Lanes = FMath::Clamp<uint8>(Out.Lanes, 1, IsWalkingPath() ? 2 : 4);
	if (IsWalkingPath() && GetRoadWidth() < 300.f)
	{
		Out.bEdgeLines = false; // side lines on 4 m walking paths only
	}
	return Out;
}

void ARMBuildableRoad::SetMarkings(const FRMMarkings& InMarkings)
{
	const FRMMarkings Clean = LimitMarkings(InMarkings);
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
	// One lane = one marking spot in the middle, as wide as the road inside its side lines. (Vehicle paths keep
	// their own rule: every 2 m across on a one-lane road, see RMVanillaSnapping.)
	const int32 Lanes = FMath::Clamp<int32>(mMarkings.Lanes, 1, 4);
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
	const float DashScale = DashScaleFor(GetLength());
	ForEachMeshComponent([&P, &S, DashScale](UProceduralMeshComponent* M)
	{
		M->SetCustomPrimitiveDataVector4(PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
		M->SetCustomPrimitiveDataVector4(SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		M->SetCustomPrimitiveDataFloat(DashScaleIndex, DashScale);
	});
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
		UE_LOG(LogRoadMaker, Verbose, TEXT("Cut Road: shortest %s piece = %.0f cm"), *Class->GetName(), Min);
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
	return GetWidthUnits() >= 2;
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
	if (!Other || Other == this || IsWalkingPath() != Other->IsWalkingPath())
	{
		return EMergeCheck::NotBeside; // a road and a walking path are not merged
	}
	// Roads up to 4 foundations together; walking paths only two 2 m paths into a 4 m one.
	if (IsWalkingPath() ? GetRoadWidth() + Other->GetRoadWidth() > 401.f : mWidthInFoundations + Other->GetWidthInFoundations() > 4)
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
	UClass *Class = nullptr, *Recipe = nullptr;
	RoadClassFor(IsWalkingPath(), GetRoadWidth() + Other->GetRoadWidth(), Class, Recipe);
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

ARMBuildableRoad::EJoinCheck ARMBuildableRoad::CheckJoin(const ARMBuildableRoad* Other, int32 MyEnd, int32 OtherEnd, TArray<FRMFrame>* OutFrames) const
{
	if (!Other || Other == this)
	{
		return EJoinCheck::NotJoined;
	}
	const ERMJoinCourse Course = JoinCourse(Other, MyEnd, OtherEnd, mMaxLength, OutFrames);
	if (Course == ERMJoinCourse::NotJoined)
	{
		return EJoinCheck::NotJoined;
	}
	if (!FMath::IsNearlyEqual(GetRoadWidth(), Other->GetRoadWidth(), 1.f) || IsWalkingPath() != Other->IsWalkingPath())
	{
		return EJoinCheck::Width; // same width, and a walking path only with a walking path
	}
	switch (Course)
	{
	case ERMJoinCourse::Ok: return EJoinCheck::Ok;
	case ERMJoinCourse::Kink: return EJoinCheck::Kink;
	case ERMJoinCourse::Curve: return EJoinCheck::Curve;
	case ERMJoinCourse::TooLong: return EJoinCheck::TooLong;
	default: return EJoinCheck::NotJoined;
	}
}

ARMBuildableRoad* ARMBuildableRoad::JoinWith(ARMBuildableRoad* Other, int32 MyEnd, int32 OtherEnd)
{
	if (!HasAuthority() || !Other || GetIsDismantled() || Other->GetIsDismantled() || IsActorBeingDestroyed() || Other->IsActorBeingDestroyed())
	{
		return nullptr;
	}
	TArray<FRMFrame> Path;
	if (CheckJoin(Other, MyEnd, OtherEnd, &Path) != EJoinCheck::Ok || Path.Num() < 2)
	{
		return nullptr;
	}
	// Refitted through both courses (keys every 2 m, exact ends and end directions), heights as they are.
	TArray<FRMSplinePoint> Points;
	RMGeometry::OffsetCurve(Path, 0.f, 200.f, Points);
	FVector JointLoc, JointOut;
	GetEndWorld(MyEnd, JointLoc, JointOut);
	// This road's lanes, look and paint; the build animation outwards from the joint (the inverse of Cut).
	ARMBuildableRoad* Joined = Cast<ARMBuildableRoad>(SpawnLike(GetClass(), GetBuiltWithRecipe(), Points,
		FMath::Max(1, GetCostMultiplier() + Other->GetCostMultiplier()), ERMRevealFrom::FromPoint, JointLoc));
	if (!Joined)
	{
		return nullptr;
	}
	DismantleReplaced();
	Other->DismantleReplaced();
	return Joined;
}

ARMBuildableRoad* ARMBuildableRoad::SplitLengthwise(int32 LeftUnits, ARMBuildableRoad** OutRight)
{
	if (OutRight)
	{
		*OutRight = nullptr;
	}
	const int32 RightUnits = GetWidthUnits() - LeftUnits;
	if (!HasAuthority() || GetIsDismantled() || IsActorBeingDestroyed() || LeftUnits < 1 || RightUnits < 1 || !CanSplitLengthwise())
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
	const float LeftW = LeftUnits * GetSplitUnit();
	const float RightW = RightUnits * GetSplitUnit();
	TArray<FRMSplinePoint> LeftPoints, RightPoints;
	RMGeometry::OffsetCurve(Frames, -Half + LeftW * 0.5f, 200.f, LeftPoints);
	RMGeometry::OffsetCurve(Frames, Half - RightW * 0.5f, 200.f, RightPoints);
	if (LeftPoints.Num() < 2 || RightPoints.Num() < 2)
	{
		return nullptr;
	}
	UClass *LeftClass = nullptr, *LeftRecipe = nullptr, *RightClass = nullptr, *RightRecipe = nullptr;
	RoadClassFor(IsWalkingPath(), LeftW, LeftClass, LeftRecipe);
	RoadClassFor(IsWalkingPath(), RightW, RightClass, RightRecipe);
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
		Road->mSideSlopes = mSideSlopes; // measured for its own shape when it begins play
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

// ---------------------------------------------------------------------------------------------
// Side slopes (1.6.0)
// ---------------------------------------------------------------------------------------------

uint32 ARMBuildableRoad::SlopeShapeKey() const
{
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 500.f);
	uint32 Key = HashCombine(SlopeVersion, GetTypeHash(FMath::RoundToInt(GetRoadWidth())));
	for (const FRMFrame& F : Frames)
	{
		Key = HashCombine(Key, GetTypeHash(FIntVector(FMath::RoundToInt(F.Pos.X), FMath::RoundToInt(F.Pos.Y), FMath::RoundToInt(F.Pos.Z))));
	}
	return Key == 0 ? 1 : Key;
}

void ARMBuildableRoad::RequestSlopeMeasure()
{
	// A tick later: a cut or smoothed road's old piece is gone by then (it would read as a neighbour road).
	if (!HasAuthority() || !mSideSlopes || mSlopeMeasureQueued || IsActorBeingDestroyed() || SlopeShapeKey() == mSlopeKey)
	{
		return;
	}
	mSlopeMeasureQueued = true;
	GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		mSlopeMeasureQueued = false;
		if (!mSideSlopes || IsActorBeingDestroyed())
		{
			return;
		}
		MeasureSlopes(mSlopeLeft, mSlopeRight);
		mSlopeKey = SlopeShapeKey();
		if (SlopeTooDeep(mSlopeLeft, mSlopeRight))
		{
			// Reshaped or copied where the ground is too far below: the banks go (no half banks).
			mSideSlopes = false;
			mSlopeLeft.Reset();
			mSlopeRight.Reset();
			UE_LOG(LogRoadMaker, Log, TEXT("%s: side slopes off, the ground is more than %.0f m below in places"), *GetName(), SlopeMaxDrop / 100.f);
		}
		RebuildMesh();
		ForceNetUpdate();
	}));
}

void ARMBuildableRoad::SetSideSlopes(bool bOn)
{
	if (!HasAuthority())
	{
		return;
	}
	mSideSlopes = bOn;
	if (bOn)
	{
		MeasureSlopes(mSlopeLeft, mSlopeRight);
		mSlopeKey = SlopeShapeKey();
		if (SlopeTooDeep(mSlopeLeft, mSlopeRight))
		{
			// The ground is too far below somewhere: no banks at all rather than half (the tool refuses it first).
			mSideSlopes = false;
			mSlopeLeft.Reset();
			mSlopeRight.Reset();
		}
	}
	else
	{
		mSlopeLeft.Reset();
		mSlopeRight.Reset();
	}
	RebuildMesh();
	ForceNetUpdate();
	UE_LOG(LogRoadMaker, Verbose, TEXT("%s: side slopes %s"), *GetName(), bOn ? TEXT("on") : TEXT("off"));
}

namespace RMSideSlopes
{
	/** Frame at a distance along the frames (interpolated). */
	FRMFrame FrameAtDist(const TArray<FRMFrame>& Frames, float Dist)
	{
		if (Dist <= Frames[0].Dist)
		{
			return Frames[0];
		}
		for (int32 I = 1; I < Frames.Num(); ++I)
		{
			if (Frames[I].Dist >= Dist)
			{
				const FRMFrame& A = Frames[I - 1];
				const FRMFrame& B = Frames[I];
				const float T = (Dist - A.Dist) / FMath::Max(B.Dist - A.Dist, KINDA_SMALL_NUMBER);
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

	/** What a bank trace stops at: the ground (true), or a road / intersection beside it (no bank: false). */
	enum class EHit { None, Ground, Neighbour };

	EHit FirstHit(UWorld* World, const FVector& From, const FVector& To, const AActor* Self, float& OutDist, const TArray<const AActor*>& PassThrough)
	{
		TArray<FHitResult> Hits;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMSideSlope), true, Self);
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic);
		Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		World->LineTraceMultiByObjectType(Hits, From, To, Objects, Params);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
		for (const FHitResult& Hit : Hits)
		{
			const AActor* Actor = Hit.GetActor();
			if (Actor == Self)
			{
				continue;
			}
			if (!Actor || Actor->IsActorBeingDestroyed() || PassThrough.Contains(Actor))
			{
				continue;
			}
			if (Cast<ARMBuildableRoad>(Actor) || Cast<ARMBuildableIntersection>(Actor))
			{
				OutDist = Hit.Distance;
				return EHit::Neighbour;
			}
			if (Cast<AFGBuildable>(Actor) || Cast<APawn>(Actor))
			{
				continue; // fences, walls, foundations, vehicles...: the bank goes on to the ground
			}
			OutDist = Hit.Distance;
			return EHit::Ground;
		}
		return EHit::None;
	}
}

float ARMBuildableRoad::MeasureBankAt(UWorld* World, const FVector& Edge, const FVector& Out, const AActor* Self, const TArray<const AActor*>& Pass)
{
	const float SlopeLen = FMath::Sqrt(1.f + SlopeRun * SlopeRun); // along the bank per cm of drop
	float Dist = 0.f;
	// The ground at or above the edge (the road runs in a dip or a cutting) or a road beside it: no bank.
	if (RMSideSlopes::FirstHit(World, Edge + FVector(0.f, 0.f, 150.f) + Out * 5.f, Edge + Out * 5.f - FVector(0.f, 0.f, 10.f), Self, Dist, Pass)
		!= RMSideSlopes::EHit::None)
	{
		return 0.f;
	}
	// Down the bank from the edge: where it meets the ground, unless a road / intersection is in the way first.
	const FVector Dir = (Out * SlopeRun - FVector(0.f, 0.f, 1.f)).GetSafeNormal();
	const FVector Start = Edge + Out * 2.f;
	switch (RMSideSlopes::FirstHit(World, Start, Start + Dir * SlopeMaxDrop * SlopeLen, Self, Dist, Pass))
	{
	case RMSideSlopes::EHit::Ground:
		return FMath::Min(Dist / SlopeLen + SlopeEmbed, SlopeMaxDrop + SlopeEmbed);
	case RMSideSlopes::EHit::None:
		return SlopeTooDeepMark; // no ground within the limit: no banks at all (no half banks)
	default:
		return 0.f;
	}
}

void ARMBuildableRoad::MeasureSlopes(TArray<FVector2D>& OutLeft, TArray<FVector2D>& OutRight) const
{
	OutLeft.Reset();
	OutRight.Reset();
	UWorld* World = GetWorld();
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 100.f);
	if (!World || Frames.Num() < 2)
	{
		return;
	}
	const float Total = Frames.Last().Dist;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(Total / SlopeSampleStep));
	// Roads joined to each end (end to end): near that end the traces pass through them (the trace at the joint itself
	// would meet the next road's end and leave the bank out there).
	TArray<const AActor*> Joined[2];
	for (int32 End = 0; End < 2; ++End)
	{
		FVector EndPos, EndOut;
		GetEndWorld(End, EndPos, EndOut);
		for (TActorIterator<ARMBuildableRoad> It(World); It; ++It)
		{
			const ARMBuildableRoad* Other = *It;
			if (Other == this || Other->IsActorBeingDestroyed() || FVector::Dist(Other->GetActorLocation(), EndPos) > Other->GetLength() + 3000.f)
			{
				continue;
			}
			for (int32 OtherEnd = 0; OtherEnd < 2; ++OtherEnd)
			{
				FVector P, O;
				Other->GetEndWorld(OtherEnd, P, O);
				if (FVector::Dist(P, EndPos) <= RMRoadUtils::JoinTolerance)
				{
					Joined[End].AddUnique(Other);
				}
			}
		}
		// An intersection this end joins (one of its mouths here): its banks carry on from the road's.
		for (TActorIterator<ARMBuildableIntersection> It(World); It; ++It)
		{
			if (It->IsActorBeingDestroyed() || FVector::Dist(It->GetActorLocation(), EndPos) > 20000.f)
			{
				continue;
			}
			TArray<FRMRoadEnd> Ends;
			It->GetWorldEnds(Ends);
			for (const FRMRoadEnd& E : Ends)
			{
				if (FVector::Dist(E.Center, EndPos) <= RMRoadUtils::JoinTolerance)
				{
					Joined[End].AddUnique(*It);
				}
			}
		}
	}
	const TArray<const AActor*> NoPass;
	const float JoinReach = 300.f; // how far from the end the joined roads are passed through
	int32 Banks = 0;
	for (int32 K = 0; K <= Steps; ++K)
	{
		const float D = Total * K / Steps;
		const FRMFrame F = RMSideSlopes::FrameAtDist(Frames, D);
		const TArray<const AActor*>& Pass = D <= JoinReach ? Joined[0] : (D >= Total - JoinReach ? Joined[1] : NoPass);
		for (const float Side : { -1.f, 1.f })
		{
			const FVector Out = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal() * Side;
			const FVector Edge = F.Pos + F.Right * (GetRoadWidth() * 0.5f * Side);
			const float Drop = MeasureBankAt(World, Edge, Out, this, Pass);
			Banks += Drop > 0.f ? 1 : 0;
			(Side < 0.f ? OutLeft : OutRight).Add(FVector2D(D, Drop));
		}
	}
	UE_LOG(LogRoadMaker, Verbose, TEXT("%s: side slopes measured, %d of %d samples with a bank"), *GetName(), Banks, (Steps + 1) * 2);
}

float ARMBuildableRoad::SlopeDropAt(const TArray<FVector2D>& Samples, float Dist)
{
	if (Samples.Num() == 0)
	{
		return 0.f;
	}
	// Too-deep marks (negative) count as no bank.
	if (Dist <= Samples[0].X)
	{
		return FMath::Max(0.f, float(Samples[0].Y));
	}
	for (int32 I = 1; I < Samples.Num(); ++I)
	{
		if (Samples[I].X >= Dist)
		{
			const FVector2D& A = Samples[I - 1];
			const FVector2D& B = Samples[I];
			return FMath::Lerp(FMath::Max(0.f, float(A.Y)), FMath::Max(0.f, float(B.Y)), float((Dist - A.X) / FMath::Max(B.X - A.X, double(KINDA_SMALL_NUMBER))));
		}
	}
	return FMath::Max(0.f, float(Samples.Last().Y));
}

bool ARMBuildableRoad::SlopeCapAt(int32 End) const
{
	if (!mSideSlopes)
	{
		return false;
	}
	for (const TArray<FVector2D>* Samples : { &mSlopeLeft, &mSlopeRight })
	{
		if (Samples->Num() > 0 && (End == 0 ? (*Samples)[0].Y : Samples->Last().Y) > 0.5f)
		{
			return true;
		}
	}
	return false;
}

bool ARMBuildableRoad::SlopeTooDeep(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right)
{
	for (const TArray<FVector2D>* Samples : { &Left, &Right })
	{
		for (const FVector2D& S : *Samples)
		{
			if (S.Y < 0.f)
			{
				return true;
			}
		}
	}
	return false;
}

void ARMBuildableRoad::SlopeCovered(const TArray<FRMFrame>& Frames, TArray<bool>& OutLeft, TArray<bool>& OutRight) const
{
	OutLeft.Reset();
	OutRight.Reset();
	if (!mSideSlopes || Frames.Num() < 2)
	{
		return;
	}
	const float Total = FMath::Max(Frames.Last().Dist, 1.f);
	for (int32 SideIndex = 0; SideIndex < 2; ++SideIndex)
	{
		const TArray<FVector2D>& Samples = SideIndex == 0 ? mSlopeLeft : mSlopeRight;
		TArray<bool>& Out = SideIndex == 0 ? OutLeft : OutRight;
		if (Samples.Num() < 2)
		{
			continue;
		}
		// As BuildSlopeMesh: frame distances mapped onto the samples' length.
		const float Scale = float(Samples.Last().X) / Total;
		for (int32 I = 0; I + 1 < Frames.Num(); ++I)
		{
			const float A = Frames[I].Dist * Scale, B = Frames[I + 1].Dist * Scale;
			float MinDrop = FMath::Min(SlopeDropAt(Samples, A), SlopeDropAt(Samples, B));
			for (const FVector2D& S : Samples)
			{
				if (S.X > A && S.X < B)
				{
					MinDrop = FMath::Min(MinDrop, FMath::Max(0.f, float(S.Y)));
				}
			}
			// Covered by the bank all along: the side face is hidden (the bank starts on its top edge), left out.
			Out.Add(MinDrop > 0.5f);
		}
	}
}

void ARMBuildableRoad::BuildSlopeMesh(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right, FRMMesh& Out) const
{
	// As the road mesh's cross-sections (RebuildMesh), without its chunk cuts.
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	RMGeometry::ReduceRoadFrames(Frames, IsWalkingPath());
	BuildSlopeMesh(Left, Right, Frames, Out);
}

void ARMBuildableRoad::BuildSlopeMesh(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right, const TArray<FRMFrame>& Frames, FRMMesh& Out) const
{
	if (Frames.Num() < 2)
	{
		return;
	}
	const float H = GetRoadWidth() * 0.5f;
	const float SlopeLen = FMath::Sqrt(1.f + SlopeRun * SlopeRun);
	const float Total = Frames.Last().Dist;
	// The road body's blocks carried on: rows counted down the bank from the edge, the same blocks along.
	const RMGeometry::FRMSideUV Layout = RMGeometry::MakeSideUV(GetShownLook() == 2, Total);
	RMGeometry::PadSideUV(Out);
	const int32 OutFirst = Out.Vertices.Num();
	const int32 OutFirstTriangle = Out.Triangles.Num();
	for (const float Side : { -1.f, 1.f })
	{
		const TArray<FVector2D>& Samples = Side < 0.f ? Left : Right;
		if (Samples.Num() < 2)
		{
			continue;
		}
		// Points along: every cross-section of the road (the bank's top edge is the road's edge, same corners), and the
		// samples between them (where the ground under the foot changes; on the chord between the two cross-sections,
		// so the edge stays the road's).
		struct FPt { float Dist; float Drop; bool bMust; FVector Edge = FVector::ZeroVector; FVector Foot = FVector::ZeroVector; FVector Out = FVector::ZeroVector; FVector Normal = FVector::UpVector; };
		TArray<FPt> Pts;
		int32 S = 0;
		for (int32 I = 0; I < Frames.Num(); ++I)
		{
			const float D = Frames[I].Dist;
			while (S < Samples.Num() && Samples[S].X < D - 5.f)
			{
				if (I > 0 && Samples[S].X > Frames[I - 1].Dist + 5.f)
				{
					Pts.Add({ float(Samples[S].X), SlopeDropAt(Samples, float(Samples[S].X)), false });
				}
				++S;
			}
			while (S < Samples.Num() && Samples[S].X <= D + 5.f)
			{
				++S;
			}
			Pts.Add({ D, SlopeDropAt(Samples, FMath::Clamp(D / FMath::Max(Total, 1.f) * Samples.Last().X, 0.f, Samples.Last().X)), true });
		}
		for (FPt& P : Pts)
		{
			const FRMFrame F = RMSideSlopes::FrameAtDist(Frames, P.Dist);
			P.Out = F.Right * Side;
			P.Edge = F.Pos + P.Out * H;
			// Straight down and level out (as measured, and as an intersection's banks): on a sloped road the foot is not
			// shifted along the road, so the banks of two pieces (or a road and an intersection) meet flush.
			P.Foot = P.Edge + P.Out * (SlopeRun * P.Drop) - FVector::UpVector * P.Drop;
			P.Normal = (P.Out + FVector::UpVector * SlopeRun).GetSafeNormal(); // square to the bank, up and out
		}
		// The inner side of a tight curve: feet that would cross are pulled together (a valley line, no bow tie).
		{
			TArray<FVector> Feet;
			for (const FPt& P : Pts)
			{
				Feet.Add(P.Foot);
			}
			RMGeometry::UntangleFeet(Feet, 0, Feet.Num() - 1);
			for (int32 I = 0; I < Pts.Num(); ++I)
			{
				Pts[I].Foot = Feet[I];
			}
		}
		auto Has = [&](int32 I) { return Pts[I].Drop > 0.5f; };
		// Optional points kept where the foot bends (10 cm: the foot is 30 cm into the ground) or a bank starts / ends.
		TArray<int32> Kept = { 0 };
		for (int32 J = 1; J < Pts.Num(); ++J)
		{
			if (Pts[J].bMust || J == Pts.Num() - 1 || Has(J) != Has(J + 1) || Has(J) != Has(J - 1))
			{
				Kept.Add(J);
				continue;
			}
			// Skip J if the foot from the last kept point to the next must point stays within 10 cm of it.
			int32 Next = J + 1;
			while (Next < Pts.Num() - 1 && !Pts[Next].bMust)
			{
				++Next;
			}
			const int32 Last = Kept.Last();
			const float T = (Pts[J].Dist - Pts[Last].Dist) / FMath::Max(Pts[Next].Dist - Pts[Last].Dist, 1.f);
			if (FVector::Dist(FMath::Lerp(Pts[Last].Foot, Pts[Next].Foot, T), Pts[J].Foot) > 10.f)
			{
				Kept.Add(J);
			}
		}
		TArray<int32> VE, VF;
		for (const int32 I : Kept)
		{
			const FVector& Normal = Pts[I].Normal;
			const FVector Tangent = FVector::CrossProduct(Normal, Pts[I].Out).GetSafeNormal();
			VE.Add(Out.AddVertex(Pts[I].Edge, Normal, FVector2D(Pts[I].Dist / 400.f, 0.f), Tangent));
			RMGeometry::AddSideUV(Out, Layout, 0.f, Pts[I].Dist / 100.f);
			VF.Add(Out.AddVertex(Pts[I].Foot, Normal, FVector2D(Pts[I].Dist / 400.f, Pts[I].Drop * SlopeLen / 400.f), Tangent));
			RMGeometry::AddSideUV(Out, Layout, Pts[I].Drop * SlopeLen / 100.f, Pts[I].Dist / 100.f);
		}
		for (int32 K = 0; K + 1 < Kept.Num(); ++K)
		{
			const int32 A = Kept[K], B = Kept[K + 1];
			if (!Has(A) && !Has(B))
			{
				continue;
			}
			const FVector& Normal = Pts[A].Normal;
			if (Has(B))
			{
				Out.AddTriangle(VE[K], VE[K + 1], VF[K + 1], Normal);
			}
			if (Has(A)) // where one end has no bank its foot is its edge: a wedge
			{
				Out.AddTriangle(VE[K], VF[K + 1], VF[K], Normal);
			}
		}
	}
	// Embankment caps: the road's cross-section widened to the banks' feet (a trapezoid), down to below the road.
	const float Height = FMath::Max(mRoadHeight, 10.f);
	for (int32 End = 0; End < 2; ++End)
	{
		const float DL = SlopeDropAt(Left, End == 0 ? 0.f : (Left.Num() ? Left.Last().X : 0.f));
		const float DR = SlopeDropAt(Right, End == 0 ? 0.f : (Right.Num() ? Right.Last().X : 0.f));
		if (DL <= 0.5f && DR <= 0.5f)
		{
			continue;
		}
		const FRMFrame& EF = End == 0 ? Frames[0] : Frames.Last();
		TArray<FVector2D> Profile = { FVector2D(-H, 0.f), FVector2D(H, 0.f) };
		int32 FootR = INDEX_NONE, FootL = INDEX_NONE;
		if (DR > 0.5f)
		{
			FootR = Profile.Num();
			Profile.Add(FVector2D(H + SlopeRun * DR, -DR));
		}
		if (DR < Height)
		{
			Profile.Add(FVector2D(H, -Height));
		}
		if (DL < Height)
		{
			Profile.Add(FVector2D(-H, -Height));
		}
		if (DL > 0.5f)
		{
			FootL = Profile.Num();
			Profile.Add(FVector2D(-H - SlopeRun * DL, -DL));
		}
		const int32 CapFirst = Out.Vertices.Num();
		RMGeometry::AddProfileCap(EF, Profile, End == 1, mUVScale, Out);
		// The feet exactly where the banks end (straight down, see above), not in the tilted cross-section's plane.
		if (FootR != INDEX_NONE)
		{
			Out.Vertices[CapFirst + FootR] = EF.Pos + EF.Right * (H + SlopeRun * DR) - FVector::UpVector * DR;
		}
		if (FootL != INDEX_NONE)
		{
			Out.Vertices[CapFirst + FootL] = EF.Pos - EF.Right * (H + SlopeRun * DL) - FVector::UpVector * DL;
		}
		RMGeometry::SideUVsFromCap(Out, Layout, CapFirst, mUVScale);
	}
	RMGeometry::WrapSideUV(Out, Layout, OutFirst, OutFirstTriangle);
}
