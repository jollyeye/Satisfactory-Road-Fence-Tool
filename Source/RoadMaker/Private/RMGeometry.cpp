#include "RMGeometry.h"
#include "ProceduralMeshComponent.h"

// If every procedural face shows up inside-out in game, flip this one switch.
// Convention used: a triangle (A,B,C) faces along Cross(B-A, C-A), matching the classic
// UProceduralMeshComponent example (verts (0,0,0),(0,100,0),(0,0,100) facing +X).
static constexpr bool GRMFlipWinding = true; // verified in game: UE front faces are the other winding

// ---------------------------------------------------------------------------------------------
// FRMMesh
// ---------------------------------------------------------------------------------------------

void FRMMesh::Reset()
{
	Vertices.Reset();
	Triangles.Reset();
	Normals.Reset();
	UVs.Reset();
	Tangents.Reset();
	MarkUV1.Reset();
	MarkUV2.Reset();
	MarkUV3.Reset();
}

int32 FRMMesh::AddVertex(const FVector& P, const FVector& N, const FVector2D& UV, const FVector& TangentX)
{
	const int32 Index = Vertices.Add(P);
	Normals.Add(N);
	UVs.Add(UV);
	Tangents.Add(FProcMeshTangent(TangentX, false));
	return Index;
}

void FRMMesh::AddTriangle(int32 A, int32 B, int32 C, const FVector& DesiredNormal)
{
	const FVector& PA = Vertices[A];
	const FVector& PB = Vertices[B];
	const FVector& PC = Vertices[C];
	const FVector Geometric = FVector::CrossProduct(PB - PA, PC - PA);
	bool bSwap = FVector::DotProduct(Geometric, DesiredNormal) < 0.f;
	if (GRMFlipWinding)
	{
		bSwap = !bSwap;
	}
	Triangles.Add(A);
	Triangles.Add(bSwap ? C : B);
	Triangles.Add(bSwap ? B : C);
}

void FRMMesh::AddQuad(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3,
	const FVector& N, const FVector2D& UV0, const FVector2D& UV1, const FVector2D& UV2, const FVector2D& UV3)
{
	const FVector TangentX = (P1 - P0).GetSafeNormal();
	const int32 I0 = AddVertex(P0, N, UV0, TangentX);
	const int32 I1 = AddVertex(P1, N, UV1, TangentX);
	const int32 I2 = AddVertex(P2, N, UV2, TangentX);
	const int32 I3 = AddVertex(P3, N, UV3, TangentX);
	AddTriangle(I0, I1, I2, N);
	AddTriangle(I0, I2, I3, N);
}

void FRMMesh::AddBox(const FVector& Center, const FVector& AxisX, const FVector& AxisY, const FVector& AxisZ,
	const FVector& HalfExtents, float UVScale)
{
	const FVector X = AxisX * HalfExtents.X;
	const FVector Y = AxisY * HalfExtents.Y;
	const FVector Z = AxisZ * HalfExtents.Z;
	const float S = 1.f / FMath::Max(UVScale, 1.f);

	auto Face = [&](const FVector& FaceCenter, const FVector& U, const FVector& V, const FVector& N)
	{
		const float LU = U.Size() * 2.f * S;
		const float LV = V.Size() * 2.f * S;
		AddQuad(FaceCenter - U - V, FaceCenter + U - V, FaceCenter + U + V, FaceCenter - U + V, N,
			FVector2D(0, 0), FVector2D(LU, 0), FVector2D(LU, LV), FVector2D(0, LV));
	};

	Face(Center + X, Y, Z, AxisX);
	Face(Center - X, Y, Z, -AxisX);
	Face(Center + Y, X, Z, AxisY);
	Face(Center - Y, X, Z, -AxisY);
	Face(Center + Z, X, Y, AxisZ);
	Face(Center - Z, X, Y, -AxisZ);
}

void FRMMesh::ApplyToSection(UProceduralMeshComponent* Comp, int32 Section, bool bCollision, const TArray<FLinearColor>* Colors) const
{
	if (!Comp)
	{
		return;
	}
	if (IsEmpty())
	{
		Comp->ClearMeshSection(Section);
		return;
	}
	static const TArray<FLinearColor> NoColors;
	auto Channel = [this](const TArray<FVector2D>& In)
	{
		if (In.Num() == Vertices.Num())
		{
			return In;
		}
		TArray<FVector2D> Zeros;
		Zeros.Init(FVector2D::ZeroVector, Vertices.Num());
		return Zeros;
	};
	Comp->CreateMeshSection_LinearColor(Section, Vertices, Triangles, Normals, UVs, Channel(MarkUV1), Channel(MarkUV2), Channel(MarkUV3),
		Colors && Colors->Num() == Vertices.Num() ? *Colors : NoColors, Tangents, bCollision);
}

// ---------------------------------------------------------------------------------------------
// Spline sampling and analysis
// ---------------------------------------------------------------------------------------------

