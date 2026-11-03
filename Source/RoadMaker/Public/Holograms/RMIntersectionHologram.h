// Intersection placement: aim between road ends, the nearest free ends (up to 6) within range are
// joined by one slab. One click builds it.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMIntersectionHologram.generated.h"

class UProceduralMeshComponent;

UCLASS()
class ROADMAKER_API ARMIntersectionHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMIntersectionHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override { return true; }
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return FMath::Max(1, mLocalEnds.Num()); }
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;

protected:
	void RebuildPreview();

	/** Road ends within this distance (cm) of the crosshair are joined. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSearchRadius = 2500.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	int32 mMaxRoads = 6;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mPreviewMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	UPROPERTY(Replicated)
	TArray<FRMRoadEnd> mLocalEnds;

	UPROPERTY(Replicated)
	float mHeight = 200.f;

	/** New intersections get side lines when a connected road (or intersection) has them. */
	UPROPERTY(Replicated)
	bool mSideLines = false;

	bool mGeometryValid = false;
};
