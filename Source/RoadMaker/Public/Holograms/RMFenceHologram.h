// Fence placement.
//  - Aim at a road: the fence follows the edge of that road on the side you look at, along the whole
//    section, one click to build. The edge curve is the road centre line offset sideways.
//  - Aim anywhere else: free two-click spline like the road tool, with no placement rules.
#pragma once

#include "CoreMinimal.h"
#include "Holograms/RMSplineHologram.h"
#include "RMFenceHologram.generated.h"

class ARMBuildableFence;

UCLASS()
class ROADMAKER_API ARMFenceHologram : public ARMSplineHologram
{
	GENERATED_BODY()

public:
	virtual bool TrySnapToActor(const FHitResult& hitResult) override;
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;

protected:
	virtual float GetCostUnitLength() const override { return 400.f; }
	virtual void RebuildPreview() override;

	const ARMBuildableFence* GetFenceDefaults() const;

	bool mOnRoadEdge = false;
};
