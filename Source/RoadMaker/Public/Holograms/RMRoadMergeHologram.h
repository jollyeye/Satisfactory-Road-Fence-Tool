// Merge Road (build menu Road Tools): the opposite of Split Road. Click a road to pick it, then click
// the road right beside it (edge to edge, starting and ending together): they become one road of
// their combined width (at most 4 foundations), with the first road's lanes, look and paint. Click
// the picked road again to drop it.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMRoadMergeHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMRoadMergeHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMRoadMergeHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

protected:
	/** Outlines: the aimed / picked road, and the merged road (cyan, red if refused). */
	void RebuildOverlay();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** The first road picked, and the road aimed at (beside it, to merge). Sent with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mFirst = nullptr;

	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mAimed = nullptr;

	double mLastClickTime = 0.0;
};
