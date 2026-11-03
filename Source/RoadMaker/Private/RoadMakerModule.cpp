#include "RoadMakerModule.h"
#include "LanePaint/RMLanePaint.h"
#include "Snapping/RMVanillaSnapping.h"
#include "Customization/RMRoadLinePaint.h"
#include "Buildables/RMBuildableProcedural.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Hologram/FGHologram.h"
#include "Patching/NativeHookManager.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"
#include "FGPlayerController.h"
#include "Net/RMRemoteCallObject.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Camera/PlayerCameraManager.h"

DEFINE_LOG_CATEGORY(LogRoadMaker);

#if !WITH_EDITOR
namespace
{
	/** True if this hologram may be placed on foundations (its mValidHitClasses covers them). */
	bool AcceptsFoundations(const AFGHologram* Hologram)
	{
		static FArrayProperty* Prop = FindFProperty<FArrayProperty>(AFGHologram::StaticClass(), TEXT("mValidHitClasses"));
		if (!Prop || !Hologram)
		{
			return false;
		}
		const TArray<TSubclassOf<AActor>>* Classes = Prop->ContainerPtrToValuePtr<TArray<TSubclassOf<AActor>>>(Hologram);
		for (const TSubclassOf<AActor>& Class : *Classes)
		{
			if (Class && AFGBuildableFoundation::StaticClass()->IsChildOf(Class))
			{
				return true;
			}
		}
		return false;
	}
}

/**
 * AFGHologram::IsValidHitActor is protected. A class derived from AFGHologram may name it as
 * &FRMHologramAccess::IsValidHitActor (same function, same pointer type), so the hook is set up here.
 * Never instantiated.
 */
struct FRMHologramAccess : public AFGHologram
{
	static void InstallValidHitHook()
	{
		// Treat roads and intersections like foundations for every hologram that accepts foundations.
		// The function is virtual, so SML needs an object to resolve the implementation from.
		SUBSCRIBE_METHOD_VIRTUAL(FRMHologramAccess::IsValidHitActor, GetMutableDefault<AFGHologram>(),
			[](auto& Scope, const AFGHologram* Hologram, AActor* HitActor)
			{
				if (Cast<ARMBuildableProcedural>(HitActor) && AcceptsFoundations(Hologram))
				{
					Scope.Override(true);
				}
			});
	}
};
#endif

void FRoadMakerModule::StartupModule()
{
#if !WITH_EDITOR
	// Buildings ignore anything not in their valid-hit list, and our roads are not in anyone's list.
	FRMHologramAccess::InstallValidHitHook();

	// Poles, street lights, railways and vehicle paths snapping to roads.
	RMVanillaSnapping::InstallHooks();

	// Road Lines tab in the customizer.
	RMRoadLinePaint::InstallHooks();

	// Detailed / far meshes of roads, intersections and fences, four times a second.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		if (!GEngine || IsRunningDedicatedServer())
		{
			return true;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (Context.WorldType != EWorldType::Game || !World)
			{
				continue;
			}
			APlayerController* PC = World->GetFirstPlayerController();
			if (PC && PC->PlayerCameraManager)
			{
				ARMBuildableProcedural::UpdateLODsForCamera(World, PC->PlayerCameraManager->GetCameraLocation());
			}
		}
		return true;
	}), 0.25f);

	// Multiplayer: every player gets Road Maker's remote call object (made on the server).
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGPlayerController::BeginPlay, GetMutableDefault<AFGPlayerController>(),
		[](AFGPlayerController* PC)
		{
			URMRemoteCallObject::RegisterFor(PC);
		});
#endif
}

IMPLEMENT_GAME_MODULE(FRoadMakerModule, RoadMaker);
