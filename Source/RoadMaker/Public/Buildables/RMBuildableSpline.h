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

protected:
	UFUNCTION()
	void OnRep_SplinePoints();

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_SplinePoints)
	TArray<FRMSplinePoint> mSplinePoints;

	/** Distance between mesh rings in cm. Lower = smoother curves, more triangles. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mMeshStep = 100.f;
};
