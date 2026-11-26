// Smooth Joint tool (1.6.0, build menu Road Tools > Edit road): rounds the corner where two roads meet. Orange circles
// mark joints of exactly two road ends (not intersections). Click one, then move the aim away from it: both roads are
// cut back by the same distance along their course (shown on screen, 1 m steps) and a new road joins the two cut
// points, meeting each flush (a Bezier curve with the old corner as its control point). The roads and the curve are
// previewed as holograms; the second click shortens the roads and builds the curve with the first road's width,
// lines, material and paint. Works on joints that are already flush too (a wider bend). Built on Adjust Joint.
#pragma once

#include "CoreMinimal.h"
#include "Holograms/RMRoadAdjustHologram.h"
#include "RMRoadSmoothHologram.generated.h"

UCLASS()
class ROADMAKER_API ARMRoadSmoothHologram : public ARMRoadAdjustHologram
{
	GENERATED_BODY()

public:
	ARMRoadSmoothHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void SetMaterialState(EHologramMaterialState state) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	virtual bool TrySnapToRoadEnd(const FHitResult& hitResult) override { return false; }
	virtual void UpdateCurve() override;

	/** One road cut back: what is left (far end to the cut), the cut point and the direction from it towards the joint. */
	struct FTrim
	{
		TArray<FRMSplinePoint> Kept;
		FVector Cut = FVector::ZeroVector;
		FVector TowardJoint = FVector::ForwardVector;
		FVector Right = FVector::RightVector; // across the road at the cut
		float CutDist = 0.f;  // along the road from its start
		float Length = 0.f;
	};
	/** Road cut back by Setback cm from its end at the joint. */
	static bool TrimRoad(const ARMBuildableRoad* Road, int32 JointEnd, float Setback, FTrim& Out);
	/** Both trims and the new curve (world points) for the current setback. */
	bool ComputeSmooth(FTrim& OutA, FTrim& OutB, TArray<FRMSplinePoint>& OutCurve) const;
	/** The longest setback both roads allow (each keeps at least its minimum length). */
	float MaxSetback() const;
	void RebuildSmoothPreviews();
	/** The previews in the roads' real surface (material, look, paint), with the hologram's outline colour on top. */
	void ApplyRoadSurfaces();
	/** Where the curve starts and ends: a bar across each road at the cut, with posts at its edges. */
	void RebuildCutMarks(const FTrim& A, const FTrim& B);

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMeshC;

	/** How far both roads are cut back from the joint (cm), sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	float mSetback = 0.f;
};
