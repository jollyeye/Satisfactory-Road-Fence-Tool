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
	ARMRoadHologram();

	virtual void CheckValidPlacement() override;
	virtual void SetMaterialState(EHologramMaterialState state) override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// A road beside a road (snap key), locked: nudging moves the whole road, up / down and along the
	// world axes nearest to where the camera looks (like snapped fences).
	virtual ENudgeFailReason NudgeHologram(const FVector& NudgeInput, const FHitResult& HitResult) override;
	virtual void SetHologramNudgeLocation() override;
	virtual void OnHologramLockStateChanged() override;

	virtual void GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const override;
	/** Long Curve mode: the build gun reaches 1 km, to place the end of a big curve far away. */
	virtual float GetBuildGunRangeOverride_Implementation() const override;

protected:
	/**
	 * "Long Curve" build mode: the default curve with no length limit and no turn or grade limits,
	 * only refused when the road would fold over itself (a bend tighter than half its width). For big
	 * sweeping curves; paths are added after with Cut Road (pieces of normal length).
	 */
	bool IsLongMode() const;

	/** Build mode asset (a UFGHologramBuildModeDescriptor Blueprint) for Long Curve. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<UFGHologramBuildModeDescriptor> mBuildModeLong;

	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	virtual float GetSurfaceOffset() const override;
	virtual int32 GetCostWidthFactor() const override;
	virtual void RebuildPreview() override;
	/** The real surface (look and paint) in the hologram, fading out along the stub before the first click. */
	virtual void UpdateHologramLook() override;
	virtual float GetStubLength() const override { return RM::FoundationSize; }
	/** Before the first click: the short stub that fades out (not a road beside another one). */
	bool IsFadingStub() const { return mStep == 0 && !mBesideRoad; }

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

	/** Same beside an intersection: the parallel of the curb aimed at, outside the junction. */
	bool SnapBesideCurb(class ARMBuildableIntersection* Intersection, const FVector& AimPoint);

	/** Uses the given curve as the new road (beside mode). */
	bool PlaceBeside(const TArray<FRMSplinePoint>& Points);
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;

	/** The current curve is a parallel of another road: only geometry limits apply. */
	UPROPERTY(Replicated, CustomSerialization)
	bool mBesideRoad = false;

	/** Where the locked beside-road hologram stood, and how far it has been nudged from there. */
	FVector mBesideBase = FVector::ZeroVector;
	FVector mBesideNudge = FVector::ZeroVector;

	/** A flat white arrow on top of the road hologram (the intersection tool's arrow), pointing the way the road runs. */
	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	class UProceduralMeshComponent* mDirectionArrow;

	void RebuildDirectionArrow(const TArray<struct FRMFrame>& Frames);
	void ApplyArrowMaterial();

	/** Markings for the new road: copied from the road it snaps to, otherwise the default. */
	UPROPERTY(CustomSerialization)
	FRMMarkings mMarkings;
	bool mHasCopiedMarkings = false;

	/** Surface look of the new road: copied from the road (or intersection) it snaps to. */
	UPROPERTY(CustomSerialization)
	uint8 mLook = 0;

	/** Along-edge snap step on foundations, in cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mFoundationEdgeStep = 400.f;

	/** How close (cm) to a road end the crosshair must be to snap to it. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSnapRadius = 600.f;
};
