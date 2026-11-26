// Junction slab joining 2..6 road ends. Only the road ends (a few vectors each) are saved;
// the variable mesh is rebuilt from them on load, so the save file stays tiny and robust.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableIntersection.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableIntersection : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableIntersection(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;
	virtual bool DismantlesRiders() const override { return true; }

	/** Ends in actor-local space, plus the slab thickness taken from the connected roads. */
	void SetEnds(const TArray<FRMRoadEnd>& LocalEnds, float Height);

	float GetSlabHeight() const { return mHeight; }

	/** Saved ends converted to world space, used to treat those road ends as occupied. */
	void GetWorldEnds(TArray<FRMRoadEnd>& OutEnds) const;

	/** Curved curbs between neighbouring roads, in world space, at the top of the slab. */
	void GetWorldCurbs(TArray<TArray<FVector>>& OutCurbs) const;

	/** Top outline in world space (closed, last point joins the first); OutIsMouth[i]: edge i -> i+1 is a road mouth. */
	bool GetWorldOutline(TArray<FVector>& OutOutline, TArray<bool>& OutIsMouth) const;
	/** Cheap value that changes whenever the shape or look changes (map layer). */
	uint32 GetShapeKey() const;

	/** Side lines along the curbs (Road Lines > Sidelines in the customizer). */
	void SetSideLines(bool bOn);
	bool HasSideLines() const { return mSideLines; }

	/** Local-only look while the customizer hovers this intersection; unset = show the real state. */
	void SetPreviewSideLines(TOptional<bool> Preview);

	/** Customizer: side lines use the secondary colour, like on roads. */
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;
	void ApplyMarkingColors(const FFactoryCustomizationData& Data);

	// Side slopes (1.6.0): banks from the curbs down to the terrain, as on roads (ARMBuildableRoad: same slope,
	// limit, traces, material), switched with the Side Slopes tool. Measured on the server and saved: one sample about
	// every 2 m along the curbs (SlopePoints), none on the road mouths. Mouths of roads with banks carry on into them.
	bool HasSideSlopes() const { return mSideSlopes; }
	/** Curbs (runs between two road mouths, in outline order) with a bank: bit per curb; old banks = all. */
	uint32 GetSlopeRuns() const { return mSideSlopes ? (mSlopeRuns != 0 ? mSlopeRuns : ~0u) : 0; }
	/** Server: the curbs with a bank; measured now, a curb too deep somewhere is left off. */
	void SetSlopeRuns(uint32 Runs);
	/** The curb nearest a world point (INDEX_NONE: none). */
	int32 RunAt(const FVector& World) const;
	/** The curb of each sample (sample order), and how many curbs. */
	void SampleRuns(TArray<int32>& OutRuns, int32& OutNumRuns) const;
	/** Samples of curbs that are off made flat. */
	void MaskSlopeRuns(TArray<FVector2D>& Samples, uint32 Runs) const;
	/** Clears the curbs with a too-deep sample (and flattens them); returns the curbs left. */
	uint32 DropTooDeepRuns(TArray<FVector2D>& Samples, uint32 Runs) const;
	/** The samples (perimeter distance, drop) for the intersection as it stands (world traces). */
	void MeasureSlopes(TArray<FVector2D>& OutSamples) const;
	/** The banks' mesh (actor space) for these samples. */
	void BuildSlopeMesh(const TArray<FVector2D>& Samples, FRMMesh& Out) const;
	/** World positions of the curb points the samples belong to (in sample order). */
	void GetSlopeWorldPoints(TArray<FVector>& Out) const;
	const TArray<FVector2D>& GetSlopeSamples() const { return mSlopeSamples; }
	/** The intersection's shape and place, for knowing when its banks must be measured again. */
	uint32 SlopeShapeKey() const;
	/** Server: measure the banks again after Delay s (a piece was built or reshaped next to it). */
	void RemeasureSlopes(float Delay);

	/** Same as roads: side lines sit this far in from the curb (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mEdgeLineInset = 90.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mTopMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mBodyMaterial = nullptr;

	/** Points per curb fillet. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	int32 mFilletSegments = 10;

protected:
	UFUNCTION()
	void OnRep_Ends();

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	TArray<FRMRoadEnd> mEnds;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	float mHeight = 200.f;

	/** Off by default, so intersections from older saves look as before. */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	bool mSideLines = false;

	TOptional<bool> mPreviewSideLines;

	/** Side slopes on, their samples (perimeter distance cm, drop cm; see ARMBuildableRoad) and the shape they are for. */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	bool mSideSlopes = false;
	/** Which curbs (bit per run between two mouths; 0 with mSideSlopes = all, from before curbs could be chosen). */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	uint32 mSlopeRuns = 0;
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	TArray<FVector2D> mSlopeSamples;
	UPROPERTY(SaveGame)
	uint32 mSlopeKey = 0;
	/** The shape neighbours were last told about (a new / reshaped / copied piece tells banked pieces nearby once). */
	UPROPERTY(SaveGame)
	uint32 mSlopeNeighbourKey = 0;
	FTimerHandle mSlopeRemeasureTimer;
	bool mSlopeMeasureQueued = false;
	void RequestSlopeMeasure();

	/** A curb point banks start from (actor space): level outward direction, distance round the outline, its edge. */
	struct FSlopePoint
	{
		FVector Pos = FVector::ZeroVector;
		FVector Out = FVector::ForwardVector;
		float S = 0.f;
		int32 Edge = 0;
		int32 Run = 0;
		bool bRunStart = false;
		bool bRunEnd = false;
		int32 Vertex = -1; // outline point it sits on (-1: between two)
	};
	/**
	 * Curb points about every 2 m, run by run (a run: the curb between two road mouths). A run's end points go out along
	 * the mouth's line (as the joined road's own bank at its end: the two meet flush).
	 */
	static void SlopePoints(const TArray<FVector>& Ring, const TArray<bool>& IsMouth, TArray<FSlopePoint>& Out, float& OutPerimeter);
	bool LocalOutline(TArray<FVector>& Ring, TArray<bool>& IsMouth) const;
	void BuildSlopeMeshFrom(const TArray<FVector>& Ring, const TArray<bool>& IsMouth, const TArray<FVector2D>& Samples, FRMMesh& Out,
		TArray<bool>* OutCoveredEdges) const;

	/** Build animation: the surface spreads from the road mouths to the middle (see ARMBuildableProcedural). */
	virtual bool UsesReveal() const override { return mEnds.Num() >= 2; }
	virtual float RevealDistance(const FVector& LocalPos) const override { return DistanceToMouths(LocalPos); }
	virtual float GetBuildAnimDuration() const override { return 2.5f; }
	/** Distance (cm, seen from above) from P (actor space) to the nearest road mouth. */
	float DistanceToMouths(const FVector& P) const;
};
