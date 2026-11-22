#include "Buildables/RMBuildableTunnel.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMSwatchGroupRoads.h"
#include "ProceduralMeshComponent.h"
#include "RoadMakerModule.h"
#include "Net/UnrealNetwork.h"
#include "FGFactoryColoringTypes.h"

ARMBuildableTunnel::ARMBuildableTunnel(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Paintable like roads (the concrete blocks read the paint as the roads' do).
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();
}

void ARMBuildableTunnel::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableTunnel, mLeft);
	DOREPLIFETIME(ARMBuildableTunnel, mRight);
	DOREPLIFETIME(ARMBuildableTunnel, mArcDeg);
}

void ARMBuildableTunnel::SetShape(const TArray<FVector>& Left, const TArray<FVector>& Right, float ArcDeg)
{
	const int32 N = FMath::Min(Left.Num(), Right.Num());
	mLeft = TArray<FVector>(Left.GetData(), N);
	mRight = TArray<FVector>(Right.GetData(), N);
	mArcDeg = FMath::Clamp(ArcDeg, MinArc, MaxArc);
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

float ARMBuildableTunnel::RiseFor(float Width, float ArcDeg)
{
	// Chord c, half angle a = arc / 2: radius c / (2 sin a), rise r (1 - cos a) = c / 2 * tan(a / 2).
	return Width * 0.5f * FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(ArcDeg, 1.f, 180.f)) * 0.25f);
}

float ARMBuildableTunnel::ArcForRise(float Width, float Rise)
{
	return FMath::RadiansToDegrees(4.f * FMath::Atan(2.f * FMath::Max(Rise, 0.f) / FMath::Max(Width, 1.f)));
}

namespace RMTunnelMesh
{
	/** One cross-section of the arch: a circle through both feet in the upright plane of the chord. */
	struct FSection
	{
		FVector Centre = FVector::ZeroVector;
		FVector Chord = FVector::RightVector; // left foot -> right foot
		FVector Up = FVector::UpVector;       // perpendicular to the chord, upwards
		float Radius = 100.f;
		float Half = PI * 0.5f;               // half the arc (radians)

		FSection(const FVector& L, const FVector& R, float ArcDeg)
		{
			const FVector C = R - L;
			const float Width = FMath::Max(C.Size(), 1.f);
			Chord = C / Width;
			Up = (FVector::UpVector - Chord * FVector::DotProduct(FVector::UpVector, Chord)).GetSafeNormal();
			if (Up.IsNearlyZero())
			{
				Up = FVector::UpVector;
			}
			Half = FMath::DegreesToRadians(FMath::Clamp(ArcDeg, ARMBuildableTunnel::MinArc, ARMBuildableTunnel::MaxArc)) * 0.5f;
			Radius = Width / (2.f * FMath::Sin(Half));
			Centre = (L + R) * 0.5f - Up * Radius * FMath::Cos(Half);
		}

		float Angle(int32 K, int32 N) const { return -Half + 2.f * Half * K / N; }
		FVector Dir(float Phi) const { return Chord * FMath::Sin(Phi) + Up * FMath::Cos(Phi); }
		/** Along the arc towards the right foot. */
		FVector Tangent(float Phi) const { return Chord * FMath::Cos(Phi) - Up * FMath::Sin(Phi); }
		FVector Point(float Phi, float Offset) const { return Centre + Dir(Phi) * (Radius + Offset); }
		float ArcLength() const { return Radius * 2.f * Half; }
	};

	/** A vertex with the blocks' UV1 (metres along, around: the walking path's blocks turned 90 degrees). */
	int32 Vertex(FRMMesh& Out, const FVector& P, const FVector& N, float AlongM, float AroundM)
	{
		const int32 I = Out.AddVertex(P, N, FVector2D(AroundM / 4.f, AlongM / 4.f), FVector::CrossProduct(N, FVector::UpVector).GetSafeNormal());
		Out.MarkUV1.Add(FVector2D(AlongM, AroundM));
		Out.MarkUV2.Add(FVector2D(10.f, 0.f)); // > 0.5: the material uses UV1 (not a world projection)
		Out.MarkUV3.Add(FVector2D::ZeroVector); // no lane lines
		return I;
	}

	void Quad(FRMMesh& Out, int32 A, int32 B, int32 C, int32 D, const FVector& N)
	{
		Out.AddTriangle(A, B, C, N);
		Out.AddTriangle(A, C, D, N);
	}
}

