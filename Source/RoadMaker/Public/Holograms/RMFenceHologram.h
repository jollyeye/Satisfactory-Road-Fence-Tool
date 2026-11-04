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
	virtual bool CanNudgeHologram() const override { return !mOnRoadEdge && Super::CanNudgeHologram(); }
	virtual void SetHologramNudgeLocation() override
	{
		if (!mOnRoadEdge)
		{
			Super::SetHologramNudgeLocation();
		}
	}
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Fence wall + snap key aimed at a fence wall: this one replaces it (the game's upgrade flow). */
	virtual AActor* GetUpgradedActor() const override { return mReplaceTarget; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

protected:
	/** Aiming at a road: follow its edge on the side you look at. */
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	virtual float GetCostUnitLength() const override { return 400.f; }
	virtual void RebuildPreview() override;

	const ARMBuildableFence* GetFenceDefaults() const;

	/** Aiming at an intersection: follow the curved curb nearest the aim point, facing the centre. */
	bool SnapToIntersectionCurb(ARMBuildableIntersection* Intersection, const FVector& AimPoint);

	/** Aiming at a foundation: the point being placed snaps to its grid, at its top. */
	bool SnapToFoundationGrid(const FHitResult& hitResult);

	/** Foundation grid step in cm (foundation edges and centre are always on it). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mFoundationGridStep = 100.f;

	bool mOnRoadEdge = false;

	/** The fence wall this hologram replaces (snap key held, aiming at it), else null. */
	UPROPERTY(Replicated, CustomSerialization)
	AActor* mReplaceTarget = nullptr;

	/** That wall, hidden locally while the hologram stands in its place. */
	TWeakObjectPtr<AActor> mHiddenTarget;
	void SetHiddenTarget(AActor* Target);

	/** Side of the fence the road is on (see ARMBuildableFence::SetInwardSign). */
	UPROPERTY(Replicated, CustomSerialization)
	float mInwardSign = 1.f;
};
