// World queries shared by the holograms: finding road ends and checking whether they are taken.
#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "RMGeometry.h"

class ARMBuildableRoad;
class ARMBuildableIntersection;

/** A free end: a road end, or an intersection mouth with no road attached. */
struct ROADMAKER_API FRMRoadEndRef
{
	ARMBuildableRoad* Road = nullptr;                 // set for road ends
	ARMBuildableIntersection* Intersection = nullptr; // set for intersection mouths
	int32 EndIndex = 0;
	FVector Location = FVector::ZeroVector; // top-centre, world
	FVector Outward = FVector::ForwardVector; // direction a road attached here continues in
	float Width = 0.f;
	float Height = 0.f;
	float Distance = 0.f; // to the query point

	/** Points every Step cm across the end (centre included, out to the edges). */
	TArray<FVector> GetPointsAcross(float Step) const;

	/**
	 * Snap points for vehicle paths and rail tracks across this end: on 1-lane roads every 2 m, on
	 * 2- and 3-lane roads one point in the middle of each lane (lanes measured between the edge
	 * lines). Intersection ends use the lanes of the road joined there (2 m rule if none).
	 */
	TArray<FVector> GetSnapPoints(UWorld* World) const;
};

namespace RMRoadUtils
{
	/** Ends closer than this are considered joined. */
	constexpr float JoinTolerance = 30.f;

	/** Every road and intersection whose collision overlaps the sphere. */
	ROADMAKER_API void GatherNearby(UWorld* World, const FVector& Center, float Radius,
		TArray<ARMBuildableRoad*>& OutRoads, TArray<ARMBuildableIntersection*>* OutIntersections = nullptr);

	/** True if another road end or an intersection already sits on this end. */
	ROADMAKER_API bool IsEndOccupied(UWorld* World, const ARMBuildableRoad* Road, int32 EndIndex);

	/** True if a road end or another intersection already sits on this mouth. */
	ROADMAKER_API bool IsMouthOccupied(UWorld* World, const ARMBuildableIntersection* Intersection, const FVector& MouthCenter);

	/**
	 * Free road ends and free intersection mouths within Radius of Center, nearest first.
	 * bIncludeOccupied also returns ends that are already joined (used for path snap points).
	 */
	ROADMAKER_API void FindFreeEnds(UWorld* World, const FVector& Center, float Radius, int32 MaxCount,
		const AActor* Ignore, TArray<FRMRoadEndRef>& OutEnds, bool bIncludeOccupied = false);

	/**
	 * Where a new road NewWidth wide can attach near Center: every road / intersection end gives its centre
	 * and, when it is wider than the new road, places across it in 8 m steps (edge to edge with its sides),
	 * so a road split lengthwise can be joined back piece by piece. A place is free when no other road or
	 * intersection end overlaps it there. Each result is a copy of its end with Location moved to the place
	 * and Width = NewWidth; nearest first.
	 */
	ROADMAKER_API void FindEndSlots(UWorld* World, const FVector& Center, float Radius, float NewWidth, TArray<FRMRoadEndRef>& OutSlots);

	/**
	 * If the hit is on a foundation-shaped building piece (classic or instanced), its transform and
	 * local bounding box. Road Maker pieces are not foundations.
	 */
	/**
	 * Lane markings lie on roads but are pieces of their own, so the aim hits them first. This turns a hit on
	 * a marking into a hit on the road under it (same point); any other hit is returned as it is.
	 */
	ROADMAKER_API FHitResult ThroughMarking(const FHitResult& Hit);

	ROADMAKER_API bool GetFoundationPiece(const FHitResult& Hit, FTransform& OutTransform, FBox& OutLocalBox);
}
