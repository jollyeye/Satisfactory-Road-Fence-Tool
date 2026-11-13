// Road limits toggle (Alt by default, rebindable, while holding a road): in any road build mode, off = no turn or slope
// limits, up to 1 km long, the build gun reaching 1 km. The choice is per player, remembered between
// sessions (GameUserSettings.ini). The state shows after the build mode's name: "Zoop (Limits: ON)".
#pragma once

#include "CoreMinimal.h"

class APlayerController;
class UFGBuildGunModeDescriptor;

namespace RMRoadLimits
{
	/** True when the limits are off (this player's choice, remembered). */
	ROADMAKER_API bool AreOff();

	/**
	 * Every frame while a road hologram is held by this (local) player: Alt pressed and released on
	 * its own (not Alt+Tab) toggles the limits. True if it toggled.
	 */
	ROADMAKER_API bool HandleInput(APlayerController* PC);

	/**
	 * Writes the state after the names of these build modes ("Default (Limits: ON)") and makes the
	 * build gun show the new name. Only for Road Maker's own road modes (they are not shared).
	 */
	/**
	 * While a road hologram is held: Road Maker's input context on (the limits key, rebindable in
	 * Options > Controls under "Road Maker", shown in the build gun's button hint bar), off after.
	 */
	ROADMAKER_API void BindRoadInput(APlayerController* PC, bool bBind);

	/** The limits input action (IA_RM_ToggleLimits), or null. */
	ROADMAKER_API class UInputAction* GetLimitsAction();

	ROADMAKER_API void LabelModes(const TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& Modes, APlayerController* PC);
}
