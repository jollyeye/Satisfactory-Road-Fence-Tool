#include "Holograms/RMRoadHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "ProceduralMeshComponent.h"

const ARMBuildableRoad* ARMRoadHologram::GetRoadDefaults() const
{
	// mBuildClass is the buildable class this hologram was spawned for (set by the game).
	return mBuildClass ? mBuildClass->GetDefaultObject<ARMBuildableRoad>() : GetDefault<ARMBuildableRoad>();
}

float ARMRoadHologram::GetSurfaceOffset() const
{
	// The bottom of the slab rests on what you aim at; the spline runs along the top.
	return GetRoadDefaults()->GetRoadHeight();
}

int32 ARMRoadHologram::GetCostWidthFactor() const
{
	return GetRoadDefaults()->mWidthInFoundations;
}

bool ARMRoadHologram::TrySnapToRoadEnd(const FHitResult& hitResult)
{
	// Any free road end near the aim point, whether you aim at the road or the ground next to it.
	TArray<FRMRoadEndRef> Ends;
	RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mSnapRadius, 1, nullptr, Ends);
	if (Ends.Num() == 0)
	{
		return false;
	}
	const FRMRoadEndRef& End = Ends[0];

	if (mStep == 0)
	{
		mStart = End.Location;
		mStartDir = End.Outward;
		mStartSnapped = true;
	}
	else
	{
		// Do not let the second click snap back onto the end we started from.
		if (mStartSnapped && FVector::Dist(End.Location, mStart) < RMRoadUtils::JoinTolerance)
		{
			return false;
		}
		mEnd = End.Location;
		mEndDir = -End.Outward; // we arrive travelling into that road
		mEndSnapped = true;
	}
	UpdateCurve();
	return true;
}

void ARMRoadHologram::RebuildPreview()
{
	const ARMBuildableRoad* Road = GetRoadDefaults();
	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 100.f);
	mPreviewMesh->ClearAllMeshSections();
	if (Frames.Num() < 2)
	{
		return;
	}
	FRMMesh Top, Body;
	RMGeometry::BuildRoad(Frames, Road->GetRoadWidth(), Road->GetRoadHeight(), 400.f, Top, Body);
	Top.ApplyToSection(mPreviewMesh, 0, false);
	Body.ApplyToSection(mPreviewMesh, 1, false);
}

void ARMRoadHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.

	if (mStep == 0)
	{
		return; // only the finished curve is judged
	}

	const ARMBuildableRoad* Road = GetRoadDefaults();
	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 50.f);
	const FRMSplineStats Stats = RMGeometry::Analyze(Frames);

	if (Stats.Length < Road->mMinLength)
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
	}
	if (Stats.Length > Road->mMaxLength)
	{
		AddConstructDisqualifier(URMCD_TooLong::StaticClass());
	}
	if (Stats.MinTurnRadius < Road->GetEffectiveMinTurnRadius())
	{
		AddConstructDisqualifier(URMCD_TurnTooTight::StaticClass());
	}
	if (Stats.MaxGradeDegrees > Road->mMaxGradeDegrees)
	{
		AddConstructDisqualifier(URMCD_TooSteep::StaticClass());
	}
}
