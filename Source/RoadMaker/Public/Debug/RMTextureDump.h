#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace RMTextureDump
{
	/** Saves the investigation textures as PNGs next to the logs, once per game start (files already there are kept). */
	void DumpListOnce(UWorld* World);
}
