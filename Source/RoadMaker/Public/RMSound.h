// The game's own sounds for Road Maker pieces (Wwise events of the game, played from code).
#pragma once

#include "CoreMinimal.h"

class AActor;

namespace RMSound
{
	/** The game's build sound at the piece (local only; nothing on a dedicated server). */
	ROADMAKER_API void PlayConstruct(AActor* Piece);
}
