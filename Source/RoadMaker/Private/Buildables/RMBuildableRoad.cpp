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
	RMGeometry::BuildRoad(Frames, GetRoadWidth(), mRoadHeight, mUVScale, LimitMarkings(mPreviewMarkings.Get(mMarkings)), mEdgeLineInset, Top, Body);
	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyNearSection(Top, 0, true, TopMat);
	ApplyNearSection(Body, 1, true, BodyMat);
	// Side slopes (1.6.0): the banks in the body's concrete, solid.
	FRMMesh Slopes;
	RequestSlopeMeasure();
	if (mSideSlopes)
	{
		BuildSlopeMesh(mSlopeLeft, mSlopeRight, Slopes);
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
		RMGeometry::BuildRoad(FarFrames, GetRoadWidth(), mRoadHeight, mUVScale, LimitMarkings(mPreviewMarkings.Get(mMarkings)), mEdgeLineInset, FarTop, FarBody);
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
	uint32 Key = GetTypeHash(FMath::RoundToInt(GetRoadWidth()));
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

	EHit FirstHit(UWorld* World, const FVector& From, const FVector& To, const AActor* Self, float& OutDist)
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
			if (!Actor || Actor->IsActorBeingDestroyed())
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
	const float SlopeLen = FMath::Sqrt(1.f + SlopeRun * SlopeRun); // along the bank per cm of drop
	int32 Banks = 0;
	for (int32 K = 0; K <= Steps; ++K)
	{
		const float D = Total * K / Steps;
		const FRMFrame F = RMSideSlopes::FrameAtDist(Frames, D);
		for (const float Side : { -1.f, 1.f })
		{
			const FVector Out = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal() * Side;
			const FVector Edge = F.Pos + F.Right * (GetRoadWidth() * 0.5f * Side);
			float Drop = 0.f;
			float Dist = 0.f;
			// The ground at or above the edge (the road runs in a dip or a cutting): no bank.
			const RMSideSlopes::EHit Above = RMSideSlopes::FirstHit(World, Edge + FVector(0.f, 0.f, 150.f) + Out * 5.f, Edge + Out * 5.f - FVector(0.f, 0.f, 10.f), this, Dist);
			if (Above == RMSideSlopes::EHit::None)
			{
				// Down the bank from the edge: where it meets the ground, unless a road / intersection is in the way first.
				const FVector Dir = (Out * SlopeRun - FVector(0.f, 0.f, 1.f)).GetSafeNormal();
				const FVector Start = Edge + Out * 2.f;
				if (RMSideSlopes::FirstHit(World, Start, Start + Dir * SlopeMaxDrop * SlopeLen, this, Dist) == RMSideSlopes::EHit::Ground)
				{
					Drop = FMath::Min(Dist / SlopeLen + SlopeEmbed, SlopeMaxDrop + SlopeEmbed);
					++Banks;
				}
			}
			(Side < 0.f ? OutLeft : OutRight).Add(FVector2D(D, Drop));
		}
	}
	UE_LOG(LogRoadMaker, Verbose, TEXT("%s: side slopes measured, %d of %d samples with a bank"), *GetName(), Banks, (Steps + 1) * 2);
}

void ARMBuildableRoad::BuildSlopeMesh(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right, FRMMesh& Out) const
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, 100.f);
	if (Frames.Num() < 2)
	{
		return;
	}
	const FVector UpL = GetActorTransform().InverseTransformVectorNoScale(FVector::UpVector);
	const float SlopeLen = FMath::Sqrt(1.f + SlopeRun * SlopeRun);
	for (const float Side : { -1.f, 1.f })
	{
		const TArray<FVector2D>& Samples = Side < 0.f ? Left : Right;
		const int32 N = Samples.Num();
		if (N < 2)
		{
			continue;
		}
		// Edge and foot of each sample.
		TArray<FVector> E, Fo, OutDir;
		for (const FVector2D& S : Samples)
		{
			const FRMFrame F = RMSideSlopes::FrameAtDist(Frames, S.X);
			const FVector O = (F.Right - UpL * FVector::DotProduct(F.Right, UpL)).GetSafeNormal() * Side;
			const FVector Edge = F.Pos + F.Right * (GetRoadWidth() * 0.5f * Side);
			E.Add(Edge);
			Fo.Add(Edge + O * (SlopeRun * S.Y) - UpL * S.Y);
			OutDir.Add(O);
		}
		// Kept samples: every start / end of a bank, and where edge or foot bends (within 3 cm), at least every 20 m.
		auto Deviates = [&](int32 From, int32 To)
		{
			for (int32 M = From + 1; M < To; ++M)
			{
				const float T = float(M - From) / float(To - From);
				if (FVector::Dist(FMath::Lerp(E[From], E[To], T), E[M]) > 3.f || FVector::Dist(FMath::Lerp(Fo[From], Fo[To], T), Fo[M]) > 3.f)
				{
					return true;
				}
			}
			return false;
		};
		auto Has = [&](int32 I) { return Samples[I].Y > 0.f; };
		TArray<int32> Kept = { 0 };
		for (int32 J = 2; J <= N; ++J)
		{
			const int32 Last = Kept.Last();
			const bool bEnd = J == N;
			// A bank starting or ending between J-2 and J-1 keeps J-1 (and the sample before a change).
			const bool bChange = !bEnd && (Has(J - 1) != Has(J) || Has(J - 1) != Has(Last));
			if (bEnd || bChange || Deviates(Last, J) || Samples[J].X - Samples[Last].X > 2000.f)
			{
				Kept.Add(J - 1);
			}
		}
		// One strip per side, vertices shared: edge and foot per kept sample.
		TArray<int32> VE, VF;
		for (const int32 I : Kept)
		{
			const FVector Normal = (OutDir[I] + UpL * SlopeRun).GetSafeNormal(); // square to the bank, up and out
			VE.Add(Out.AddVertex(E[I], Normal, FVector2D(Samples[I].X / 400.f, 0.f), FVector::CrossProduct(Normal, OutDir[I]).GetSafeNormal()));
			VF.Add(Out.AddVertex(Fo[I], Normal, FVector2D(Samples[I].X / 400.f, Samples[I].Y * SlopeLen / 400.f), FVector::CrossProduct(Normal, OutDir[I]).GetSafeNormal()));
		}
		for (int32 K = 0; K + 1 < Kept.Num(); ++K)
		{
			const int32 A = Kept[K], B = Kept[K + 1];
			if (!Has(A) && !Has(B))
			{
				continue;
			}
			const FVector Normal = (OutDir[A] + UpL * SlopeRun).GetSafeNormal();
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
}
