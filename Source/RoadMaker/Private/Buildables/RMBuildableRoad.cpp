#include "Buildables/RMBuildableRoad.h"
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
	ApplyMaterial(0, mTopMaterial);
	ApplyMaterial(1, mBodyMaterial);

	// Far version: the same road sampled every 4 m instead of every 1 m.
	ClearFarMesh();
	TArray<FRMFrame> FarFrames;
	GetLocalFrames(FarFrames, 400.f);
	if (FarFrames.Num() >= 2)
	{
		FRMMesh FarTop, FarBody;
		RMGeometry::BuildRoad(FarFrames, GetRoadWidth(), mRoadHeight, mUVScale, mPreviewMarkings.Get(mMarkings), mEdgeLineInset, FarTop, FarBody);
		ApplyFarSection(FarTop, 0, mTopMaterial);
		ApplyFarSection(FarBody, 1, mBodyMaterial);
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
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
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
	/** Spawns a copy of Source (class, recipe, paint, lines) along the given world curve. */
	ARMBuildableRoad* SpawnRoadPiece(ARMBuildableRoad* Source, const TArray<FRMSplinePoint>& World, int32 CostMultiplier)
	{
		UWorld* W = Source->GetWorld();
		if (!W || World.Num() < 2)
		{
			return nullptr;
		}
		// Same convention as the hologram: actor at the first point, yawed along its direction.
		const FVector Dir = FVector(World[0].Tangent.X, World[0].Tangent.Y, 0.f).GetSafeNormal();
		const FRotator Yaw(0.f, Dir.IsNearlyZero() ? 0.f : Dir.Rotation().Yaw, 0.f);
		const FTransform T(Yaw, World[0].Location);
		TArray<FRMSplinePoint> Local;
		for (const FRMSplinePoint& P : World)
		{
			Local.Add(FRMSplinePoint(T.InverseTransformPosition(P.Location), T.InverseTransformVectorNoScale(P.Tangent)));
		}
		ARMBuildableRoad* New = W->SpawnActorDeferred<ARMBuildableRoad>(Source->GetClass(), T, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!New)
		{
			return nullptr;
		}
		New->SetBuiltWithRecipe(Source->GetBuiltWithRecipe());
		New->SetCostMultiplier(CostMultiplier);
		New->SetSplinePoints(Local);
		New->SetMarkings(Source->GetMarkings());
		New->SetCustomizationData_Native(Source->GetCustomizationData_Native());
		UGameplayStatics::FinishSpawningActor(New, T);
		return New;
	}

	/**
	 * Where a track / path spline crosses the cut: the plane through the cut point facing along the
	 * road, within the road's width (plus 1 m) and 4 m above / below. Not within 1 m of its ends.
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
				if (FMath::Abs(FVector::DotProduct(P, Cut.Right)) <= HalfWidth + 100.f && FMath::Abs(FVector::DotProduct(P, Cut.Up)) <= 400.f
					&& Cross > 100.f && Cross < Length - 100.f)
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
}

ARMBuildableRoad* ARMBuildableRoad::CutAt(float Dist, ARMBuildableRoad** OutSecond)
{
	if (OutSecond)
	{
		*OutSecond = nullptr;
	}
	if (!HasAuthority() || GetIsDismantled() || IsActorBeingDestroyed())
	{
		return nullptr;
	}
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 25.f);
	if (Frames.Num() < 3)
	{
		return nullptr;
	}
	const float Total = Frames.Last().Dist;
	if (Dist <= 1.f || Dist >= Total - 1.f)
	{
		return nullptr;
	}

	// The exact cut frame, between the two samples around Dist.
	FRMFrame Cut = Frames.Last();
	for (int32 I = 1; I < Frames.Num(); ++I)
	{
		if (Frames[I].Dist >= Dist)
		{
			const FRMFrame& A = Frames[I - 1];
			const FRMFrame& B = Frames[I];
			const float Alpha = (Dist - A.Dist) / FMath::Max(B.Dist - A.Dist, KINDA_SMALL_NUMBER);
			Cut = A;
			Cut.Pos = FMath::Lerp(A.Pos, B.Pos, Alpha);
			Cut.Fwd = FMath::Lerp(A.Fwd, B.Fwd, Alpha).GetSafeNormal();
			Cut.Right = FMath::Lerp(A.Right, B.Right, Alpha).GetSafeNormal();
			Cut.Up = FMath::Lerp(A.Up, B.Up, Alpha).GetSafeNormal();
			Cut.Dist = Dist;
			break;
		}
	}
	TArray<FRMFrame> FirstFrames;
	TArray<FRMFrame> SecondFrames;
	SecondFrames.Add(Cut);
	for (const FRMFrame& F : Frames)
	{
		if (F.Dist < Dist - 0.5f)
		{
			FirstFrames.Add(F);
		}
		else if (F.Dist > Dist + 0.5f)
		{
			SecondFrames.Add(F);
		}
	}
	FirstFrames.Add(Cut);
	// Second piece measured from its own start.
	for (FRMFrame& F : SecondFrames)
	{
		F.Dist -= Dist;
	}

	// Each piece is refitted through its own part of the curve (keys every 2 m, exact end points and
	// end directions), so both follow the original road.
	TArray<FRMSplinePoint> FirstPoints, SecondPoints;
	RMGeometry::OffsetCurve(FirstFrames, 0.f, 200.f, FirstPoints);
	RMGeometry::OffsetCurve(SecondFrames, 0.f, 200.f, SecondPoints);
	if (FirstPoints.Num() < 2 || SecondPoints.Num() < 2)
	{
		return nullptr;
	}
	const int32 FirstCost = FMath::Max(1, FMath::RoundToInt(mCostMultiplier * Dist / Total));
	const int32 SecondCost = FMath::Max(1, mCostMultiplier - FirstCost);
	ARMBuildableRoad* First = SpawnRoadPiece(this, FirstPoints, FirstCost);
	ARMBuildableRoad* Second = SpawnRoadPiece(this, SecondPoints, SecondCost);
	if (!First || !Second)
	{
		if (First)
		{
			First->Destroy();
		}
		if (Second)
		{
			Second->Destroy();
		}
		return nullptr;
	}
	// Game rail tracks and vehicle paths built on this road are split at the same place (the game's
	// own split, which keeps the pieces connected).
	TArray<AActor*> Riders;
	IFGDismantleInterface::Execute_GetChildDismantleActors(this, Riders);
	const float HalfWidth = GetRoadWidth() * 0.5f;
	for (AActor* Rider : Riders)
	{
		float At = 0.f;
		if (AFGBuildableRailroadTrack* Track = Cast<AFGBuildableRailroadTrack>(Rider))
		{
			if (Track->GetSplineComponent() && FindCutCrossing(Track->GetSplineComponent(), Cut, HalfWidth, At))
			{
				AFGBuildableRailroadTrack::Split(Track, At, true);
			}
		}
		else if (AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(Rider))
		{
			if (Path->GetSplineComponent() && FindCutCrossing(Path->GetSplineComponent(), Cut, HalfWidth, At))
			{
				Path->Split(At);
			}
		}
	}

	// Out of the way at once (the dismantle takes a moment): aiming must find the new pieces. Its
	// riders stay (they were split above and now run on the new pieces).
	mBeingCut = true;
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	IFGDismantleInterface::Execute_Dismantle(this);
	if (OutSecond)
	{
		*OutSecond = Second;
	}
	return First;
}
