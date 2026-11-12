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
	FRMMesh Top, Body;
	TArray<FVector> Outline;
	TArray<bool> IsMouth;
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Outline, &IsMouth))
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: could not rebuild intersection from %d saved ends"), *GetName(), mEnds.Num());
		return;
	}
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	if (mRevealing)
	{
		mRevealTop = Top;
		mRevealBody = Body;
		mRevealLines.Reset();
	}
	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyMaterial(0, TopMat);
	ApplyMaterial(1, BodyMat);

	if (mPreviewSideLines.Get(mSideLines))
	{
		TArray<TArray<FVector>> Curbs;
		RMGeometry::GetIntersectionCurbs(Outline, IsMouth, Curbs);
		FRMMesh Lines;
		RMGeometry::BuildCurbLines(Curbs, mEdgeLineInset, Top, Lines);
		Lines.ApplyToSection(mMesh, 2, false);
		ApplyMaterial(2, TopMat);
		if (mRevealing)
		{
			mRevealLines = Lines;
		}
	}
	if (mRevealing)
	{
		for (int32 Section = 0; Section < 3; ++Section)
		{
			if (Section < mMesh->GetNumSections())
			{
				mMesh->SetMeshSectionVisible(Section, false);
			}
		}
	}

	// Far version: 3 points per curb fillet instead of mFilletSegments.
	FRMMesh FarTop, FarBody;
	TArray<FVector> FarOutline;
	TArray<bool> FarIsMouth;
	if (RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, FMath::Min(mFilletSegments, 3), FarTop, FarBody, &FarOutline, &FarIsMouth, 500.f))
	{
		ApplyFarSection(FarTop, 0, TopMat);
		ApplyFarSection(FarBody, 1, BodyMat);
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

namespace
{
	/** One vertex of the reveal mesh, with everything a section carries, and its distance to the mouths. */
	struct FRMRevealVert
	{
		FVector P, N, T;
		FVector2D UV, M1, M2, M3;
		float D = 0.f;
	};

	FRMRevealVert RevealLerp(const FRMRevealVert& A, const FRMRevealVert& B, float Alpha)
	{
		FRMRevealVert V;
		V.P = FMath::Lerp(A.P, B.P, Alpha);
		V.N = FMath::Lerp(A.N, B.N, Alpha).GetSafeNormal();
		V.T = FMath::Lerp(A.T, B.T, Alpha).GetSafeNormal();
		V.UV = FMath::Lerp(A.UV, B.UV, Alpha);
		V.M1 = FMath::Lerp(A.M1, B.M1, Alpha);
		V.M2 = FMath::Lerp(A.M2, B.M2, Alpha);
		V.M3 = FMath::Lerp(A.M3, B.M3, Alpha);
		V.D = FMath::Lerp(A.D, B.D, Alpha);
		return V;
	}

	/** Keeps the part of the polygon where Sign * (D - Limit) <= 0 (Sutherland-Hodgman). */
	void RevealClip(TArray<FRMRevealVert, TInlineAllocator<8>>& Poly, float Limit, float Sign)
	{
		TArray<FRMRevealVert, TInlineAllocator<8>> Out;
		for (int32 I = 0; I < Poly.Num(); ++I)
		{
			const FRMRevealVert& A = Poly[I];
			const FRMRevealVert& B = Poly[(I + 1) % Poly.Num()];
			const float FA = Sign * (A.D - Limit);
			const float FB = Sign * (B.D - Limit);
			if (FA <= 0.f)
			{
				Out.Add(A);
			}
			if ((FA <= 0.f) != (FB <= 0.f))
			{
				Out.Add(RevealLerp(A, B, FA / (FA - FB)));
			}
		}
		Poly = MoveTemp(Out);
	}

	/** Output arrays of one reveal section. */
	struct FRMRevealSection
	{
		TArray<FVector> V, N;
		TArray<FVector2D> UV, M1, M2, M3;
		TArray<FProcMeshTangent> T;
		TArray<FLinearColor> C;
		TArray<int32> Tri;

		void Add(const TArray<FRMRevealVert, TInlineAllocator<8>>& Poly, const FVector& Lift, const FLinearColor* Color)
		{
			const int32 Base = V.Num();
			for (const FRMRevealVert& P : Poly)
			{
				V.Add(P.P + Lift);
				N.Add(P.N);
				UV.Add(P.UV);
				M1.Add(P.M1);
				M2.Add(P.M2);
				M3.Add(P.M3);
				T.Add(FProcMeshTangent(P.T, false));
				if (Color)
				{
					C.Add(*Color);
				}
			}
			for (int32 I = 1; I + 1 < Poly.Num(); ++I)
			{
				Tri.Append({ Base, Base + I, Base + I + 1 });
			}
		}
	};

	/** The triangles of Mesh where Lo <= distance <= Hi, cut smoothly at both limits. */
	void RevealCut(const FRMMesh& Mesh, const TArray<float>& Dist, float Lo, float Hi, FRMRevealSection& Out,
		const FVector& Lift = FVector::ZeroVector, const FLinearColor* Color = nullptr)
	{
		auto Vert = [&Mesh, &Dist](int32 I)
		{
			FRMRevealVert V;
			V.P = Mesh.Vertices[I];
			V.N = Mesh.Normals.IsValidIndex(I) ? Mesh.Normals[I] : FVector::UpVector;
			V.T = Mesh.Tangents.IsValidIndex(I) ? Mesh.Tangents[I].TangentX : FVector::ForwardVector;
			V.UV = Mesh.UVs.IsValidIndex(I) ? Mesh.UVs[I] : FVector2D::ZeroVector;
			V.M1 = Mesh.MarkUV1.Num() == Mesh.Vertices.Num() ? Mesh.MarkUV1[I] : FVector2D::ZeroVector;
			V.M2 = Mesh.MarkUV2.Num() == Mesh.Vertices.Num() ? Mesh.MarkUV2[I] : FVector2D::ZeroVector;
			V.M3 = Mesh.MarkUV3.Num() == Mesh.Vertices.Num() ? Mesh.MarkUV3[I] : FVector2D::ZeroVector;
			V.D = Dist[I];
			return V;
		};
		TArray<FRMRevealVert, TInlineAllocator<8>> Poly;
		for (int32 K = 0; K + 2 < Mesh.Triangles.Num(); K += 3)
		{
			const float D0 = Dist[Mesh.Triangles[K]], D1 = Dist[Mesh.Triangles[K + 1]], D2 = Dist[Mesh.Triangles[K + 2]];
			if (FMath::Min3(D0, D1, D2) > Hi || FMath::Max3(D0, D1, D2) < Lo)
			{
				continue;
			}
			Poly.Reset();
			Poly.Add(Vert(Mesh.Triangles[K]));
			Poly.Add(Vert(Mesh.Triangles[K + 1]));
			Poly.Add(Vert(Mesh.Triangles[K + 2]));
			RevealClip(Poly, Hi, 1.f);
			RevealClip(Poly, Lo, -1.f);
			if (Poly.Num() >= 3)
			{
				Out.Add(Poly, Lift, Color);
			}
		}
	}
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

void ARMBuildableIntersection::StartRiseAnimation()
{
	UWorld* World = GetWorld();
	if (!World || IsRunningDedicatedServer() || !mMesh || mEnds.Num() < 2)
	{
		return;
	}
	mRevealing = true;
	RebuildMesh(); // keeps copies of the meshes and hides the real ones
	if (mRevealTop.IsEmpty())
	{
		EndReveal();
		return;
	}
	mRevealMaxDist = 0.f;
	for (const FRMMesh* M : { &mRevealTop, &mRevealBody })
	{
		for (const FVector& V : M->Vertices)
		{
			mRevealMaxDist = FMath::Max(mRevealMaxDist, DistanceToMouths(V));
		}
	}
	mRevealMesh = NewObject<UProceduralMeshComponent>(this, TEXT("RevealMesh"), RF_Transient);
	mRevealMesh->SetupAttachment(mMesh);
	mRevealMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mRevealMesh->SetCanEverAffectNavigation(false);
	mRevealMesh->RegisterComponent();
	// Same paint as the real mesh (custom primitive data carries the line colours).
	const TArray<float>& Custom = mMesh->GetCustomPrimitiveData().Data;
	for (int32 I = 0; I < Custom.Num(); ++I)
	{
		mRevealMesh->SetCustomPrimitiveDataFloat(I, Custom[I]);
	}
	mRevealStart = World->GetTimeSeconds();
	World->GetTimerManager().SetTimer(mRevealTimer, this, &ARMBuildableIntersection::StepReveal, 0.02f, true);
	StepReveal();
}

void ARMBuildableIntersection::StepReveal()
{
	UWorld* World = GetWorld();
	if (!World || !mRevealMesh)
	{
		EndReveal();
		return;
	}
	const float T = FMath::Clamp(static_cast<float>(World->GetTimeSeconds() - mRevealStart) / RevealDuration, 0.f, 1.f);
	if (T >= 1.f)
	{
		EndReveal();
		return;
	}
	const float Eased = T * T * (3.f - 2.f * T);
	const float Front = Eased * (mRevealMaxDist + RevealGlowWidth);

	auto Distances = [this](const FRMMesh& M)
	{
		TArray<float> D;
		D.Reserve(M.Vertices.Num());
		for (const FVector& V : M.Vertices)
		{
			D.Add(DistanceToMouths(V));
		}
		return D;
	};
	const TArray<float> DTop = Distances(mRevealTop);
	const TArray<float> DBody = Distances(mRevealBody);
	const TArray<float> DLines = Distances(mRevealLines);

	const float Lowest = -1.f;
	FRMRevealSection Top, Body, Lines, Glow;
	RevealCut(mRevealTop, DTop, Lowest, Front, Top);
	RevealCut(mRevealBody, DBody, Lowest, Front, Body);
	RevealCut(mRevealLines, DLines, Lowest, Front, Lines);
	const FLinearColor GlowColor(1.f, 0.55f, 0.12f);
	RevealCut(mRevealTop, DTop, Front - RevealGlowWidth, Front, Glow, FVector(0.f, 0.f, 4.f), &GlowColor);

	int32 Section = 0;
	auto Apply = [this, &Section](const FRMRevealSection& S, UMaterialInterface* Material)
	{
		if (S.Tri.Num() == 0)
		{
			mRevealMesh->ClearMeshSection(Section);
		}
		else
		{
			mRevealMesh->CreateMeshSection_LinearColor(Section, S.V, S.Tri, S.N, S.UV, S.M1, S.M2, S.M3, S.C, S.T, false);
			mRevealMesh->SetMaterial(Section, Material);
		}
		++Section;
	};
	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* GlowMaterial = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_BuildGlow.M_RM_BuildGlow")); if (M) { M->AddToRoot(); } return M; }();
	Apply(Top, mMesh->GetMaterial(0));
	Apply(Body, mMesh->GetMaterial(1));
	Apply(Lines, mMesh->GetMaterial(2));
	Apply(Glow, GlowMaterial ? GlowMaterial : mMesh->GetMaterial(0));
}

void ARMBuildableIntersection::EndReveal()
{
	mRevealing = false;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(mRevealTimer);
	}
	if (mRevealMesh)
	{
		mRevealMesh->DestroyComponent();
		mRevealMesh = nullptr;
	}
	mRevealTop.Reset();
	mRevealBody.Reset();
	mRevealLines.Reset();
	if (mMesh)
	{
		for (int32 Section = 0; Section < mMesh->GetNumSections(); ++Section)
		{
			mMesh->SetMeshSectionVisible(Section, true);
		}
	}
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
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Outline, &IsMouth))
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
