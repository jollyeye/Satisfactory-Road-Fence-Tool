// Tunnel tool (1.5.0): click the first side (a road or walking path edge, or a fence wall: the arch then springs from
// its top), click the other side (the same road's other edge, a parallel road's, or another fence wall), then the aim
// sets the arc (up to a semicircle, shown on screen in degrees) and a click builds it. The arch runs along the whole
// stretch where the two sides run side by side. Click the first side again to drop it.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMTunnelHologram.generated.h"

class UProceduralMeshComponent;

/** One side of the tunnel: where the arch's foot runs (world, every metre) and what it stands on. */
struct FRMTunnelSide
{
	TWeakObjectPtr<AActor> Source;
	int32 Key = 0; // road: 0 left edge, 1 right edge; fence wall: 2
	TArray<FVector> Line;
};

UCLASS()
class ROADMAKER_API ARMTunnelHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMTunnelHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual bool IsValidHitResult(const FHitResult& hitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override;
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void SetMaterialState(EHologramMaterialState state) override;

	/**
	 * The arch's feet for two sides: one cross-section per metre of the first side where the other runs beside it
	 * (straight across, 3 to 60 m apart), the longest such stretch, then thinned where it runs straight. World space.
	 */
	static bool PairSides(const FRMTunnelSide& A, const FRMTunnelSide& B, TArray<FVector>& OutLeft, TArray<FVector>& OutRight);

protected:
	bool FindSide(const FHitResult& Hit, FRMTunnelSide& Out) const;
	void RebuildPreview();
	void RebuildOverlay();
	void UpdateLook();
	void UpdateHint();
	void ResetTool();

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	/** 0: pick the first side; 1: pick the other side; 2: set the arc. */
	int32 mStep = 0;
	FRMTunnelSide mFirst;
	FRMTunnelSide mHover;
	bool mHasHover = false;
	/** Step 1: the aimed side pairs with the first (the arch is shown). */
	bool mPaired = false;
	double mLastClickTime = 0.0;
	uint32 mPreviewKey = 0;

	/** Sent with the build click: the feet around the hologram's location, and the arc. */
	UPROPERTY(Replicated, CustomSerialization)
	TArray<FVector> mLocalLeft;

	UPROPERTY(Replicated, CustomSerialization)
	TArray<FVector> mLocalRight;

	UPROPERTY(Replicated, CustomSerialization)
	float mArcDeg = 180.f;
};
