// Road Fill tool: click road / walking path edges and intersection curbs around an area. Each picked edge joins the one
// before: cut where they cross, else a straight line between them. A yellow line closes the loop from the last edge
// back to the first (shown unless it would cross the outline): click it to build. A loop the edges close by themselves
// builds on the next click away from the edges. Click a picked edge again to drop it. Overlay lines are 4 px wide.
#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGBuildableHologram.h"
#include "RMFillHologram.generated.h"

class UProceduralMeshComponent;

/** One picked edge: its curve (world, top surface) and where it was clicked (which part of it is meant). */
struct FRMFillPick
{
	TWeakObjectPtr<AActor> Source;
	int32 Key = 0;               // which edge of the source (road side, intersection curb)
	TArray<FVector> Curve;
	FVector Click = FVector::ZeroVector;
	float Height = 100.f;        // slab thickness of the source
	uint8 Look = 0;
};

UCLASS()
class ROADMAKER_API ARMFillHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMFillHologram();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;
	virtual void CheckValidPlacement() override;
	virtual int32 GetBaseCostMultiplier() const override { return 1; }
	virtual void ConfigureActor(AFGBuildable* inBuildable) const override;
	virtual AActor* Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID) override;
	virtual void SetMaterialState(EHologramMaterialState state) override;

protected:
	/** The edge curve under the aim (road / walking path side, intersection curb), or false. */
	bool FindAimedEdge(const FHitResult& Hit, FRMFillPick& Out) const;
	/** The outline from the picks (see the header comment); fills mRing, mClosedByCurves, mCloseA / B, mValid. */
	void BuildLoop();
	void RebuildOverlay();
	void RebuildPreview();
	void UpdateLook();
	bool CanBuildNow() const { return mValid && (mClosedByCurves || mAimClosing); }

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mPreviewMesh;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mOverlayMesh;

	TArray<FRMFillPick> mPicks;
	FRMFillPick mHover;
	bool mHasHover = false;
	bool mAimClosing = false;
	double mLastClickTime = 0.0;

	/** The outline (world): the picked parts, joined; closed by the closing line unless mClosedByCurves. */
	TArray<FVector> mRing;
	bool mClosedByCurves = false;
	bool mValid = false;
	FVector mCloseA = FVector::ZeroVector, mCloseB = FVector::ZeroVector;
	uint32 mPreviewKey = 0;

	/** Sent with the build click: the outline around the hologram's location, thickness, look. */
	UPROPERTY(Replicated, CustomSerialization)
	TArray<FVector> mLocalOutline;
	UPROPERTY(Replicated, CustomSerialization)
	float mHeight = 100.f;
	UPROPERTY(Replicated, CustomSerialization)
	uint8 mLook = 1;
};
