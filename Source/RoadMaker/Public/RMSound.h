// The game's own sounds for Road Maker pieces (Wwise events of the game, played from code).
#pragma once

#include "CoreMinimal.h"

class AActor;

namespace RMSound
{
	/**
	 * The game's build effect sounds at the piece, spread over its build animation (Duration seconds):
	 * start sound, the components "swish" during it, end sound. Local only; nothing on a dedicated server.
	 */
	ROADMAKER_API void PlayBuild(AActor* Piece, float Duration);
}
