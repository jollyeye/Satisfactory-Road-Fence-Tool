// Road placement: two clicks, snaps to free road ends, enforces turn radius / grade / length.
#pragma once

#include "CoreMinimal.h"
#include "Holograms/RMSplineHologram.h"
#include "RMRoadHologram.generated.h"

class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMRoadHologram : public ARMSplineHologram
{
	GENERATED_BODY()

public:
	virtual void CheckValidPlacement() override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	virtual float GetSurfaceOffset() const override;
	virtual int32 GetCostWidthFactor() const override;
	virtual void RebuildPreview() override;

	const ARMBuildableRoad* GetRoadDefaults() const;

	/**
	 * Aiming at a foundation: snap the current end to the middle of its nearest edge (or +-4 m along
	 * it), road top level with the foundation top, pointing straight out of / into that edge.
	 */
	bool TrySnapToFoundation(const FHitResult& hitResult);

	/**
	 * Snap key held, aiming at a built road: the new road is the parallel of that whole road, on the
	 * side aimed at, edge to edge and flush with its top (to fill gaps with a flat surface). One
	 * click builds it.
	 */
	bool TrySnapBesideRoad(const FHitResult& hitResult);
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;

	/** The current curve is a parallel of another road: only geometry limits apply. */
	UPROPERTY(Replicated, CustomSerialization)
	bool mBesideRoad = false;

	/** Markings for the new road: copied from the road it snaps to, otherwise the default. */
	UPROPERTY(CustomSerialization)
	FRMMarkings mMarkings;
	bool mHasCopiedMarkings = false;

	/** Along-edge snap step on foundations, in cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mFoundationEdgeStep = 400.f;

	/** How close (cm) to a road end the crosshair must be to snap to it. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSnapRadius = 600.f;
};