namespace
{
	FVector LevelRight(const FVector& Fwd, const FVector& Fallback)
	{
		FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd);
		if (!Right.Normalize(1e-4f))
		{
			return Fallback;
		}
		return Right;
	}

	float Cross2D(const FVector& A, const FVector& B)
	{
		return A.X * B.Y - A.Y * B.X;
	}

	/** True if segments AB and CD cross (touching at shared end points does not count). */
	bool SegmentsCross2D(const FVector& A, const FVector& B, const FVector& C, const FVector& D)
	{
		const float D1 = Cross2D(B - A, C - A);
		const float D2 = Cross2D(B - A, D - A);
		const float D3 = Cross2D(D - C, A - C);
		const float D4 = Cross2D(D - C, B - C);
		return ((D1 > 1.f && D2 < -1.f) || (D1 < -1.f && D2 > 1.f)) && ((D3 > 1.f && D4 < -1.f) || (D3 < -1.f && D4 > 1.f));
	}

	/**
	 * Triangulates a simple polygon (seen from above) by ear clipping, so concave outlines work too
	 * (e.g. two roads at 90 degrees joined by a curved, ring-shaped piece). Returns false if the
	 * outline crosses itself.
	 */
	bool TriangulatePolygon2D(const TArray<FVector>& P, TArray<int32>& OutTris)
	{
		OutTris.Reset();
		const int32 N = P.Num();
		if (N < 3)
		{
			return false;
		}
		for (int32 I = 0; I < N; ++I)
		{
			for (int32 J = I + 2; J < N; ++J)
			{
				if (I == 0 && J == N - 1)
				{
					continue; // neighbours through the wrap
				}
				if (SegmentsCross2D(P[I], P[(I + 1) % N], P[J], P[(J + 1) % N]))
				{
					return false;
				}
			}
		}
		float Area = 0.f;
		for (int32 I = 0; I < N; ++I)
		{
			Area += Cross2D(P[I], P[(I + 1) % N]);
		}
		TArray<int32> V;
		for (int32 I = 0; I < N; ++I)
		{
			V.Add(Area >= 0.f ? I : N - 1 - I); // counter-clockwise
		}
		int32 Guard = N * N + 10;
		while (V.Num() > 3 && Guard-- > 0)
		{
			bool bClipped = false;
			for (int32 K = 0; K < V.Num(); ++K)
			{
				const int32 IA = V[(K + V.Num() - 1) % V.Num()];
				const int32 IB = V[K];
				const int32 IC = V[(K + 1) % V.Num()];
				const FVector& A = P[IA];
				const FVector& B = P[IB];
				const FVector& C = P[IC];
				const float Turn = Cross2D(B - A, C - B);
				if (FMath::Abs(Turn) <= 1e-4f * FVector::Dist2D(A, B) * FVector::Dist2D(B, C) && FVector::DotProduct(B - A, C - B) >= 0.f)
				{
					V.RemoveAt(K); // straight through: no triangle needed
					bClipped = true;
					break;
				}
				if (Turn <= 0.f)
				{
					continue; // reflex corner
				}
				bool bEmpty = true;
				for (const int32 IO : V)
				{
					if (IO == IA || IO == IB || IO == IC)
					{
						continue;
					}
					const FVector& O = P[IO];
					if (Cross2D(B - A, O - A) >= 0.f && Cross2D(C - B, O - B) >= 0.f && Cross2D(A - C, O - C) >= 0.f)
					{
						bEmpty = false;
						break;
					}
				}
				if (bEmpty)
				{
					OutTris.Add(IA);
					OutTris.Add(IB);
					OutTris.Add(IC);
					V.RemoveAt(K);
					bClipped = true;
					break;
				}
			}
			if (!bClipped)
			{
				return false;
			}
		}
		if (V.Num() == 3)
		{
			OutTris.Add(V[0]);
			OutTris.Add(V[1]);
			OutTris.Add(V[2]);
		}
		return OutTris.Num() >= 3;
	}

	// --- Intersection top surface: points inside the outline, smooth heights ("soap film") -------

	bool IsectInside2D(const TArray<FVector>& Ring, const FVector& P)
	{
		bool bIn = false;
		for (int32 I = 0, J = Ring.Num() - 1; I < Ring.Num(); J = I++)
		{
			const FVector& A = Ring[I];
			const FVector& B = Ring[J];
			if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X)
			{
				bIn = !bIn;
			}
		}
		return bIn;
	}

	float IsectDistToRing2D(const TArray<FVector>& Ring, const FVector& P)
	{
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; I < Ring.Num(); ++I)
		{
			const FVector2D A(Ring[I]), B(Ring[(I + 1) % Ring.Num()]), Q(P);
			const FVector2D AB = B - A;
			const float T = FMath::Clamp(FVector2D::DotProduct(Q - A, AB) / FMath::Max(AB.SizeSquared(), 1.f), 0.f, 1.f);
			Best = FMath::Min(Best, FVector2D::Distance(Q, A + AB * T));
		}
		return Best;
	}

	/** Delaunay triangulation (Bowyer-Watson) of points seen from above; a few hundred points at most. */
	void IsectDelaunay2D(const TArray<FVector>& P, TArray<int32>& OutTris)
	{
		OutTris.Reset();
		const int32 N = P.Num();
		// Doubles, around the middle of the piece: world coordinates are large for float circumcircles.
		FVector2d Mid(0.0, 0.0);
		for (const FVector& V : P)
		{
			Mid += FVector2d(V.X, V.Y) / N;
		}
		double Extent = 100.0;
		TArray<FVector2d> Pts;
		for (const FVector& V : P)
		{
			Pts.Add(FVector2d(V.X, V.Y) - Mid);
			Extent = FMath::Max(Extent, Pts.Last().GetAbsMax());
		}
		const double Span = Extent * 20.0;
		Pts.Add(FVector2d(-Span, -Span));
		Pts.Add(FVector2d(Span, -Span));
		Pts.Add(FVector2d(0.0, Span));
		struct FTri { int32 A, B, C; FVector2d Center; double R2; };
		auto Make = [&Pts](int32 A, int32 B, int32 C) -> FTri
		{
			const FVector2d& a = Pts[A]; const FVector2d& b = Pts[B]; const FVector2d& c = Pts[C];
			const double D = 2.0 * (a.X * (b.Y - c.Y) + b.X * (c.Y - a.Y) + c.X * (a.Y - b.Y));
			FTri T{ A, B, C, FVector2d(0.0, 0.0), TNumericLimits<double>::Max() };
			if (FMath::Abs(D) > 1e-9)
			{
				const double A2 = a.SizeSquared(), B2 = b.SizeSquared(), C2 = c.SizeSquared();
				T.Center = FVector2d((A2 * (b.Y - c.Y) + B2 * (c.Y - a.Y) + C2 * (a.Y - b.Y)) / D,
					(A2 * (c.X - b.X) + B2 * (a.X - c.X) + C2 * (b.X - a.X)) / D);
				T.R2 = FVector2d::DistSquared(T.Center, a);
			}
			return T;
		};
		TArray<FTri> Tris;
		Tris.Add(Make(N, N + 1, N + 2));
		for (int32 I = 0; I < N; ++I)
		{
			TArray<TPair<int32, int32>> Edges;
			for (int32 T = Tris.Num() - 1; T >= 0; --T)
			{
				if (FVector2d::DistSquared(Tris[T].Center, Pts[I]) < Tris[T].R2)
				{
					Edges.Add({ Tris[T].A, Tris[T].B });
					Edges.Add({ Tris[T].B, Tris[T].C });
					Edges.Add({ Tris[T].C, Tris[T].A });
					Tris.RemoveAtSwap(T);
				}
			}
			for (int32 E = 0; E < Edges.Num(); ++E)
			{
				bool bShared = false;
				for (int32 F = 0; F < Edges.Num(); ++F)
				{
					if (E != F && ((Edges[E].Key == Edges[F].Value && Edges[E].Value == Edges[F].Key)
						|| (Edges[E].Key == Edges[F].Key && Edges[E].Value == Edges[F].Value)))
					{
						bShared = true;
						break;
					}
				}
				if (!bShared)
				{
					Tris.Add(Make(Edges[E].Key, Edges[E].Value, I));
				}
			}
		}
		for (const FTri& T : Tris)
		{
			if (T.A < N && T.B < N && T.C < N)
			{
				OutTris.Add(T.A);
				OutTris.Add(T.B);
				OutTris.Add(T.C);
			}
		}
	}

	/**
	 * The intersection's top: the outline (Ring) plus points every GridSpacing inside it, triangulated, and
	 * the inside points' heights smoothed between the outline's (each the average of its neighbours, the
	 * smoothest surface that meets every road end). False if the triangulation does not fill the outline
	 * exactly (then the caller keeps the plain outline triangulation).
	 */
	bool IsectSmoothTop(const TArray<FVector>& Ring, float GridSpacing, TArray<FVector>& OutPoints, TArray<int32>& OutTris)
	{
		const int32 N = Ring.Num();
		OutPoints = Ring;
		FBox2D Box(ForceInit);
		for (const FVector& V : Ring)
		{
			Box += FVector2D(V);
		}
		float MeanZ = 0.f;
		for (const FVector& V : Ring)
		{
			MeanZ += V.Z / N;
		}
		const float G = FMath::Max(GridSpacing, 50.f);
		for (float X = Box.Min.X + G * 0.5f; X < Box.Max.X; X += G)
		{
			for (float Y = Box.Min.Y + G * 0.5f; Y < Box.Max.Y; Y += G)
			{
				const FVector P(X, Y, MeanZ);
				if (IsectInside2D(Ring, P) && IsectDistToRing2D(Ring, P) > G * 0.6f)
				{
					OutPoints.Add(P);
				}
				if (OutPoints.Num() > 600)
				{
					return false; // a huge piece: keep it simple
				}
			}
		}
		if (OutPoints.Num() == N)
		{
			return false; // nothing inside (small piece): the outline is enough
		}
		TArray<int32> All;
		IsectDelaunay2D(OutPoints, All);
		// Keep the triangles inside the outline; they must cover it exactly.
		float RingArea = 0.f;
		for (int32 K = 0; K < N; ++K)
		{
			RingArea += Cross2D(Ring[K], Ring[(K + 1) % N]) * 0.5f;
		}
		const float WantSign = RingArea >= 0.f ? 1.f : -1.f;
		float Covered = 0.f;
		OutTris.Reset();
		for (int32 T = 0; T + 2 < All.Num(); T += 3)
		{
			const FVector& A = OutPoints[All[T]];
			const FVector& B = OutPoints[All[T + 1]];
			const FVector& C = OutPoints[All[T + 2]];
			// Inside the outline: its middle, and each side's middle (nudged a hair toward the middle, since
			// sides along the outline lie exactly on it).
			const FVector Centroid = (A + B + C) / 3.f;
			auto SideInside = [&Ring, &Centroid](const FVector& U, const FVector& V)
			{
				const FVector M = (U + V) * 0.5f;
				return IsectInside2D(Ring, M + (Centroid - M) * 0.02f);
			};
			if (!IsectInside2D(Ring, Centroid) || !SideInside(A, B) || !SideInside(B, C) || !SideInside(C, A))
			{
				continue;
			}
			const float Area = Cross2D(B - A, C - A) * 0.5f;
			Covered += FMath::Abs(Area);
			if (Area * WantSign >= 0.f)
			{
				OutTris.Append({ All[T], All[T + 1], All[T + 2] });
			}
			else
			{
				OutTris.Append({ All[T], All[T + 2], All[T + 1] });
			}
		}
		if (FMath::Abs(Covered - FMath::Abs(RingArea)) > FMath::Abs(RingArea) * 0.002f + 100.f)
		{
			return false; // gaps or overhangs: not safe
		}
		// Every outline side must be a triangle side, so the surface meets the curbs exactly.
		TSet<uint64> Sides;
		for (int32 T = 0; T + 2 < OutTris.Num(); T += 3)
		{
			for (int32 E = 0; E < 3; ++E)
			{
				const uint32 U = OutTris[T + E], V = OutTris[T + (E + 1) % 3];
				Sides.Add((uint64(FMath::Min(U, V)) << 32) | FMath::Max(U, V));
			}
		}
		for (int32 K = 0; K < N; ++K)
		{
			const uint32 U = K, V = (K + 1) % N;
			if (!Sides.Contains((uint64(FMath::Min(U, V)) << 32) | FMath::Max(U, V)))
			{
				return false;
			}
		}
		// Heights: the outline stays; the inside settles to the average of its neighbours.
		TArray<TSet<int32>> Near;
		Near.SetNum(OutPoints.Num());
		for (int32 T = 0; T + 2 < OutTris.Num(); T += 3)
		{
			for (int32 E = 0; E < 3; ++E)
			{
				const int32 I = OutTris[T + E];
				Near[I].Add(OutTris[T + (E + 1) % 3]);
				Near[I].Add(OutTris[T + (E + 2) % 3]);
			}
		}
		for (int32 Pass = 0; Pass < 300; ++Pass)
		{
			for (int32 I = N; I < OutPoints.Num(); ++I)
			{
				if (Near[I].Num() == 0)
				{
					continue;
				}
				float Sum = 0.f;
				for (int32 J : Near[I])
				{
					Sum += OutPoints[J].Z;
				}
				OutPoints[I].Z = Sum / Near[I].Num();
			}
		}
		return true;
	}
}

void RMGeometry::SampleSpline(const TArray<FRMSplinePoint>& Points, float Step, TArray<FRMFrame>& OutFrames)
{
	OutFrames.Reset();
	if (Points.Num() < 2)
	{
		return;
	}
	Step = FMath::Max(Step, 10.f);

	FVector PrevPos = Points[0].Location;
	FVector PrevRight = LevelRight(Points[0].Tangent.GetSafeNormal(), FVector::RightVector);
	float Dist = 0.f;

	for (int32 Seg = 0; Seg + 1 < Points.Num(); ++Seg)
	{
		const FRMSplinePoint& A = Points[Seg];
		const FRMSplinePoint& B = Points[Seg + 1];

		// Rough length (control polygon) decides the sample count, curved parts get extra samples.
		const float Rough = FVector::Dist(A.Location, B.Location) + 0.25f * (A.Tangent.Size() + B.Tangent.Size()) / 3.f;
		const int32 N = FMath::Clamp(FMath::CeilToInt(Rough / Step), 2, 512);

		for (int32 K = (Seg == 0 ? 0 : 1); K <= N; ++K)
		{
			const float T = static_cast<float>(K) / N;
			FRMFrame F;
			F.Pos = FMath::CubicInterp(A.Location, A.Tangent, B.Location, B.Tangent, T);
			FVector Deriv = FMath::CubicInterpDerivative(A.Location, A.Tangent, B.Location, B.Tangent, T);
			if (!Deriv.Normalize(1e-4f))
			{
				Deriv = OutFrames.Num() > 0 ? OutFrames.Last().Fwd : (B.Location - A.Location).GetSafeNormal();
			}
			F.Fwd = Deriv;
			F.Right = LevelRight(F.Fwd, PrevRight);
			F.Up = FVector::CrossProduct(F.Fwd, F.Right).GetSafeNormal();
			if (F.Up.Z < 0.f)
			{
				F.Up = -F.Up;
			}
			Dist += FVector::Dist(PrevPos, F.Pos);
			F.Dist = Dist;
			PrevPos = F.Pos;
			PrevRight = F.Right;
			OutFrames.Add(F);
		}
	}
}

FRMSplineStats RMGeometry::Analyze(const TArray<FRMFrame>& Frames)
{
	FRMSplineStats Stats;
	if (Frames.Num() < 2)
	{
		return Stats;
	}
	Stats.Length = Frames.Last().Dist;

	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		const FRMFrame& F = Frames[I];
		const float Horizontal = FVector2D(F.Fwd.X, F.Fwd.Y).Size();
		const float Grade = FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(F.Fwd.Z), Horizontal));
		Stats.MaxGradeDegrees = FMath::Max(Stats.MaxGradeDegrees, Grade);

		if (I > 0)
		{
			const FRMFrame& P = Frames[I - 1];
			const FVector A = FVector(P.Fwd.X, P.Fwd.Y, 0.f).GetSafeNormal();
			const FVector B = FVector(F.Fwd.X, F.Fwd.Y, 0.f).GetSafeNormal();
			const float Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(A, B), -1.f, 1.f));
			const float Ds = FVector2D(F.Pos.X - P.Pos.X, F.Pos.Y - P.Pos.Y).Size();
			if (Angle > 1e-4f && Ds > 1e-2f)
			{
				Stats.MinTurnRadius = FMath::Min(Stats.MinTurnRadius, Ds / Angle);
			}
		}
	}
	return Stats;
}

