#include "Buildables/RMBuildableIntersection.h"
#include "ProceduralMeshComponent.h"
#include "RoadMakerModule.h"
#include "Net/UnrealNetwork.h"
#include "FGFactoryColoringTypes.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMSwatchGroupRoads.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Materials/MaterialInterface.h"
#include "RMRoadUtils.h"
#include "EngineUtils.h"

ARMBuildableIntersection::ARMBuildableIntersection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Paintable like roads (same "Roads" default swatch), for the side line colour.
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();
}

void ARMBuildableIntersection::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableIntersection, mEnds);
	DOREPLIFETIME(ARMBuildableIntersection, mHeight);
	DOREPLIFETIME(ARMBuildableIntersection, mSideLines);
	DOREPLIFETIME(ARMBuildableIntersection, mSideSlopes);
	DOREPLIFETIME(ARMBuildableIntersection, mSlopeSamples);
}

void ARMBuildableIntersection::SetEnds(const TArray<FRMRoadEnd>& LocalEnds, float Height)
{
	mEnds = LocalEnds;
	mHeight = Height;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::OnRep_Ends()
{
	RebuildMesh();
}

void ARMBuildableIntersection::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	// Side slopes first: their curb stretches leave out the slab's side there.
	RequestSlopeMeasure();
	FRMMesh Slopes;
	TArray<bool> Covered;
	if (mSideSlopes && mSlopeSamples.Num() > 0)
	{
		TArray<FVector> SlopeRing;
		TArray<bool> SlopeMouth;
		if (LocalOutline(SlopeRing, SlopeMouth))
		{
			BuildSlopeMeshFrom(SlopeRing, SlopeMouth, mSlopeSamples, Slopes, &Covered);
		}
	}
	FRMMesh Top, Body;
	TArray<FVector> Outline;
	TArray<bool> IsMouth;
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Outline, &IsMouth, 250.f, GetShownLook() == 2,
		Covered.Num() > 0 ? &Covered : nullptr))
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: could not rebuild intersection from %d saved ends"), *GetName(), mEnds.Num());
		return;
	}
	// A broken point (NaN) would break the bounds, collision and the build sound: no mesh then, and the ends logged.
	auto HasBadPoint = [](const FRMMesh& M)
	{
		for (const FVector& V : M.Vertices)
		{
			if (V.ContainsNaN() || V.GetAbsMax() > 1.0e7)
			{
				return true;
			}
		}
		return false;
	};
	if (HasBadPoint(Top) || HasBadPoint(Body))
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: intersection mesh has broken points, not shown. Ends:"), *GetName());
		for (const FRMRoadEnd& E : mEnds)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("  centre %s inward %s width %.0f"), *E.Center.ToString(), *E.Inward.ToString(), E.Width);
		}
		return;
	}
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);

	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyMaterial(0, TopMat);
	ApplyMaterial(1, BodyMat);
	Slopes.ApplyToSection(mMesh, 3, true);
	if (!Slopes.IsEmpty())
	{
		ApplyMaterial(3, BodyMat);
	}

	if (mPreviewSideLines.Get(mSideLines))
	{
		TArray<TArray<FVector>> Curbs;
		RMGeometry::GetIntersectionCurbs(Outline, IsMouth, Curbs);
		FRMMesh Lines;
		RMGeometry::BuildCurbLines(Curbs, mEdgeLineInset, Top, Lines);
		Lines.ApplyToSection(mMesh, 2, false);
		ApplyMaterial(2, TopMat);
	}

	// Far version: 3 points per curb fillet instead of mFilletSegments.
	FRMMesh FarTop, FarBody;
	TArray<FVector> FarOutline;
	TArray<bool> FarIsMouth;
	if (RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, FMath::Min(mFilletSegments, 3), FarTop, FarBody, &FarOutline, &FarIsMouth, 500.f, GetShownLook() == 2))
	{
		ApplyFarSection(FarTop, 0, TopMat);
		ApplyFarSection(FarBody, 1, BodyMat);
		if (!Slopes.IsEmpty())
		{
			ApplyFarSection(Slopes, 3, BodyMat);
		}
		if (mPreviewSideLines.Get(mSideLines))
		{
			TArray<TArray<FVector>> FarCurbs;
			RMGeometry::GetIntersectionCurbs(FarOutline, FarIsMouth, FarCurbs);
			FRMMesh FarLines;
			RMGeometry::BuildCurbLines(FarCurbs, mEdgeLineInset, FarTop, FarLines);
			ApplyFarSection(FarLines, 2, TopMat);
		}
	}
	ApplyMarkingColors(mCustomizationData);
}

