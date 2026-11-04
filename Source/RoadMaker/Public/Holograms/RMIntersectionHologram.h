// Intersection placement: aim between road ends, the nearest free ends (up to 6) within range are
// joined by one slab. One click builds it.
// "Pick Road Ends" build mode, for big intersections: click each free road end to pick it (click it
// again to drop it), then click away from the ends to build. Picked ends can be far apart.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMGeometry.h"
#include "RMRoadUtils.h"
#include "RMIntersectionHologram.generated.h"

class UProceduralMeshComponent;
class UFGHologramBuildModeDescriptor;
class UFGBuildGunModeDescriptor;

UCLASS()
class ROADMAKER_API ARMIntersectionHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMIntersectionHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const override;
	virtual void OnBuildModeChanged(TSubclassOf<UFGHologramBuildModeDescriptor> buildMode) override;
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return FMath::Max(1, mLocalEnds.Num()); }
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;

	// Game hologram material + valid/invalid colour on the procedural preview.
	virtual void SetMaterialState(EHologramMaterialState state) override;
	void UpdateHologramLook();

protected:
	void RebuildPreview();

	/** Joins these ends (or shows nothing with fewer than 2). */
	void ApplyEnds(const TArray<FRMRoadEndRef>& Found, const FVector& FallbackLocation);

	bool IsPickMode() const;

	/** Drops picked ends that are gone or taken (e.g. by the intersection just built), refreshes the rest. */
	void RefreshPicks();

	/** Bars on picked ends and posts on the aimed end, in the preview (section 2). */
	void BuildMarkers(FRMMesh& Out) const;
	uint32 MarkerKey() const;

	/** Build mode asset for picking road ends one by one (big intersections). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<UFGHologramBuildModeDescriptor> mBuildModePick;

	/** Pick mode: a free end within this distance (cm) of the crosshair is the one aimed at. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mPickRadius = 1200.f;

	/** Pick mode: picked ends must be within this distance (cm) of each other. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mPickMaxSpan = 20000.f;

	TArray<FRMRoadEndRef> mPicked;
	FRMRoadEndRef mHover;
	bool mHasHover = false;
	uint32 mMarkerKey = 0;
	double mLastPickTime = 0.0;

	/** Road ends within this distance (cm) of the crosshair are joined (50 m: big enough for rail curves). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSearchRadius = 3200.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	int32 mMaxRoads = 6;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mPreviewMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	UPROPERTY(Replicated, CustomSerialization)
	TArray<FRMRoadEnd> mLocalEnds;

	UPROPERTY(Replicated, CustomSerialization)
	float mHeight = 200.f;

	/** New intersections get side lines when a connected road (or intersection) has them. */
	UPROPERTY(Replicated, CustomSerialization)
	bool mSideLines = false;

	bool mGeometryValid = false;
};