// ---------------------------------------------------------------------------------------------
// Spline construction
// ---------------------------------------------------------------------------------------------

void RMGeometry::MakeArc(const FVector& Start, const FVector& StartDir, const FVector& End, TArray<FRMSplinePoint>& OutPoints)
{
	OutPoints.Reset();
	const FVector Chord = End - Start;
	const FVector Chord2D(Chord.X, Chord.Y, 0.f);
	const float Horizontal = Chord2D.Size();
	const float ChordLength = Chord.Size();

	if (Horizontal < 1.f)
	{
		OutPoints.Add(FRMSplinePoint(Start, Chord));
		OutPoints.Add(FRMSplinePoint(End, Chord));
		return;
	}

	const FVector C = Chord2D / Horizontal;
	FVector S = FVector(StartDir.X, StartDir.Y, 0.f).GetSafeNormal();
	if (S.IsNearlyZero())
	{
		S = C;
	}

	// Mirror the start direction across the chord: the end direction of a circular arc.
	const float CosHalf = FMath::Clamp(FVector::DotProduct(S, C), -1.f, 1.f);
	const FVector E = (2.f * CosHalf * C - S).GetSafeNormal();
	const float HalfTheta = FMath::Acos(CosHalf);
	const float Theta = 2.f * HalfTheta;

	// Hermite tangent length that best approximates a circular arc: 4 R tan(theta / 4).
	float Magnitude = Horizontal;
	if (Theta > 1e-3f)
	{
		const float Radius = Horizontal / (2.f * FMath::Sin(HalfTheta));
		Magnitude = 4.f * Radius * FMath::Tan(Theta * 0.25f);
	}
	// Past ~180 degrees a single arc loops back on itself; clamp so the curve stays sane.
	Magnitude = FMath::Clamp(Magnitude, Horizontal * 0.5f, ChordLength * 2.5f);

	const FVector StartTangent = S * Magnitude + FVector(0.f, 0.f, Chord.Z);
	const FVector EndTangent = E * Magnitude + FVector(0.f, 0.f, Chord.Z);
	OutPoints.Add(FRMSplinePoint(Start, StartTangent));
	OutPoints.Add(FRMSplinePoint(End, EndTangent));
}

void RMGeometry::MakeFixedEnds(const FVector& Start, const FVector& StartDir, const FVector& End, const FVector& EndDir, TArray<FRMSplinePoint>& OutPoints)
{
	OutPoints.Reset();
	const FVector Chord = End - Start;
	const float Length = FMath::Max(FVector(Chord.X, Chord.Y, 0.f).Size(), 1.f);
	const FVector S = FVector(StartDir.X, StartDir.Y, 0.f).GetSafeNormal();
	const FVector E = FVector(EndDir.X, EndDir.Y, 0.f).GetSafeNormal();

	// Tangent length of a circular arc turning by Theta over this chord: chord / cos^2(Theta / 4).
	// (Equals 4 R tan(Theta / 4).) A 90 degree turn between two fixed ends becomes a true quarter
	// circle instead of a flattened curve. Capped so near-U-turns stay sane.
	const float Theta = FMath::Acos(FMath::Clamp(FVector::DotProduct(S, E), -1.f, 1.f));
	const float CosQuarter = FMath::Cos(FMath::Min(Theta, FMath::DegreesToRadians(170.f)) * 0.25f);
	const float Magnitude = Length / FMath::Max(CosQuarter * CosQuarter, 0.25f);
	OutPoints.Add(FRMSplinePoint(Start, S * Magnitude + FVector(0.f, 0.f, Chord.Z)));
	OutPoints.Add(FRMSplinePoint(End, E * Magnitude + FVector(0.f, 0.f, Chord.Z)));
}

void RMGeometry::MakeQuadratic(const FVector& Start, const FVector& StartDir, const FVector& End, const FVector& EndDir, TArray<FRMSplinePoint>& OutPoints)
{
	OutPoints.Reset();
	const FVector2D P0(Start.X, Start.Y);
	const FVector2D P1(End.X, End.Y);
	const FVector2D D = P1 - P0;
	const float Dist = D.Size();
	const float DZ = End.Z - Start.Z;

	// One quadratic P0 -> C -> P2 as a cubic Hermite piece; height linear along t.
	auto AddQuadratic = [&OutPoints](const FVector& A, const FVector& C, const FVector& B, bool bFirst)
	{
		if (bFirst)
		{
			OutPoints.Add(FRMSplinePoint(A, 2.f * (C - A)));
		}
		OutPoints.Add(FRMSplinePoint(B, 2.f * (B - C)));
	};

	if (Dist < 1.f)
	{
		AddQuadratic(Start, (Start + End) * 0.5f, End, true);
		return;
	}

	FVector2D U0 = FVector2D(StartDir.X, StartDir.Y).GetSafeNormal();
	FVector2D U1 = FVector2D(EndDir.X, EndDir.Y).GetSafeNormal();
	if (U0.IsNearlyZero())
	{
		U0 = D / Dist;
	}
	if (U1.IsNearlyZero())
	{
		U1 = D / Dist;
	}

	auto DiffAngle = [](float A, float B)
	{
		return FMath::UnwindRadians(A - B);
	};
	const float ChordAngle = FMath::Atan2(D.Y, D.X);
	const float Angle0 = FMath::Atan2(U0.Y, U0.X);
	const float Angle1 = FMath::Atan2(U1.Y, U1.X);
	const float Diff0 = DiffAngle(Angle0, ChordAngle);
	const float Diff1 = DiffAngle(Angle1, ChordAngle);

	// S-curve: both directions on the same side of the chord need an inflection, which a single
	// quadratic cannot make.
	const bool bInflection = FMath::Sin(Diff0) * FMath::Sin(Diff1) > 0.04f;
	// Wide turns / U-turns: the direction lines meet far away (or behind).
	const float Limit = FMath::DegreesToRadians(68.f);
	const bool bWide = FMath::Abs(Diff0) > Limit || FMath::Abs(Diff1) > Limit
		|| FMath::Abs(DiffAngle(Angle1, Angle0)) > FMath::DegreesToRadians(110.f)
		|| FMath::Cos(Diff0) <= 0.2f || FMath::Cos(Diff1) <= 0.2f;
	const float Det = U0.X * U1.Y - U0.Y * U1.X;

	if (!bInflection && !bWide && FMath::Abs(Det) > 0.08f)
	{
		// Where the start and end direction lines meet: Start + S * U0 = End - T * U1.
		const float S = (D.X * U1.Y - D.Y * U1.X) / Det;
		const float T = (U0.X * D.Y - U0.Y * D.X) / Det;
		if (S > Dist * 0.04f && T > Dist * 0.04f && S < Dist * 1.35f && T < Dist * 1.35f
			&& FMath::Max(S / T, T / S) <= 3.f)
		{
			const FVector2D C = P0 + U0 * S;
			AddQuadratic(Start, FVector(C.X, C.Y, Start.Z + DZ * 0.5f), End, true);
			return;
		}
	}

	// Two quadratics joined smoothly at M, the middle of their control points C0 and C1.
	auto Arm = [Dist, bInflection](float Diff)
	{
		if (bInflection)
		{
			return Dist * 0.38f;
		}
		const float Deviation = FMath::Min(1.f, FMath::Abs(FMath::Sin(Diff)));
		return Dist * (0.35f + 0.18f * Deviation);
	};
	const FVector2D C0 = P0 + U0 * Arm(Diff0);
	const FVector2D C1 = P1 - U1 * Arm(Diff1);
	const FVector2D M = (C0 + C1) * 0.5f;
	const FVector M3(M.X, M.Y, Start.Z + DZ * 0.5f);
	AddQuadratic(Start, FVector(C0.X, C0.Y, Start.Z + DZ * 0.25f), M3, true);
	AddQuadratic(M3, FVector(C1.X, C1.Y, End.Z - DZ * 0.25f), End, false);
}

void RMGeometry::OffsetCurve(const TArray<FRMFrame>& Frames, float LateralOffset, float KeyEvery, TArray<FRMSplinePoint>& OutPoints)
{
	OutPoints.Reset();
	if (Frames.Num() < 2)
	{
		return;
	}
	KeyEvery = FMath::Max(KeyEvery, 50.f);

	// Q(s) = P(s) + d * N(s), picked at roughly even arc length along the centre line.
	TArray<FVector> Keys;
	float NextDist = 0.f;
	for (const FRMFrame& F : Frames)
	{
		if (F.Dist >= NextDist)
		{
			Keys.Add(F.Pos + F.Right * LateralOffset);
			NextDist = F.Dist + KeyEvery;
		}
	}
	const FVector LastKey = Frames.Last().Pos + Frames.Last().Right * LateralOffset;
	if (Keys.Num() == 1 || FVector::Dist(Keys.Last(), LastKey) > KeyEvery * 0.25f)
	{
		Keys.Add(LastKey);
	}
	else
	{
		Keys.Last() = LastKey;
	}

	// Catmull-Rom tangents; ends use the offset of the true end direction so joins stay smooth.
	for (int32 I = 0; I < Keys.Num(); ++I)
	{
		FVector Tangent;
		if (I == 0)
		{
			Tangent = Frames[0].Fwd * FVector::Dist(Keys[0], Keys[1]);
		}
		else if (I == Keys.Num() - 1)
		{
			Tangent = Frames.Last().Fwd * FVector::Dist(Keys[I - 1], Keys[I]);
		}
		else
		{
			Tangent = (Keys[I + 1] - Keys[I - 1]) * 0.5f;
		}
		OutPoints.Add(FRMSplinePoint(Keys[I], Tangent));
	}
}

// ---------------------------------------------------------------------------------------------
// Extrusion
// ---------------------------------------------------------------------------------------------

