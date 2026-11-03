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
