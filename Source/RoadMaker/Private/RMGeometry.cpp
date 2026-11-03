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

void FRMMesh::ApplyToSection(UProceduralMeshComponent* Comp, int32 Section, bool bCollision) const
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
		NoColors, Tangents, bCollision);
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
	for (int32 I = 1; I + 1 < Profile.Num(); ++I)
	{
		Out.AddTriangle(Base, Base + I, Base + I + 1, N);
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
	ExtrudeProfile(Frames, Profile, true, FVector2D(0.f, 30.f), UVScale, Concrete);
	AddProfileCap(Frames[0], Profile, false, UVScale, Concrete);
	AddProfileCap(Frames.Last(), Profile, true, UVScale, Concrete);
}

void RMGeometry::BuildGridFence(const TArray<FRMFrame>& Frames, float UVScale, FRMMesh& Posts, FRMMesh& Panel)
{
	// Thin double-faced panel 2 m tall on the road side, uses its own material slot (masked grid).
	const TArray<FVector2D> PanelProfile = { FVector2D(2.f, 5.f), FVector2D(4.f, 5.f), FVector2D(4.f, 200.f), FVector2D(2.f, 200.f) };
	ExtrudeProfile(Frames, PanelProfile, true, FVector2D(3.f, 100.f), 200.f, Panel);

	// Top and bottom tubes plus posts every 3 m, behind the panel (away from the road).
	const TArray<FVector2D> Tube = { FVector2D(-6.f, -3.f), FVector2D(0.f, -3.f), FVector2D(0.f, 3.f), FVector2D(-6.f, 3.f) };
	const TArray<FRMFrame> TopFrames = LiftFrames(Frames, 200.f);
	const TArray<FRMFrame> BottomFrames = LiftFrames(Frames, 8.f);
	ExtrudeProfile(TopFrames, Tube, true, FVector2D::ZeroVector, UVScale, Posts);
	ExtrudeProfile(BottomFrames, Tube, true, FVector2D::ZeroVector, UVScale, Posts);
	ForEvery(Frames, 300.f, [&](const FRMFrame& F)
	{
		Posts.AddBox(F.Pos + FVector(0.f, 0.f, 105.f) - F.Right * 3.f, F.Fwd, F.Right, FVector::UpVector, FVector(5.f, 5.f, 105.f), UVScale);
	});
}

// ---------------------------------------------------------------------------------------------
// Intersections
// ---------------------------------------------------------------------------------------------

bool RMGeometry::BuildIntersection(const TArray<FRMRoadEnd>& Ends, float Height, float UVScale, int32 FilletSegments,
	FRMMesh& Top, FRMMesh& Body, TArray<FVector>* OutOutline, TArray<bool>* OutEdgeIsMouth)
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

	// Outline: road mouth (First -> Second) then a curb fillet to the next road's First corner.
	TArray<FVector> Ring;
	TArray<bool> EdgeIsMouth; // EdgeIsMouth[k] covers the edge Ring[k] -> Ring[k + 1]
	for (int32 I = 0; I < Sorted.Num(); ++I)
	{
		const FSortedEnd& Cur = Sorted[I];
		const FSortedEnd& Next = Sorted[(I + 1) % Sorted.Num()];

		Ring.Add(Cur.First);
		EdgeIsMouth.Add(true);
		Ring.Add(Cur.Second);
		EdgeIsMouth.Add(false);

		// Control point: where the edge of this road meets the edge of the next one, extended into the junction.
		const FVector P = Cur.Second;
		const FVector Q = Next.First;
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

		for (int32 K = 1; K < FilletSegments; ++K)
		{
			const float T = static_cast<float>(K) / FilletSegments;
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
		return false;
	}

	// The fan from the centre only works if no edge turns backwards around it (straight is fine).
	for (int32 K = 0; K < Ring.Num(); ++K)
	{
		const FVector& A = Ring[K];
		const FVector& B = Ring[(K + 1) % Ring.Num()];
		if (Cross2D(A - Center, B - Center) < -1.f)
		{
			return false;
		}
	}

	// Top: fan around the centre, planar UVs so the asphalt matches across pieces.
	{
		const int32 C = Top.AddVertex(Center, FVector::UpVector, FVector2D(Center.X, Center.Y) * S, FVector::ForwardVector);
		const int32 Base = Top.Vertices.Num();
		for (const FVector& P : Ring)
		{
			Top.AddVertex(P, FVector::UpVector, FVector2D(P.X, P.Y) * S, FVector::ForwardVector);
		}
		for (int32 K = 0; K < Ring.Num(); ++K)
		{
			Top.AddTriangle(C, Base + K, Base + (K + 1) % Ring.Num(), FVector::UpVector);
		}
	}

	// Bottom: same fan, lowered and facing down.
	{
		const FVector Down(0.f, 0.f, -Height);
		const int32 C = Body.AddVertex(Center + Down, FVector::DownVector, FVector2D(Center.X, Center.Y) * S, FVector::ForwardVector);
		const int32 Base = Body.Vertices.Num();
		for (const FVector& P : Ring)
		{
			Body.AddVertex(P + Down, FVector::DownVector, FVector2D(P.X, P.Y) * S, FVector::ForwardVector);
		}
		for (int32 K = 0; K < Ring.Num(); ++K)
		{
			Body.AddTriangle(C, Base + K, Base + (K + 1) % Ring.Num(), FVector::DownVector);
		}
	}

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
			const FVector Mid = (A + B) * 0.5f;
			FVector N = FVector::CrossProduct(B - A, FVector::UpVector).GetSafeNormal();
			if (FVector::DotProduct(N, Mid - Center) < 0.f)
			{
				N = -N;
			}
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
