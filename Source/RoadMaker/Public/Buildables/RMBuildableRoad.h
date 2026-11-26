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
	float GetRoadWidth() const { return mWidthCm > 0.f ? mWidthCm : mWidthInFoundations * RM::FoundationSize; }

	/**
	 * Walking path (1.4.0): a narrow road piece (2 m / 4 m) to widen a road beside it or for foot traffic.
	 * Same slab, snapping and tools as a road, but no vehicle path / track snapping, at most one middle
	 * line, no lane markings, Concrete Blocks by default, and it joins only walking paths end to end.
	 */
	bool IsWalkingPath() const { return mIsWalkingPath; }

	/**
	 * Split / Merge step across the width: 8 m for roads, 2 m for walking paths (a 4 m path splits into two
	 * 2 m paths, two 2 m paths merge into a 4 m one).
	 */
	float GetSplitUnit() const { return IsWalkingPath() ? 200.f : RM::FoundationSize; }
	int32 GetWidthUnits() const { return FMath::Max(1, FMath::RoundToInt(GetRoadWidth() / GetSplitUnit())); }

	/** The markings this piece can have: a walking path at most one middle line (2 lanes), side lines only when 4 m wide. */
	FRMMarkings LimitMarkings(const FRMMarkings& In) const;

	/** The look a new piece of this kind starts with (0 asphalt, 1 concrete blocks, 2 cyberpunk). */
	uint8 GetDefaultLook() const { return mDefaultLook; }

	/** Customizer: primary colour = dashed middle lines, secondary colour = side lines. */
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	/** The customizer applies colours through this one (virtual via IFGColorInterface). */
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;

	/** Custom primitive data slots read by M_RoadAsphalt (after the 20 floats the game itself uses). */
	static constexpr int32 PrimaryColorIndex = 24;
	static constexpr int32 SecondaryColorIndex = 28;
	/**
	 * Dashed lane lines (1.6.0): custom primitive data slot of the dash scale. Each road fits a whole number of 9 m
	 * dash cycles (3 m dash between half gaps), stretched to its length like the grid fence's panels, so the dashes
	 * meet the next road's evenly whichever way it runs.
	 */
	static constexpr int32 DashScaleIndex = 32;
	static float DashScaleFor(float LengthCm)
	{
		const int32 Cycles = FMath::Max(1, FMath::RoundToInt(LengthCm / 900.f));
		return FMath::Max(LengthCm, 1.f) / (Cycles * 900.f);
	}

	/** The resolved paint colours: primary = dashed middle lines, secondary = side lines (and the cyberpunk glow). */
	void GetLineColours(FLinearColor& OutPrimary, FLinearColor& OutSecondary) const;

	/** Lane markings; changing them rebuilds the mesh (and replicates). */
	void SetMarkings(const FRMMarkings& InMarkings);
	const FRMMarkings& GetMarkings() const { return mMarkings; }

	/**
	 * Lane centres across the road (cm from the middle, left to right) and the width of one lane.
	 * For lane markings. With side lines the lanes sit inside them; 1 lane = one spot in the middle.
	 */
	void GetLaneCentres(TArray<float>& OutOffsets, float& OutLaneWidth) const;

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
	/** Split Road: lengthwise split possible (roads 2+ foundations wide, walking paths 4 m)? */
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

	/** LeftUnits: the left piece's width in split units (GetSplitUnit). */
	ARMBuildableRoad* SplitLengthwise(int32 LeftUnits, ARMBuildableRoad** OutRight = nullptr);

	/** Merge Road: can Other (lying beside this road) be merged with it? */
	enum class EMergeCheck : uint8 { Ok, NotBeside, LengthMismatch, TooWide };
	/**
	 * Other must lie right beside this road, edge to edge (gap under 50 cm) and level with it, along
	 * the whole length (either direction), starting and ending with it, and the two together at most
	 * 4 foundations wide. OutCentre: the merged road's centre line (world frames), OutWidth its width.
	 */
	EMergeCheck CheckMerge(const ARMBuildableRoad* Other, TArray<FRMFrame>* OutCentre = nullptr, float* OutWidth = nullptr) const;

	/**
	 * Merge Road (server): replaces this road and Other with one road of their combined width along
	 * the middle of the pair, with this road's lanes, look and paint (side lines on if either had them).
	 * Markings, paths, tracks and fences stay where they are. Null if not possible.
	 */
	ARMBuildableRoad* MergeWith(ARMBuildableRoad* Other);

	/** Join Road: can Other be joined to this road end to end (this road's end MyEnd meets Other's OtherEnd)? */
	enum class EJoinCheck : uint8 { Ok, NotJoined, Width, Kink, Curve, TooLong };
	/**
	 * The two ends must meet (within 50 cm, same height) and continue smoothly; same width; the joined
	 * length within the road's maximum; and the two must follow the one curve a single road would have
	 * between the outer ends (seen from above, within 50 cm): a straight piece and a curved one, or a
	 * curve and an S, cannot become one road. OutFrames: the joined course (world, from this road's
	 * outer end to Other's outer end).
	 */
	EJoinCheck CheckJoin(const ARMBuildableRoad* Other, int32 MyEnd, int32 OtherEnd, TArray<FRMFrame>* OutFrames = nullptr) const;

	/**
	 * Join Road (server): replaces this road and Other with one road along both, with this road's lanes,
	 * look and paint. Markings, paths, tracks and fences stay where they are. Null if not possible.
	 */
	ARMBuildableRoad* JoinWith(ARMBuildableRoad* Other, int32 MyEnd, int32 OtherEnd);

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

	/** Width in cm when not whole foundations (walking paths: 200 / 400); 0 = mWidthInFoundations. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Road")
	float mWidthCm = 0.f;

	/** A walking path (see IsWalkingPath). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Road")
	bool mIsWalkingPath = false;

	/** The look a new piece starts with (0 asphalt, 1 concrete blocks, 2 cyberpunk). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	uint8 mDefaultLook = 0;

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

	// --- Side slopes (1.6.0) -------------------------------------------------------------------
	// Banks from a raised road's edges down to the terrain (fill only), per road, off by default (Side Slopes tool).
	// Measured once (server) when switched on or when the piece is made, and saved; the mesh is built from that.

	bool HasSideSlopes() const { return mSideSlopes; }
	const TArray<FVector2D>& GetSlopeLeft() const { return mSlopeLeft; }
	const TArray<FVector2D>& GetSlopeRight() const { return mSlopeRight; }
	/** Server: switches the banks on (measured now) or off, and rebuilds the mesh. */
	void SetSideSlopes(bool bOn);
	/** The road's shape and place, for knowing when its banks must be measured again. */
	uint32 SlopeShapeKey() const;
	/** Server: measure the banks again after Delay s (a piece was built or reshaped next to it). */
	void RemeasureSlopes(float Delay);
	/**
	 * Measures the banks as the road stands now (world traces): every SlopeSampleStep cm along each edge,
	 * (distance along cm, drop cm); 0 = no bank there (ground at or above the edge, a road / intersection beside it,
	 * or no ground within SlopeMaxDrop).
	 */
	void MeasureSlopes(TArray<FVector2D>& OutLeft, TArray<FVector2D>& OutRight) const;
	/**
	 * One bank sample (roads and intersections): from a world edge point going out along the level direction Out.
	 * Returns the drop (cm, > 0), 0 = no bank (ground at or above the edge, or a road / intersection in the way), or
	 * SlopeTooDeepMark (no ground within SlopeMaxDrop). Traces ignore Self and pass through Pass.
	 */
	/** Server: banked roads and intersections near Piece (within a bank's reach) measure again shortly. */
	static void NudgeSlopeNeighbours(AActor* Piece);
	static float MeasureBankAt(UWorld* World, const FVector& Edge, const FVector& Out, const AActor* Self, const TArray<const AActor*>& Pass);
	/** The banks' mesh in actor space for measured samples (straight runs merged, nothing where there is no bank). */
	void BuildSlopeMesh(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right, FRMMesh& Out) const;
	/**
	 * Same, on the road mesh's own cross-sections (Frames, actor space): the banks start on the road's edge exactly.
	 * At an end with a bank the cap closes the whole embankment (road and banks, down to the feet; the road's own
	 * cap is left out there: SlopeCapAt).
	 */
	void BuildSlopeMesh(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right, const TArray<FRMFrame>& Frames, FRMMesh& Out) const;
	/** Whether this end (0 start, 1 end) has a bank on either side: the embankment cap replaces the road's. */
	bool SlopeCapAt(int32 End) const;
	/** Bank drop (cm) at a distance along, from samples (linear between them). */
	static float SlopeDropAt(const TArray<FVector2D>& Samples, float Dist);
	/** A sample with no ground within SlopeMaxDrop (its drop is SlopeTooDeepMark): such a road gets no banks at all. */
	static bool SlopeTooDeep(const TArray<FVector2D>& Left, const TArray<FVector2D>& Right);
	static constexpr float SlopeTooDeepMark = -1.f;
	/** Part of every slope key: raised when the measuring changes, so banks saved by older builds are measured again. */
	static constexpr uint32 SlopeVersion = 11;
	/** Per stretch between two frames and side: true where a bank covers the road's side all along (no side face there). */
	void SlopeCovered(const TArray<FRMFrame>& Frames, TArray<bool>& OutLeft, TArray<bool>& OutRight) const;

	static constexpr float SlopeRun = 1.5f;        // 1 : 1.5 (about 34 degrees)
	static constexpr float SlopeMaxDrop = 800.f;   // banks down to 8 m below the edge
	static constexpr float SlopeSampleStep = 200.f;
	static constexpr float SlopeEmbed = 30.f;      // the foot goes this far into the ground (no gaps on bumps)

	/** Pushes the resolved customizer colours to the mesh for the marking material. */
	void ApplyMarkingColors(const FFactoryCustomizationData& Data);

protected:

	UFUNCTION()
	void OnRep_Markings() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Markings)
	FRMMarkings mMarkings;

	/** Side slopes on, and their measured samples per side (distance along, drop), cm. */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Markings)
	bool mSideSlopes = false;
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Markings)
	TArray<FVector2D> mSlopeLeft;
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Markings)
	TArray<FVector2D> mSlopeRight;
	/** The shape and place the banks were measured for: a reshaped piece, a cut one or a copy elsewhere (a blueprint)
	 *  measures again, a tick later on the server (once the pieces it replaces are gone). */
	UPROPERTY(SaveGame)
	uint32 mSlopeKey = 0;
	/** The shape neighbours were last told about (a new / reshaped / copied piece tells banked pieces nearby once). */
	UPROPERTY(SaveGame)
	uint32 mSlopeNeighbourKey = 0;
	FTimerHandle mSlopeRemeasureTimer;
	void RequestSlopeMeasure();
	bool mSlopeMeasureQueued = false;

	TOptional<FRMMarkings> mPreviewMarkings;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	USceneComponent* mStartAnchor;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	USceneComponent* mEndAnchor;

};
