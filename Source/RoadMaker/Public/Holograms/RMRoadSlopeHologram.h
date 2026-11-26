// Side Slopes tool (1.6.0, build menu Road Tools > Edit road): turns the banks of a road or walking path on or off.
// Aim at one: a cyan cage shows the banks it would get (from its edges down to the terrain, 1 : 1.5, at most 8 m;
// none where it meets the ground or another road), a red one the banks it would lose. Click to switch. The banks are
// part of the road (its concrete, solid), measured once on the server and saved (ARMBuildableRoad::SetSideSlopes).
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMRoadSlopeHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMRoadSlopeHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMRoadSlopeHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override { return true; }
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	void RebuildOverlay();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** Road to switch. Sent to the server with the click. */
	UPROPERTY(Replicated, CustomSerialization)
	ARMBuildableRoad* mTarget = nullptr;

	/** The banks the aimed road would get, measured once per road (and again when it changes). */
	TWeakObjectPtr<ARMBuildableRoad> mMeasuredFor;
	uint32 mMeasuredKey = 0;
	TArray<FVector2D> mMeasuredLeft;
	TArray<FVector2D> mMeasuredRight;
};
