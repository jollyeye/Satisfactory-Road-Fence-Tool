// Build animation by reveal, shared by roads and intersections: see ARMBuildableProcedural::UsesReveal.
#include "Buildables/RMBuildableProcedural.h"
#include "RoadMakerModule.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Materials/MaterialInterface.h"

namespace
{
	/** One vertex of the reveal mesh and its reveal distance. */
	struct FRMRevealVert
	{
		FProcMeshVertex V;
		float D = 0.f;
	};

	FRMRevealVert RevealLerp(const FRMRevealVert& A, const FRMRevealVert& B, float Alpha)
	{
		FRMRevealVert O;
		O.V.Position = FMath::Lerp(A.V.Position, B.V.Position, Alpha);
		O.V.Normal = FMath::Lerp(A.V.Normal, B.V.Normal, Alpha).GetSafeNormal();
		O.V.Tangent = FProcMeshTangent(FMath::Lerp(A.V.Tangent.TangentX, B.V.Tangent.TangentX, Alpha).GetSafeNormal(), A.V.Tangent.bFlipTangentY);
		O.V.Color = A.V.Color;
		O.V.UV0 = FMath::Lerp(A.V.UV0, B.V.UV0, Alpha);
		O.V.UV1 = FMath::Lerp(A.V.UV1, B.V.UV1, Alpha);
		O.V.UV2 = FMath::Lerp(A.V.UV2, B.V.UV2, Alpha);
		O.V.UV3 = FMath::Lerp(A.V.UV3, B.V.UV3, Alpha);
		O.D = FMath::Lerp(A.D, B.D, Alpha);
		return O;
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
	struct FRMRevealOut
	{
		TArray<FVector> V, N;
		TArray<FVector2D> UV0, UV1, UV2, UV3;
		TArray<FProcMeshTangent> T;
		TArray<FLinearColor> C;
		TArray<int32> Tri;

		void Add(const TArray<FRMRevealVert, TInlineAllocator<8>>& Poly, const FVector& Lift, const FLinearColor* Color)
		{
			const int32 Base = V.Num();
			for (const FRMRevealVert& P : Poly)
			{
				V.Add(P.V.Position + Lift);
				N.Add(P.V.Normal);
				UV0.Add(P.V.UV0);
				UV1.Add(P.V.UV1);
				UV2.Add(P.V.UV2);
				UV3.Add(P.V.UV3);
				T.Add(P.V.Tangent);
				C.Add(Color ? *Color : FLinearColor(P.V.Color));
			}
			for (int32 I = 1; I + 1 < Poly.Num(); ++I)
			{
				Tri.Append({ Base, Base + I, Base + I + 1 });
			}
		}
	};

	/**
	 * The triangles of Section (the listed ones: first index of each) where Lo <= distance <= Hi, cut
	 * smoothly at both limits; bWhole: no cut (all of them lie inside).
	 */
	void RevealCut(const FProcMeshSection& Section, const TArray<float>& Dist, const TArray<int32>& Triangles, float Lo, float Hi, FRMRevealOut& Out,
		bool bWhole = false, const FVector& Lift = FVector::ZeroVector, const FLinearColor* Color = nullptr)
	{
		const TArray<FProcMeshVertex>& Verts = Section.ProcVertexBuffer;
		const TArray<uint32>& Index = Section.ProcIndexBuffer;
		TArray<FRMRevealVert, TInlineAllocator<8>> Poly;
		for (const int32 K : Triangles)
		{
			if (K + 2 >= Index.Num())
			{
				continue;
			}
			const int32 I0 = Index[K], I1 = Index[K + 1], I2 = Index[K + 2];
			if (!Verts.IsValidIndex(I0) || !Verts.IsValidIndex(I1) || !Verts.IsValidIndex(I2))
			{
				continue;
			}
			const float D0 = Dist[I0], D1 = Dist[I1], D2 = Dist[I2];
			if (FMath::Min3(D0, D1, D2) > Hi || FMath::Max3(D0, D1, D2) < Lo)
			{
				continue;
			}
			Poly.Reset();
			Poly.Add({ Verts[I0], D0 });
			Poly.Add({ Verts[I1], D1 });
			Poly.Add({ Verts[I2], D2 });
			if (!bWhole)
			{
				RevealClip(Poly, Hi, 1.f);
				RevealClip(Poly, Lo, -1.f);
			}
			if (Poly.Num() >= 3)
			{
				Out.Add(Poly, Lift, Color);
			}
		}
	}
}

void ARMBuildableProcedural::StartReveal()
{
	UWorld* World = GetWorld();
	if (!World || IsRunningDedicatedServer() || !mMesh)
	{
		return;
	}
	PrepareReveal();
	mRevealSections.Reset();
	mRevealDist.Reset();
	mRevealMaxDist = 0.f;
	mRevealSources.Reset();
	mRevealBins.Reset();
	mRevealBinMin.Reset();
	mRevealDoneBins = -1;
	// Every section of every chunk's detailed mesh (long pieces are drawn in chunks).
	for (int32 C = 0; C < GetBuiltChunkCount(); ++C)
	{
		UProceduralMeshComponent* Source = GetChunkMesh(C, 0);
		for (int32 S = 0; Source && S < Source->GetNumSections(); ++S)
		{
			const FProcMeshSection* Section = Source->GetProcMeshSection(S);
			mRevealSections.Add(Section ? *Section : FProcMeshSection());
			mRevealSources.Add(TPair<UProceduralMeshComponent*, int32>(Source, S));
			TArray<float>& D = mRevealDist.AddDefaulted_GetRef();
			for (const FProcMeshVertex& V : mRevealSections.Last().ProcVertexBuffer)
			{
				D.Add(RevealDistance(V.Position));
				mRevealMaxDist = FMath::Max(mRevealMaxDist, D.Last());
			}
			// Triangles in 10 m bins by their farthest distance.
			TArray<TArray<int32>>& Bins = mRevealBins.AddDefaulted_GetRef();
			TArray<float>& BinMin = mRevealBinMin.AddDefaulted_GetRef();
			const TArray<uint32>& Index = mRevealSections.Last().ProcIndexBuffer;
			for (int32 K = 0; K + 2 < Index.Num(); K += 3)
			{
				if (!D.IsValidIndex(Index[K]) || !D.IsValidIndex(Index[K + 1]) || !D.IsValidIndex(Index[K + 2]))
				{
					continue;
				}
				const float Lo = FMath::Min3(D[Index[K]], D[Index[K + 1]], D[Index[K + 2]]);
				const float Hi = FMath::Max3(D[Index[K]], D[Index[K + 1]], D[Index[K + 2]]);
				const int32 Bin = FMath::Max(0, FMath::FloorToInt(Hi / RevealBin));
				if (Bins.Num() <= Bin)
				{
					Bins.SetNum(Bin + 1);
					while (BinMin.Num() <= Bin)
					{
						BinMin.Add(TNumericLimits<float>::Max());
					}
				}
				Bins[Bin].Add(K);
				BinMin[Bin] = FMath::Min(BinMin[Bin], Lo);
			}
		}
	}
	mRevealing = true;
	mRevealDuration = FMath::Max(GetBuildAnimDuration(), 0.1f);
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
	World->GetTimerManager().SetTimer(mRevealTimer, this, &ARMBuildableProcedural::StepReveal, 0.02f, true);
	StepReveal();
}

void ARMBuildableProcedural::StepReveal()
{
	UWorld* World = GetWorld();
	if (!World || !mRevealMesh || !mMesh)
	{
		EndReveal();
		return;
	}
	const float T = FMath::Clamp(static_cast<float>(World->GetTimeSeconds() - mRevealStart) / mRevealDuration, 0.f, 1.f);
	if (T >= 1.f)
	{
		EndReveal();
		return;
	}
	// The real mesh stays hidden (also if it was rebuilt meanwhile).
	for (int32 C = 0; C < GetBuiltChunkCount(); ++C)
	{
		if (UProceduralMeshComponent* Source = GetChunkMesh(C, 0))
		{
			for (int32 S = 0; S < Source->GetNumSections(); ++S)
			{
				Source->SetMeshSectionVisible(S, false);
			}
		}
	}
	const float Eased = T * T * (3.f - 2.f * T);
	const float Front = Eased * (mRevealMaxDist + RevealGlowWidth);

	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* GlowMaterial = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_BuildGlow.M_RM_BuildGlow")); if (M) { M->AddToRoot(); } return M; }();
	const FLinearColor GlowColor(1.f, 0.55f, 0.12f);
	auto Apply = [this](int32 Index, const FRMRevealOut& O, UMaterialInterface* Material)
	{
		if (O.Tri.Num() == 0)
		{
			mRevealMesh->ClearMeshSection(Index);
			return;
		}
		mRevealMesh->CreateMeshSection_LinearColor(Index, O.V, O.Tri, O.N, O.UV0, O.UV1, O.UV2, O.UV3, O.C, O.T, false);
		mRevealMesh->SetMaterial(Index, Material);
	};
	// Reveal mesh sections: [0, N) the bins already passed, whole (re-sent only when another bin is
	// done); [N, 2N) the triangles at the front, cut (every step, few); 2N the glowing band.
	const int32 N = mRevealSections.Num();
	const int32 Done = FMath::Max(0, FMath::FloorToInt(Front / RevealBin));
	const bool bDoneChanged = Done != mRevealDoneBins;
	mRevealDoneBins = Done;
	FRMRevealOut Glow;
	for (int32 S = 0; S < N; ++S)
	{
		UProceduralMeshComponent* Source = mRevealSources.IsValidIndex(S) ? mRevealSources[S].Key : nullptr;
		const int32 SourceSection = mRevealSources.IsValidIndex(S) ? mRevealSources[S].Value : S;
		UMaterialInterface* Material = Source ? Source->GetMaterial(SourceSection) : mMesh->GetMaterial(0);
		const TArray<TArray<int32>>& Bins = mRevealBins[S];
		const TArray<float>& BinMin = mRevealBinMin[S];
		if (bDoneChanged)
		{
			TArray<int32> Whole;
			for (int32 B = 0; B < FMath::Min(Done, Bins.Num()); ++B)
			{
				Whole.Append(Bins[B]);
			}
			FRMRevealOut Out;
			RevealCut(mRevealSections[S], mRevealDist[S], Whole, -1.f, TNumericLimits<float>::Max(), Out, true);
			Apply(S, Out, Material);
		}
		TArray<int32> AtFront, InBand;
		const int32 BandFirst = FMath::Max(0, FMath::FloorToInt((Front - RevealGlowWidth) / RevealBin));
		for (int32 B = FMath::Min(BandFirst, Done); B < Bins.Num(); ++B)
		{
			if (BinMin[B] > Front)
			{
				continue; // nothing of this bin is built yet
			}
			if (B >= Done)
			{
				AtFront.Append(Bins[B]);
			}
			if (B >= BandFirst)
			{
				InBand.Append(Bins[B]);
			}
		}
		FRMRevealOut Out;
		RevealCut(mRevealSections[S], mRevealDist[S], AtFront, -1.f, Front, Out);
		Apply(N + S, Out, Material);
		// The glowing band just behind the front, on the top (section 0 of each chunk).
		if (SourceSection == 0)
		{
			RevealCut(mRevealSections[S], mRevealDist[S], InBand, Front - RevealGlowWidth, Front, Glow, false, FVector(0.f, 0.f, 4.f), &GlowColor);
		}
	}
	Apply(2 * N, Glow, GlowMaterial ? GlowMaterial : mMesh->GetMaterial(0));
}

void ARMBuildableProcedural::EndReveal()
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
	mRevealSections.Reset();
	mRevealDist.Reset();
	mRevealSources.Reset();
	mRevealBins.Reset();
	mRevealBinMin.Reset();
	mRevealDoneBins = -1;
	for (int32 C = 0; C < GetBuiltChunkCount(); ++C)
	{
		if (UProceduralMeshComponent* Source = GetChunkMesh(C, 0))
		{
			for (int32 S = 0; S < Source->GetNumSections(); ++S)
			{
				Source->SetMeshSectionVisible(S, true);
			}
		}
	}
}
