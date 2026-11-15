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

/**
 * Middle click (the game's Sample key) on a road or walking path: the game copies the recipe and the paint
 * swatch, not Road Maker's own look (Concrete Blocks / Asphalt / Cyberpunk) or lines. The key handler
 * notes the piece here and the new road hologram takes its look and lines.
 */
namespace RMRoadSample
{
	/** The Sample key was pressed aiming at Piece: remembered for a few seconds. */
	ROADMAKER_API void Note(const class ARMBuildableProcedural* Piece);

	/** Look and lines of a piece of exactly Class sampled in the last few seconds; taken once. */
	ROADMAKER_API bool Take(const UClass* Class, uint8& OutLook, struct FRMMarkings& OutMarkings, bool& bOutHasMarkings);
}
