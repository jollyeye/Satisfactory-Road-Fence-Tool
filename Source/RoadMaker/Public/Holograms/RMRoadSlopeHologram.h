// Side Slopes tool (1.6.0, build menu Road Tools > Edit road): turns the bank on the aimed side of a road or walking
// path on or off (each side on its own: the player decides; other roads are ignored), or an intersection curb's (a run
// between two road mouths). Snap key held: every side / every curb at once.
// Aim at one: a cyan cage shows the banks it would get (from its edges down to the terrain, 1 : 1.5, at most 8 m;
// none where it meets the ground or another road), a red one the banks it would lose. Click to switch. The banks are
// part of the road (its concrete, solid), measured once on the server and saved (ARMBuildableRoad::SetSlopeSides).
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMRoadSlopeHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableRoad;
class ARMBuildableIntersection;

UCLASS()
class ROADMAKER_API ARMRoadSlopeHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMRoadSlopeHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override { return true; }
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	void RebuildOverlay();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** Road (or intersection) to switch. Sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mTarget = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableIntersection* mTargetX = nullptr;
	/** Road: the side aimed at (0 left, 1 right), whose bank the click switches. Sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	int32 mSide = 0;
	/** Intersection: the curb aimed at (run between two mouths). */
	UPROPERTY(Replicated, CustomSerialization)
	int32 mRun = 0;
	/** Snap key held: every side of the road / every curb of the intersection at once. */
	UPROPERTY(Replicated, CustomSerialization)
	bool mAll = false;
	bool HasTarget() const { return mTarget || mTargetX; }
	/** Whether the aimed side / curb has a bank now (the click then removes, else adds). */
	bool TargetHasSlopes() const;
	/** The sides / curbs the click switches, the target's mask now, and the mask after the click. */
	uint32 RequestedMask() const;
	uint32 CurrentMask() const;
	uint32 NewMask() const;

	/** The banks the aimed road would get, measured once per road (and again when it changes); an intersection's in Left. */
	TWeakObjectPtr<AActor> mMeasuredFor;
	uint32 mMeasuredKey = 0;
	TArray<FVector2D> mMeasuredLeft;
	TArray<FVector2D> mMeasuredRight;
};
