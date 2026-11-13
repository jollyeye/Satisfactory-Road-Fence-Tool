// Reasons shown in red on the hologram when a piece cannot be placed.
#pragma once

#include "CoreMinimal.h"
#include "FGConstructDisqualifier.h"
#include "RMDisqualifiers.generated.h"

// Note: "mDisqfualifyingText" is the game's own (misspelled) field name.

UCLASS()
class ROADMAKER_API URMCD_JoinAimAtJoint : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_JoinAimAtJoint() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "JoinAimAtJoint", "Aim near where two roads meet end to end"); }
};

UCLASS()
class ROADMAKER_API URMCD_JoinWidth : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_JoinWidth() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "JoinWidth", "The roads are not the same width"); }
};

UCLASS()
class ROADMAKER_API URMCD_JoinKink : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_JoinKink() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "JoinKink", "The roads do not continue smoothly into each other"); }
};

UCLASS()
class ROADMAKER_API URMCD_JoinCurve : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_JoinCurve() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "JoinCurve", "One road would not follow both curves (e.g. a straight and a curved piece)"); }
};

UCLASS()
class ROADMAKER_API URMCD_MergePickSecond : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_MergePickSecond() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "MergePickSecond", "Aim at the road right beside it (click the picked road again to drop it)"); }
};

UCLASS()
class ROADMAKER_API URMCD_MergeNotBeside : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_MergeNotBeside() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "MergeNotBeside", "Not right beside the picked road (edge to edge, same height, all along)"); }
};

UCLASS()
class ROADMAKER_API URMCD_MergeLength : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_MergeLength() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "MergeLength", "The roads must start and end together (cut them to match)"); }
};

UCLASS()
class ROADMAKER_API URMCD_MergeTooWide : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_MergeTooWide() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "MergeTooWide", "Together they would be wider than 4 foundations"); }
};

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
class ROADMAKER_API URMCD_SameFenceTaken : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_SameFenceTaken() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "SameFenceTaken", "The same fence is already there"); }
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

UCLASS()
class ROADMAKER_API URMCD_FenceJoinAimAtJoint : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceJoinAimAtJoint() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceJoinAimAtJoint", "Aim near where two fences meet end to end"); }
};

UCLASS()
class ROADMAKER_API URMCD_FenceJoinType : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceJoinType() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceJoinType", "The fences are not the same kind (or not the same look)"); }
};

UCLASS()
class ROADMAKER_API URMCD_FenceJoinSide : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceJoinSide() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceJoinSide", "The fences face opposite sides of the road"); }
};

UCLASS()
class ROADMAKER_API URMCD_FenceJoinKink : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceJoinKink() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceJoinKink", "The fences do not continue smoothly into each other"); }
};

UCLASS()
class ROADMAKER_API URMCD_FenceJoinCurve : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceJoinCurve() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceJoinCurve", "One fence would not follow both curves (e.g. a straight and a curved piece)"); }
};

UCLASS()
class ROADMAKER_API URMCD_FenceJoinTooLong : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	URMCD_FenceJoinTooLong() { mDisqfualifyingText = NSLOCTEXT("RoadMaker", "FenceJoinTooLong", "The joined fence would be longer than 1 km"); }
};
