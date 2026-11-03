// "Roads" entry in the customizer's "set as default for" list.
#pragma once

#include "CoreMinimal.h"
#include "FGSwatchGroup.h"
#include "RMSwatchGroupRoads.generated.h"

UCLASS()
class ROADMAKER_API URMSwatchGroup_Roads : public UFGSwatchGroup
{
	GENERATED_BODY()

public:
	URMSwatchGroup_Roads()
	{
		mGroupName = NSLOCTEXT("RoadMaker", "SwatchGroupRoads", "Roads");
	}
};
