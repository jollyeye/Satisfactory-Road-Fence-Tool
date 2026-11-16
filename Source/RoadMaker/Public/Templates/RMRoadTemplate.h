// Road mode for blueprints (1.4.0): holding a blueprint that contains a Road Maker road adds a "Road Mode" build mode.
// Picked, the blueprint is read as a template: its main road (width, look, paint, lines) and the fences, fence walls
// and walking paths running along it, each as a distance from the road's centre. Then a road of any curve is drawn
// and built with those pieces along it (steps 2 and 3).
#pragma once

#include "CoreMinimal.h"

class AFGBlueprintHologram;
class ARMBuildableProcedural;
class UClass;

/** One piece running along the template's road. */
struct FRMTemplatePiece
{
	UClass* Class = nullptr;     // fence / fence wall / walking path class
	float Offset = 0.f;          // cm from the road's centre line, + = the road's right side
	float Lift = 0.f;            // cm above the road's top (stacked walls)
	float InwardSign = 1.f;      // fences: which way they face
	uint8 Look = 0;              // walking paths: look; fence walls: wall look
	bool bWalkingPath = false;
};

/** A blueprint read as a road template. */
struct FRMRoadTemplate
{
	UClass* RoadClass = nullptr;
	float RoadWidth = 0.f;
	uint8 Look = 0;
	TArray<FRMTemplatePiece> Pieces;
	/** The blueprint-world buildables read: the main road first, then the pieces along it. */
	TArray<class AFGBuildable*> Sources;
	bool IsValid() const { return RoadClass != nullptr; }
};

namespace RMRoadTemplate
{
	void InstallHooks();
	/** The blueprint hologram shows Piece (called by the blueprint visualization of every Road Maker piece). */
	void NotePiece(AFGBlueprintHologram* Hologram, ARMBuildableProcedural* Piece);
	/** The template of a blueprint hologram's pieces (its main road and the pieces along it). */
	FRMRoadTemplate Read(AFGBlueprintHologram* Hologram);
}
