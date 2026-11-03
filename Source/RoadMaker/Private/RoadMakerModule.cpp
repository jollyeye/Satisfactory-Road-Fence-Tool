#include "RoadMakerModule.h"
#include "LanePaint/RMLanePaint.h"
#include "Buildables/RMBuildableProcedural.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Hologram/FGHologram.h"
#include "Patching/NativeHookManager.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogRoadMaker);

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

void FRoadMakerModule::StartupModule()
{
#if !WITH_EDITOR
	// R (the build gun's mode-select action) opens the lane menu while the Lane Paint tool is held,
	// instead of cycling build modes. Runs on the local player's input.
	SUBSCRIBE_METHOD(AFGBuildGun::OnModeSelectPressed, [](auto& Scope, AFGBuildGun* BuildGun)
	{
		UFGBuildGunStateBuild* BuildState = BuildGun ? Cast<UFGBuildGunStateBuild>(BuildGun->GetCurrentState()) : nullptr;
		if (!BuildState || !Cast<ARMLanePaintHologram>(BuildState->GetHologram()))
		{
			return;
		}
		Scope.Cancel();
		APawn* Pawn = Cast<APawn>(BuildGun->GetOwner());
		UE_LOG(LogRoadMaker, Display, TEXT("Lane Paint: R pressed, toggling menu"));
		RMLanePaint::ToggleMenu(Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr);
	});

	// Buildings ignore anything not in their valid-hit list, and our roads are not in anyone's list.
	// Treat roads and intersections like foundations for every hologram that accepts foundations.
	// IsValidHitActor is virtual, so the hook needs an object to resolve the implementation from.
	SUBSCRIBE_METHOD_VIRTUAL(AFGHologram::IsValidHitActor, GetMutableDefault<AFGHologram>(),
		[](auto& Scope, const AFGHologram* Hologram, AActor* HitActor)
		{
			if (Cast<ARMBuildableProcedural>(HitActor) && AcceptsFoundations(Hologram))
			{
				Scope.Override(true);
			}
		});
#endif
}

IMPLEMENT_GAME_MODULE(FRoadMakerModule, RoadMaker);
