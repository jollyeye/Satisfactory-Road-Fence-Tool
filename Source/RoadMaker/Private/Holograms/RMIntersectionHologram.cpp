#include "Holograms/RMIntersectionHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Holograms/RMHologramLook.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "HAL/PlatformTime.h"

ARMIntersectionHologram::ARMIntersectionHologram()
{
	mPreviewMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMesh"));
	if (RootComponent)
	{
		mPreviewMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mPreviewMesh);
	}
	mPreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMesh->SetCastShadow(false);
}

void ARMIntersectionHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMIntersectionHologram, mLocalEnds);
	DOREPLIFETIME(ARMIntersectionHologram, mHeight);
	DOREPLIFETIME(ARMIntersectionHologram, mSideLines);
}

namespace
{
	bool SameEnd(const FRMRoadEndRef& A, const FRMRoadEndRef& B)
	{
		return A.Road == B.Road && A.Intersection == B.Intersection && A.EndIndex == B.EndIndex;
	}
}

bool ARMIntersectionHologram::IsPickMode() const
{
	return mBuildModePick && IsCurrentBuildMode(mBuildModePick);
}

void ARMIntersectionHologram::GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const
{
	Super::GetSupportedBuildModes_Implementation(out_buildmodes);
	if (mBuildModePick)
	{
		out_buildmodes.AddUnique(TSubclassOf<UFGBuildGunModeDescriptor>(mBuildModePick.Get()));
	}
}

void ARMIntersectionHologram::OnBuildModeChanged(TSubclassOf<UFGHologramBuildModeDescriptor> buildMode)
{
	Super::OnBuildModeChanged(buildMode);
	mPicked.Reset();
	mHasHover = false;
	mLocalEnds.Reset();
	RebuildPreview();
}

void ARMIntersectionHologram::RefreshPicks()
{
	for (int32 I = mPicked.Num() - 1; I >= 0; --I)
	{
		TArray<FRMRoadEndRef> Near;
		RMRoadUtils::FindFreeEnds(GetWorld(), mPicked[I].Location, 200.f, 0, nullptr, Near);
		const FRMRoadEndRef* Same = Near.FindByPredicate([this, I](const FRMRoadEndRef& R) { return SameEnd(R, mPicked[I]); });
		if (Same)
		{
			mPicked[I] = *Same;
		}
		else
		{
			mPicked.RemoveAt(I);
		}
	}
}

void ARMIntersectionHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	TArray<FRMRoadEndRef> Found;
	if (IsPickMode())
	{
		RefreshPicks();
		TArray<FRMRoadEndRef> Near;
		RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mPickRadius, 1, nullptr, Near);
		mHasHover = Near.Num() > 0;
		if (mHasHover)
		{
			mHover = Near[0];
		}
		Found = mPicked;
	}
	else
	{
		mHasHover = false;
		RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mSearchRadius, mMaxRoads, nullptr, Found);
	}
	ApplyEnds(Found, hitResult.Location);
}

bool ARMIntersectionHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (!IsPickMode())
	{
		return true;
	}
	if (mHasHover)
	{
		// One toggle per click, even if the game reports the click twice (press and release).
		const double Now = FPlatformTime::Seconds();
		if (Now - mLastPickTime > 0.25)
		{
			mLastPickTime = Now;
			const int32 Index = mPicked.IndexOfByPredicate([this](const FRMRoadEndRef& R) { return SameEnd(R, mHover); });
			if (Index != INDEX_NONE)
			{
				mPicked.RemoveAt(Index);
			}
			else if (mPicked.Num() < mMaxRoads
				&& !mPicked.ContainsByPredicate([this](const FRMRoadEndRef& R) { return FVector::Dist(R.Location, mHover.Location) > mPickMaxSpan; }))
			{
				mPicked.Add(mHover);
			}
			mMarkerKey = 0; // redraw
		}
		return false;
	}
	return mPicked.Num() >= 2; // clicked away from the ends: build
}

uint32 ARMIntersectionHologram::MarkerKey() const
{
	uint32 Key = GetTypeHash(mPicked.Num()) + 1;
	if (mHasHover)
	{
		Key = HashCombine(Key, HashCombine(GetTypeHash(mHover.Road), HashCombine(GetTypeHash(mHover.Intersection), GetTypeHash(mHover.EndIndex))));
	}
	for (const FRMRoadEndRef& R : mPicked)
	{
		Key = HashCombine(Key, HashCombine(GetTypeHash(R.Road), HashCombine(GetTypeHash(R.Intersection), GetTypeHash(R.EndIndex))));
	}
	return Key;
}

