#include "RMRoadUtils.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "CollisionQueryParams.h"

void RMRoadUtils::GatherNearby(UWorld* World, const FVector& Center, float Radius,
	TArray<ARMBuildableRoad*>& OutRoads, TArray<ARMBuildableIntersection*>* OutIntersections)
{
	OutRoads.Reset();
	if (OutIntersections)
	{
		OutIntersections->Reset();
	}
	if (!World)
	{
		return;
	}

	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RMGatherNearby), true);
	World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, ObjectParams, FCollisionShape::MakeSphere(Radius), Params);

	for (const FOverlapResult& O : Overlaps)
	{
		AActor* Actor = O.GetActor();
		if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Actor))
		{
			OutRoads.AddUnique(Road);
		}
		else if (OutIntersections)
		{
			if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(Actor))
			{
				OutIntersections->AddUnique(X);
			}
		}
	}
}

bool RMRoadUtils::IsEndOccupied(UWorld* World, const ARMBuildableRoad* Road, int32 EndIndex)
{
	if (!Road)
	{
		return true;
	}
	FVector Loc, Out;
	Road->GetEndWorld(EndIndex, Loc, Out);

	TArray<ARMBuildableRoad*> Roads;
	TArray<ARMBuildableIntersection*> Intersections;
	GatherNearby(World, Loc, 150.f, Roads, &Intersections);

	for (ARMBuildableRoad* Other : Roads)
	{
		if (Other == Road)
		{
			continue;
		}
		for (int32 E = 0; E < 2; ++E)
		{
			FVector OL, OO;
			Other->GetEndWorld(E, OL, OO);
			if (FVector::Dist(OL, Loc) < JoinTolerance)
			{
				return true;
			}
		}
	}
	for (ARMBuildableIntersection* X : Intersections)
	{
		TArray<FRMRoadEnd> Ends;
		X->GetWorldEnds(Ends);
		for (const FRMRoadEnd& E : Ends)
		{
			if (FVector::Dist(E.Center, Loc) < JoinTolerance)
			{
				return true;
			}
		}
	}
	return false;
}

void RMRoadUtils::FindFreeEnds(UWorld* World, const FVector& Center, float Radius, int32 MaxCount,
	const AActor* Ignore, TArray<FRMRoadEndRef>& OutEnds)
{
	OutEnds.Reset();
	TArray<ARMBuildableRoad*> Roads;
	GatherNearby(World, Center, Radius, Roads);

	for (ARMBuildableRoad* Road : Roads)
	{
		if (Road == Ignore)
		{
			continue;
		}
		for (int32 E = 0; E < 2; ++E)
		{
			FRMRoadEndRef Ref;
			Ref.Road = Road;
			Ref.EndIndex = E;
			Road->GetEndWorld(E, Ref.Location, Ref.Outward);
			Ref.Distance = FVector::Dist(Ref.Location, Center);
			if (Ref.Distance <= Radius && !IsEndOccupied(World, Road, E))
			{
				OutEnds.Add(Ref);
			}
		}
	}

	OutEnds.Sort([](const FRMRoadEndRef& A, const FRMRoadEndRef& B) { return A.Distance < B.Distance; });
	if (MaxCount > 0 && OutEnds.Num() > MaxCount)
	{
		OutEnds.SetNum(MaxCount);
	}
}
