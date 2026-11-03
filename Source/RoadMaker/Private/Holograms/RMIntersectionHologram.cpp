#include "Holograms/RMIntersectionHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"

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

void ARMIntersectionHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	TArray<FRMRoadEndRef> Found;
	RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mSearchRadius, mMaxRoads, nullptr, Found);

	if (Found.Num() == 0)
	{
		SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
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

	// Only rebuild the preview when the set of ends actually changed.
	bool bChanged = Local.Num() != mLocalEnds.Num() || !FMath::IsNearlyEqual(Height, mHeight);
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

void ARMIntersectionHologram::RebuildPreview()
{
	mPreviewMesh->ClearAllMeshSections();
	mGeometryValid = false;
	if (mLocalEnds.Num() < 2)
	{
		return;
	}
	FRMMesh Top, Body;
	mGeometryValid = RMGeometry::BuildIntersection(mLocalEnds, mHeight, 400.f, 10, Top, Body);
	if (mGeometryValid)
	{
		Top.ApplyToSection(mPreviewMesh, 0, false);
		Body.ApplyToSection(mPreviewMesh, 1, false);
		if (mPreviewMaterial)
		{
			mPreviewMesh->SetMaterial(0, mPreviewMaterial);
			mPreviewMesh->SetMaterial(1, mPreviewMaterial);
		}
	}
}

void ARMIntersectionHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.
	if (mLocalEnds.Num() < 2)
	{
		AddConstructDisqualifier(URMCD_NotEnoughRoads::StaticClass());
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
