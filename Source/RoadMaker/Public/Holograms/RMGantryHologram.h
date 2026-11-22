// Sign gantry placement (reworked 1.5.0 for carriageways of several roads and paths): click the first side (a road or
// walking path edge: a column on its fence line), click the other side (the same road's other edge, or the far edge of
// a road beside it: the second column straight across), then the mouse sets the height of the beam in 2 m steps (at
// least 8 m above the road) and a click builds it. Click the first side again to drop it.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "Holograms/RMTunnelHologram.h"
#include "RMGantryHologram.generated.h"

class UProceduralMeshComponent;

UCLASS()
class ROADMAKER_API ARMGantryHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMGantryHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	/** While setting the height every aim is valid (also at the sky), like the game's pole height step. */
	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

	virtual void SetMaterialState(EHologramMaterialState state) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void UpdateHologramLook();

protected:
	void RebuildPreview();
	void RebuildOverlay();
	void UpdateHint();
	void ResetTool();
	/** The second column for the aimed side: straight across from the first; false if it does not face it. */
	bool PairColumn(const FRMTunnelSide& Side, FVector& OutColumn) const;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	FRMTunnelSide mFirst;
	FRMTunnelSide mHover;
	bool mHasHover = false;
	/** The first column (world, on the first side's fence line) and the side's direction there. */
	FVector mColumnA = FVector::ZeroVector;
	FVector mFirstFwd = FVector::ForwardVector;
	/** Where the column goes on the aimed side (step 0: under the aim; step 1: straight across from the first). */
	FVector mHoverColumn = FVector::ZeroVector;
	double mLastClickTime = 0.0;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mPreviewMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	/** The real gantry (the game's beams and cubes, their own materials), as it will be built. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> mParts;

	/** 0: picking the first side; 1: picking the other side; 2: setting the beam height. */
	UPROPERTY(Replicated, CustomSerialization)
	int32 mStep = 0;

	UPROPERTY(Replicated, CustomSerialization)
	float mHalfSpan = 365.f;

	UPROPERTY(Replicated, CustomSerialization)
	float mBeamBottom = 800.f;

	/** Step 1: the aimed side faces the first one (the gantry is shown across). */
	bool mOnRoad = false;

	float mShownHalfSpan = -1.f;
	float mShownBottom = -1.f;
};
