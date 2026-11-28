// Boom barrier placement (1.6.0), like the Sign Gantry's first two steps: click the side for the post (a road or
// walking path edge: the post goes on its fence line, where the white arrow is), then aim at the other side (the same
// road's other edge, or the far edge of a road beside it, straight across): the barrier is shown with its arm reaching
// it, and the click builds it. Click the first side again to drop it.
// On foundations (1.6.0): the post snaps to the foundation's 0.5 m grid on its top, the arm along one of its axes (the
// one nearest the view, the scroll wheel turns it by 90 degrees); the arm's end snaps to the grid of the foundation
// aimed at (or a road edge), measured along the arm.
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
	/** Aiming at the first pick again (its road side, or the foundation post's own grid point): a click drops it. */
	bool IsOnFirst() const
	{
		return mFirstOnFoundation ? (mHoverOnFoundation && FVector::Dist(mHoverPoint, mPost) < 10.f)
			: (mHasHover && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key);
	}
	/** A point on a foundation's top on its 0.5 m grid, and the foundation axis nearest the view (horizontal). */
	bool FoundationPoint(const FHitResult& Hit, FVector& OutPoint, FVector& OutAxis) const;
	/** The arm's direction from a foundation post: the base axis turned by the scroll wheel's quarter turns. */
	FVector FoundationArmDir() const;
	/** Step 1: the arm to End (along Across from the post); false when too short or long or too high or low. */
	bool ArmTo(const FVector& End, const FVector& Across);

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

	// Foundations.
	bool mHoverOnFoundation = false;
	FVector mHoverAxis = FVector::RightVector;
	bool mFirstOnFoundation = false;
	FVector mFirstAxis = FVector::RightVector;
	int32 mQuarterTurns = 0;
	int32 mLastScroll = TNumericLimits<int32>::Min();
	static constexpr float FoundationGrid = 50.f;

	/** 0: picking the post's side; 1: picking the other side. */
	UPROPERTY(Replicated, CustomSerialization)
	int32 mStep = 0;

	UPROPERTY(Replicated, CustomSerialization)
	float mArmLength = 600.f;
};