void RMGeometry::ExtrudeProfile(const TArray<FRMFrame>& Frames, const TArray<FVector2D>& Profile, bool bClosed,
	const FVector2D& ProfileCentroid, float UVScale, FRMMesh& Out)
{
	if (Frames.Num() < 2 || Profile.Num() < 2)
	{
		return;
	}
	const float S = 1.f / FMath::Max(UVScale, 1.f);
	const int32 EdgeCount = bClosed ? Profile.Num() : Profile.Num() - 1;

	float U0 = 0.f;
	for (int32 E = 0; E < EdgeCount; ++E)
	{
		const FVector2D A = Profile[E];
		const FVector2D B = Profile[(E + 1) % Profile.Num()];
		const float EdgeLen = FVector2D::Distance(A, B);
		const float U1 = U0 + EdgeLen * S;

		// Profile-space edge normal, pointing away from the centroid.
		FVector2D N2(B.Y - A.Y, -(B.X - A.X));
		N2.Normalize();
		if (FVector2D::DotProduct(N2, (A + B) * 0.5f - ProfileCentroid) < 0.f)
		{
			N2 = -N2;
		}

		// Hard edge: each profile edge gets its own vertex strip.
		const int32 Base = Out.Vertices.Num();
		for (const FRMFrame& F : Frames)
		{
			const FVector N = (F.Right * N2.X + F.Up * N2.Y).GetSafeNormal();
			const float V = F.Dist * S;
			Out.AddVertex(F.ProfileToWorld(A), N, FVector2D(U0, V), F.Fwd);
			Out.AddVertex(F.ProfileToWorld(B), N, FVector2D(U1, V), F.Fwd);
		}
		for (int32 I = 0; I + 1 < Frames.Num(); ++I)
		{
			const int32 A0 = Base + I * 2;
			const int32 B0 = A0 + 1;
			const int32 A1 = A0 + 2;
			const int32 B1 = A0 + 3;
			const FVector N = Out.Normals[A0];
			Out.AddTriangle(A0, B0, B1, N);
			Out.AddTriangle(A0, B1, A1, N);
		}
		U0 = U1;
	}
}

void RMGeometry::ExtrudeProfileBeveled(const TArray<FRMFrame>& Frames, const TArray<FVector2D>& Profile,
	const FVector2D& ProfileCentroid, float UVScale, float BevelWidth, FRMMesh& Out)
{
	const int32 Num = Profile.Num();
	if (Frames.Num() < 2 || Num < 3)
	{
		return;
	}
	const float S = 1.f / FMath::Max(UVScale, 1.f);
	// Face normal of each profile edge (away from the centroid).
	TArray<FVector2D> EdgeN;
	for (int32 E = 0; E < Num; ++E)
	{
		const FVector2D A = Profile[E];
		const FVector2D B = Profile[(E + 1) % Num];
		FVector2D N2(B.Y - A.Y, -(B.X - A.X));
		N2.Normalize();
		if (FVector2D::DotProduct(N2, (A + B) * 0.5f - ProfileCentroid) < 0.f)
		{
			N2 = -N2;
		}
		EdgeN.Add(N2);
	}

	float U0 = 0.f;
	for (int32 E = 0; E < Num; ++E)
	{
		const FVector2D A = Profile[E];
		const FVector2D B = Profile[(E + 1) % Num];
		const float Len = FVector2D::Distance(A, B);
		const FVector2D Dir = (B - A) / FMath::Max(Len, 0.01f);
		const float W = FMath::Min(BevelWidth, Len * 0.4f);
		const FVector2D NA = (EdgeN[(E + Num - 1) % Num] + EdgeN[E]).GetSafeNormal();   // corner at A
		const FVector2D NB = (EdgeN[E] + EdgeN[(E + 1) % Num]).GetSafeNormal();         // corner at B
		// Across the edge: corner, bevel end, bevel start, corner.
		const FVector2D Pts[4] = { A, A + Dir * W, B - Dir * W, B };
		const FVector2D Ns[4] = { NA, EdgeN[E], EdgeN[E], NB };
		const float Us[4] = { U0, U0 + W * S, U0 + (Len - W) * S, U0 + Len * S };

		const int32 Base = Out.Vertices.Num();
		for (const FRMFrame& F : Frames)
		{
			const float V = F.Dist * S;
			for (int32 K = 0; K < 4; ++K)
			{
				const FVector N = (F.Right * Ns[K].X + F.Up * Ns[K].Y).GetSafeNormal();
				Out.AddVertex(F.ProfileToWorld(Pts[K]), N, FVector2D(Us[K], V), F.Fwd);
			}
		}
		for (int32 I = 0; I + 1 < Frames.Num(); ++I)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 A0 = Base + I * 4 + K;
				const int32 B0 = A0 + 1;
				const int32 A1 = A0 + 4;
				const int32 B1 = A1 + 1;
				const FVector N = Out.Normals[Base + I * 4 + 1]; // the flat face's normal (winding check)
				Out.AddTriangle(A0, B0, B1, N);
				Out.AddTriangle(A0, B1, A1, N);
			}
		}
		U0 += Len * S;
	}
}

void RMGeometry::AddProfileCap(const FRMFrame& Frame, const TArray<FVector2D>& Profile, bool bFacingForward, float UVScale, FRMMesh& Out)
{
	if (Profile.Num() < 3)
	{
		return;
	}
	const float S = 1.f / FMath::Max(UVScale, 1.f);
	const FVector N = bFacingForward ? Frame.Fwd : -Frame.Fwd;
	const int32 Base = Out.Vertices.Num();
	for (const FVector2D& P : Profile)
	{
		Out.AddVertex(Frame.ProfileToWorld(P), N, P * S, Frame.Right);
	}
	// Ear clipping: profiles are not always convex (the cement block's step), and a plain fan from
	// the first corner then covers area outside the profile.
	float Area2 = 0.f;
	for (int32 I = 0; I < Profile.Num(); ++I)
	{
		const FVector2D& A = Profile[I];
		const FVector2D& B = Profile[(I + 1) % Profile.Num()];
		Area2 += A.X * B.Y - B.X * A.Y;
	}
	const float Orient = Area2 >= 0.f ? 1.f : -1.f;
	auto Cross = [](const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
	};
	TArray<int32> Left;
	for (int32 I = 0; I < Profile.Num(); ++I)
	{
		Left.Add(I);
	}
	while (Left.Num() > 3)
	{
		bool bClipped = false;
		for (int32 K = 0; K < Left.Num(); ++K)
		{
			const int32 IA = Left[(K + Left.Num() - 1) % Left.Num()];
			const int32 IB = Left[K];
			const int32 IC = Left[(K + 1) % Left.Num()];
			const FVector2D& A = Profile[IA];
			const FVector2D& B = Profile[IB];
			const FVector2D& C = Profile[IC];
			if (Cross(A, B, C) * Orient <= KINDA_SMALL_NUMBER)
			{
				continue; // reflex or flat corner: not an ear
			}
			bool bInside = false;
			for (const int32 IP : Left)
			{
				if (IP == IA || IP == IB || IP == IC)
				{
					continue;
				}
				const FVector2D& P = Profile[IP];
				if (Cross(A, B, P) * Orient >= 0.f && Cross(B, C, P) * Orient >= 0.f && Cross(C, A, P) * Orient >= 0.f)
				{
					bInside = true;
					break;
				}
			}
			if (bInside)
			{
				continue;
			}
			Out.AddTriangle(Base + IA, Base + IB, Base + IC, N);
			Left.RemoveAt(K);
			bClipped = true;
			break;
		}
		if (!bClipped)
		{
			break; // degenerate profile: fan the rest below
		}
	}
	for (int32 I = 1; I + 1 < Left.Num(); ++I)
	{
		Out.AddTriangle(Base + Left[0], Base + Left[I], Base + Left[I + 1], N);
	}
}

// ---------------------------------------------------------------------------------------------
// Roads and fences
// ---------------------------------------------------------------------------------------------

void RMGeometry::BuildRoad(const TArray<FRMFrame>& Frames, float Width, float Height, float UVScale,
	const FRMMarkings& Markings, float EdgeLineInset, FRMMesh& Top, FRMMesh& Body)
{
	const float H = Width * 0.5f;
	const FVector2D Centroid(0.f, -Height * 0.5f);

	// Asphalt top surface: one strip, two vertices per frame (left edge, right edge).
	const int32 TopBase = Top.Vertices.Num();
	ExtrudeProfile(Frames, { FVector2D(-H, 0.f), FVector2D(H, 0.f) }, false, Centroid, UVScale, Top);

	// Marking data in metres for the asphalt material.
	Top.MarkUV1.SetNumZeroed(Top.Vertices.Num());
	Top.MarkUV2.SetNumZeroed(Top.Vertices.Num());
	Top.MarkUV3.SetNumZeroed(Top.Vertices.Num());
	const FVector2D Layout(Width / 100.f, EdgeLineInset / 100.f);
	// UV3.y packs flags: 1 = side lines, 2 = continuous dividers.
	const FVector2D Style(Markings.Lanes, (Markings.bEdgeLines ? 1.f : 0.f) + (Markings.bContinuous ? 2.f : 0.f));
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		const float Along = Frames[I].Dist / 100.f;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const int32 V = TopBase + I * 2 + Side;
			Top.MarkUV1[V] = FVector2D(Side == 0 ? 0.f : Width / 100.f, Along);
			Top.MarkUV2[V] = Layout;
			Top.MarkUV3[V] = Style;
		}
	}

	// Concrete: right side, bottom, left side.
	ExtrudeProfile(Frames, { FVector2D(H, 0.f), FVector2D(H, -Height), FVector2D(-H, -Height), FVector2D(-H, 0.f) },
		false, Centroid, UVScale, Body);

	const TArray<FVector2D> Section = { FVector2D(-H, 0.f), FVector2D(H, 0.f), FVector2D(H, -Height), FVector2D(-H, -Height) };
	AddProfileCap(Frames[0], Section, false, UVScale, Body);
	AddProfileCap(Frames.Last(), Section, true, UVScale, Body);
}

