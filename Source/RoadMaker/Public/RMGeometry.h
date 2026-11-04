// Pure geometry helpers for Road Maker. No game classes in here, only math and mesh building,
// so everything that ends up in the world is regenerated from small saved inputs.
#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "RMGeometry.generated.h"

namespace RM
{
	/** Satisfactory foundation edge length in cm. */
	constexpr float FoundationSize = 800.f;
}

/** One control point of a cubic Hermite spline. Saved with the buildable. */
USTRUCT(BlueprintType)
struct ROADMAKER_API FRMSplinePoint
{
	GENERATED_BODY()

	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector Location = FVector::ZeroVector;

	/** Hermite tangent (direction * magnitude), shared as arrive and leave tangent. */
	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector Tangent = FVector::ForwardVector;

	FRMSplinePoint() = default;
	FRMSplinePoint(const FVector& InLocation, const FVector& InTangent) : Location(InLocation), Tangent(InTangent) {}
};

/** A road end that feeds into an intersection. Saved with the intersection buildable. */
USTRUCT(BlueprintType)
struct ROADMAKER_API FRMRoadEnd
{
	GENERATED_BODY()

	/** Top-centre point of the road end. */
	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector Center = FVector::ZeroVector;

	/** Horizontal unit vector pointing out of the road, into the intersection. */
	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	FVector Inward = FVector::ForwardVector;

	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	float Width = RM::FoundationSize;
};

/** Lane markings of a road piece. Saved with the road; drawn by the asphalt material. */
USTRUCT(BlueprintType)
struct ROADMAKER_API FRMMarkings
{
	GENERATED_BODY()

	/** 1, 2 or 3 lanes (0 = no markings at all, used for intersections). */
	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	uint8 Lanes = 2;

	/** Solid lines along both sides. */
	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	bool bEdgeLines = true;

	/** Lane dividers drawn solid instead of dashed. */
	UPROPERTY(SaveGame, EditAnywhere, BlueprintReadWrite, Category = "Road")
	bool bContinuous = false;

	bool operator==(const FRMMarkings& O) const { return Lanes == O.Lanes && bEdgeLines == O.bEdgeLines && bContinuous == O.bContinuous; }
};

/** A sampled point along a spline with a level (no banking) orthonormal frame. */
struct ROADMAKER_API FRMFrame
{
	FVector Pos = FVector::ZeroVector;
	FVector Fwd = FVector::ForwardVector;
	FVector Right = FVector::RightVector;
	FVector Up = FVector::UpVector;
	float Dist = 0.f;

	/** Profile coordinates: X = lateral (positive = right), Y = vertical (positive = up). */
	FVector ProfileToWorld(const FVector2D& P) const { return Pos + Right * P.X + Up * P.Y; }
};

/**
 * Where an 8 m x 4 m wall panel sits in a game wall texture (for the game's own wall materials):
 * u at the panel centre, texture units per metre, v at the top of the wall. Mirrored: the texture
 * holds half a panel (centre to edge) and is folded at the panel centre.
 */
struct ROADMAKER_API FRMWallTexLayout
{
	float UCentre = 0.f;
	float UPerMetre = 0.214f;
	float VTop = 0.021f;
	float VPerMetre = 0.21675f;
	bool bMirror = true;
};

/** Mesh buffers in the layout UProceduralMeshComponent wants. */
struct ROADMAKER_API FRMMesh
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;

	/**
	 * Extra UV channels, only filled for road tops (lane markings): UV1 = metres across / along,
	 * UV2 = (road width m, side line inset m), UV3 = (lanes, side lines 0/1). Missing channels are
	 * sent as zeros, which the asphalt material reads as "no markings".
	 */
	TArray<FVector2D> MarkUV1;
	TArray<FVector2D> MarkUV2;
	TArray<FVector2D> MarkUV3;

	void Reset();
	bool IsEmpty() const { return Triangles.Num() == 0; }

	int32 AddVertex(const FVector& P, const FVector& N, const FVector2D& UV, const FVector& TangentX);

	/** Adds a triangle and fixes its winding so the front face points along DesiredNormal. */
	void AddTriangle(int32 A, int32 B, int32 C, const FVector& DesiredNormal);

	/** Adds a flat quad P0..P3 (in order around the edge) with hard normal N. */
	void AddQuad(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3,
		const FVector& N, const FVector2D& UV0, const FVector2D& UV1, const FVector2D& UV2, const FVector2D& UV3);

	/** Adds an oriented box centred on Center with half extents along the given axes. */
	void AddBox(const FVector& Center, const FVector& AxisX, const FVector& AxisY, const FVector& AxisZ,
		const FVector& HalfExtents, float UVScale);

	/** Writes this buffer into a section of a procedural mesh component. */
	void ApplyToSection(class UProceduralMeshComponent* Comp, int32 Section, bool bCollision) const;
};

/** Results of checking a spline against placement rules. */
struct ROADMAKER_API FRMSplineStats
{
	float Length = 0.f;
	float MinTurnRadius = TNumericLimits<float>::Max();
	float MaxGradeDegrees = 0.f;
};

namespace RMGeometry
{
	/** Samples a Hermite spline into frames roughly every Step cm. */
	ROADMAKER_API void SampleSpline(const TArray<FRMSplinePoint>& Points, float Step, TArray<FRMFrame>& OutFrames);

	/** Length, tightest horizontal turn and steepest grade along sampled frames. */
	ROADMAKER_API FRMSplineStats Analyze(const TArray<FRMFrame>& Frames);

