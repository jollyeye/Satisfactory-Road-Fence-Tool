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

	for (int32 I = 0; I + 1 < N; ++I)
	{
		const FSection& A = Sections[I];
		const FSection& B = Sections[I + 1];
		// Metres along counted from a whole block row below this stretch (UVs are half floats: long tunnels band).
		const float Base = FMath::FloorToFloat(Along[I] / 4.f) * 4.f;
		const float UA = Along[I] - Base;
		const float UB = Along[I + 1] - Base;
		// Inner (Offset -HalfT, facing in) and outer (+HalfT, facing out) faces.
		for (const float Offset : { -HalfT, HalfT })
		{
			const float Sign = Offset < 0.f ? -1.f : 1.f;
			for (int32 K = 0; K < S; ++K)
			{
				const float PA0 = A.Angle(K, S), PA1 = A.Angle(K + 1, S);
				const float PB0 = B.Angle(K, S), PB1 = B.Angle(K + 1, S);
				const float SA0 = (PA0 + A.Half) * (A.Radius + Offset) / 100.f, SA1 = (PA1 + A.Half) * (A.Radius + Offset) / 100.f;
				const float SB0 = (PB0 + B.Half) * (B.Radius + Offset) / 100.f, SB1 = (PB1 + B.Half) * (B.Radius + Offset) / 100.f;
				const int32 V0 = Vertex(Out, A.Point(PA0, Offset), A.Dir(PA0) * Sign, UA, SA0);
				const int32 V1 = Vertex(Out, A.Point(PA1, Offset), A.Dir(PA1) * Sign, UA, SA1);
				const int32 V2 = Vertex(Out, B.Point(PB1, Offset), B.Dir(PB1) * Sign, UB, SB1);
				const int32 V3 = Vertex(Out, B.Point(PB0, Offset), B.Dir(PB0) * Sign, UB, SB0);
				const float PM = (PA0 + PA1) * 0.5f;
				Quad(Out, V0, V1, V2, V3, A.Dir(PM) * Sign);
			}
		}
		// The two feet (flat strips across the thickness, facing down along the arc).
		for (const int32 K : { 0, S })
		{
			const float PA = A.Angle(K, S), PB = B.Angle(K, S);
			const FVector N = K == 0 ? -A.Tangent(PA) : A.Tangent(PA);
			const int32 V0 = Vertex(Out, A.Point(PA, -HalfT), N, UA, 0.f);
			const int32 V1 = Vertex(Out, A.Point(PA, HalfT), N, UA, Thickness / 100.f);
			const int32 V2 = Vertex(Out, B.Point(PB, HalfT), N, UB, Thickness / 100.f);
			const int32 V3 = Vertex(Out, B.Point(PB, -HalfT), N, UB, 0.f);
			Quad(Out, V0, V1, V2, V3, N);
		}
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