void ARMBuildableTunnel::BuildTunnelMesh(const TArray<FVector>& Left, const TArray<FVector>& Right, float ArcDeg, int32 NumSegments, FRMMesh& Out)
{
	using namespace RMTunnelMesh;
	const int32 N = FMath::Min(Left.Num(), Right.Num());
	if (N < 2)
	{
		return;
	}
	const int32 S = FMath::Max(NumSegments, 2);
	const float HalfT = Thickness * 0.5f;
	TArray<FSection> Sections;
	TArray<float> Along; // metres along the middle line
	for (int32 I = 0; I < N; ++I)
	{
		Sections.Emplace(Left[I], Right[I], ArcDeg);
		const float D = I == 0 ? 0.f : FVector::Dist((Left[I] + Right[I]) * 0.5f, (Left[I - 1] + Right[I - 1]) * 0.5f) / 100.f;
		Along.Add(I == 0 ? 0.f : Along.Last() + D);
	}

	// Vertices shared between neighbouring quads: one ring of points per cross-section for each face (inner, outer) and
	// one pair per foot. A ring is only made again where the blocks' "metres along" are counted from a new start: UVs are
	// half floats, so on long tunnels they restart every 64 m or so (at a whole block row, the pattern seamless).
	constexpr float UVRestart = 64.f;
	const int32 RingSize = S + 1;
	auto MakeRing = [&](int32 I, float Base, TArray<int32>& OutRing)
	{
		const FSection& Sec = Sections[I];
		const float U = Along[I] - Base;
		OutRing.Reset();
		for (const float Offset : { -HalfT, HalfT })
		{
			const float Sign = Offset < 0.f ? -1.f : 1.f;
			for (int32 K = 0; K <= S; ++K)
			{
				const float Phi = Sec.Angle(K, S);
				OutRing.Add(Vertex(Out, Sec.Point(Phi, Offset), Sec.Dir(Phi) * Sign, U, (Phi + Sec.Half) * (Sec.Radius + Offset) / 100.f));
			}
		}
		// The feet: inner and outer corner of each (flat, facing down along the arc).
		for (const int32 K : { 0, S })
		{
			const float Phi = Sec.Angle(K, S);
			const FVector Foot = K == 0 ? -Sec.Tangent(Phi) : Sec.Tangent(Phi);
			OutRing.Add(Vertex(Out, Sec.Point(Phi, -HalfT), Foot, U, 0.f));
			OutRing.Add(Vertex(Out, Sec.Point(Phi, HalfT), Foot, U, Thickness / 100.f));
		}
	};
	float Base = 0.f;
	TArray<int32> RingA, RingB;
	MakeRing(0, Base, RingA);
	for (int32 I = 0; I + 1 < N; ++I)
	{
		if (Along[I + 1] - Base > UVRestart)
		{
			Base = FMath::FloorToFloat(Along[I] / 4.f) * 4.f;
			MakeRing(I, Base, RingA); // this cross-section again, counted from the new start
		}
		MakeRing(I + 1, Base, RingB);
		const FSection& A = Sections[I];
		for (int32 Face = 0; Face < 2; ++Face)
		{
			const float Sign = Face == 0 ? -1.f : 1.f;
			for (int32 K = 0; K < S; ++K)
			{
				const int32 J = Face * RingSize + K;
				Quad(Out, RingA[J], RingA[J + 1], RingB[J + 1], RingB[J], A.Dir((A.Angle(K, S) + A.Angle(K + 1, S)) * 0.5f) * Sign);
			}
		}
		for (int32 Foot = 0; Foot < 2; ++Foot)
		{
			const int32 J = 2 * RingSize + Foot * 2;
			const float Phi = A.Angle(Foot == 0 ? 0 : S, S);
			Quad(Out, RingA[J], RingA[J + 1], RingB[J + 1], RingB[J], Foot == 0 ? -A.Tangent(Phi) : A.Tangent(Phi));
		}
		Swap(RingA, RingB);
	}

	// The two cut ends: the arch's cross-section, facing out of the tunnel.
	for (const int32 I : { 0, N - 1 })
	{
		const FSection& A = Sections[I];
		const FVector Mid = (Left[I] + Right[I]) * 0.5f;
		const FVector MidNext = (Left[I == 0 ? 1 : N - 2] + Right[I == 0 ? 1 : N - 2]) * 0.5f;
		const FVector N0 = (Mid - MidNext).GetSafeNormal();
		for (int32 K = 0; K < S; ++K)
		{
			const float P0 = A.Angle(K, S), P1 = A.Angle(K + 1, S);
			const float S0 = (P0 + A.Half) * A.Radius / 100.f, S1 = (P1 + A.Half) * A.Radius / 100.f;
			const int32 V0 = Vertex(Out, A.Point(P0, -HalfT), N0, -HalfT / 100.f, S0);
			const int32 V1 = Vertex(Out, A.Point(P0, HalfT), N0, HalfT / 100.f, S0);
			const int32 V2 = Vertex(Out, A.Point(P1, HalfT), N0, HalfT / 100.f, S1);
			const int32 V3 = Vertex(Out, A.Point(P1, -HalfT), N0, -HalfT / 100.f, S1);
			Quad(Out, V0, V1, V2, V3, N0);
		}
	}
}

