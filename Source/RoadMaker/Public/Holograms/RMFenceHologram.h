// Fence placement.
//  - Aim at a road: the fence follows the edge of that road on the side you look at, along the whole
//    section, one click to build. The edge curve is the road centre line offset sideways.
//  - Aim anywhere else: free two-click spline like the road tool, with no placement rules.
#pragma once

#include "CoreMinimal.h"
#include "Holograms/RMSplineHologram.h"
#include "Buildables/RMBuildableFence.h"
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
	// Nudging (lock with H, then the nudge keys): a free fence moves its point like the road tool; a fence
	// following a road edge or curb moves as a whole, up / down and along the horizontal axes nearest the camera.
	virtual void SetHologramNudgeLocation() override;
	virtual ENudgeFailReason NudgeHologram(const FVector& NudgeInput, const FHitResult& HitResult) override;
	virtual void OnHologramLockStateChanged() override;

	/**
	 * A fence wall where this one would stand (same course, overlapping heights), not counting the one being
	 * replaced; with SameRoadFence: a road fence of that kind there instead. True only when one follows the
	 * other for most of the shorter one's length (the same curve); a partial overlap (ends meeting, curves
	 * crossing) is allowed and only sets OutTouching (the hologram shows the game's yellow warning look).
	 */
	/** Snapped to a road edge where fences of the same family already stand (a wall for a wall, the same road fence for
	 *  a road fence): the centre-line frames of only the free stretch around the aimed sample, from one fence's end to the
	 *  next. False (keep the whole edge) when the edge is free or the aim is on a fence. */
	bool ClipEdgeToFreeGap(const TArray<FRMFrame>& Frames, float Offset, int32 Aim, TArray<FRMFrame>& OutGap) const;
	bool OverlapsAnotherWall(TOptional<ERMFenceType> SameRoadFence = TOptional<ERMFenceType>(), bool* OutTouching = nullptr) const;

	/** Touching another wall / same road fence in part (allowed): yellow, like the game's touching buildings. */
	virtual EHologramMaterialState GetHologramMaterialState() const override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Snap key aimed at another fence of the same family (wall / road fence): this one replaces it (the game's upgrade flow). */
	virtual AActor* GetUpgradedActor() const override { return mReplaceTarget; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

protected:
	/** Aiming at a road: follow its edge on the side you look at. */
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	/** 4 m; the sign wall 8 m (it costs a Small Billboard per 8 m design). */
	virtual float GetCostUnitLength() const override;
	virtual void RebuildPreview() override;

	/** The real fence: its materials (look) and paint, with the game's valid / invalid tint on top. */
	virtual void UpdateHologramLook() override;

	/** Set by CheckValidPlacement: this fence overlaps another in part (allowed, shown yellow). */
	bool mTouchingOther = false;
	virtual void ApplyCustomizationData() override;

	const ARMBuildableFence* GetFenceDefaults() const;

	/** Aiming at an intersection: follow the curved curb nearest the aim point, facing the centre. */
	/** Fence walls only: the side of a Gap Fill nearest the aim (its outline split at corners), on the slab. */
	bool SnapToFillSide(class ARMBuildableFill* Fill, const FVector& AimPoint);
	bool SnapToIntersectionCurb(ARMBuildableIntersection* Intersection, const FVector& AimPoint);

	/** Aiming at a foundation: the point being placed snaps to its grid, at its top. */
	bool SnapToFoundationGrid(const FHitResult& hitResult);

	/** Foundation grid step in cm (foundation edges and centre are always on it). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mFoundationGridStep = 100.f;

	bool mOnRoadEdge = false;

	/** Snapped fence, locked: where the hologram was when the lock started, and the nudge since (world). */
	FVector mSnapBase = FVector::ZeroVector;
	FVector mSnapNudge = FVector::ZeroVector;

	/** The fence this hologram replaces (snap key held, aiming at it), else null. */
	UPROPERTY(Replicated, CustomSerialization)
	AActor* mReplaceTarget = nullptr;

	/** That wall, hidden locally while the hologram stands in its place. */
	TWeakObjectPtr<AActor> mHiddenTarget;
	void SetHiddenTarget(AActor* Target);

	/** Side of the fence the road is on (see ARMBuildableFence::SetInwardSign). */
	UPROPERTY(Replicated, CustomSerialization)
	float mInwardSign = 1.f;

	/**
	 * Snapped to a road edge or an intersection curb (snap key): each scroll step turns the fence round
	 * (a guard rail facing away from the road), like the street lights. Kept while the hologram lives.
	 */
	bool mOnRoadOrCurb = false;
	bool mFacingFlipped = false;
	int32 mLastScroll = TNumericLimits<int32>::Min();
};