float ARMBuildableIntersection::DistanceToMouths(const FVector& P) const
{
	float Best = TNumericLimits<float>::Max();
	for (const FRMRoadEnd& E : mEnds)
	{
		const FVector In = FVector(E.Inward.X, E.Inward.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, In);
		const FVector A = E.Center - Right * E.Width * 0.5f;
		const FVector B = E.Center + Right * E.Width * 0.5f;
		const FVector Closest = FMath::ClosestPointOnSegment(FVector(P.X, P.Y, 0.f), FVector(A.X, A.Y, 0.f), FVector(B.X, B.Y, 0.f));
		Best = FMath::Min(Best, FVector::Dist2D(P, Closest));
	}
	return Best == TNumericLimits<float>::Max() ? 0.f : Best;
}

void ARMBuildableIntersection::SetSideLines(bool bOn)
{
	if (mSideLines == bOn)
	{
		return;
	}
	mSideLines = bOn;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::SetPreviewSideLines(TOptional<bool> Preview)
{
	if (Preview == mPreviewSideLines)
	{
		return;
	}
	mPreviewSideLines = Preview;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::ApplyMarkingColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh)
	{
		return;
	}
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	FLinearColor P = Resolved.PrimaryColor;
	FLinearColor S = Resolved.SecondaryColor;
	AdjustColorsForLook(Data, P, S);
	for (UProceduralMeshComponent* M : { mMesh, mFarMesh })
	{
		if (M)
		{
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		}
	}
}

void ARMBuildableIntersection::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableIntersection::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableIntersection::GetWorldEnds(TArray<FRMRoadEnd>& OutEnds) const
{
	OutEnds.Reset();
	const FTransform& T = GetActorTransform();
	for (const FRMRoadEnd& E : mEnds)
	{
		FRMRoadEnd W = E;
		W.Center = T.TransformPosition(E.Center);
		W.Inward = T.TransformVectorNoScale(E.Inward);
		OutEnds.Add(W);
	}
}

bool ARMBuildableIntersection::GetWorldOutline(TArray<FVector>& OutOutline, TArray<bool>& OutIsMouth) const
{
	OutOutline.Reset();
	OutIsMouth.Reset();
	FRMMesh Top, Body;
	// A coarse inside grid: only the outline is wanted.
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &OutOutline, &OutIsMouth, 2000.f))
	{
		return false;
	}
	const FTransform& T = GetActorTransform();
	for (FVector& P : OutOutline)
	{
		P = T.TransformPosition(P);
	}
	return OutOutline.Num() >= 3;
}

uint32 ARMBuildableIntersection::GetShapeKey() const
{
	uint32 Key = GetTypeHash(GetActorTransform().GetLocation()) ^ (GetTypeHash(GetActorRotation().Yaw) * 31u) ^ (GetLook() * 7919u);
	for (const FRMRoadEnd& E : mEnds)
	{
		Key = HashCombine(Key, HashCombine(GetTypeHash(E.Center), GetTypeHash(E.Inward)));
	}
	return HashCombine(Key, GetTypeHash(mHeight));
}

