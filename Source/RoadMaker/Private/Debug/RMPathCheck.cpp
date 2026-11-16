// Console command RM.PathCheck: for every vehicle path within 15 m of the aim, logs each vehicle type's check result
// (why it is red and where), checks it again on the spot, and lists what stands in the vehicle's box there.
#include "RoadMakerModule.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Components/SplineComponent.h"
#include "HAL/IConsoleManager.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehiclePathPreset.h"
#include "WheeledVehicles/FGVehiclePathValidation.h"

namespace
{
	const TCHAR* ResultName(EVehiclePathValidationResult R)
	{
		switch (R)
		{
		case EVehiclePathValidationResult::Valid: return TEXT("Valid");
		case EVehiclePathValidationResult::TurnTooSharp: return TEXT("TurnTooSharp");
		case EVehiclePathValidationResult::NoFloor: return TEXT("NoFloor");
		case EVehiclePathValidationResult::TerrainTooSteep: return TEXT("TerrainTooSteep");
		case EVehiclePathValidationResult::StaticObstacle: return TEXT("StaticObstacle");
		case EVehiclePathValidationResult::InvalidShape: return TEXT("InvalidShape");
		case EVehiclePathValidationResult::SubmergedInWater: return TEXT("SubmergedInWater");
		}
		return TEXT("?");
	}

	/** Everything blocking in the vehicle's box at a point of the path (its direction there). */
	void LogObstacles(UWorld* World, const USplineComponent* Spline, float Dist, const FBox& VehicleBox)
	{
		const FTransform T = Spline->GetTransformAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);
		const FVector Centre = T.TransformPosition(VehicleBox.GetCenter());
		TArray<FOverlapResult> Overlaps;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMPathCheck), true);
		for (const ECollisionChannel Channel : { ECC_WorldStatic, ECC_Vehicle, ECC_Visibility })
		{
			Overlaps.Reset();
			World->OverlapMultiByChannel(Overlaps, Centre, T.GetRotation(), Channel, FCollisionShape::MakeBox(VehicleBox.GetExtent()), Params);
			for (const FOverlapResult& O : Overlaps)
			{
				const UPrimitiveComponent* C = O.GetComponent();
				UE_LOG(LogRoadMaker, Display, TEXT("      channel %d: %s (%s) on %s (%s), object type %d"), static_cast<int32>(Channel), *GetNameSafe(C),
					C ? *C->GetClass()->GetName() : TEXT("-"), *GetNameSafe(O.GetActor()), O.GetActor() ? *O.GetActor()->GetClass()->GetName() : TEXT("-"),
					C ? static_cast<int32>(C->GetCollisionObjectType()) : -1);
			}
		}
	}

	FAutoConsoleCommandWithWorld GRMPathCheck(
		TEXT("RM.PathCheck"),
		TEXT("Road Maker: log why the vehicle paths near the aim are red (each vehicle type) and what is in the way."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (!PC)
			{
				return;
			}
			FVector Eye;
			FRotator View;
			PC->GetPlayerViewPoint(Eye, View);
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(RMPathCheckAim), true, PC->GetPawn());
			const FVector At = World->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Params) ? Hit.Location : Eye;
			UE_LOG(LogRoadMaker, Display, TEXT("RM.PathCheck at %s"), *At.ToString());
			for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It)
			{
				AFGVehiclePathSegment* Path = *It;
				const USplineComponent* Spline = Path->GetSplineComponent();
				if (!Spline)
				{
					continue;
				}
				const float Key = Spline->FindInputKeyClosestToWorldLocation(At);
				const FVector Closest = Spline->GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World);
				if (FVector::Dist(Closest, At) > 1500.f)
				{
					continue;
				}
				UE_LOG(LogRoadMaker, Display, TEXT("  %s, %.0f cm long, from %s"), *Path->GetName(), Spline->GetSplineLength(),
					*Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World).ToString());
				for (const FVehiclePathSegmentValidationData& Data : Path->GetPathSegmentValidationData())
				{
					const UFGVehiclePathPreset* Preset = Data.PathPreset;
					const FVehiclePathValidationInfo& Saved = Data.ValidationInfo;
					UE_LOG(LogRoadMaker, Display, TEXT("    %s: stored %s at %.0f cm (%s)"), *GetNameSafe(Preset), ResultName(Saved.Result),
						Saved.SplineValidationFailDistance, *Spline->GetLocationAtDistanceAlongSpline(Saved.SplineValidationFailDistance, ESplineCoordinateSpace::World).ToString());
					if (!Preset)
					{
						continue;
					}
					const FVehiclePathValidationInfo Now = UFGVehiclePathValidation::ValidateVehiclePathSegment(Spline, Preset->mPathValidationConfig, 30.f);
					UE_LOG(LogRoadMaker, Display, TEXT("    %s: checked now %s at %.0f cm (%s), vehicle box %s"), *GetNameSafe(Preset), ResultName(Now.Result),
						Now.SplineValidationFailDistance, *Spline->GetLocationAtDistanceAlongSpline(Now.SplineValidationFailDistance, ESplineCoordinateSpace::World).ToString(),
						*Preset->mPathValidationConfig.VehicleCollisionBox.ToString());
					if (Now.Result != EVehiclePathValidationResult::Valid)
					{
						LogObstacles(World, Spline, Now.SplineValidationFailDistance, Preset->mPathValidationConfig.VehicleCollisionBox);
					}
				}
			}
		}));
}