namespace
{
	/** Calls Fn(Frame) every Spacing cm along the frames (interpolated), including both ends. */
	template <typename TFunc>
	void ForEvery(const TArray<FRMFrame>& Frames, float Spacing, TFunc Fn)
	{
		if (Frames.Num() < 2)
		{
			return;
		}
		const float Total = Frames.Last().Dist;
		const int32 Count = FMath::Max(1, FMath::RoundToInt(Total / Spacing));
		int32 Seg = 0;
		for (int32 K = 0; K <= Count; ++K)
		{
			const float Target = Total * K / Count;
			while (Seg + 2 < Frames.Num() && Frames[Seg + 1].Dist < Target)
			{
				++Seg;
			}
			const FRMFrame& A = Frames[Seg];
			const FRMFrame& B = Frames[Seg + 1];
			const float Alpha = (B.Dist > A.Dist) ? FMath::Clamp((Target - A.Dist) / (B.Dist - A.Dist), 0.f, 1.f) : 0.f;
			FRMFrame F;
			F.Pos = FMath::Lerp(A.Pos, B.Pos, Alpha);
			F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, Alpha).GetSafeNormal();
			F.Right = FMath::Lerp(A.Right, B.Right, Alpha).GetSafeNormal();
			F.Up = FVector::UpVector;
			F.Dist = Target;
			Fn(F);
		}
	}

	/** Frames from Start cm to (length - End) cm along the curve; distances are kept as they were. */
	TArray<FRMFrame> TrimFrames(const TArray<FRMFrame>& Frames, float Start, float End)
	{
		if (Frames.Num() < 2)
		{
			return Frames;
		}
		const float Total = Frames.Last().Dist;
		const float From = FMath::Clamp(Start, 0.f, Total * 0.45f);
		const float To = FMath::Clamp(Total - End, Total * 0.55f, Total);
		auto At = [&Frames](float D)
		{
			int32 Seg = 0;
			while (Seg + 2 < Frames.Num() && Frames[Seg + 1].Dist < D)
			{
				++Seg;
			}
			const FRMFrame& A = Frames[Seg];
			const FRMFrame& B = Frames[Seg + 1];
			const float Alpha = (B.Dist > A.Dist) ? FMath::Clamp((D - A.Dist) / (B.Dist - A.Dist), 0.f, 1.f) : 0.f;
			FRMFrame F = A;
			F.Pos = FMath::Lerp(A.Pos, B.Pos, Alpha);
			F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, Alpha).GetSafeNormal();
			F.Right = FMath::Lerp(A.Right, B.Right, Alpha).GetSafeNormal();
			F.Up = FMath::Lerp(A.Up, B.Up, Alpha).GetSafeNormal();
			F.Dist = D;
			return F;
		};
		TArray<FRMFrame> Out;
		Out.Add(At(From));
		for (const FRMFrame& F : Frames)
		{
			if (F.Dist > From + 1.f && F.Dist < To - 1.f)
			{
				Out.Add(F);
			}
		}
		Out.Add(At(To));
		return Out;
	}

	TArray<FRMFrame> LiftFrames(const TArray<FRMFrame>& Frames, float Z)
	{
		TArray<FRMFrame> Out = Frames;
		for (FRMFrame& F : Out)
		{
			F.Pos += FVector(0.f, 0.f, Z);
		}
		return Out;
	}
}

void RMGeometry::MirrorFrames(TArray<FRMFrame>& Frames)
{
	for (FRMFrame& F : Frames)
	{
		F.Right = -F.Right;
	}
}

void RMGeometry::SplineThroughPoints(const TArray<FVector>& Points, TArray<FRMSplinePoint>& OutPoints)
{
	OutPoints.Reset();
	const int32 N = Points.Num();
	for (int32 I = 0; I < N; ++I)
	{
		FVector Tangent;
		if (N < 2)
		{
			Tangent = FVector::ForwardVector;
		}
		else if (I == 0)
		{
			Tangent = Points[1] - Points[0];
		}
		else if (I == N - 1)
		{
			Tangent = Points[I] - Points[I - 1];
		}
		else
		{
			Tangent = (Points[I + 1] - Points[I - 1]) * 0.5f;
		}
		OutPoints.Add(FRMSplinePoint(Points[I], Tangent));
	}
}

void RMGeometry::BuildGuardRail(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Metal)
{
	const float S = 1.f; // road on +Right; mirroring is done by the caller

	// W-beam approximated by a shallow box section 32 cm tall, centred at 60 cm, bulging towards the road.
	const TArray<FRMFrame> RailFrames = LiftFrames(Frames, 60.f);
	TArray<FVector2D> Beam = {
		FVector2D(-4.f, 16.f), FVector2D(4.f, 16.f), FVector2D(6.f, 0.f), FVector2D(4.f, -16.f), FVector2D(-4.f, -16.f), FVector2D(-2.f, 0.f)
	};
	for (FVector2D& P : Beam)
	{
		P.X *= S;
	}
	ExtrudeProfile(RailFrames, Beam, true, FVector2D::ZeroVector, UVScale, Metal);
	AddProfileCap(RailFrames[0], Beam, false, UVScale, Metal);
	AddProfileCap(RailFrames.Last(), Beam, true, UVScale, Metal);

	// Posts every 2 m, behind the beam (away from the road).
	ForEvery(Frames, 200.f, [&](const FRMFrame& F)
	{
		Metal.AddBox(F.Pos + FVector(0.f, 0.f, 40.f) - F.Right * (9.f * S), F.Fwd, F.Right, FVector::UpVector, FVector(5.f, 5.f, 40.f), UVScale);
	});
}

void RMGeometry::BuildJerseyBarrier(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Concrete)
{
	// Classic New Jersey profile: 60 cm base, 15 cm top, 80 cm tall.
	const TArray<FVector2D> Profile = {
		FVector2D(-30.f, 0.f), FVector2D(30.f, 0.f), FVector2D(28.f, 8.f), FVector2D(12.f, 30.f),
		FVector2D(7.5f, 80.f), FVector2D(-7.5f, 80.f), FVector2D(-12.f, 30.f), FVector2D(-28.f, 8.f)
	};
	const int32 First = Concrete.Vertices.Num();
	// Corners lit as if rounded over 2.5 cm (normals only; the outline stays sharp).
	ExtrudeProfileBeveled(Frames, Profile, FVector2D(0.f, 30.f), UVScale, 2.5f, Concrete);
	const int32 FirstCap = Concrete.Vertices.Num();
	AddProfileCap(Frames[0], Profile, false, UVScale, Concrete);
	const int32 SecondCap = Concrete.Vertices.Num();
	AddProfileCap(Frames.Last(), Profile, true, UVScale, Concrete);

	// Block joints are drawn by the material (M_RoadJersey): UV1 = (metres along the fence, 1 on
	// the end caps), UV2 = (block length m). Blocks are about 2 m, fitted so the fence ends on a joint.
	const float Total = Frames.Last().Dist;
	const float Block = Total / FMath::Max(1, FMath::RoundToInt(Total / 200.f));
	const float S = FMath::Max(UVScale, 1.f) / 100.f; // UV0.y * S = metres along
	Concrete.MarkUV1.SetNumZeroed(Concrete.Vertices.Num());
	Concrete.MarkUV2.SetNumZeroed(Concrete.Vertices.Num());
	for (int32 I = First; I < Concrete.Vertices.Num(); ++I)
	{
		float Along = Concrete.UVs[I].Y * S;
		float Cap = 0.f;
		if (I >= FirstCap)
		{
			Along = (I < SecondCap ? Frames[0].Dist : Total) / 100.f;
			Cap = 1.f;
		}
		Concrete.MarkUV1[I] = FVector2D(Along, Cap);
		Concrete.MarkUV2[I] = FVector2D(Block / 100.f, 0.f);
	}
}

void RMGeometry::BuildGridFence(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Posts, FRMMesh& Panel, float Scale)
{
	// Scale 1: 2 m tall. Scale 2: the 4 m version (twice as tall, thicker posts and tubes).
	// Tubes and the mesh sheet stop inside the end posts, so two fences meeting at a corner or on a
	// slope do not run through each other. Everything sits in the posts' centre plane.
	const float Height = 200.f * Scale;
	const float PostHalf = 5.f * (Scale > 1.f ? 1.6f : 1.f);
	const float TubeHalf = 3.f * (Scale > 1.f ? 1.5f : 1.f);
	const float FrameX = -3.f;
	const float Bottom = 8.f + (TubeHalf - 3.f);

	// Built against true vertical (not the road's tilted up) so the sheet, tubes and posts stay
	// upright and line up on slopes.
	TArray<FRMFrame> Upright = Frames;
	for (FRMFrame& F : Upright)
	{
		F.Up = FVector::UpVector;
		F.Right = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal();
	}
	const TArray<FRMFrame> Inner = TrimFrames(Upright, PostHalf - 2.f, PostHalf - 2.f); // ends 2 cm inside the end posts

	// Single sheet between the tubes, own material slot (two-sided masked diamond grid). The grid
	// is laid out by the material from UV1.x = metres along the fence and the world height, so it
	// stays upright on slopes.
	const int32 PanelFirst = Panel.Vertices.Num();
	ExtrudeProfile(Inner, { FVector2D(FrameX, Height - TubeHalf), FVector2D(FrameX, Bottom + TubeHalf) }, false,
		FVector2D(FrameX + 10.f, Height * 0.5f), 200.f, Panel);
	Panel.MarkUV1.SetNumZeroed(Panel.Vertices.Num());
	for (int32 I = PanelFirst; I < Panel.Vertices.Num(); ++I)
	{
		Panel.MarkUV1[I] = FVector2D(Panel.UVs[I].Y * 2.f, 0.f); // UV0.y = cm / 200
	}

	// Top and bottom tubes plus posts about every 3 m (and at both ends).
	const TArray<FVector2D> Tube = { FVector2D(FrameX - TubeHalf, -TubeHalf), FVector2D(FrameX + TubeHalf, -TubeHalf),
		FVector2D(FrameX + TubeHalf, TubeHalf), FVector2D(FrameX - TubeHalf, TubeHalf) };
	ExtrudeProfile(LiftFrames(Inner, Height), Tube, true, FVector2D(FrameX, 0.f), UVScale, Posts);
	ExtrudeProfile(LiftFrames(Inner, Bottom), Tube, true, FVector2D(FrameX, 0.f), UVScale, Posts);
	const float PostTop = Height + TubeHalf + 2.f;
	ForEvery(Upright, 300.f, [&](const FRMFrame& F)
	{
		Posts.AddBox(F.Pos + FVector(0.f, 0.f, PostTop * 0.5f) + F.Right * FrameX, F.Fwd, F.Right, FVector::UpVector,
			FVector(PostHalf, PostHalf, PostTop * 0.5f), UVScale);
	});
}

namespace
{
	// Layout of the game's concrete wall texture (TX_WallSetConcrete_01, read from the real texture):
	// one half of an 8 m x 4 m panel, panel centre at u = 0, panel edge at u ~ 0.856, mirrored for
	// the other half; ~0.214 units per metre; top of the wall at v ~ 0.021, bottom at ~0.888.
	constexpr float WallPanel = 800.f;        // cm

