// Boom barrier placement (1.6.0), like the Sign Gantry's first two steps: click the side for the post (a road or
// walking path edge: the post goes on its fence line, where the white arrow is), then aim at the other side (the same
// road's other edge, or the far edge of a road beside it, straight across): the barrier is shown with its arm reaching
// it, and the click builds it. Click the first side again to drop it.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "Holograms/RMTunnelHologram.h"
#include "RMBarrierHologram.generated.h"

class UProceduralMeshComponent;

UCLASS()
class ROADMAKER_API ARMBarrierHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMBarrierHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	void RebuildPreview();
	void RebuildOverlay();
	void UpdateHint();
	void ResetTool();
	/** The arm's end on the aimed side: straight across from the post; false if it does not face it. */
	bool PairEnd(const FRMTunnelSide& Side, FVector& OutEnd) const;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	FRMTunnelSide mFirst;
	FRMTunnelSide mHover;
	bool mHasHover = false;
	FVector mPost = FVector::ZeroVector;
	FVector mFirstFwd = FVector::ForwardVector;
	FVector mHoverPoint = FVector::ZeroVector;
	double mLastClickTime = 0.0;
	/** Step 1: the aimed side faces the post (the barrier is shown across). */
	bool mAcross = false;
	float mShownLength = -1.f;

	/** 0: picking the post's side; 1: picking the other side. */
	UPROPERTY(Replicated, CustomSerialization)
	int32 mStep = 0;

	UPROPERTY(Replicated, CustomSerialization)
	float mArmLength = 600.f;
};
