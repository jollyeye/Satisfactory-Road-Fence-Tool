#include "RoadMakerModule.h"
#include "LanePaint/RMLanePaint.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/FGBuildable.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Patching/NativeHookManager.h"
#include "GameFramework/PlayerController.h"

DEFINE_LOG_CATEGORY(LogRoadMaker);

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
#endif
}

IMPLEMENT_GAME_MODULE(FRoadMakerModule, RoadMaker);
