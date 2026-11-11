// A buildable defined by a spline stored in actor-local space.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableSpline.generated.h"

UCLASS(Abstract)
class ROADMAKER_API ARMBuildableSpline : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableSpline(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Points in actor-local space. Called by the hologram before the actor finishes spawning. */
	void SetSplinePoints(const TArray<FRMSplinePoint>& LocalPoints);
	const TArray<FRMSplinePoint>& GetSplinePoints() const { return mSplinePoints; }

	/** Sampled frames of the spline in world space. */
	void GetWorldFrames(TArray<FRMFrame>& OutFrames, float Step = 100.f) const;

	/** Same, in local space (what the mesh is built from). */
	void GetLocalFrames(TArray<FRMFrame>& OutFrames, float Step = 100.f) const;

	float GetLength() const;

	/** Frame (position and directions) at Dist cm along the spline, in world space. */
	bool GetFrameAt(float Dist, FRMFrame& OutFrame) const;

	/**
	 * Cut tools (server): replaces this piece with two of the same kind split at Dist cm along it,
	 * following its curve, with its recipe, paint and settings; this one goes away. Returns the first
	 * piece (start side), the second in OutSecond; null if the cut is not possible.
	 */
	ARMBuildableSpline* CutAt(float Dist, ARMBuildableSpline** OutSecond = nullptr);

	/**
	 * Server: a new piece of Class (built with Recipe) following World points (world space), with this
	 * one's paint and cut settings. Null on failure.
	 */
	ARMBuildableSpline* SpawnLike(TSubclassOf<ARMBuildableSpline> Class, TSubclassOf<class UFGRecipe> Recipe,
		const TArray<FRMSplinePoint>& World, int32 CostMultiplier) const;

	/** Used while spawning cut pieces (CopyCutSettingsTo is protected). */
	void CopyCutSettingsForSpawn(ARMBuildableSpline* New) const { CopyCutSettingsTo(New); }

	/** Extra conditions for a cut at Dist (e.g. what is built on it). */
	virtual bool CanCutAt(float Dist) const { return true; }

	/**
	 * Cross-section shown by the cut tools: half width, and bottom / top along the spline's up.
	 * Upright pieces (fence walls, grid fences) stand vertical on slopes: their outline does too.
	 */
	virtual void GetCutSection(float& OutHalfWidth, float& OutBottom, float& OutTop) const
	{
		OutHalfWidth = 50.f;
		OutBottom = 0.f;
		OutTop = 100.f;
	}
	virtual bool IsCutSectionUpright() const { return false; }

protected:
	/** Cut: settings of this piece that the two new pieces take over (lines, side, look...). */
	virtual void CopyCutSettingsTo(ARMBuildableSpline* New) const {}

	/** Cut: the new pieces exist, this one is about to be dismantled. */
	virtual void BeforeCutDismantle(float Dist) {}

	UFUNCTION()
	void OnRep_SplinePoints();

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_SplinePoints)
	TArray<FRMSplinePoint> mSplinePoints;

	/** Distance between mesh rings in cm. Lower = smoother curves, more triangles. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mMeshStep = 100.f;
};
