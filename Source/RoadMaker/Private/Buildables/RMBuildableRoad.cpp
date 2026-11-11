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
	ARMBuildableSpline* Left = SpawnLike(LeftClass, LeftRecipe, LeftPoints, Cost);
	ARMBuildableSpline* Right = SpawnLike(RightClass, RightRecipe, RightPoints, Cost);
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
