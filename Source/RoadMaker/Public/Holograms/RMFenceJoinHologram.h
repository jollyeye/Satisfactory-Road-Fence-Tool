// Join Fence (build menu Road Tools, fence section): the opposite of Cut Fence. Aim near where two
// fences meet end to end: if one fence can follow both (same kind and look, road on the same side,
// smooth, the same curve a single fence would have, at most 1 km), the green outline shows it; click to
// replace them with that fence (the aimed fence's side, look and paint).
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMFenceJoinHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableFence;

UCLASS()
class ROADMAKER_API ARMFenceJoinHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMFenceJoinHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override { return true; }
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

protected:
	/** Outline of the joined fence (green), or of both fences (white / red) when they cannot be joined. */
	void RebuildOverlay();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** The aimed fence (first: its side, look and paint), the fence joined to it, and which ends meet. Sent with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableFence* mFence = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableFence* mOther = nullptr;
	UPROPERTY(Replicated, CustomSerialization)
	int32 mFenceEnd = 0;
	UPROPERTY(Replicated, CustomSerialization)
	int32 mOtherEnd = 0;
};