int32 ARMBuildableTunnel::CostUnits(const TArray<FVector>& Left, const TArray<FVector>& Right, float ArcDeg)
{
	const int32 N = FMath::Min(Left.Num(), Right.Num());
	float Area = 0.f;
	for (int32 I = 0; I + 1 < N; ++I)
	{
		const RMTunnelMesh::FSection A(Left[I], Right[I], ArcDeg);
		const RMTunnelMesh::FSection B(Left[I + 1], Right[I + 1], ArcDeg);
		const float Len = FVector::Dist((Left[I] + Right[I]) * 0.5f, (Left[I + 1] + Right[I + 1]) * 0.5f);
		Area += (A.ArcLength() + B.ArcLength()) * 0.5f * Len;
	}
	return FMath::Max(1, FMath::CeilToInt(Area / (400.f * 400.f)));
}

void ARMBuildableTunnel::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Shell;
	BuildTunnelMesh(mLeft, mRight, mArcDeg, Segments, Shell);
	if (Shell.IsEmpty())
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: no tunnel shell from %d / %d points"), *GetName(), mLeft.Num(), mRight.Num());
		return;
	}
	Shell.ApplyToSection(mMesh, 0, true);
	ApplyMaterial(0, mShellMaterial);
	FRMMesh Far;
	BuildTunnelMesh(mLeft, mRight, mArcDeg, FarSegments, Far);
	ApplyFarSection(Far, 0, mShellMaterial);
	ApplyPaint(mCustomizationData);
	UpdateLODDistances();
}

void ARMBuildableTunnel::ApplyPaint(const FFactoryCustomizationData& Data)
{
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	ForEachMeshComponent([&P, &S](UProceduralMeshComponent* M)
	{
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
	});
}

void ARMBuildableTunnel::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyPaint(customizationData);
}

void ARMBuildableTunnel::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyPaint(customizationData);
}

FVector ARMBuildableTunnel::GetDirectionAt(const FVector& World) const
{
	const FTransform& T = GetActorTransform();
	const int32 N = FMath::Min(mLeft.Num(), mRight.Num());
	int32 Best = 0;
	float BestD = TNumericLimits<float>::Max();
	for (int32 I = 0; I < N; ++I)
	{
		const float D = FVector::DistSquared2D(T.TransformPosition((mLeft[I] + mRight[I]) * 0.5f), World);
		if (D < BestD)
		{
			BestD = D;
			Best = I;
		}
	}
	if (N < 2)
	{
		return GetActorForwardVector();
	}
	const int32 A = FMath::Max(Best - 1, 0), B = FMath::Min(Best + 1, N - 1);
	const FVector Dir = T.TransformVectorNoScale((mLeft[B] + mRight[B]) * 0.5f - (mLeft[A] + mRight[A]) * 0.5f);
	const FVector Level(Dir.X, Dir.Y, 0.f);
	return Level.IsNearlyZero() ? GetActorForwardVector() : Level.GetSafeNormal();
}