	/** Metres-free panel position: cm from the nearest panel centre (negative before it). */
	float WallPanelOffset(float Dist)
	{
		const float InPanel = FMath::Fmod(Dist, WallPanel);
		return (InPanel < 0.f ? InPanel + WallPanel : InPanel) - WallPanel * 0.5f;
	}
}

namespace
{
	/** Frame at a distance along the frames (interpolated), upright. */
	FRMFrame FrameAtDist(const TArray<FRMFrame>& Frames, float D)
	{
		int32 I = 1;
		while (I < Frames.Num() - 1 && Frames[I].Dist < D)
		{
			++I;
		}
		const FRMFrame& A = Frames[I - 1];
		const FRMFrame& B = Frames[I];
		const float T = FMath::Clamp((D - A.Dist) / FMath::Max(B.Dist - A.Dist, 0.01f), 0.f, 1.f);
		FRMFrame F;
		F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
		F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
		F.Right = FVector(FMath::Lerp(A.Right, B.Right, T).X, FMath::Lerp(A.Right, B.Right, T).Y, 0.f).GetSafeNormal();
		F.Up = FVector::UpVector;
		F.Dist = D;
		return F;
	}

	/**
	 * Extrudes a closed rectangle profile (X across, Y up) along the frames with hard edges. UVOf gives
	 * each vertex's UV0 from (frame, profile point, is a vertical face). Tangents run along the wall,
	 * binormals down the wall (the way a wall texture's v grows).
	 */
	template <typename TUV>
	void ExtrudeBox(const TArray<FRMFrame>& Frames, const TArray<FVector2D>& Profile, FRMMesh& Out, TUV UVOf)
	{
		if (Frames.Num() < 2)
		{
			return;
		}
		FVector2D Centroid = FVector2D::ZeroVector;
		for (const FVector2D& P : Profile)
		{
			Centroid += P / Profile.Num();
		}
		for (int32 E = 0; E < Profile.Num(); ++E)
		{
			const FVector2D A = Profile[E];
			const FVector2D B = Profile[(E + 1) % Profile.Num()];
			FVector2D N2(B.Y - A.Y, -(B.X - A.X));
			N2.Normalize();
			if (FVector2D::DotProduct(N2, (A + B) * 0.5f - Centroid) < 0.f)
			{
				N2 = -N2;
			}
			const bool bVertical = FMath::IsNearlyEqual(A.X, B.X);
			const int32 Base = Out.Vertices.Num();
			for (const FRMFrame& F : Frames)
			{
				const FVector N = (F.Right * N2.X + F.Up * N2.Y).GetSafeNormal();
				for (const FVector2D& P : { A, B })
				{
					const int32 V = Out.AddVertex(F.ProfileToWorld(P), N, UVOf(F, P, bVertical), F.Fwd);
					const bool bFlip = FVector::DotProduct(FVector::CrossProduct(N, F.Fwd), FVector::UpVector) > 0.f;
					Out.Tangents[V] = FProcMeshTangent(F.Fwd, bFlip);
				}
			}
			for (int32 I = 0; I + 1 < Frames.Num(); ++I)
			{
				const int32 A0 = Base + I * 2;
				const FVector N = Out.Normals[A0];
				Out.AddTriangle(A0, A0 + 1, A0 + 3, N);
				Out.AddTriangle(A0, A0 + 3, A0 + 2, N);
			}
		}
	}
}

void RMGeometry::BuildSteelFenceWall(const TArray<FRMFrame>& InFrames, FRMMesh& Out, float Height)
{
	if (InFrames.Num() < 2)
	{
		return;
	}
	TArray<FRMFrame> Frames = InFrames;
	for (FRMFrame& F : Frames)
	{
		F.Up = FVector::UpVector;
		F.Right = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal();
	}
	const float Length = Frames.Last().Dist;
	const float Cell = 200.f;          // the panel texture repeats every 2 m (as on the game's wall)
	const float PostDepth = 25.f;      // half thickness (50 cm wall)
	const float KickTop = 37.f;        // kick rail at the bottom
	const float RailBottom = Height - 21.f; // top rail
	const float PanelHalf = 8.f;       // ribbed sheet, recessed between the rails

	// Panel UVs (measured from the game's texture and wall): one bolt-column spacing (0.46 in u) every
	// 2 m, so the bolts keep the game's spacing. The window u 0.08..0.54 holds one bolt column (0.19)
	// and stays clear of the texture's dark frame strip (it starts at u ~0.875) and of the other bolt
	// column (0.65), so the ribs run straight through the cut. v 0.2 at the top rail down to 0.9 at the
	// kick rail.
	const float PanelV0 = 0.2f, PanelV1 = 0.9f, PanelUStart = 0.08f, PanelUSize = 0.46f;

	// Cells of 2 m; each is built on its own so the panel texture restarts cleanly.
	const int32 Cells = FMath::Max(1, FMath::CeilToInt((Length - 1.f) / Cell));
	for (int32 C = 0; C < Cells; ++C)
	{
		const float D0 = C * Cell;
		const float D1 = FMath::Min((C + 1) * Cell, Length);
		TArray<FRMFrame> Part;
		Part.Add(FrameAtDist(Frames, D0));
		for (const FRMFrame& F : Frames)
		{
			if (F.Dist > D0 + 1.f && F.Dist < D1 - 1.f)
			{
				Part.Add(F);
			}
		}
		Part.Add(FrameAtDist(Frames, D1));

		auto PanelU = [&](const FRMFrame& F)
		{
			return PanelUStart + FMath::Clamp((F.Dist - D0) / Cell, 0.f, 1.f) * PanelUSize;
		};
		// Ribbed sheet.
		ExtrudeBox(Part, { FVector2D(-PanelHalf, KickTop - 2.f), FVector2D(PanelHalf, KickTop - 2.f),
			FVector2D(PanelHalf, RailBottom + 2.f), FVector2D(-PanelHalf, RailBottom + 2.f) }, Out,
			[&](const FRMFrame& F, const FVector2D& P, bool bVertical)
			{
				const float T = FMath::Clamp((RailBottom - P.Y) / (RailBottom - KickTop), 0.f, 1.f);
				return bVertical ? FVector2D(PanelU(F), FMath::Lerp(PanelV0, PanelV1, T)) : FVector2D(PanelU(F), PanelV0);
			});
		// Top rail and kick rail: the dark bands above and below the panel in the texture.
		ExtrudeBox(Part, { FVector2D(-PostDepth, RailBottom), FVector2D(PostDepth, RailBottom),
			FVector2D(PostDepth, Height), FVector2D(-PostDepth, Height) }, Out,
			[&](const FRMFrame& F, const FVector2D& P, bool bVertical)
			{
				return FVector2D(PanelU(F), 0.06f + 0.1f * (Height - P.Y) / FMath::Max(Height - RailBottom, 1.f) + (bVertical ? 0.f : 0.02f));
			});
		ExtrudeBox(Part, { FVector2D(-PostDepth + 5.f, -2.f), FVector2D(PostDepth - 5.f, -2.f),
			FVector2D(PostDepth - 5.f, KickTop), FVector2D(-PostDepth + 5.f, KickTop) }, Out,
			[&](const FRMFrame& F, const FVector2D& P, bool)
			{
				return FVector2D(PanelU(F), 0.92f + 0.06f * FMath::Clamp((KickTop - P.Y) / KickTop, 0.f, 1.f));
			});
	}

	// Close both ends with the wall's own cross-section (top rail, sheet, kick rail): the cut-out
	// shape, on the dark frame part of the texture.
	const TArray<TArray<FVector2D>> Sections = {
		{ FVector2D(-PostDepth, RailBottom), FVector2D(PostDepth, RailBottom), FVector2D(PostDepth, Height), FVector2D(-PostDepth, Height) },
		{ FVector2D(-PanelHalf, KickTop), FVector2D(PanelHalf, KickTop), FVector2D(PanelHalf, RailBottom), FVector2D(-PanelHalf, RailBottom) },
		{ FVector2D(-PostDepth + 5.f, -2.f), FVector2D(PostDepth - 5.f, -2.f), FVector2D(PostDepth - 5.f, KickTop), FVector2D(-PostDepth + 5.f, KickTop) },
	};
	for (const bool bEnd : { false, true })
	{
		const FRMFrame& F = bEnd ? Frames.Last() : Frames[0];
		for (const TArray<FVector2D>& Cap : Sections)
		{
			const int32 First = Out.Vertices.Num();
			AddProfileCap(F, Cap, bEnd, 100.f, Out);
			for (int32 V = First; V < Out.Vertices.Num(); ++V)
			{
				const FVector2D& P = Cap[V - First];
				Out.UVs[V] = FVector2D(0.91f + 0.07f * (P.X + PostDepth) / (2.f * PostDepth), 0.22f + 0.66f * (1.f - FMath::Clamp(P.Y, 0.f, Height) / Height));
			}
		}
	}
}

