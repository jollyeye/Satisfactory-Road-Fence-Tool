// Reasons shown in red on the hologram when a piece cannot be placed.
#pragma once

#include "CoreMinimal.h"
#include "FGConstructDisqualifier.h"
#include "RMDisqualifiers.generated.h"

// Note: "mDisqfualifyingText" is the game's own (misspelled) field name.

UCLASS()
class ROADMAKER_API URMCD_TurnTooTight : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_TurnTooTight() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "TurnTooTight", "Turn is too tight for this road"); }
};

UCLASS()
class ROADMAKER_API URMCD_TooSteep : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_TooSteep() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "TooSteep", "Road is too steep"); }
};

UCLASS()
class ROADMAKER_API URMCD_TooLong : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_TooLong() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "TooLong", "Road section is too long"); }
};

UCLASS()
class ROADMAKER_API URMCD_TooShort : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_TooShort() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "TooShort", "Too short"); }
};

UCLASS()
class ROADMAKER_API URMCD_NotEnoughRoads : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_NotEnoughRoads() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "NotEnoughRoads", "Aim near at least 2 free road ends"); }
};

UCLASS()
class ROADMAKER_API URMCD_PickRoadEnds : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_PickRoadEnds() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "PickRoadEnds", "Click at least 2 free road ends to pick them"); }
};

UCLASS()
class ROADMAKER_API URMCD_AimAtRoad : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_AimAtRoad() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "AimAtRoad", "Aim at a road"); }
};

UCLASS()
class ROADMAKER_API URMCD_BadIntersection : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_BadIntersection() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "BadIntersection", "These road ends cannot form an intersection"); }
};

UCLASS()
class ROADMAKER_API URMCD_RiderTooShort : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_RiderTooShort() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "RiderTooShort", "A train track or vehicle path on the road would be cut too close to its end"); }
};

UCLASS()
class ROADMAKER_API URMCD_AimAtFence : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_AimAtFence() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "AimAtFence", "Aim at a road fence or fence wall"); }
};

UCLASS()
class ROADMAKER_API URMCD_RoadTooNarrow : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_RoadTooNarrow() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "RoadTooNarrow", "Only roads 2 or more wide can be split"); }
};

UCLASS()
class ROADMAKER_API URMCD_RidersOnRoad : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_RidersOnRoad() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "RidersOnRoad", "Remove the train tracks and vehicle paths on the road first"); }
};

UCLASS()
class ROADMAKER_API URMCD_FenceWallTaken : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceWallTaken() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceWallTaken", "Another fence wall is already there"); }
};

UCLASS()
class ROADMAKER_API URMCD_AimAtRoadEnd : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_AimAtRoadEnd() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "AimAtRoadEnd", "Aim at an orange circle on a road end"); }
};
