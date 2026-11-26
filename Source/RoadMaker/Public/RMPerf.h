// Slow-step log (1.6.0): a scope that logs how long it took when that is longer than RM.PerfLogMs (default 8 ms; 0 =
// off). Used on the mod's heavier steps (mesh rebuilds, the dismantle tool's rider search, side slope measuring and
// the Side Slopes tool) to find frame drops: the log says which step and on which piece.
#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "RoadMakerModule.h"

namespace RMPerf
{
	ROADMAKER_API float ThresholdMs();

	struct FScope
	{
		const TCHAR* What;
		FString Who;
		double Start;
		FScope(const TCHAR* InWhat, const FString& InWho) : What(InWhat), Who(InWho), Start(FPlatformTime::Seconds()) {}
		~FScope()
		{
			const float Limit = ThresholdMs();
			const float Ms = float((FPlatformTime::Seconds() - Start) * 1000.0);
			if (Limit > 0.f && Ms >= Limit)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Slow step: %s on %s took %.1f ms"), What, *Who, Ms);
			}
		}
	};
}
