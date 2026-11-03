#include "RoadMakerModule.h"
#include "LanePaint/RMLanePaint.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Patching/NativeHookManager.h"
#include "GameFramework/PlayerController.h"

DEFINE_LOG_CATEGORY(LogRoadMaker);

void FRoadMakerModule::StartupModule()
{
#if !WITH_EDITOR
	// Right-click normally steps back / cancels in build mode. While the Lane Paint tool is held it
	// opens the lane menu instead. This runs on the local player's input, before any server call.
	SUBSCRIBE_METHOD(AFGBuildGun::OnSecondaryFirePressed, [](auto& Scope, AFGBuildGun* BuildGun)
	{
		UFGBuildGunStateBuild* BuildState = BuildGun ? Cast<UFGBuildGunStateBuild>(BuildGun->GetCurrentState()) : nullptr;
		if (!BuildState || !Cast<ARMLanePaintHologram>(BuildState->GetHologram()))
		{
			return;
		}
		Scope.Cancel();
		if (APawn* Pawn = Cast<APawn>(BuildGun->GetOwner()))
		{
			RMLanePaint::ToggleMenu(Cast<APlayerController>(Pawn->GetController()));
		}
	});
#endif
}

IMPLEMENT_GAME_MODULE(FRoadMakerModule, RoadMaker);
