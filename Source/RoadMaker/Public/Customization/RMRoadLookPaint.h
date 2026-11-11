#pragma once

#include "CoreMinimal.h"

/**
 * Customizer Material tab > Roads (Asphalt / Concrete / Cyberpunk): with one of our road materials
 * selected, aiming at a road or intersection previews that look on it and firing switches it in place.
 * Free; keeps the piece (the game's own material swap would rebuild it from a recipe).
 */
namespace RMRoadLookPaint
{
	void InstallHooks();
}
