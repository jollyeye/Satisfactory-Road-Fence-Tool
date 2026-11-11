// Lane marking placement (arrows and digits): aim at a road and the marking snaps to the middle of the
// lane you aim at (lanes follow the road's side lines and dividers), at the spot along the road you
// aim at, facing the direction of the road you are looking along. The mouse wheel cycles the variants
// (which arrow / which digit); the click builds it. One click, no steps.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMMarkingHologram.generated.h"

class UProceduralMeshComponent;
class ARMBuildableRoad;

UCLASS()
class ROADMAKER_API ARMMarkingHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMMarkingHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual void CheckValidPlacement() override;
	/** The wheel picks the variant (not a rotation: the direction comes from where you look). */
	virtual void ScrollRotate(int32 delta, int32 step) override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;

	virtual void SetMaterialState(EHologramMaterialState state) override;
	void UpdateHologramLook();

protected:
	void RebuildPreview();

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mPreviewMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	UPROPERTY(Replicated, CustomSerialization)
	int32 mVariant = 0;

	UPROPERTY(Replicated, CustomSerialization)
	float mLaneWidth = 200.f;

	/** The look of the road under the hologram (the marking takes it). */
	UPROPERTY(Replicated, CustomSerialization)
	uint8 mRoadLook = 0;

	/** Aiming at a road. */
	bool mOnRoad = false;

	int32 mShownVariant = -1;
	float mShownLaneWidth = -1.f;
	TWeakObjectPtr<ARMBuildableRoad> mPaintedFrom;
};
