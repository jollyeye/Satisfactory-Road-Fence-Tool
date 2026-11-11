// Adjust Curvature tool (build menu Road Tools > Edit road): orange circles mark the road ends near
// you. Click one to grab the end (and the end of the road joined to it, when two roads meet end to end
// at their middle); move it like a normal road end (aim, snap to a free road end, mouse wheel turns
// it, hold H and nudge), the curves of the road(s) follow as holograms; the second click replaces them
// with the new curves, keeping their other ends, width, lines and paint.
#pragma once

#include "CoreMinimal.h"
#include "Holograms/RMSplineHologram.h"
#include "RMRoadAdjustHologram.generated.h"

class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMRoadAdjustHologram : public ARMSplineHologram
{
	GENERATED_BODY()

public:
	ARMRoadAdjustHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void SetMaterialState(EHologramMaterialState state) override;
	virtual void Destroyed() override;

protected:
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override;
	virtual void RebuildPreview() override {}
	virtual void UpdateCurve() override;
	virtual FVector GetPlacedPointAxis() const override;
	virtual float GetSurfaceOffset() const override;

	/** A road end that can be grabbed, with the road joined to it (if any). */
	struct FEnd
	{
		TWeakObjectPtr<ARMBuildableRoad> Road;
		int32 EndIndex = 0;
		TWeakObjectPtr<ARMBuildableRoad> Partner;
		int32 PartnerEnd = 0;
		FVector Location = FVector::ZeroVector;
	};

	/**
	 * Aiming at a foundation. One road: its end snaps to the middle of the nearest edge (or +-4 m along
	 * it), road top level with the foundation, pointing into it, like a new road. Two roads: the joint
	 * snaps to the 4 m grid on top of the foundation (road resting on it), direction free.
	 */
	bool TrySnapToFoundation(const FHitResult& hitResult);

	/** The grabbed roads are hidden while their replacement is previewed. */
	void SetGrabbedRoadsHidden(bool bMakeHidden);
	bool mRoadsHidden = false;

	/** The road ends near the player (refreshed twice a second). */
	void RefreshEnds();
	int32 FindAimedEnd(const FVector& Aim) const;

	/** The new curves from the joint position / direction (world points), for road A and road B. */
	bool ComputeCurves(TArray<FRMSplinePoint>& OutA, TArray<FRMSplinePoint>& OutB) const;

	/** Circles at the ends (step 0) or at the grabbed point (step 1). */
	void RebuildCircles();
	void RebuildRoadPreviews();
	void ApplyLookToSecondRoad();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMeshB;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mCircleMesh;

	/** Grabbed roads and ends, sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mRoadA = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mRoadB = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	int32 mEndA = 0;
	UPROPERTY(Replicated, CustomSerialization)
	int32 mEndB = 0;

	/** Where the grabbed end(s) go, and the direction road A leaves the joint in (B the opposite). */
	UPROPERTY(Replicated, CustomSerialization)
	FVector mJoint = FVector::ZeroVector;
	UPROPERTY(Replicated, CustomSerialization)
	FVector mJointDir = FVector::ForwardVector;

	TArray<FEnd> mEnds;
	FVector mCircleBase = FVector::ZeroVector;
	double mEndsTime = -100.0;
	int32 mAimed = INDEX_NONE;
	int32 mDrawnAimed = -2;

	/** Circles show ends within this distance (cm) of the camera. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mShowRadius = 15000.f;

	/** How close (cm) to a road end the crosshair must be to pick it. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mPickRadius = 700.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSnapRadius = 600.f;
};
