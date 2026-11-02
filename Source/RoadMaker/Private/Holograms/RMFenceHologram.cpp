#include "Holograms/RMFenceHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"

const ARMBuildableFence* ARMFenceHologram::GetFenceDefaults() const
{
	return mBuildClass ? mBuildClass->GetDefaultObject<ARMBuildableFence>() : GetDefault<ARMBuildableFence>();
}

bool ARMFenceHologram::TrySnapToActor(const FHitResult& hitResult)
{
	mOnRoadEdge = false;

	// Edge mode only makes sense before a free fence has been started.
	ARMBuildableRoad* Road = mStep == 0 ? Cast<ARMBuildableRoad>(hitResult.GetActor()) : nullptr;
	if (!Road)
	{
		return false;
	}

	TArray<FRMFrame> Frames;
	Road->GetWorldFrames(Frames, 50.f);
	if (Frames.Num() < 2)
	{
		return false;
	}

	// Which side: the sign of the hit point along the road's right vector at the closest sample.
	int32 Closest = 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		const float D = FVector::DistSquared(Frames[I].Pos, hitResult.Location);
		if (D < Best)
		{
			Best = D;
			Closest = I;
		}
	}
	const float Side = FVector::DotProduct(hitResult.Location - Frames[Closest].Pos, Frames[Closest].Right) >= 0.f ? 1.f : -1.f;

	// Edge curve: P(s) + d * N(s), with d just inside the edge so the fence stands on the asphalt.
	const float Offset = Side * (Road->GetRoadWidth() * 0.5f - GetFenceDefaults()->mEdgeInset);
	TArray<FRMSplinePoint> Edge;
	RMGeometry::OffsetCurve(Frames, Offset, 400.f, Edge);

	mOnRoadEdge = true;
	SetWorldPoints(Edge);
	return true;
}

void ARMFenceHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	mOnRoadEdge = false;
	Super::SetHologramLocationAndRotation(hitResult);
}

bool ARMFenceHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mOnRoadEdge)
	{
		return true; // the whole edge is already known: build right away
	}
	return Super::DoMultiStepPlacement(isInputFromARelease);
}

void ARMFenceHologram::RebuildPreview()
{
	const ARMBuildableFence* Fence = GetFenceDefaults();
	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 50.f);
	mPreviewMesh->ClearAllMeshSections();
	if (Frames.Num() < 2)
	{
		return;
	}

	FRMMesh Main, Panel;
	switch (Fence->mFenceType)
	{
	case ERMFenceType::GuardRail:     RMGeometry::BuildGuardRail(Frames, 400.f, Main); break;
	case ERMFenceType::JerseyBarrier: RMGeometry::BuildJerseyBarrier(Frames, 400.f, Main); break;
	case ERMFenceType::GridFence:     RMGeometry::BuildGridFence(Frames, 400.f, Main, Panel); break;
	}
	Main.ApplyToSection(mPreviewMesh, 0, false);
	Panel.ApplyToSection(mPreviewMesh, 1, false);
}

void ARMFenceHologram::CheckValidPlacement()
{
	Super::CheckValidPlacement();
	if (mStep == 1 && !mOnRoadEdge)
	{
		TArray<FRMFrame> Frames;
		GetPreviewFrames(Frames, 100.f);
		if (Frames.Num() < 2 || Frames.Last().Dist < 100.f)
		{
			AddConstructDisqualifier(URMCD_TooShort::StaticClass());
		}
	}
}
