// Two-click spline placement shared by roads and free-standing fences:
// click 1 fixes the start, click 2 fixes the end and builds. Without snapping, the curve is a single
// circular arc that keeps the start direction (like railways and vehicle paths); the straight build
// mode forces a straight line instead.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMSplineHologram.generated.h"

class UProceduralMeshComponent;
class UFGHologramBuildModeDescriptor;
class UFGBuildGunModeDescriptor;

UCLASS(Abstract)
class ROADMAKER_API ARMSplineHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMSplineHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// AFGHologram
	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override;
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual int32 GetBaseCostMultiplier() const override;
	virtual void GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const override;

	// AFGBuildableHologram
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;

protected:
	/** Height added above the aimed surface (roads sit on top of what they are placed on). */
	virtual float GetSurfaceOffset() const { return 0.f; }

	/** Length in cm that one recipe unit pays for. */
	virtual float GetCostUnitLength() const { return RM::FoundationSize; }

	/** Extra multiplier on top of length (road width in foundations). */
	virtual int32 GetCostWidthFactor() const { return 1; }

	/**
	 * Snap the current step to something near the aim point (a free road end for roads).
	 * Runs every frame from SetHologramLocationAndRotation, so it does not depend on the game
	 * passing the aimed actor to TrySnapToActor. Return true if snapped (and the curve updated).
	 */
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) { return false; }

	/** Mouse-wheel rotation in degrees since the current step began. */
	float GetStepScrollYaw() const;

	/** Rebuild the preview mesh from mLocalPoints. */
	virtual void RebuildPreview() PURE_VIRTUAL(ARMSplineHologram::RebuildPreview, );

	/** Recomputes the spline from the start/end state, moves the actor to the start, rebuilds preview. */
	void UpdateCurve();

	/** Directly set world-space points (used by fence edge mode). Actor goes to the first point. */
	void SetWorldPoints(const TArray<FRMSplinePoint>& WorldPoints);

	bool IsStraightMode() const;

	/** Samples the current local points. */
	void GetPreviewFrames(TArray<FRMFrame>& OutFrames, float Step) const;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	/** Optional material for the preview; leave empty to let the game apply its hologram material. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mPreviewMaterial = nullptr;

	/** Build mode asset (a UFGHologramBuildModeDescriptor Blueprint) for straight placement. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<UFGHologramBuildModeDescriptor> mBuildModeStraight;

	/** Current points, actor-local. Replicated so the server builds what the client sees. */
	UPROPERTY(Replicated)
	TArray<FRMSplinePoint> mLocalPoints;

	int32 mStep = 0;

	FVector mStart = FVector::ZeroVector;
	FVector mStartDir = FVector::ForwardVector;
	bool mStartSnapped = false;

	FVector mEnd = FVector::ZeroVector;
	FVector mEndDir = FVector::ForwardVector;
	bool mEndSnapped = false;

	/** Scroll value when the current step started, so each step rotates from zero. */
	int32 mScrollAtStepStart = 0;
};