void ARMBuildableIntersection::GetWorldCurbs(TArray<TArray<FVector>>& OutCurbs) const
{
	OutCurbs.Reset();
	FRMMesh Top, Body;
	TArray<FVector> Outline;
	TArray<bool> IsMouth;
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Outline, &IsMouth, 250.f, GetShownLook() == 2))
	{
		return;
	}
	RMGeometry::GetIntersectionCurbs(Outline, IsMouth, OutCurbs);
	const FTransform& T = GetActorTransform();
	for (TArray<FVector>& Curb : OutCurbs)
	{
		for (FVector& P : Curb)
		{
			P = T.TransformPosition(P);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Side slopes (1.6.0)
// ---------------------------------------------------------------------------------------------

bool ARMBuildableIntersection::LocalOutline(TArray<FVector>& Ring, TArray<bool>& IsMouth) const
{
	FRMMesh Top, Body;
	return RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Ring, &IsMouth, 2000.f)
		&& Ring.Num() >= 3 && IsMouth.Num() == Ring.Num();
}

void ARMBuildableIntersection::SlopePoints(const TArray<FVector>& Ring, const TArray<bool>& IsMouth, TArray<FSlopePoint>& Out, float& OutPerimeter)
{
	Out.Reset();
	OutPerimeter = 0.f;
	const int32 N = Ring.Num();
	if (N < 3 || IsMouth.Num() != N)
	{
		return;
	}
	// Outward side from the winding (as the slab's sides), and each edge's distance round the outline from point 0
	// (the slab's side blocks are counted the same way, so the joints run on into the banks).
	float Area = 0.f;
	TArray<float> EdgeS;
	for (int32 K = 0; K < N; ++K)
	{
		const FVector& A = Ring[K];
		const FVector& B = Ring[(K + 1) % N];
		Area += A.X * B.Y - B.X * A.Y;
		EdgeS.Add(OutPerimeter);
		OutPerimeter += FVector::Dist(A, B);
	}
	const float OutSign = Area >= 0.f ? 1.f : -1.f;
	auto Normal = [&](int32 K)
	{
		const FVector E = Ring[(K + 1) % N] - Ring[K];
		return (FVector(E.Y, -E.X, 0.f) * OutSign).GetSafeNormal();
	};
	int32 Start = 0;
	while (Start < N && !IsMouth[Start])
	{
		++Start;
	}
	if (Start == N)
	{
		return; // no road mouth: not an intersection outline
	}
	auto Point = [](const FVector& Pos, const FVector& Dir, float S, int32 Edge, int32 Run, bool bStart, bool bEnd, int32 Vertex = -1)
	{
		FSlopePoint P;
		P.Vertex = Vertex;
		P.Pos = Pos;
		P.Out = Dir;
		P.S = S;
		P.Edge = Edge;
		P.Run = Run;
		P.bRunStart = bStart;
		P.bRunEnd = bEnd;
		return P;
	};
	int32 Run = -1;
	for (int32 Step = 1; Step <= N; ++Step)
	{
		const int32 K = (Start + Step) % N;
		if (IsMouth[K])
		{
			continue;
		}
		const FVector A = Ring[K];
		const FVector B = Ring[(K + 1) % N];
		const float Len = FVector::Dist(A, B);
		const FVector NK = Normal(K);
		// Along the mouth's line, away from the mouth (level): the joined road's sideways direction at its end.
		auto AlongMouth = [&](int32 Corner, int32 OtherCorner, const FVector& Fallback)
		{
			const FVector D = Ring[Corner] - Ring[OtherCorner];
			const FVector L = FVector(D.X, D.Y, 0.f).GetSafeNormal();
			return L.IsNearlyZero() ? Fallback : L;
		};
		if (IsMouth[(K + N - 1) % N] || Run < 0)
		{
			++Run;
			const FVector StartOut = IsMouth[(K + N - 1) % N] ? AlongMouth(K, (K + N - 1) % N, NK) : NK;
			Out.Add(Point(A, StartOut, EdgeS[K], K, Run, true, false, K));
		}
		const int32 Parts = FMath::Max(1, FMath::CeilToInt(Len / ARMBuildableRoad::SlopeSampleStep));
		for (int32 I = 1; I < Parts; ++I)
		{
			const float T = float(I) / Parts;
			Out.Add(Point(FMath::Lerp(A, B, T), NK, EdgeS[K] + Len * T, K, Run, false, false));
		}
		const bool bEnd = IsMouth[(K + 1) % N];
		const FVector EndOut = bEnd ? AlongMouth((K + 1) % N, (K + 2) % N, NK) : (NK + Normal((K + 1) % N)).GetSafeNormal();
		Out.Add(Point(B, EndOut.IsNearlyZero() ? NK : EndOut, EdgeS[K] + Len, K, Run, false, bEnd, (K + 1) % N));
	}
}

void ARMBuildableIntersection::GetSlopeWorldPoints(TArray<FVector>& Out) const
{
	Out.Reset();
	TArray<FVector> Ring;
	TArray<bool> IsMouth;
	TArray<FSlopePoint> Points;
	float Perimeter = 0.f;
	if (LocalOutline(Ring, IsMouth))
	{
		SlopePoints(Ring, IsMouth, Points, Perimeter);
	}
	for (const FSlopePoint& P : Points)
	{
		Out.Add(GetActorTransform().TransformPosition(P.Pos));
	}
}

void ARMBuildableIntersection::MeasureSlopes(TArray<FVector2D>& OutSamples) const
{
	OutSamples.Reset();
	UWorld* World = GetWorld();
	TArray<FVector> Ring;
	TArray<bool> IsMouth;
	if (!World || !LocalOutline(Ring, IsMouth))
	{
		return;
	}
	TArray<FSlopePoint> Points;
	float Perimeter = 0.f;
	SlopePoints(Ring, IsMouth, Points, Perimeter);
	const FTransform& T = GetActorTransform();
	// The roads joined to the mouths: near a mouth's corners the traces pass through them (their ends and banks).
	TArray<FRMRoadEnd> Ends;
	GetWorldEnds(Ends);
	TArray<const AActor*> Joined;
	for (TActorIterator<ARMBuildableRoad> It(World); It; ++It)
	{
		const ARMBuildableRoad* Road = *It;
		if (Road->IsActorBeingDestroyed() || FVector::Dist(Road->GetActorLocation(), GetActorLocation()) > Road->GetLength() + 20000.f)
		{
			continue;
		}
		for (int32 End = 0; End < 2; ++End)
		{
			FVector P, O;
			Road->GetEndWorld(End, P, O);
			for (const FRMRoadEnd& E : Ends)
			{
				if (FVector::Dist(P, E.Center) <= RMRoadUtils::JoinTolerance)
				{
					Joined.AddUnique(Road);
				}
			}
		}
	}
	TArray<FVector> Corners;
	for (int32 K = 0; K < Ring.Num(); ++K)
	{
		if (IsMouth[K])
		{
			Corners.Add(T.TransformPosition(Ring[K]));
			Corners.Add(T.TransformPosition(Ring[(K + 1) % Ring.Num()]));
		}
	}
	const TArray<const AActor*> NoPass;
	int32 Banks = 0;
	for (const FSlopePoint& P : Points)
	{
		const FVector Edge = T.TransformPosition(P.Pos);
		const FVector OutW = T.TransformVectorNoScale(P.Out);
		const FVector Out = FVector(OutW.X, OutW.Y, 0.f).GetSafeNormal();
		bool bNearMouth = false;
		for (const FVector& C : Corners)
		{
			bNearMouth |= FVector::Dist(C, Edge) < 300.f;
		}
		const float Drop = ARMBuildableRoad::MeasureBankAt(World, Edge, Out, this, bNearMouth ? Joined : NoPass);
		Banks += Drop > 0.f ? 1 : 0;
		OutSamples.Add(FVector2D(P.S, Drop));
	}
	UE_LOG(LogRoadMaker, Verbose, TEXT("%s: side slopes measured, %d of %d samples with a bank"), *GetName(), Banks, OutSamples.Num());
}

void ARMBuildableIntersection::BuildSlopeMesh(const TArray<FVector2D>& Samples, FRMMesh& Out) const
{
	TArray<FVector> Ring;
	TArray<bool> IsMouth;
	if (LocalOutline(Ring, IsMouth))
	{
		BuildSlopeMeshFrom(Ring, IsMouth, Samples, Out, nullptr);
	}
}

void ARMBuildableIntersection::BuildSlopeMeshFrom(const TArray<FVector>& Ring, const TArray<bool>& IsMouth, const TArray<FVector2D>& Samples,
	FRMMesh& Out, TArray<bool>* OutCoveredEdges) const
{
	TArray<FSlopePoint> Points;
	float Perimeter = 0.f;
	SlopePoints(Ring, IsMouth, Points, Perimeter);
	if (OutCoveredEdges)
	{
		OutCoveredEdges->Init(false, Ring.Num());
	}
	if (Points.Num() == 0 || Points.Num() != Samples.Num())
	{
		return; // measured for another shape: measured again shortly
	}
	const float Run = ARMBuildableRoad::SlopeRun;
	const float SlopeLen = FMath::Sqrt(1.f + Run * Run);
	const FVector Up = FVector::UpVector;
	const RMGeometry::FRMSideUV Layout = RMGeometry::MakeSideUV(GetShownLook() == 2, Perimeter);
	RMGeometry::PadSideUV(Out);
	const int32 First = Out.Vertices.Num();
	const int32 FirstTriangle = Out.Triangles.Num();
	auto DropOf = [&](int32 I) { return FMath::Max(0.f, float(Samples[I].Y)); };
	auto Has = [&](int32 I) { return DropOf(I) > 0.5f; };
	// Feet, untangled run by run (inside corners: the curb fillets between two roads bend towards the banks).
	TArray<FVector> Feet, Tops;
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		Feet.Add(Points[I].Pos + Points[I].Out * (Run * DropOf(I)) - Up * DropOf(I));
		Tops.Add(Points[I].Pos);
	}
	for (int32 I = 0; I < Points.Num();)
	{
		int32 J = I;
		while (J + 1 < Points.Num() && Points[J + 1].Run == Points[I].Run)
		{
			++J;
		}
		RMGeometry::UntangleFeet(Feet, I, J, &Tops);
		I = J + 1;
	}
	TArray<int32> VE, VF;
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		const FSlopePoint& P = Points[I];
		const float D = DropOf(I);
		const FVector Normal = (P.Out + Up * Run).GetSafeNormal();
		const FVector Tangent = FVector::CrossProduct(Normal, P.Out).GetSafeNormal();
		VE.Add(Out.AddVertex(P.Pos, Normal, FVector2D(P.S / 400.f, 0.f), Tangent));
		RMGeometry::AddSideUV(Out, Layout, 0.f, P.S / 100.f);
		VF.Add(Out.AddVertex(Feet[I], Normal, FVector2D(P.S / 400.f, D * SlopeLen / 400.f), Tangent));
		RMGeometry::AddSideUV(Out, Layout, D * SlopeLen / 100.f, P.S / 100.f);
	}
	if (OutCoveredEdges)
	{
		for (const FSlopePoint& P : Points)
		{
			(*OutCoveredEdges)[P.Edge] = true; // cleared below by any stretch of it without a bank
		}
	}
	for (int32 I = 0; I + 1 < Points.Num(); ++I)
	{
		if (Points[I].Run != Points[I + 1].Run)
		{
			continue;
		}
		if (OutCoveredEdges && !(Has(I) && Has(I + 1)))
		{
			(*OutCoveredEdges)[Points[I + 1].Edge] = false;
		}
		if (!Has(I) && !Has(I + 1))
		{
			continue;
		}
		const FVector Normal = (Points[I].Out + Up * Run).GetSafeNormal();
		if (Has(I + 1))
		{
			Out.AddTriangle(VE[I], VE[I + 1], VF[I + 1], Normal);
		}
		if (Has(I)) // where one end has no bank its foot is its edge: a wedge
		{
			Out.AddTriangle(VE[I], VF[I + 1], VF[I], Normal);
		}
	}
	// Road mouths next to banks: the slab's end face widened to the banks' feet (a trapezoid, as a road's cap; the
	// slab's own face there is left out). A road with banks joined there has its own back to back with it.
	const int32 N = Ring.Num();
	float Area = 0.f;
	for (int32 K = 0; K < N; ++K)
	{
		Area += Ring[K].X * Ring[(K + 1) % N].Y - Ring[(K + 1) % N].X * Ring[K].Y;
	}
	const float OutSign = Area >= 0.f ? 1.f : -1.f;
	for (int32 K = 0; K < N; ++K)
	{
		if (!IsMouth[K])
		{
			continue;
		}
		const int32 KB = (K + 1) % N;
		float DA = 0.f, DB = 0.f;
		int32 IA = INDEX_NONE, IB = INDEX_NONE;
		for (int32 I = 0; I < Points.Num(); ++I)
		{
			if (Points[I].bRunEnd && Points[I].Vertex == K)
			{
				IA = I;
				DA = DropOf(I);
			}
			if (Points[I].bRunStart && Points[I].Vertex == KB)
			{
				IB = I;
				DB = DropOf(I);
			}
		}
		if (DA <= 0.5f && DB <= 0.5f)
		{
			continue; // no bank at either corner: the slab's own mouth face
		}
		const FVector CA = Ring[K], CB = Ring[KB];
		const FVector Edge = CB - CA;
		FRMFrame F;
		F.Pos = CA;
		F.Right = FVector(Edge.X, Edge.Y, 0.f).GetSafeNormal();
		F.Up = Up;
		F.Fwd = (FVector(Edge.Y, -Edge.X, 0.f) * OutSign).GetSafeNormal(); // out of the intersection
		if (F.Right.IsNearlyZero() || F.Fwd.IsNearlyZero())
		{
			continue;
		}
		TArray<FVector> Corners3D = { CA, CB };
		if (DB > 0.5f && IB != INDEX_NONE)
		{
			Corners3D.Add(Feet[IB]);
		}
		if (DB < mHeight)
		{
			Corners3D.Add(CB - Up * mHeight);
		}
		if (DA < mHeight)
		{
			Corners3D.Add(CA - Up * mHeight);
		}
		if (DA > 0.5f && IA != INDEX_NONE)
		{
			Corners3D.Add(Feet[IA]);
		}
		TArray<FVector2D> Profile;
		for (const FVector& C : Corners3D)
		{
			Profile.Add(FVector2D(FVector::DotProduct(C - CA, F.Right), FVector::DotProduct(C - CA, F.Up)));
		}
		const int32 CapFirst = Out.Vertices.Num();
		RMGeometry::AddProfileCap(F, Profile, true, mUVScale, Out);
		for (int32 V = 0; V < Corners3D.Num() && CapFirst + V < Out.Vertices.Num(); ++V)
		{
			Out.Vertices[CapFirst + V] = Corners3D[V]; // the exact corners (feet), triangulated in the mouth's plane
		}
		RMGeometry::SideUVsFromCap(Out, Layout, CapFirst, mUVScale);
		if (OutCoveredEdges)
		{
			(*OutCoveredEdges)[K] = true; // this cap replaces the slab's mouth face
		}
	}
	RMGeometry::WrapSideUV(Out, Layout, First, FirstTriangle);
}

