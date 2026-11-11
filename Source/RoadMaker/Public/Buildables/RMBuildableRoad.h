// Road section. One C++ class; make Blueprint children with mWidthInFoundations = 1, 2, 3 and 4.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableSpline.h"
#include "RMBuildableRoad.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableRoad : public ARMBuildableSpline
{
	GENERATED_BODY()

public:
	ARMBuildableRoad(const FObjectInitializer& ObjectInitializer);

	virtual void RebuildMesh() override;
	// Not while being cut: its tracks / paths are split and stay.
	virtual bool DismantlesRiders() const override { return !mBeingCut; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	int32 GetWidthInFoundations() const { return mWidthInFoundations; }
	float GetRoadWidth() const { return mWidthInFoundations * RM::FoundationSize; }

	/** Customizer: primary colour = dashed middle lines, secondary colour = side lines. */
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	/** The customizer applies colours through this one (virtual via IFGColorInterface). */
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;

	/** Custom primitive data slots read by M_RoadAsphalt (after the 20 floats the game itself uses). */
	static constexpr int32 PrimaryColorIndex = 24;
	static constexpr int32 SecondaryColorIndex = 28;

	/** Lane markings; changing them rebuilds the mesh (and replicates). */
	void SetMarkings(const FRMMarkings& InMarkings);
	const FRMMarkings& GetMarkings() const { return mMarkings; }

	/** Local-only look while the customizer hovers this road; unset = show the real markings. */
	void SetPreviewMarkings(const TOptional<FRMMarkings>& Preview);
	float GetRoadHeight() const { return mRoadHeight; }

	// Cut Road: lines go to both pieces; tracks / paths on it are split too (refused if too short).
	virtual bool CanCutAt(float Dist) const override;
	virtual void GetCutSection(float& OutHalfWidth, float& OutBottom, float& OutTop) const override;

	/**
	 * Game rail tracks / vehicle paths on this road that a cut at Dist crosses, with where along them
	 * (OutSplits). Returns how many would be left with a piece shorter than the game allows (the
	 * track's / path's own minimum length): the cut is refused then.
	 */
	/** Split Road: lengthwise split possible (2+ wide, nothing of the game built on it)? */
	bool CanSplitLengthwise() const;

	/**
	 * Split Road (server): replaces this road with two roads side by side, the left one LeftFoundations
	 * wide and the right one the rest, following its curve, with its lines and paint. Returns the left
	 * road (OutRight the other); null if not possible.
	 */
	/**
	 * A tool replaced this road with a new one (Split Road, Adjust Curvature): out of the way at once
	 * and dismantled, leaving the tracks and paths on it as they are.
	 */
	void DismantleReplaced();

	ARMBuildableRoad* SplitLengthwise(int32 LeftFoundations, ARMBuildableRoad** OutRight = nullptr);

	int32 PlanRiderCuts(float Dist, TArray<TPair<AActor*, float>>& OutSplits) const;


protected:
	virtual void CopyCutSettingsTo(ARMBuildableSpline* New) const override;
	virtual void BeforeCutDismantle(float Dist) override;

private:
	/** Set while this road is replaced by a cut, so dismantling it leaves its riders alone. */
	bool mBeingCut = false;

public:

	/**
	 * End 0 = start, end 1 = end. OutOutward points away from the road, the direction a road or
	 * intersection attached here would continue in. Location is the top-centre of the end.
	 */
	void GetEndWorld(int32 EndIndex, FVector& OutLocation, FVector& OutOutward) const;

	/** Anchor components at the two ends (forward axis = outward), for snapping. */
	USceneComponent* GetEndAnchor(int32 EndIndex) const { return EndIndex == 0 ? mStartAnchor : mEndAnchor; }

	// --- Placement rules, read by the hologram from the class default object -----------------

	/** Road width in foundations (1, 2 or 3 for the three build menu entries). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Road")
	int32 mWidthInFoundations = 1;

	/** Thickness in cm. 100 = a 1 m foundation. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Road")
	float mRoadHeight = 100.f;

	/**
	 * Smallest allowed radius of the road's inner edge in a turn, in cm. The centre-line limit is
	 * half the road width plus this: 6 m for 1 wide, 10 m for 2 wide, 14 m for 3 wide.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Placement")
	float mMinInnerEdgeRadius = 200.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Placement")
	float mMaxGradeDegrees = 30.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Placement")
	float mMinLength = 200.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Placement")
	float mMaxLength = 10000.f;

	float GetEffectiveMinTurnRadius() const { return GetRoadWidth() * 0.5f + mMinInnerEdgeRadius; }

	// --- Look -------------------------------------------------------------------------------

	/** Side lines sit this far in from the road edge (cm), leaving room for fences. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mEdgeLineInset = 90.f;

	/** Section 0. Pick the game's asphalt material here. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mTopMaterial = nullptr;

	/** Section 1. Pick the game's concrete material here. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mBodyMaterial = nullptr;

	/** Pushes the resolved customizer colours to the mesh for the marking material. */
	void ApplyMarkingColors(const FFactoryCustomizationData& Data);

protected:

	UFUNCTION()
	void OnRep_Markings() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Markings)
	FRMMarkings mMarkings;

	TOptional<FRMMarkings> mPreviewMarkings;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	USceneComponent* mStartAnchor;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	USceneComponent* mEndAnchor;
};
