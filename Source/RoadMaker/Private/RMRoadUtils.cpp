#include "RMRoadUtils.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "CollisionQueryParams.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

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
	const AActor* Ignore, TArray<FRMRoadEndRef>& OutEnds, bool bIncludeOccupied)
{
	OutEnds.Reset();
	TArray<ARMBuildableRoad*> Roads;
	TArray<ARMBuildableIntersection*> Intersections;
	GatherNearby(World, Center, Radius, Roads, &Intersections);

	// Intersection mouths whose road was removed (or never built) are free ends too.
	for (ARMBuildableIntersection* X : Intersections)
	{
		if (X == Ignore)
		{
			continue;
		}
		TArray<FRMRoadEnd> Mouths;
		X->GetWorldEnds(Mouths);
		for (int32 M = 0; M < Mouths.Num(); ++M)
		{
			const FRMRoadEnd& Mouth = Mouths[M];
			FRMRoadEndRef Ref;
			Ref.Intersection = X;
			Ref.EndIndex = M;
			Ref.Location = Mouth.Center;
			Ref.Outward = -FVector(Mouth.Inward.X, Mouth.Inward.Y, 0.f).GetSafeNormal();
			Ref.Width = Mouth.Width;
			Ref.Height = X->GetSlabHeight();
			Ref.Distance = FVector::Dist(Ref.Location, Center);
			if (Ref.Distance <= Radius && (bIncludeOccupied || !IsMouthOccupied(World, X, Mouth.Center)))
			{
				OutEnds.Add(Ref);
			}
		}
	}

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
			Ref.Width = Road->GetRoadWidth();
			Ref.Height = Road->GetRoadHeight();
			Ref.Distance = FVector::Dist(Ref.Location, Center);
			if (Ref.Distance <= Radius && (bIncludeOccupied || !IsEndOccupied(World, Road, E)))
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

bool RMRoadUtils::IsMouthOccupied(UWorld* World, const ARMBuildableIntersection* Intersection, const FVector& MouthCenter)
{
	TArray<ARMBuildableRoad*> Roads;
	TArray<ARMBuildableIntersection*> Intersections;
	GatherNearby(World, MouthCenter, 150.f, Roads, &Intersections);
	for (ARMBuildableRoad* Road : Roads)
	{
		for (int32 E = 0; E < 2; ++E)
		{
			FVector L, O;
			Road->GetEndWorld(E, L, O);
			if (FVector::Dist(L, MouthCenter) < JoinTolerance)
			{
				return true;
			}
		}
	}
	for (ARMBuildableIntersection* Other : Intersections)
	{
		if (Other == Intersection)
		{
			continue;
		}
		TArray<FRMRoadEnd> Ends;
		Other->GetWorldEnds(Ends);
		for (const FRMRoadEnd& E : Ends)
		{
			if (FVector::Dist(E.Center, MouthCenter) < JoinTolerance)
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FVector> FRMRoadEndRef::GetPointsAcross(float Step) const
{
	TArray<FVector> Points;
	const FVector Across = FVector::CrossProduct(FVector::UpVector, FVector(Outward.X, Outward.Y, 0.f)).GetSafeNormal();
	const float Half = Width * 0.5f;
	const int32 Count = FMath::FloorToInt(Half / FMath::Max(Step, 1.f));
	for (int32 K = -Count; K <= Count; ++K)
	{
		Points.Add(Location + Across * (K * Step));
	}
	return Points;
}

TArray<FVector> FRMRoadEndRef::GetSnapPoints(UWorld* World) const
{
	// Lane layout of this end: the road itself, or the road joined to this intersection end.
	const ARMBuildableRoad* LaneRoad = Road;
	if (!LaneRoad && Intersection && World)
	{
		TArray<ARMBuildableRoad*> Nearby;
		RMRoadUtils::GatherNearby(World, Location, Width, Nearby);
		for (const ARMBuildableRoad* Candidate : Nearby)
		{
			for (int32 E = 0; E < 2 && !LaneRoad; ++E)
			{
				FVector EndLoc, EndOut;
				Candidate->GetEndWorld(E, EndLoc, EndOut);
				if (FVector::Dist(EndLoc, Location) < RMRoadUtils::JoinTolerance)
				{
					LaneRoad = Candidate;
				}
			}
		}
	}
	const int32 Lanes = LaneRoad ? FMath::Clamp<int32>(LaneRoad->GetMarkings().Lanes, 1, 3) : 1;
	if (Lanes <= 1)
	{
		return GetPointsAcross(200.f);
	}

	TArray<FVector> Points;
	const FVector Across = FVector::CrossProduct(FVector::UpVector, FVector(Outward.X, Outward.Y, 0.f)).GetSafeNormal();
	const float Inset = LaneRoad->mEdgeLineInset;
	const float Inner = FMath::Max(Width - 2.f * Inset, 1.f);
	for (int32 I = 0; I < Lanes; ++I)
	{
		const float Offset = -Width * 0.5f + Inset + Inner * (I + 0.5f) / Lanes;
		Points.Add(Location + Across * Offset);
	}
	return Points;
}

bool RMRoadUtils::GetFoundationPiece(const FHitResult& Hit, FTransform& OutTransform, FBox& OutLocalBox)
{
	AActor* Actor = Hit.GetActor();
	UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Hit.GetComponent());
	if (!Actor || !Mesh || !Mesh->GetStaticMesh() || Cast<ARMBuildableProcedural>(Actor))
	{
		return false;
	}
	// Foundations since 1.0 are instances owned by an abstract instance manager.
	const bool bBuilding = Actor->IsA<AFGBuildable>()
		|| Actor->GetClass()->GetName().Contains(TEXT("AbstractInstance"))
		|| Actor->GetClass()->GetName().Contains(TEXT("Lightweight"));
	if (!bBuilding)
	{
		return false;
	}
	OutTransform = Mesh->GetComponentTransform();
	if (UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Mesh))
	{
		if (Hit.Item == INDEX_NONE || !Instanced->GetInstanceTransform(Hit.Item, OutTransform, true))
		{
			return false;
		}
	}
	OutLocalBox = Mesh->GetStaticMesh()->GetBoundingBox();
	const FVector Size = OutLocalBox.GetSize() * OutTransform.GetScale3D().GetAbs();
	return Size.X >= 390.f && Size.Y >= 390.f && Size.Z >= 50.f && Size.Z <= 450.f;
}
