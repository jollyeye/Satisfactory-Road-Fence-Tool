// Build menu entry for a Road Tools tool (e.g. Cut Road). Never built itself: its hologram does the
// tool's work when clicked and hands back the pieces it made.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildable.h"
#include "RMBuildableRoadTool.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableRoadTool : public AFGBuildable
{
	GENERATED_BODY()
};
