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
class ROADMAKER_API URMCD_AimAtRoad : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_AimAtRoad() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "AimAtRoad", "Aim at a road. Press R to choose lanes"); }
};

UCLASS()
class ROADMAKER_API URMCD_BadIntersection : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_BadIntersection() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "BadIntersection", "These road ends cannot form an intersection"); }
};
