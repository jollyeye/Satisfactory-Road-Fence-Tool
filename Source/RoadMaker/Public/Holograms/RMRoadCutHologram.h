// Cut Road (build menu Road Tools > Edit road): aim at a road, a line across its width shows where it
// is cut (through other geometry, like the intersection end outlines); click to split it there into
// two roads that keep its curve, lines and paint.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMRoadCutHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMRoadCutHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMRoadCutHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override { return true; }
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

protected:
	/** The cut line: the road's cross-section outline at the cut, 4 px wide, facing the camera. */
	void RebuildOverlay();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** Road to cut and where (distance along it, cm). Sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mTarget = nullptr;

	UPROPERTY(Replicated, CustomSerialization)
	float mCutDist = 0.f;

	/** Cut point is far enough from both ends (each piece at least this long, cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mMinPieceLength = 100.f;

	bool IsCutValid() const;
};
