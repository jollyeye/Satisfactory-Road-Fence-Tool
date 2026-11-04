// Intersection placement: click each free road end (or free intersection end) to pick it, click it
// again to drop it, then click away from the ends to build. The aimed end gets an orange outline,
// picked ends a white outline and an arrow; both show through other geometry.
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
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return FMath::Max(1, mLocalEnds.Num()); }
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;

	// Game hologram material + valid/invalid colour on the procedural preview.
	virtual void SetMaterialState(EHologramMaterialState state) override;
	void UpdateHologramLook();

protected:
	void RebuildPreview();

	/** Joins these ends (or shows nothing with fewer than 2). */
	void ApplyEnds(const TArray<FRMRoadEndRef>& Found, const FVector& FallbackLocation);

	/** Drops picked ends that are gone or taken (e.g. by the intersection just built), refreshes the rest. */
	void RefreshPicks();

	/**
	 * Outlines of the aimed (orange) and picked (white) end caps, 4 pixels wide, plus a 4 m arrow over
	 * each picked end, always facing the camera. Drawn through other geometry. Rebuilt every frame.
	 */
	void RebuildOverlay();

	/** Unlit material drawn on top of everything, coloured by vertex colour (M_RM_Overlay). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	UMaterialInterface* mOverlayMaterial = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** A free end within this distance (cm) of the crosshair is the one aimed at. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mPickRadius = 1200.f;

	/** Pick mode: picked ends must be within this distance (cm) of each other. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mPickMaxSpan = 20000.f;

	TArray<FRMRoadEndRef> mPicked;
	FRMRoadEndRef mHover;
	bool mHasHover = false;
	uint32 mPickKey = 0;
	double mLastPickTime = 0.0;

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
