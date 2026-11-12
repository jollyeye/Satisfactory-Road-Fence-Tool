// Join Road (build menu Road Tools): the opposite of Cut Road. Aim near where two roads meet end to
// end: if one road can follow both (same width, smooth, the same curve a single road would have),
// the cyan outline shows it; click to replace them with that road (the aimed road's lanes, look and paint).
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMRoadJoinHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMRoadJoinHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMRoadJoinHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override { return true; }
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

protected:
	/** Outline of the joined road (cyan), or of both roads (white / red) when they cannot be joined. */
	void RebuildOverlay();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** The aimed road (first: its lanes, look and paint), the road joined to it, and which ends meet. Sent with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mRoad = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mOther = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	int32 mRoadEnd = 0;
	UPROPERTY(Replicated, CustomSerialization)
	int32 mOtherEnd = 0;
};
