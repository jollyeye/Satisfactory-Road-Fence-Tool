// World queries shared by the holograms: finding road ends and checking whether they are taken.
#pragma once

#include "CoreMinimal.h"
#include "RMGeometry.h"

class ARMBuildableRoad;
class ARMBuildableIntersection;

struct ROADMAKER_API FRMRoadEndRef
{
	ARMBuildableRoad* Road = nullptr;
	int32 EndIndex = 0;
	FVector Location = FVector::ZeroVector; // top-centre, world
	FVector Outward = FVector::ForwardVector;
	float Distance = 0.f; // to the query point
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

	/** Free road ends within Radius of Center, nearest first. */
	ROADMAKER_API void FindFreeEnds(UWorld* World, const FVector& Center, float Radius, int32 MaxCount,
		const AActor* Ignore, TArray<FRMRoadEndRef>& OutEnds);
}