bool ARMBuildableTunnel::GetAttachFace(const FVector& WorldHit, const FVector& ImpactNormal, FVector& OutLocation, FVector& OutNormal,
	FVector& OutUp, float& OutDepth) const
{
	const int32 N = FMath::Min(mLeft.Num(), mRight.Num());
	if (N < 2)
	{
		return false;
	}
	const FTransform& T = GetActorTransform();
	const FVector P = T.InverseTransformPosition(WorldHit);
	// The cross-section through the aim: between the two nearest ones, by the middle line.
	int32 Seg = 0;
	float Frac = 0.f;
	float Best = TNumericLimits<float>::Max();
	for (int32 I = 0; I + 1 < N; ++I)
	{
		const FVector A = (mLeft[I] + mRight[I]) * 0.5f;
		const FVector B = (mLeft[I + 1] + mRight[I + 1]) * 0.5f;
		const FVector2D AB(B - A), AP(P - A);
		const float Len2 = AB.SizeSquared();
		const float F = Len2 > 1e-4f ? FMath::Clamp(FVector2D::DotProduct(AP, AB) / Len2, 0.f, 1.f) : 0.f;
		const float D = FVector::DistSquared2D(FMath::Lerp(A, B, F), P);
		if (D < Best)
		{
			Best = D;
			Seg = I;
			Frac = F;
		}
	}
	const FVector L = FMath::Lerp(mLeft[Seg], mLeft[Seg + 1], Frac);
	const FVector R = FMath::Lerp(mRight[Seg], mRight[Seg + 1], Frac);
	const FVector Along = ((mLeft[Seg + 1] + mRight[Seg + 1]) - (mLeft[Seg] + mRight[Seg])).GetSafeNormal();
	const RMTunnelMesh::FSection S(L, R, mArcDeg);
	// Where around the arch, snapped to the nearest eighth of the arc.
	const FVector V = P - S.Centre;
	const float Phi = FMath::Atan2(FVector::DotProduct(V, S.Chord), FVector::DotProduct(V, S.Up));
	const float Step = 2.f * S.Half / (AttachPoints - 1);
	const int32 K = FMath::Clamp(FMath::RoundToInt((FMath::Clamp(Phi, -S.Half, S.Half) + S.Half) / Step), 0, AttachPoints - 1);
	const float SnapPhi = -S.Half + K * Step;
	const FVector Dir = S.Dir(SnapPhi);
	// Inside or outside: the face hit (its normal), else which side of the middle of the shell the aim is.
	const FVector LocalNormal = T.InverseTransformVectorNoScale(ImpactNormal);
	float Out = FVector::DotProduct(LocalNormal, S.Dir(Phi));
	if (FMath::Abs(Out) < 0.3f)
	{
		Out = V.Size() - S.Radius;
	}
	const float Sign = Out >= 0.f ? 1.f : -1.f;
	const FVector Normal = Dir * Sign;
	const FVector Face = S.Point(SnapPhi, Sign * (Thickness * 0.5f + 1.f));
	// "Up" in the face: towards the top of the arch, square to the tunnel's course (so a sign on a climbing tunnel leans
	// with its slope, like on a climbing fence wall); at the top, along the tunnel.
	FVector Up = Along;
	if (K * 2 != AttachPoints - 1)
	{
		const FVector TowardsTop = SnapPhi < 0.f ? S.Tangent(SnapPhi) : -S.Tangent(SnapPhi);
		Up = FVector::CrossProduct(Normal, Along);
		if (FVector::DotProduct(Up, TowardsTop) < 0.f)
		{
			Up = -Up;
		}
	}
	Up = (Up - Normal * FVector::DotProduct(Up, Normal)).GetSafeNormal();
	OutLocation = T.TransformPosition(Face);
	OutNormal = T.TransformVectorNoScale(Normal);
	OutUp = T.TransformVectorNoScale(Up.IsNearlyZero() ? Along : Up);
	OutDepth = Thickness * 0.5f + 1.f;
	return true;
}

void ARMBuildableTunnel::Densify(const TArray<FVector>& InLeft, const TArray<FVector>& InRight, float Step, TArray<FVector>& OutLeft,
	TArray<FVector>& OutRight)
{
	OutLeft.Reset();
	OutRight.Reset();
	const int32 N = FMath::Min(InLeft.Num(), InRight.Num());
	for (int32 I = 0; I < N; ++I)
	{
		if (I > 0)
		{
			const float Len = FVector::Dist((InLeft[I] + InRight[I]) * 0.5f, (InLeft[I - 1] + InRight[I - 1]) * 0.5f);
			const int32 Parts = FMath::Max(1, FMath::CeilToInt(Len / FMath::Max(Step, 1.f)));
			for (int32 J = 1; J < Parts; ++J)
			{
				const float F = float(J) / Parts;
				OutLeft.Add(FMath::Lerp(InLeft[I - 1], InLeft[I], F));
				OutRight.Add(FMath::Lerp(InRight[I - 1], InRight[I], F));
			}
		}
		OutLeft.Add(InLeft[I]);
		OutRight.Add(InRight[I]);
	}
}

void ARMBuildableTunnel::Thin(TArray<FVector>& Left, TArray<FVector>& Right, float Tol, float MaxGap)
{
	const int32 N = FMath::Min(Left.Num(), Right.Num());
	if (N <= 2)
	{
		return;
	}
	auto Deviates = [&](int32 From, int32 To)
	{
		for (int32 M = From + 1; M < To; ++M)
		{
			const float F = float(M - From) / float(To - From);
			if (FVector::Dist(FMath::Lerp(Left[From], Left[To], F), Left[M]) > Tol
				|| FVector::Dist(FMath::Lerp(Right[From], Right[To], F), Right[M]) > Tol)
			{
				return true;
			}
		}
		return false;
	};
	TArray<FVector> OutL, OutR;
	int32 Kept = 0;
	OutL.Add(Left[0]);
	OutR.Add(Right[0]);
	for (int32 J = 2; J <= N; ++J)
	{
		if (J == N || Deviates(Kept, J) || FVector::Dist(Left[Kept], Left[J]) > MaxGap)
		{
			Kept = J - 1;
			OutL.Add(Left[Kept]);
			OutR.Add(Right[Kept]);
		}
	}
	Left = MoveTemp(OutL);
	Right = MoveTemp(OutR);
}
