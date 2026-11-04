#pragma once

#include "CoreMinimal.h"

/**
 * Customizer Material tab on fence walls: with one of the game's wall materials selected (concrete,
 * FICSIT, steel), aiming at a fence wall previews that look on it and firing switches it in place
 * (the game itself would replace the building, which loses the wall's course).
 */
namespace RMFenceWallPaint
{
	void InstallHooks();
}