void ARMIntersectionHologram::ApplyEnds(const TArray<FRMRoadEndRef>& Found, const FVector& FallbackLocation)
{
	const uint32 Key = IsPickMode() ? MarkerKey() : 0;
	const bool bMarkersChanged = Key != mMarkerKey;
	mMarkerKey = Key;

	if (Found.Num() == 0)
	{
		SetActorLocationAndRotation(FallbackLocation, FRotator::ZeroRotator);
		mLocalEnds.Reset();
		RebuildPreview();
		return;
	}

	// The slab is as thick as the thickest connected road.
	FVector Origin = FVector::ZeroVector;
	float Height = 0.f;
	bool bSideLines = false;
	for (const FRMRoadEndRef& R : Found)
	{
		Origin += R.Location;
		Height = FMath::Max(Height, R.Height);
		bSideLines |= (R.Road && R.Road->GetMarkings().bEdgeLines) || (R.Intersection && R.Intersection->HasSideLines());
	}
	mSideLines = bSideLines;
	Origin /= Found.Num();
	SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);

	TArray<FRMRoadEnd> Local;
	for (const FRMRoadEndRef& R : Found)
	{
		FRMRoadEnd E;
		E.Center = R.Location - Origin;
		E.Inward = FVector(R.Outward.X, R.Outward.Y, 0.f).GetSafeNormal();
		E.Width = R.Width;
		Local.Add(E);
	}

	// Only rebuild the preview when the set of ends (or the pick markers) actually changed.
	bool bChanged = bMarkersChanged || Local.Num() != mLocalEnds.Num() || !FMath::IsNearlyEqual(Height, mHeight);
	for (int32 I = 0; !bChanged && I < Local.Num(); ++I)
	{
		bChanged = !Local[I].Center.Equals(mLocalEnds[I].Center, 1.f);
	}
	if (bChanged)
	{
		mLocalEnds = Local;
		mHeight = Height;
		RebuildPreview();
	}
}

void ARMIntersectionHologram::BuildMarkers(FRMMesh& Out) const
{
	const FVector Base = GetActorLocation();
	auto EndAxes = [](const FRMRoadEndRef& R, FVector& In, FVector& Right)
	{
		In = FVector(R.Outward.X, R.Outward.Y, 0.f).GetSafeNormal();
		Right = FVector::CrossProduct(FVector::UpVector, In);
	};
	// Picked: a bar across the end.
	for (const FRMRoadEndRef& R : mPicked)
	{
		FVector In, Right;
		EndAxes(R, In, Right);
		Out.AddBox(R.Location - Base + In * 100.f + FVector(0.f, 0.f, 20.f), In, Right, FVector::UpVector,
			FVector(100.f, R.Width * 0.5f, 20.f), 0.01f);
	}
	// Aimed at: two posts at the end's corners (a click picks it, or drops it if already picked).
	if (mHasHover)
	{
		FVector In, Right;
		EndAxes(mHover, In, Right);
		for (const float Side : { -1.f, 1.f })
		{
			Out.AddBox(mHover.Location - Base + Right * (Side * (mHover.Width * 0.5f - 50.f)) + FVector(0.f, 0.f, 200.f),
				In, Right, FVector::UpVector, FVector(50.f, 50.f, 200.f), 0.01f);
		}
	}
}

void ARMIntersectionHologram::RebuildPreview()
{
	mPreviewMesh->ClearAllMeshSections();
	mGeometryValid = false;
	if (mLocalEnds.Num() >= 2)
	{
		FRMMesh Top, Body;
		mGeometryValid = RMGeometry::BuildIntersection(mLocalEnds, mHeight, 400.f, 10, Top, Body);
		if (mGeometryValid)
		{
			Top.ApplyToSection(mPreviewMesh, 0, false);
			Body.ApplyToSection(mPreviewMesh, 1, false);
		}
	}
	if (IsPickMode())
	{
		FRMMesh Markers;
		BuildMarkers(Markers);
		if (!Markers.IsEmpty())
		{
			Markers.ApplyToSection(mPreviewMesh, 2, false);
		}
	}
	UpdateHologramLook();
}

void ARMIntersectionHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateHologramLook();
}

void ARMIntersectionHologram::UpdateHologramLook()
{
	UMaterialInterface* Material = mPreviewMaterial;
	if (!Material)
	{
		Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
			? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	}
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}


void ARMIntersectionHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.
	if (IsPickMode() && mHasHover)
	{
		return; // this click picks (or drops) the aimed road end
	}
	if (mLocalEnds.Num() < 2)
	{
		AddConstructDisqualifier(IsPickMode() ? URMCD_PickRoadEnds::StaticClass() : URMCD_NotEnoughRoads::StaticClass());
	}
	else if (!mGeometryValid)
	{
		AddConstructDisqualifier(URMCD_BadIntersection::StaticClass());
	}
}

void ARMIntersectionHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(inBuildable))
	{
		X->SetSideLines(mSideLines);
		X->SetEnds(mLocalEnds, mHeight);
		X->SetCostMultiplier(GetBaseCostMultiplier());
	}
}
