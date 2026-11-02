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

protected:
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	virtual float GetSurfaceOffset() const override;
	virtual int32 GetCostWidthFactor() const override;
	virtual void RebuildPreview() override;

	const ARMBuildableRoad* GetRoadDefaults() const;

	/** How close (cm) to a road end the crosshair must be to snap to it. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSnapRadius = 600.f;
};
