// Fence placement.
//  - Aim at a road: the fence follows the edge of that road on the side you look at, along the whole
//    section, one click to build. The edge curve is the road centre line offset sideways.
//  - Aim anywhere else: free two-click spline like the road tool, with no placement rules.
#pragma once

#include "CoreMinimal.h"
#include "Holograms/RMSplineHologram.h"
#include "RMFenceHologram.generated.h"

class ARMBuildableFence;
class ARMBuildableIntersection;

UCLASS()
class ROADMAKER_API ARMFenceHologram : public ARMSplineHologram
{
	GENERATED_BODY()

public:
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	/** Aiming at a road: follow its edge on the side you look at. */
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	virtual float GetCostUnitLength() const override { return 400.f; }
	virtual void RebuildPreview() override;

	const ARMBuildableFence* GetFenceDefaults() const;

	/** Aiming at an intersection: follow the curved curb nearest the aim point, facing the centre. */
	bool SnapToIntersectionCurb(ARMBuildableIntersection* Intersection, const FVector& AimPoint);

	bool mOnRoadEdge = false;

	/** Side of the fence the road is on (see ARMBuildableFence::SetInwardSign). */
	UPROPERTY(Replicated)
	float mInwardSign = 1.f;
};
