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
								UE_LOG(LogRoadMaker, Display, TEXT("  instance %d of %s (%s) on %s (%s), mesh %s at %s"), I, *P->GetName(), *P->GetClass()->GetName(),
									*Actor->GetName(), *Actor->GetClass()->GetName(), *Mesh, *T.GetLocation().ToString());
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
