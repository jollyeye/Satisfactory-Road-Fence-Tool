// Console command RM.WhatIsHere: logs everything drawn within 6 m of where the player aims (actors, their mesh
// components, and single instances of instanced meshes, e.g. the game's lightweight buildings), to identify
// leftovers that cannot be aimed at.
#include "RoadMakerModule.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Buildables/FGBuildable.h"
#include "FGBlueprintProxy.h"

namespace
{
	bool AimPoint(UWorld* World, FVector& Out)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (!PC)
		{
			return false;
		}
		FVector Eye;
		FRotator View;
		PC->GetPlayerViewPoint(Eye, View);
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMWhatIsHere), true, PC->GetPawn());
		Out = World->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Params)
			? Hit.Location : Eye + View.Vector() * 800.f;
		return true;
	}

	FAutoConsoleCommandWithWorld GRMWhatIsHere(
		TEXT("RM.WhatIsHere"),
		TEXT("Road Maker: log everything drawn within 6 m of the aim (to identify leftovers that cannot be aimed at)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			FVector At;
			if (!AimPoint(World, At))
			{
				return;
			}
			constexpr float Radius = 600.f;
			UE_LOG(LogRoadMaker, Display, TEXT("RM.WhatIsHere at %s"), *At.ToString());
			// Buildings nearby and the blueprint placement each belongs to (registered in it or only pointing at it).
			for (TActorIterator<AFGBuildable> It(World); It; ++It)
			{
				AFGBuildable* B = *It;
				if (FVector::Dist(B->GetActorLocation(), At) > Radius)
				{
					continue;
				}
				AFGBlueprintProxy* Proxy = B->GetBlueprintProxy();
				const bool bRegistered = Proxy && Proxy->GetBuildables().Contains(B);
				UE_LOG(LogRoadMaker, Display, TEXT("  building %s at %s, blueprint %s (%s), dismantled %d"), *B->GetName(), *B->GetActorLocation().ToString(),
					*GetNameSafe(Proxy), Proxy ? (bRegistered ? TEXT("registered") : TEXT("NOT registered")) : TEXT("none"), B->GetIsDismantled() ? 1 : 0);
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				TInlineComponentArray<UPrimitiveComponent*> Prims(Actor);
				for (UPrimitiveComponent* P : Prims)
				{
					if (!P->IsRegistered())
					{
						continue;
					}
					const UStaticMeshComponent* SM = Cast<UStaticMeshComponent>(P);
					const FString Mesh = SM && SM->GetStaticMesh() ? SM->GetStaticMesh()->GetName() : FString(TEXT("-"));
					if (const UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(P))
					{
						for (int32 I = 0; I < ISM->GetInstanceCount(); ++I)
						{
							FTransform T;
							if (ISM->GetInstanceTransform(I, T, true) && FVector::Dist(T.GetLocation(), At) < Radius)
							{
								// Rotation, scale and the instance's drawing box (what the game culls it by) too.
								FString Custom;
								const int32 NumCustom = ISM->NumCustomDataFloats;
								for (int32 C = 0; C < NumCustom && ISM->PerInstanceSMCustomData.IsValidIndex(I * NumCustom + C); ++C)
								{
									Custom += FString::Printf(TEXT("%.0f "), ISM->PerInstanceSMCustomData[I * NumCustom + C]);
								}
								const FBox Box = SM && SM->GetStaticMesh() ? SM->GetStaticMesh()->GetBoundingBox().TransformBy(T) : FBox(ForceInit);
								UE_LOG(LogRoadMaker, Display, TEXT("  instance %d of %s (%s) on %s (%s), mesh %s at %s rot %s scale %s box %s..%s custom [%s]"), I, *P->GetName(),
									*P->GetClass()->GetName(), *Actor->GetName(), *Actor->GetClass()->GetName(), *Mesh, *T.GetLocation().ToString(),
									*T.Rotator().ToString(), *T.GetScale3D().ToString(), *Box.Min.ToString(), *Box.Max.ToString(), *Custom);
							}
						}
						continue;
					}
					if (P->Bounds.SphereRadius < 20000.f && FVector::Dist(P->Bounds.Origin, At) < Radius + P->Bounds.SphereRadius * 0.5f)
					{
						UE_LOG(LogRoadMaker, Display, TEXT("  %s (%s) on %s (%s), mesh %s, visible %d, hidden actor %d, collision %d"), *P->GetName(),
							*P->GetClass()->GetName(), *Actor->GetName(), *Actor->GetClass()->GetName(), *Mesh, P->IsVisible() ? 1 : 0,
							Actor->IsHidden() ? 1 : 0, static_cast<int32>(P->GetCollisionEnabled()));
					}
				}
			}
		}));
}