	/**
	 * Builds a single-arc spline from Start (with a fixed start direction) to End, the way rail and
	 * vehicle path tools do: the end direction is the start direction mirrored across the chord, which
	 * gives a circular arc in plan view. Height changes linearly from start to end.
	 */
	ROADMAKER_API void MakeArc(const FVector& Start, const FVector& StartDir, const FVector& End, TArray<FRMSplinePoint>& OutPoints);

	/** Spline with both end directions fixed (both ends snapped to something). */
	ROADMAKER_API void MakeFixedEnds(const FVector& Start, const FVector& StartDir, const FVector& End, const FVector& EndDir, TArray<FRMSplinePoint>& OutPoints);

	/**
	 * Offset curve of a road edge: Q(s) = P(s) + Offset * Right(s), with Right the level normal of the
	 * centreline. The exact offset of a cubic is not a cubic, so it is resampled every KeyEvery cm and
	 * refitted with Catmull-Rom tangents.
	 */
	ROADMAKER_API void OffsetCurve(const TArray<FRMFrame>& Frames, float LateralOffset, float KeyEvery, TArray<FRMSplinePoint>& OutPoints);

	/** Sweeps a 2D profile (X lateral, Y up) along frames. Normals face away from ProfileCentroid. */
	ROADMAKER_API void ExtrudeProfile(const TArray<FRMFrame>& Frames, const TArray<FVector2D>& Profile, bool bClosed,
		const FVector2D& ProfileCentroid, float UVScale, FRMMesh& Out);

	/** Fan-triangulated cap of a convex profile at a frame. Normal is +Fwd or -Fwd. */
	ROADMAKER_API void AddProfileCap(const FRMFrame& Frame, const TArray<FVector2D>& Profile, bool bFacingForward, float UVScale, FRMMesh& Out);

	/**
	 * Road slab: asphalt top into Top, concrete sides, bottom and end caps into Body. Frames are top-centre.
	 * Markings and EdgeLineInset (cm from the road edge, leaves room for fences) go into the top's UV1-3.
	 */
	ROADMAKER_API void BuildRoad(const TArray<FRMFrame>& Frames, float Width, float Height, float UVScale,
		const FRMMarkings& Markings, float EdgeLineInset, FRMMesh& Top, FRMMesh& Body);

	// Fence builders all use one convention: the road is on the +Right side of the frames.
	// ARMBuildableFence::BuildFenceMesh mirrors the frames for fences whose road is on the other side.

	/** Mirror frames sideways (Right = -Right), so a fence built for +Right faces -Right instead. */
	ROADMAKER_API void MirrorFrames(TArray<FRMFrame>& Frames);

	/** Smooth spline through world points (Catmull-Rom tangents). */
	ROADMAKER_API void SplineThroughPoints(const TArray<FVector>& Points, TArray<FRMSplinePoint>& OutPoints);

	/** Steel W-beam guard rail with posts. Frames are at the base of the fence. */
	ROADMAKER_API void BuildGuardRail(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Metal);

	/** Continuous concrete jersey barrier. */
	ROADMAKER_API void BuildJerseyBarrier(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Concrete);

	/** Posts plus a mesh panel (panel uses a masked grid material). */
	ROADMAKER_API void BuildGridFence(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Posts, FRMMesh& Panel, float Scale = 1.f);

	/**
	 * Concrete fence wall: a solid upright slab Height tall and Thickness thick, standing on the
	 * frames' line and reaching towards +Right (the road side), so its outer face is on the line.
	 * Stays upright on slopes like the grid fence; the top follows the ground line.
	 */
	ROADMAKER_API void BuildFenceWall(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Out, float Height = 400.f, float Thickness = 50.f,
		const FRMWallTexLayout& Layout = FRMWallTexLayout());

	/**
	 * Builds the slab that joins 2..N road ends. Ends are ordered by angle around their centroid, the
	 * corners of neighbouring ends are joined by curb fillets (quadratic Bezier whose control point is
	 * where the two road edges meet), and the outline is fan-triangulated from the centroid.
	 * Returns false if the outline is not star-shaped around the centroid (roads too irregular).
	 * OutOutline receives the top outline, useful for fences or debugging.
	 */
	ROADMAKER_API bool BuildIntersection(const TArray<FRMRoadEnd>& Ends, float Height, float UVScale, int32 FilletSegments,
		FRMMesh& Top, FRMMesh& Body, TArray<FVector>* OutOutline = nullptr, TArray<bool>* OutEdgeIsMouth = nullptr);

	/**
	 * The curved curbs of an intersection: each is the outline run between two neighbouring road
	 * mouths. Points run counter-clockwise around the centre, so the centre is on their +Right side.
	 */
	ROADMAKER_API void GetIntersectionCurbs(const TArray<FVector>& Outline, const TArray<bool>& EdgeIsMouth, TArray<TArray<FVector>>& OutCurbs);

	/**
	 * Side lines of an intersection: a thin strip just above the slab along every curb, Inset cm in
	 * from it, so it continues the side lines of the roads. The strip carries the same marking UVs
	 * as a road top (one side line, no lanes), so the asphalt material draws it in the secondary
	 * colour and shows plain asphalt around the line. Heights are taken from Surface (the slab top),
	 * so the line lies on the slab even where the roads joined are at different heights.
	 */
	ROADMAKER_API void BuildCurbLines(const TArray<TArray<FVector>>& Curbs, float Inset, const FRMMesh& Surface, FRMMesh& Out);
}
