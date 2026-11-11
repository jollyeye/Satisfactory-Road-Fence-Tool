// Sign gantry placement, two clicks like the game's signs: aim at a road and click to put the gantry
// across it (columns on the road's fence lines); then the mouse sets the height of the beam in 2 m
// steps (at least 8 m above the road) and the second click builds it.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
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
	void UpdateHologramLook();

protected:
	void RebuildPreview();

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mPreviewMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	/** 0: placing across a road; 1: setting the beam height. */
	UPROPERTY(Replicated, CustomSerialization)
	int32 mStep = 0;

	UPROPERTY(Replicated, CustomSerialization)
	float mHalfSpan = 365.f;

	UPROPERTY(Replicated, CustomSerialization)
	float mBeamBottom = 800.f;

	/** Step 0: the aimed piece is a road. */
	bool mOnRoad = false;

	float mShownHalfSpan = -1.f;
	float mShownBottom = -1.f;
};