void RMGeometry::BuildFenceWall(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Out, float Height, float Thickness,
	const FRMWallTexLayout& Layout)
{
	const float WallUPerCm = Layout.UPerMetre / 100.f;
	const float WallVPerCm = Layout.VPerMetre / 100.f;
	const float WallVTop = Layout.VTop;
	// u along the wall: folded at each panel centre (mirrored half panel), or for a texture that tiles
	// sideways simply growing along the wall (the texture repeats by itself, no seams).
	auto PanelU = [&Layout, WallUPerCm](float Offset, float Dist)
	{
		return Layout.bMirror ? Layout.UCentre + FMath::Abs(Offset) * WallUPerCm : Layout.UCentre + Dist * WallUPerCm;
	};
	if (Frames.Num() < 2)
	{
		return;
	}
	// Upright frames, with extra frames at every panel centre and edge (every 4 m) so the mirrored
	// texture folds exactly there.
	TArray<FRMFrame> Upright;
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		if (I > 0)
		{
			const FRMFrame& A = Frames[I - 1];
			const FRMFrame& B = Frames[I];
			for (float D = FMath::FloorToFloat(A.Dist / 400.f) * 400.f + 400.f; D < B.Dist - 1.f; D += 400.f)
			{
				if (D <= A.Dist + 1.f)
				{
					continue;
				}
				const float T = (D - A.Dist) / FMath::Max(B.Dist - A.Dist, 1.f);
				FRMFrame F;
				F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
				F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
				F.Right = FMath::Lerp(A.Right, B.Right, T).GetSafeNormal();
				F.Dist = D;
				Upright.Add(F);
			}
		}
		Upright.Add(Frames[I]);
	}
	for (FRMFrame& F : Upright)
	{
		F.Up = FVector::UpVector;
		F.Right = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal();
	}
	// Centred on the line (like the street lights on the road edge). A little below the ground line
	// so no gap shows on uneven ground; stacked walls overlap there.
	const float Half = Thickness * 0.5f;
	const float Bottom = -2.f;
	const TArray<FVector2D> Profile = { FVector2D(-Half, Bottom), FVector2D(Half, Bottom),
		FVector2D(Half, Height), FVector2D(-Half, Height) };

	// UV0 = the game's wall texture layout (for the game's own wall material); UV1 = (metres along,
	// metres up) for Road Maker's own wall material. The tangent runs the way the texture's u grows
	// (away from the panel centre), the binormal down the wall (the way v grows).
	auto Mark = [&Out](int32 Index, const FVector2D& UV1)
	{
		if (Out.MarkUV1.Num() <= Index)
		{
			Out.MarkUV1.SetNumZeroed(Index + 1);
		}
		Out.MarkUV1[Index] = UV1;
	};
	auto SetTangent = [&Out](int32 Index, const FVector& TangentX)
	{
		const FVector N = Out.Normals[Index];
		const bool bFlip = FVector::DotProduct(FVector::CrossProduct(N, TangentX), FVector::UpVector) > 0.f;
		Out.Tangents[Index] = FProcMeshTangent(TangentX, bFlip);
	};
	for (int32 E = 0; E < Profile.Num(); ++E)
	{
		const FVector2D A = Profile[E];
		const FVector2D B = Profile[(E + 1) % Profile.Num()];
		FVector2D N2(B.Y - A.Y, -(B.X - A.X));
		N2.Normalize();
		if (FVector2D::DotProduct(N2, (A + B) * 0.5f - FVector2D(0.f, Height * 0.5f)) < 0.f)
		{
			N2 = -N2;
		}
		const bool bVertical = FMath::IsNearlyEqual(A.X, B.X);
		const int32 Base = Out.Vertices.Num();
		for (const FRMFrame& F : Upright)
		{
			const FVector N = (F.Right * N2.X + F.Up * N2.Y).GetSafeNormal();
			const float Offset = WallPanelOffset(F.Dist);
			for (const FVector2D& P : { A, B })
			{
				const float Up = FMath::Clamp(P.Y, 0.f, Height);
				FVector2D UV0;
				if (bVertical)
				{
					UV0 = FVector2D(PanelU(Offset, F.Dist), WallVTop + (Height - Up) * WallVPerCm);
				}
				else
				{
					// Top and bottom: a plain stretch of the texture, mirrored every metre (no seams).
					const float Mirror = FMath::Abs(FMath::Fmod(F.Dist / 100.f, 2.f) - 1.f);
					UV0 = FVector2D(Layout.UCentre + 0.6f * (Layout.bMirror ? 1.f : 0.5f) + (P.X + Half) * WallUPerCm, WallVTop + 0.2f + Mirror * 0.2f);
				}
				const int32 V = Out.AddVertex(F.ProfileToWorld(P), N, UV0, F.Fwd);
				SetTangent(V, bVertical && Layout.bMirror && Offset < 0.f ? -F.Fwd : F.Fwd);
				Mark(V, FVector2D(F.Dist / 100.f, (bVertical ? Up : P.X + Half) / 100.f));
			}
		}
		for (int32 I = 0; I + 1 < Upright.Num(); ++I)
		{
			const int32 A0 = Base + I * 2;
			const FVector N = Out.Normals[A0];
			Out.AddTriangle(A0, A0 + 1, A0 + 3, N);
			Out.AddTriangle(A0, A0 + 3, A0 + 2, N);
		}
	}
	for (const bool bEnd : { false, true })
	{
		const FRMFrame& F = bEnd ? Upright.Last() : Upright[0];
		const int32 First = Out.Vertices.Num();
		AddProfileCap(F, Profile, bEnd, UVScale, Out);
		for (int32 V = First; V < Out.Vertices.Num(); ++V)
		{
			const FVector2D& P = Profile[V - First];
			const float Up = FMath::Clamp(P.Y, 0.f, Height);
			Out.UVs[V] = FVector2D(Layout.UCentre + 0.6f * (Layout.bMirror ? 1.f : 0.5f) + (P.X + Half) * WallUPerCm, WallVTop + (Height - Up) * WallVPerCm);
			SetTangent(V, F.Right);
			Mark(V, FVector2D((P.X + Half) / 100.f, Up / 100.f));
		}
	}
	Out.MarkUV1.SetNumZeroed(Out.Vertices.Num());
}

// ---------------------------------------------------------------------------------------------
// Intersections
// ---------------------------------------------------------------------------------------------

bool RMGeometry::BuildIntersection(const TArray<FRMRoadEnd>& Ends, float Height, float UVScale, int32 FilletSegments,
	FRMMesh& Top, FRMMesh& Body, TArray<FVector>* OutOutline, TArray<bool>* OutEdgeIsMouth, float GridSpacing)
{
	if (Ends.Num() < 2)
	{
		return false;
	}
	FilletSegments = FMath::Clamp(FilletSegments, 1, 32);
	const float S = 1.f / FMath::Max(UVScale, 1.f);

	FVector Center = FVector::ZeroVector;
	for (const FRMRoadEnd& E : Ends)
	{
		Center += E.Center;
	}
	Center /= Ends.Num();

	struct FSortedEnd
	{
		float Angle;
		FVector First;  // corner reached first when walking counter-clockwise
		FVector Second;
		FVector Inward;
	};
	TArray<FSortedEnd> Sorted;
	for (const FRMRoadEnd& E : Ends)
	{
		const FVector In = FVector(E.Inward.X, E.Inward.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, In).GetSafeNormal();
		const FVector P1 = E.Center - Right * E.Width * 0.5f;
		const FVector P2 = E.Center + Right * E.Width * 0.5f;
		const FVector D = E.Center - Center;

		FSortedEnd SE;
		SE.Angle = FMath::Atan2(D.Y, D.X);
		SE.Inward = In;
		// Order the two corners by angle around the centre.
		const bool bP1First = Cross2D(P1 - Center, P2 - Center) > 0.f;
		SE.First = bP1First ? P1 : P2;
		SE.Second = bP1First ? P2 : P1;
		Sorted.Add(SE);
	}
	Sorted.Sort([](const FSortedEnd& A, const FSortedEnd& B) { return A.Angle < B.Angle; });

	// Outline: road mouth (First -> Second) then a curb to the next road's First corner. The curbs are
	// first made like the vehicle paths / rails through the junction (MakeFixedEnds); if that outline
	// crosses itself or cannot be triangulated, the simple curbs are used (one quadratic whose control
	// point is where the road edges meet, straight when they do not meet nearby).
	TArray<FVector> Ring;
	TArray<bool> EdgeIsMouth; // EdgeIsMouth[k] covers the edge Ring[k] -> Ring[k + 1]
	TArray<int32> Tris;
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		const bool bRoadCurves = Attempt == 0;
		Ring.Reset();
		EdgeIsMouth.Reset();
		Tris.Reset();
		for (int32 I = 0; I < Sorted.Num(); ++I)
		{
			const FSortedEnd& Cur = Sorted[I];
			const FSortedEnd& Next = Sorted[(I + 1) % Sorted.Num()];

			Ring.Add(Cur.First);
			EdgeIsMouth.Add(true);
			Ring.Add(Cur.Second);
			EdgeIsMouth.Add(false);

			const FVector P = Cur.Second;
			const FVector Q = Next.First;
			if (bRoadCurves && FVector::Dist2D(P, Q) < 10.f)
			{
				continue; // ends side by side (e.g. a split road): no curb between them
			}
			if (bRoadCurves)
			{
				// The same curve as vehicle paths and rails through the junction (MakeFixedEnds: tangents of a
				// circular arc), from P heading in to Q leaving along the next road's edge, so the paths that
				// follow the lanes stay inside the curb.
				TArray<FRMSplinePoint> Curve;
				MakeFixedEnds(P, Cur.Inward, Q, -Next.Inward, Curve);
				const float Step = FMath::Min(FilletSegments <= 3 ? 1600.f : 400.f,
					FMath::Max(FVector::Dist2D(P, Q) / FilletSegments, 25.f));
				TArray<FRMFrame> CurbFrames;
				SampleSpline(Curve, Step, CurbFrames);
				for (int32 K = 1; K + 1 < CurbFrames.Num(); ++K)
				{
					Ring.Add(CurbFrames[K].Pos);
					EdgeIsMouth.Add(false);
				}
				continue;
			}
			// Control point: where the edge of this road meets the edge of the next one, extended into the junction.
			const FVector DP = Cur.Inward;
			const FVector DQ = Next.Inward;
			const float Denom = Cross2D(DP, DQ);
			FVector Control = (P + Q) * 0.5f;
			if (FMath::Abs(Denom) > 1e-3f)
			{
				const FVector PQ = Q - P;
				const float T = Cross2D(PQ, DQ) / Denom;
				const float U = Cross2D(PQ, DP) / Denom;
				const float Limit = 4.f * FVector::Dist2D(P, Q) + 100.f;
				if (T > 0.f && U > 0.f && T < Limit && U < Limit)
				{
					Control = P + DP * T;
				}
			}
			Control.Z = (P.Z + Q.Z) * 0.5f;

			// Long curbs (big intersections) get more points so they stay round: about every 4 m
			// (every 16 m for the far mesh, which asks for few segments).
			const float CurbLength = FVector::Dist2D(P, Control) + FVector::Dist2D(Control, Q);
			const int32 Segments = FMath::Clamp(FMath::Max(FilletSegments, FMath::CeilToInt(CurbLength / (FilletSegments <= 3 ? 1600.f : 400.f))), 1, 64);
			for (int32 K = 1; K < Segments; ++K)
			{
				const float T = static_cast<float>(K) / Segments;
				const float A = (1.f - T) * (1.f - T);
				const float B = 2.f * (1.f - T) * T;
				const float C = T * T;
				FVector Pt = P * A + Control * B + Q * C;
				Pt.Z = FMath::Lerp(P.Z, Q.Z, T);
				Ring.Add(Pt);
				EdgeIsMouth.Add(false);
			}
		}

		// Merge points that coincide (e.g. a side road whose corners sit exactly on the corners of the
		// gap it joins: its curb fillets have zero length). The kept edge takes the dropped edge's flag.
		{
			TArray<FVector> Points;
			TArray<bool> Flags;
			for (int32 K = 0; K < Ring.Num(); ++K)
			{
				if (Points.Num() > 0 && FVector::Dist2D(Ring[K], Points.Last()) < 1.f)
				{
					Flags.Last() = EdgeIsMouth[K];
					continue;
				}
				Points.Add(Ring[K]);
				Flags.Add(EdgeIsMouth[K]);
			}
			while (Points.Num() > 2 && FVector::Dist2D(Points.Last(), Points[0]) < 1.f)
			{
				Points.Pop();
				Flags.Pop();
			}
			Ring = MoveTemp(Points);
			EdgeIsMouth = MoveTemp(Flags);
		}
		if (Ring.Num() < 3)
		{
			if (bRoadCurves)
			{
				continue;
			}
			return false;
		}
		bool bCrosses = false;
		for (int32 E1 = 0; E1 < Ring.Num() && !bCrosses; ++E1)
		{
			for (int32 E2 = E1 + 2; E2 < Ring.Num() && !bCrosses; ++E2)
			{
				if (E1 == 0 && E2 == Ring.Num() - 1)
				{
					continue;
				}
				bCrosses = SegmentsCross2D(Ring[E1], Ring[E1 + 1], Ring[E2], Ring[(E2 + 1) % Ring.Num()]);
			}
		}
		if (!bCrosses && TriangulatePolygon2D(Ring, Tris))
		{
			break;
		}
		if (!bRoadCurves)
		{
			return false;
		}
	}

	// Top and bottom: the outline triangulated (works for concave outlines too), planar UVs so the
	// asphalt matches across pieces.
	{
		// The top: points inside too, with smooth heights between the road ends, so an uneven intersection
		// is one even surface (no folds along long thin triangles). Small pieces keep the plain outline.
		TArray<FVector> TopPoints;
		TArray<int32> TopTris;
		if (!IsectSmoothTop(Ring, GridSpacing, TopPoints, TopTris))
		{
			TopPoints = Ring;
			TopTris = Tris;
		}
		// Normals from the surface (slopes are lit right), pointing up.
		TArray<FVector> Normals;
		Normals.Init(FVector::ZeroVector, TopPoints.Num());
		for (int32 T = 0; T + 2 < TopTris.Num(); T += 3)
		{
			const FVector& A = TopPoints[TopTris[T]];
			const FVector& B = TopPoints[TopTris[T + 1]];
			const FVector& C = TopPoints[TopTris[T + 2]];
			FVector FaceN = FVector::CrossProduct(B - A, C - A);
			if (FaceN.Z < 0.f)
			{
				FaceN = -FaceN;
			}
			for (int32 E = 0; E < 3; ++E)
			{
				Normals[TopTris[T + E]] += FaceN;
			}
		}
		const int32 Base = Top.Vertices.Num();
		for (int32 I = 0; I < TopPoints.Num(); ++I)
		{
			const FVector N = Normals[I].IsNearlyZero() ? FVector::UpVector : Normals[I].GetSafeNormal();
			Top.AddVertex(TopPoints[I], N, FVector2D(TopPoints[I].X, TopPoints[I].Y) * S, FVector::ForwardVector);
		}
		for (int32 T = 0; T + 2 < TopTris.Num(); T += 3)
		{
			Top.AddTriangle(Base + TopTris[T], Base + TopTris[T + 1], Base + TopTris[T + 2], FVector::UpVector);
		}
	}
	{
		const FVector Down(0.f, 0.f, -Height);
		const int32 Base = Body.Vertices.Num();
		for (const FVector& P : Ring)
		{
			Body.AddVertex(P + Down, FVector::DownVector, FVector2D(P.X, P.Y) * S, FVector::ForwardVector);
		}
		for (int32 T = 0; T + 2 < Tris.Num(); T += 3)
		{
			Body.AddTriangle(Base + Tris[T], Base + Tris[T + 1], Base + Tris[T + 2], FVector::DownVector);
		}
	}

	// Outward side of each edge, from the outline's winding (the centre can lie outside concave outlines).
	float RingArea = 0.f;
	for (int32 K = 0; K < Ring.Num(); ++K)
	{
		RingArea += Cross2D(Ring[K], Ring[(K + 1) % Ring.Num()]);
	}
	const float OutSign = RingArea >= 0.f ? 1.f : -1.f;

	// Curb sides, and an end face (cap) on every road mouth: the mouth stays closed when its road is
	// removed. Where a road is attached its own end cap meets this face inside the solid, unseen.
	float U = 0.f;
	for (int32 K = 0; K < Ring.Num(); ++K)
	{
		const FVector A = Ring[K];
		const FVector B = Ring[(K + 1) % Ring.Num()];
		const float EdgeLen = FVector::Dist(A, B);
		if (EdgeLen > 0.5f)
		{
			const FVector Edge = B - A;
			const FVector N = (FVector(Edge.Y, -Edge.X, 0.f) * OutSign).GetSafeNormal();
			const FVector Down(0.f, 0.f, -Height);
			const float U1 = U + EdgeLen * S;
			Body.AddQuad(A, B, B + Down, A + Down, N,
				FVector2D(U, 0.f), FVector2D(U1, 0.f), FVector2D(U1, Height * S), FVector2D(U, Height * S));
		}
		U += EdgeLen * S;
	}

	if (OutOutline)
	{
		*OutOutline = Ring;
	}
	if (OutEdgeIsMouth)
	{
		*OutEdgeIsMouth = EdgeIsMouth;
	}
	return true;
}

