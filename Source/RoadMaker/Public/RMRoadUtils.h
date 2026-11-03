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
	 * If the hit is on a foundation-shaped building piece (classic or instanced), its transform and
	 * local bounding box. Road Maker pieces are not foundations.
	 */
	ROADMAKER_API bool GetFoundationPiece(const FHitResult& Hit, FTransform& OutTransform, FBox& OutLocalBox);
}
