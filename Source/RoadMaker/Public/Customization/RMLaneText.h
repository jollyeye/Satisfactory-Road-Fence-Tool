// Lane Text (1.5.0): text painted along a lane, written vertically (see docs/ROADMAP-1.5.0.md "Lane Text").
// Step 1: the shared glyph texture, drawn at run time with the game's font, and a test command (RM.LaneText).
#pragma once

#include "CoreMinimal.h"

class UTextureRenderTarget2D;
class UWorld;

namespace RMLaneText
{
	/** The shared glyph texture: white characters on transparent, 128 px cells (16 x 16). Made on first use. */
	ROADMAKER_API UTextureRenderTarget2D* GetGlyphTexture(UWorld* World);

	/** Draws the characters of Text not drawn yet. False if the texture or the font is missing. */
	ROADMAKER_API bool EnsureGlyphs(UWorld* World, const FString& Text);

	/** A character's cell in the glyph texture as UVs (min = top left, max = bottom right). */
	ROADMAKER_API FBox2D CellUV(TCHAR Char);
}