void RMGeometry::GetIntersectionCurbs(const TArray<FVector>& Outline, const TArray<bool>& EdgeIsMouth, TArray<TArray<FVector>>& OutCurbs)
{
	OutCurbs.Reset();
	const int32 N = Outline.Num();
	if (N < 3 || EdgeIsMouth.Num() != N)
	{
		return;
	}
	// Start right after a mouth edge, then collect runs of non-mouth edges.
	int32 Start = 0;
	while (Start < N && !EdgeIsMouth[Start])
	{
		++Start;
	}
	if (Start == N)
	{
		return;
	}
	TArray<FVector> Current;
	for (int32 Step = 1; Step <= N; ++Step)
	{
		const int32 E = (Start + Step) % N; // edge E goes Outline[E] -> Outline[E + 1]
		if (EdgeIsMouth[E])
		{
			if (Current.Num() >= 2)
			{
				OutCurbs.Add(Current);
			}
			Current.Reset();
			continue;
		}
		if (Current.Num() == 0)
		{
			Current.Add(Outline[E]);
		}
		Current.Add(Outline[(E + 1) % N]);
	}
	if (Current.Num() >= 2)
	{
		OutCurbs.Add(Current);
	}
}

namespace
{
	/** Height of a triangle mesh at an XY point (top-most hit), or Fallback when outside it. */
	float SurfaceHeightAt(const FRMMesh& Surface, const FVector& P, float Fallback)
	{
		float Best = -BIG_NUMBER;
		for (int32 T = 0; T + 2 < Surface.Triangles.Num(); T += 3)
		{
			const FVector& A = Surface.Vertices[Surface.Triangles[T]];
			const FVector& B = Surface.Vertices[Surface.Triangles[T + 1]];
			const FVector& C = Surface.Vertices[Surface.Triangles[T + 2]];
			const float Den = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);
			if (FMath::Abs(Den) < 1e-3f)
			{
				continue;
			}
			const float L1 = ((B.Y - C.Y) * (P.X - C.X) + (C.X - B.X) * (P.Y - C.Y)) / Den;
			const float L2 = ((C.Y - A.Y) * (P.X - C.X) + (A.X - C.X) * (P.Y - C.Y)) / Den;
			const float L3 = 1.f - L1 - L2;
			const float Tol = -1e-3f;
			if (L1 >= Tol && L2 >= Tol && L3 >= Tol)
			{
				Best = FMath::Max(Best, L1 * A.Z + L2 * B.Z + L3 * C.Z);
			}
		}
		return Best > -BIG_NUMBER ? Best : Fallback;
	}
}

void RMGeometry::BuildCurbLines(const TArray<TArray<FVector>>& Curbs, float Inset, const FRMMesh& Surface, FRMMesh& Out)
{
	Out.Reset();
	const float E = Inset / 100.f;            // inset in metres, as on road tops
	const float HalfStrip = 12.f;             // cm; the line itself is 15 cm wide, drawn by the material
	const float Lift = 1.5f;                  // cm above the slab, against z-fighting
	const float MaxPiece = 50.f;              // short pieces so the strip follows the slab's facets
	const FVector2D Layout(100.f, E);         // "road" so wide that only this one side line shows
	const FVector2D Style(1.f, 1.f);          // 1 lane (no dividers), side lines on

	for (const TArray<FVector>& Raw : Curbs)
	{
		if (Raw.Num() < 2)
		{
			continue;
		}
		// Subdivide long curb edges (straight corners can be several metres).
		TArray<FVector> Curb;
		Curb.Add(Raw[0]);
		for (int32 I = 1; I < Raw.Num(); ++I)
		{
			const int32 Pieces = FMath::Max(1, FMath::CeilToInt(FVector::Dist(Raw[I - 1], Raw[I]) / MaxPiece));
			for (int32 K = 1; K <= Pieces; ++K)
			{
				Curb.Add(FMath::Lerp(Raw[I - 1], Raw[I], float(K) / Pieces));
			}
		}
		const int32 N = Curb.Num();
		const int32 Base = Out.Vertices.Num();
		for (int32 I = 0; I < N; ++I)
		{
			const FVector Prev = Curb[FMath::Max(I - 1, 0)];
			const FVector Next = Curb[FMath::Min(I + 1, N - 1)];
			FVector Fwd = Next - Prev;
			Fwd.Z = 0.f;
			if (!Fwd.Normalize())
			{
				Fwd = FVector::ForwardVector;
			}
			// Curbs run so the intersection centre is on their +Right side.
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
			const FVector Mid = Curb[I] + Right * Inset;
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const float Off = Side == 0 ? -HalfStrip : HalfStrip;
				FVector P = Mid + Right * Off;
				P.Z = SurfaceHeightAt(Surface, P, Curb[I].Z) + Lift;
				Out.AddVertex(P, FVector::UpVector, FVector2D(P.X, P.Y) / 400.f, Fwd);
				Out.MarkUV1.Add(FVector2D(E + Off / 100.f, 0.f));
				Out.MarkUV2.Add(Layout);
				Out.MarkUV3.Add(Style);
			}
		}
		for (int32 I = 0; I + 1 < N; ++I)
		{
			const int32 A = Base + I * 2;
			const int32 B = A + 2;
			Out.AddTriangle(A, B, B + 1, FVector::UpVector);
			Out.AddTriangle(A, B + 1, A + 1, FVector::UpVector);
		}
	}
}
