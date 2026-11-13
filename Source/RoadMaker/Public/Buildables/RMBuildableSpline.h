// A buildable defined by a spline stored in actor-local space.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableSpline.generated.h"

/** Where a spline piece's build animation starts. */
UENUM()
enum class ERMRevealFrom : uint8
{
	Start,         // its first point (where the player clicked first)
	End,           // its last point
	NearestPlayer, // the end nearer the local player (pieces built beside another one in one click)
	NearestPoint,  // the end nearer mRevealPoint (Adjust Curvature: the grabbed point)
	FromPoint,     // outwards both ways from mRevealPoint (Join Road: the joint)
};

UCLASS(Abstract)
class ROADMAKER_API ARMBuildableSpline : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableSpline(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Points in actor-local space. Called by the hologram before the actor finishes spawning. */
	void SetSplinePoints(const TArray<FRMSplinePoint>& LocalPoints);
	const TArray<FRMSplinePoint>& GetSplinePoints() const { return mSplinePoints; }

	/** Sampled frames of the spline in world space. */
	void GetWorldFrames(TArray<FRMFrame>& OutFrames, float Step = 100.f) const;

	/** Same, in local space (what the mesh is built from). */
	void GetLocalFrames(TArray<FRMFrame>& OutFrames, float Step = 100.f) const;

	float GetLength() const;

	/** Frame (position and directions) at Dist cm along the spline, in world space. */
	bool GetFrameAt(float Dist, FRMFrame& OutFrame) const;

	/**
	 * Cut tools (server): replaces this piece with two of the same kind split at Dist cm along it,
	 * following its curve, with its recipe, paint and settings; this one goes away. Returns the first
	 * piece (start side), the second in OutSecond; null if the cut is not possible.
	 */
	ARMBuildableSpline* CutAt(float Dist, ARMBuildableSpline** OutSecond = nullptr);

	/**
	 * Server: a new piece of Class (built with Recipe) following World points (world space), with this
	 * one's paint and cut settings. Null on failure.
	 */
	ARMBuildableSpline* SpawnLike(TSubclassOf<ARMBuildableSpline> Class, TSubclassOf<class UFGRecipe> Recipe,
		const TArray<FRMSplinePoint>& World, int32 CostMultiplier,
		ERMRevealFrom RevealFrom = ERMRevealFrom::Start, const FVector& RevealPoint = FVector::ZeroVector) const;

	/** Where the build animation starts (server, before the piece finishes spawning). */
	void SetRevealFrom(ERMRevealFrom From, const FVector& Point = FVector::ZeroVector) { mRevealFrom = From; mRevealPoint = Point; }

	/** Used while spawning cut pieces (CopyCutSettingsTo is protected). */
	void CopyCutSettingsForSpawn(ARMBuildableSpline* New) const { CopyCutSettingsTo(New); }

	/** Extra conditions for a cut at Dist (e.g. what is built on it). */
	virtual bool CanCutAt(float Dist) const { return true; }

	/**
	 * Cross-section shown by the cut tools: half width, and bottom / top along the spline's up.
	 * Upright pieces (fence walls, grid fences) stand vertical on slopes: their outline does too.
	 */
	virtual void GetCutSection(float& OutHalfWidth, float& OutBottom, float& OutTop) const
	{
		OutHalfWidth = 50.f;
		OutBottom = 0.f;
		OutTop = 100.f;
	}
	virtual bool IsCutSectionUpright() const { return false; }

protected:
	/** Cut: settings of this piece that the two new pieces take over (lines, side, look...). */
	virtual void CopyCutSettingsTo(ARMBuildableSpline* New) const {}

	/** Cut: the new pieces exist, this one is about to be dismantled. */
	virtual void BeforeCutDismantle(float Dist) {}

	UFUNCTION()
	void OnRep_SplinePoints();

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_SplinePoints)
	TArray<FRMSplinePoint> mSplinePoints;

	/** Build animation: the piece appears along its curve, from mRevealFrom; about 30 m/s, 1 to 4 s. */
	virtual bool UsesReveal() const override { return mSplinePoints.Num() >= 2; }
	virtual void PrepareReveal() override;
	virtual float RevealDistance(const FVector& LocalPos) const override;
	virtual float GetBuildAnimDuration() const override;

	/**
	 * Long pieces (over 125 m, Long Curve roads) are drawn in equal sections of at most 100 m (see
	 * ARMBuildableProcedural chunks). UpdateChunkLayout() at the start of RebuildMesh.
	 */
	virtual int32 GetChunkCount() const override { return FMath::Max(1, mChunkCount); }
	virtual int32 GetChunkOf(const FVector& LocalPos) const override;
	void UpdateChunkLayout();
	int32 mChunkCount = 1;
	float mChunkLength = 0.f;
	TArray<FRMFrame> mChunkFrames; // actor space, every 10 m: which chunk a point belongs to

public:
	/** Sections of the piece along its length (cm, from its start): what the mesh is drawn in, and what
	 *  snap-key lane paths and tracks are laid in on long roads. One section for normal pieces. */
	int32 GetSectionCount() const;
	void GetSectionRange(int32 Section, float& OutFrom, float& OutTo) const;

	/** Longest piece drawn as one section (cm); longer ones use sections of at most SectionLength. */
	static constexpr float SingleSectionMax = 12500.f;
	static constexpr float SectionLength = 10000.f;

protected:
	UPROPERTY(Replicated)
	ERMRevealFrom mRevealFrom = ERMRevealFrom::Start;
	UPROPERTY(Replicated)
	FVector mRevealPoint = FVector::ZeroVector;

	TArray<FRMFrame> mRevealFrames;
	bool mRevealFromEnd = false;
	float mRevealPointAlong = -1.f; // FromPoint: distance along the piece of mRevealPoint

	/** Distance between mesh rings in cm. Lower = smoother curves, more triangles. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mMeshStep = 100.f;
};