uint32 ARMBuildableIntersection::SlopeShapeKey() const
{
	TArray<FRMRoadEnd> Ends;
	GetWorldEnds(Ends);
	uint32 Key = HashCombine(ARMBuildableRoad::SlopeVersion, GetTypeHash(FMath::RoundToInt(mHeight)));
	for (const FRMRoadEnd& E : Ends)
	{
		Key = HashCombine(Key, GetTypeHash(FIntVector(FMath::RoundToInt(E.Center.X), FMath::RoundToInt(E.Center.Y), FMath::RoundToInt(E.Center.Z))));
		Key = HashCombine(Key, GetTypeHash(FIntVector(FMath::RoundToInt(E.Inward.X * 1000.f), FMath::RoundToInt(E.Inward.Y * 1000.f), FMath::RoundToInt(E.Width))));
	}
	return Key == 0 ? 1 : Key;
}

void ARMBuildableIntersection::RequestSlopeMeasure()
{
	// A tick later, as roads (pieces being replaced are gone by then).
	if (!HasAuthority() || !mSideSlopes || mSlopeMeasureQueued || IsActorBeingDestroyed() || SlopeShapeKey() == mSlopeKey)
	{
		return;
	}
	mSlopeMeasureQueued = true;
	GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		mSlopeMeasureQueued = false;
		if (!mSideSlopes || IsActorBeingDestroyed())
		{
			return;
		}
		MeasureSlopes(mSlopeSamples);
		mSlopeKey = SlopeShapeKey();
		if (ARMBuildableRoad::SlopeTooDeep(mSlopeSamples, TArray<FVector2D>()))
		{
			mSideSlopes = false;
			mSlopeSamples.Reset();
			UE_LOG(LogRoadMaker, Log, TEXT("%s: side slopes off, the ground is too far below in places"), *GetName());
		}
		RebuildMesh();
		ForceNetUpdate();
	}));
}

void ARMBuildableIntersection::SetSideSlopes(bool bOn)
{
	if (!HasAuthority())
	{
		return;
	}
	mSideSlopes = bOn;
	mSlopeSamples.Reset();
	if (bOn)
	{
		MeasureSlopes(mSlopeSamples);
		mSlopeKey = SlopeShapeKey();
		if (ARMBuildableRoad::SlopeTooDeep(mSlopeSamples, TArray<FVector2D>()))
		{
			mSideSlopes = false; // no half banks (the tool refuses it first)
			mSlopeSamples.Reset();
		}
	}
	RebuildMesh();
	ForceNetUpdate();
}
