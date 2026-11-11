// Cut tools (build menu Road Tools): Cut Road (Edit road) and Cut Fence (Edit fence). Aim at a piece,
// an outline of its cross-section shows where it is cut (through other geometry, like the
// intersection end outlines); click to split it there into two that keep its curve and settings.
// Cut Road's build mode "Cut also fences" also cuts the fences along the road's sides and the roads
// built beside it, at the same place.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMRoadCutHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableSpline;
class UFGHologramBuildModeDescriptor;
class UFGBuildGunModeDescriptor;

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
	virtual void GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const override;

protected:
	/** The cut lines: each cut piece's cross-section outline, 4 px wide, facing the camera. */
	void RebuildOverlay();

	/** Cut Fence (fences) or Cut Road (roads). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	bool mFenceTool = false;

	/** Cut Road: build mode that also cuts the fences along the road and the roads beside it. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<UFGHologramBuildModeDescriptor> mBuildModeWithFences;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** Piece to cut and where (distance along it, cm). Sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableSpline* mTarget = nullptr;

	UPROPERTY(Replicated, CustomSerialization)
	float mCutDist = 0.f;

	bool IsWithFences() const;
	bool IsCutValid() const;
	float MinPieceLength() const { return mFenceTool ? 50.f : 100.f; }

	/** Cut Road with fences: fences on the road's sides and roads beside it crossing the cut, and where. */
	void FindSideCuts(TArray<TPair<ARMBuildableSpline*, float>>& Out) const;
};
