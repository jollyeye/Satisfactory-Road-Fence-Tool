#include "Holograms/RMFillHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableFill.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "RMRoadUtils.h"
#include "RMGeometry.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Algo/Reverse.h"

// Named, not anonymous: unity builds put other files' helpers next to these.
namespace RMFill
{
	constexpr float OverlayPixels = 4.f;
	constexpr float CloseAimRadius = 150.f; // cm from the closing line counts as aiming at it
	constexpr float MinArea = 40000.f;      // cm^2 (2 x 2 m)

	struct FParam
	{
		int32 Seg = 0;
		float T = 0.f;
		float Key() const { return Seg + T; }
	};

	FParam Start() { return FParam(); }
	FParam End(const TArray<FVector>& L) { FParam P; P.Seg = FMath::Max(L.Num() - 2, 0); P.T = 1.f; return P; }

	FVector At(const TArray<FVector>& L, const FParam& P)
	{
		return FMath::Lerp(L[P.Seg], L[FMath::Min(P.Seg + 1, L.Num() - 1)], P.T);
	}

	FVector ClosestOnSeg2D(const FVector& A, const FVector& B, const FVector& P, float& OutT)
	{
		const FVector2D AB(B - A), AP(P - A);
		const float Len2 = AB.SizeSquared();
		OutT = Len2 > 1e-4f ? FMath::Clamp(FVector2D::DotProduct(AP, AB) / Len2, 0.f, 1.f) : 0.f;
		return FMath::Lerp(A, B, OutT);
	}

	FParam Nearest(const TArray<FVector>& L, const FVector& P, float* OutDist = nullptr)
	{
		FParam Best;
		float BestD = TNumericLimits<float>::Max();
		for (int32 I = 0; I + 1 < L.Num(); ++I)
		{
			float T;
			const FVector C = ClosestOnSeg2D(L[I], L[I + 1], P, T);
			const float D = FVector::DistSquared2D(C, P);
			if (D < BestD)
			{
				BestD = D;
				Best.Seg = I;
				Best.T = T;
			}
		}
		if (OutDist)
		{
			*OutDist = FMath::Sqrt(BestD);
		}
		return Best;
	}

	/** The part of L from A to B (A before B). */
	TArray<FVector> Sub(const TArray<FVector>& L, const FParam& A, const FParam& B)
	{
		TArray<FVector> Out;
		Out.Add(At(L, A));
		for (int32 I = A.Seg + 1; I <= B.Seg && I < L.Num(); ++I)
		{
			if (FVector::DistSquared(L[I], Out.Last()) > 1.f)
			{
				Out.Add(L[I]);
			}
		}
		const FVector E = At(L, B);
		if (FVector::DistSquared(E, Out.Last()) > 1.f || Out.Num() == 1)
		{
			Out.Add(E);
		}
		return Out;
	}

	/** Proper crossing of A-B and C-D seen from above: the parameters along each. */
	bool SegCross(const FVector& A, const FVector& B, const FVector& C, const FVector& D, float& OutT, float& OutU)
	{
		const FVector2D R(B - A), S(D - C), QP(C - A);
		const float Den = FVector2D::CrossProduct(R, S);
		if (FMath::Abs(Den) < 1e-3f)
		{
			return false;
		}
		OutT = FVector2D::CrossProduct(QP, S) / Den;
		OutU = FVector2D::CrossProduct(QP, R) / Den;
		return OutT >= 0.f && OutT <= 1.f && OutU >= 0.f && OutU <= 1.f;
	}

	/** Where curves A and B cross (the crossing nearest to Near), if they do. */
	bool Crossing(const TArray<FVector>& A, const TArray<FVector>& B, const FVector& Near, FParam& OutA, FParam& OutB)
	{
		float BestD = TNumericLimits<float>::Max();
		bool bFound = false;
		for (int32 I = 0; I + 1 < A.Num(); ++I)
		{
			for (int32 J = 0; J + 1 < B.Num(); ++J)
			{
				float T, U;
				if (SegCross(A[I], A[I + 1], B[J], B[J + 1], T, U))
				{
					const FVector X = FMath::Lerp(A[I], A[I + 1], T);
					const float D = FVector::DistSquared2D(X, Near);
					if (D < BestD)
					{
						BestD = D;
						OutA.Seg = I; OutA.T = T;
						OutB.Seg = J; OutB.T = U;
						bFound = true;
					}
				}
			}
		}
		return bFound;
	}

	bool RingCrossesItself(const TArray<FVector>& R)
	{
		const int32 N = R.Num();
		for (int32 I = 0; I < N; ++I)
		{
			for (int32 J = I + 2; J < N; ++J)
			{
				if (I == 0 && J == N - 1)
				{
					continue;
				}
				float T, U;
				if (SegCross(R[I], R[(I + 1) % N], R[J], R[(J + 1) % N], T, U) && T > 1e-3f && T < 1.f - 1e-3f && U > 1e-3f && U < 1.f - 1e-3f)
				{
					return true;
				}
			}
		}
		return false;
	}

	float Area2D(const TArray<FVector>& R)
	{
		float A = 0.f;
		for (int32 I = 0; I < R.Num(); ++I)
		{
			const FVector& P = R[I];
			const FVector& Q = R[(I + 1) % R.Num()];
			A += P.X * Q.Y - Q.X * P.Y;
		}
		return FMath::Abs(A) * 0.5f;
	}

	/** Camera-facing lines, in actor space. */
	struct FRMFillOverlay
	{
		FVector Base;
		FVector Camera;
		float WorldPerPixel = 0.f;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FLinearColor> Colors;

		void Line(FVector A, FVector B, const FLinearColor& Color)
		{
			A.Z += 8.f;
			B.Z += 8.f;
			const FVector Dir = (B - A).GetSafeNormal();
			if (Dir.IsNearlyZero())
			{
				return;
			}
			const float HA = OverlayPixels * WorldPerPixel * FMath::Max(FVector::Dist(A, Camera), 10.f) * 0.5f;
			const float HB = OverlayPixels * WorldPerPixel * FMath::Max(FVector::Dist(B, Camera), 10.f) * 0.5f;
			A -= Dir * HA;
			B += Dir * HB;
			const FVector SA = FVector::CrossProduct(Dir, Camera - A).GetSafeNormal();
			const FVector SB = FVector::CrossProduct(Dir, Camera - B).GetSafeNormal();
			const int32 I = Vertices.Num();
			for (const FVector& P : { A - SA * HA, A + SA * HA, B + SB * HB, B - SB * HB })
			{
				Vertices.Add(P - Base);
				Colors.Add(Color);
			}
			Triangles.Append({ I, I + 1, I + 2, I, I + 2, I + 3 });
		}

		void Polyline(const TArray<FVector>& L, const FLinearColor& Color)
		{
			for (int32 I = 0; I + 1 < L.Num(); ++I)
			{
				Line(L[I], L[I + 1], Color);
			}
		}
	};

	UMaterialInterface* OverlayMaterial()
	{
		// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
		static UMaterialInterface* M = [] { UMaterialInterface* L = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (L) { L->AddToRoot(); } return L; }();
		return M;
	}
}

ARMFillHologram::ARMFillHologram()
{
	mPreviewMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMesh"));
	if (RootComponent)
	{
		mPreviewMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mPreviewMesh);
	}
	mPreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMesh->SetCastShadow(false);
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FillOverlay"));
	mOverlayMesh->SetupAttachment(mPreviewMesh);
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMFillHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMFillHologram, mLocalOutline);
	DOREPLIFETIME(ARMFillHologram, mHeight);
	DOREPLIFETIME(ARMFillHologram, mLook);
}

bool ARMFillHologram::IsValidHitResult(const FHitResult& InHit) const
{
	// Roads and intersections are what this tool aims at (the game's check may refuse them as a floor).
	const FHitResult Hit = RMRoadUtils::ThroughMarking(InHit);
	return Cast<ARMBuildableProcedural>(Hit.GetActor()) != nullptr || Super::IsValidHitResult(Hit);
}

bool ARMFillHologram::FindAimedEdge(const FHitResult& Hit, FRMFillPick& Out) const
{
	AActor* Actor = Hit.GetActor();
	if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Actor))
	{
		if (Road->GetIsDismantled() || Road->IsActorBeingDestroyed())
		{
			return false;
		}
		TArray<FRMFrame> Frames;
		Road->GetWorldFrames(Frames, 100.f);
		if (Frames.Num() < 2)
		{
			return false;
		}
		int32 Closest = 0;
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; I < Frames.Num(); ++I)
		{
			const float D = FVector::DistSquared(Frames[I].Pos, Hit.Location);
			if (D < Best)
			{
				Best = D;
				Closest = I;
			}
		}
		const float Side = FVector::DotProduct(Hit.Location - Frames[Closest].Pos, Frames[Closest].Right) >= 0.f ? 1.f : -1.f;
		const float Half = Road->GetRoadWidth() * 0.5f;
		Out = FRMFillPick();
		Out.Source = Road;
		Out.Key = Side > 0.f ? 1 : 0;
		for (const FRMFrame& F : Frames)
		{
			Out.Curve.Add(F.Pos + F.Right * (Half * Side));
		}
		Out.Click = Hit.Location;
		Out.Height = Road->GetRoadHeight();
		Out.Look = Road->GetLook();
		return true;
	}
	if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(Actor))
	{
		TArray<TArray<FVector>> Curbs;
		X->GetWorldCurbs(Curbs);
		int32 BestCurb = INDEX_NONE;
		float Best = TNumericLimits<float>::Max();
		for (int32 C = 0; C < Curbs.Num(); ++C)
		{
			float D = 0.f;
			if (Curbs[C].Num() >= 2)
			{
				RMFill::Nearest(Curbs[C], Hit.Location, &D);
				if (D < Best)
				{
					Best = D;
					BestCurb = C;
				}
			}
		}
		if (BestCurb == INDEX_NONE)
		{
			return false;
		}
		Out = FRMFillPick();
		Out.Source = X;
		Out.Key = 100 + BestCurb;
		Out.Curve = Curbs[BestCurb];
		Out.Click = Hit.Location;
		Out.Height = X->GetSlabHeight();
		Out.Look = X->GetLook();
		return true;
	}
	return false;
}

void ARMFillHologram::BuildLoop()
{
	using namespace RMFill;
	mRing.Reset();
	mValid = false;
	mClosedByCurves = false;
	const int32 N = mPicks.Num();
	if (N == 0)
	{
		return;
	}
	TArray<TArray<FVector>> Parts;
	TArray<FVector> Cur = mPicks[0].Curve;
	bool bFixed = false; // Cur's start is settled (it continues the part before it)
	for (int32 I = 1; I < N; ++I)
	{
		TArray<FVector> Nxt = mPicks[I].Curve;
		if (Cur.Num() < 2 || Nxt.Num() < 2)
		{
			continue;
		}
		FParam PA, PB;
		if (Crossing(Cur, Nxt, mPicks[I].Click, PA, PB))
		{
			// Each keeps the part on the side where it was clicked; they meet at the crossing.
			const FParam CA = Nearest(Cur, mPicks[I - 1].Click);
			if (!bFixed && CA.Key() > PA.Key())
			{
				Cur = Sub(Cur, PA, End(Cur));
				Algo::Reverse(Cur);
			}
			else
			{
				Cur = Sub(Cur, Start(), PA);
			}
			const FParam CB = Nearest(Nxt, mPicks[I].Click);
			if (CB.Key() >= PB.Key())
			{
				Nxt = Sub(Nxt, PB, End(Nxt));
			}
			else
			{
				Nxt = Sub(Nxt, Start(), PB);
				Algo::Reverse(Nxt);
			}
		}
		else
		{
			// No crossing: a straight line from Cur's end to the nearer end of Nxt.
			if (!bFixed)
			{
				const float D0 = FMath::Min(FVector::Dist2D(Cur[0], Nxt[0]), FVector::Dist2D(Cur[0], Nxt.Last()));
				const float D1 = FMath::Min(FVector::Dist2D(Cur.Last(), Nxt[0]), FVector::Dist2D(Cur.Last(), Nxt.Last()));
				if (D0 < D1)
				{
					Algo::Reverse(Cur);
				}
			}
			if (FVector::Dist2D(Nxt.Last(), Cur.Last()) < FVector::Dist2D(Nxt[0], Cur.Last()))
			{
				Algo::Reverse(Nxt);
			}
		}
		Parts.Add(MoveTemp(Cur));
		Cur = MoveTemp(Nxt);
		bFixed = true;
	}
	if (Parts.Num() > 0 && Cur.Num() >= 2 && Parts[0].Num() >= 2)
	{
		// The last edge crossing the first closes the loop by itself.
		FParam PL, PF;
		if (Crossing(Cur, Parts[0], Cur.Last(), PL, PF))
		{
			Cur = Sub(Cur, Start(), PL);
			Parts[0] = Sub(Parts[0], PF, End(Parts[0]));
			mClosedByCurves = true;
		}
	}
	Parts.Add(MoveTemp(Cur));

	// One outline, points at least 50 cm apart.
	for (const TArray<FVector>& P : Parts)
	{
		for (const FVector& V : P)
		{
			if (mRing.Num() == 0 || FVector::Dist2D(V, mRing.Last()) > 50.f)
			{
				mRing.Add(V);
			}
		}
	}
	if (mRing.Num() >= 2 && FVector::Dist2D(mRing[0], mRing.Last()) < 50.f)
	{
		mRing.Pop();
		mClosedByCurves = true;
	}
	if (mRing.Num() < 3)
	{
		return;
	}
	mCloseA = mRing.Last();
	mCloseB = mRing[0];
	mValid = !RingCrossesItself(mRing) && Area2D(mRing) >= MinArea;
}

void ARMFillHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult Hit = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	mPicks.RemoveAll([](const FRMFillPick& P) { return !P.Source.IsValid(); });
	BuildLoop();

	// Aiming at the closing line (shown unless it crosses the outline) comes first.
	mAimClosing = false;
	if (mValid && !mClosedByCurves)
	{
		float T;
		const FVector C = RMFill::ClosestOnSeg2D(mCloseA, mCloseB, Hit.Location, T);
		mAimClosing = FVector::Dist2D(C, Hit.Location) < RMFill::CloseAimRadius;
	}
	mHasHover = !mAimClosing && FindAimedEdge(Hit, mHover);

	// The hologram sits in the middle of the outline (the fill is built around it).
	if (mValid)
	{
		FVector Mid = FVector::ZeroVector;
		for (const FVector& P : mRing)
		{
			Mid += P;
		}
		Mid /= mRing.Num();
		SetActorLocationAndRotation(Mid, FRotator::ZeroRotator);
		mLocalOutline.Reset();
		for (const FVector& P : mRing)
		{
			mLocalOutline.Add(P - Mid);
		}
		mHeight = 20.f;
		for (const FRMFillPick& P : mPicks)
		{
			mHeight = FMath::Max(mHeight, P.Height);
		}
		mLook = mPicks.Num() > 0 ? mPicks[0].Look : 1;
	}
	else
	{
		SetActorLocationAndRotation(Hit.Location, FRotator::ZeroRotator);
		mLocalOutline.Reset();
	}
	RebuildPreview();
	RebuildOverlay();
}

bool ARMFillHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	// One action per click, even if the game reports the click twice (press and release).
	const double Now = FPlatformTime::Seconds();
	if (Now - mLastClickTime < 0.25)
	{
		return false;
	}
	mLastClickTime = Now;
	if (mAimClosing && mValid)
	{
		return true; // the closing line: build
	}
	if (mHasHover)
	{
		const int32 Index = mPicks.IndexOfByPredicate([this](const FRMFillPick& P) { return P.Source == mHover.Source && P.Key == mHover.Key; });
		if (Index != INDEX_NONE)
		{
			mPicks.RemoveAt(Index);
		}
		else
		{
			mPicks.Add(mHover);
		}
		mPreviewKey = 0;
		return false;
	}
	return mClosedByCurves && mValid; // a loop the edges closed: build on a click away from the edges
}

void ARMFillHologram::CheckValidPlacement()
{
	// No Super: the game's floor / clearance checks are for box-shaped buildings.
	if (mHasHover)
	{
		return; // this click picks (or drops) the aimed edge
	}
	if (!CanBuildNow())
	{
		AddConstructDisqualifier(mPicks.Num() >= 2 && mRing.Num() >= 3 && !mValid ? URMCD_FillCrosses::StaticClass() : URMCD_FillPickEdges::StaticClass());
	}
}

void ARMFillHologram::RebuildPreview()
{
	uint32 Key = mValid ? GetTypeHash(mLocalOutline.Num()) + 1 : 0;
	for (const FVector& P : mLocalOutline)
	{
		Key = HashCombine(Key, GetTypeHash(P));
	}
	Key = HashCombine(Key, GetTypeHash(mHeight) + mLook);
	if (Key == mPreviewKey && (mPreviewMesh->GetNumSections() > 0) == mValid)
	{
		return;
	}
	mPreviewKey = Key;
	mPreviewMesh->ClearAllMeshSections();
	if (mValid)
	{
		FRMMesh Top, Body;
		if (RMGeometry::BuildSlab(mLocalOutline, mHeight, 400.f, Top, Body, 250.f))
		{
			Top.ApplyToSection(mPreviewMesh, 0, false);
			Body.ApplyToSection(mPreviewMesh, 1, false);
		}
	}
	UpdateLook();
}

void ARMFillHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateLook();
}

void ARMFillHologram::UpdateLook()
{
	const ARMBuildableFill* CDO = mBuildClass ? Cast<ARMBuildableFill>(mBuildClass->GetDefaultObject()) : nullptr;
	if (CDO && mPreviewMesh->GetNumSections() > 0)
	{
		// The real surface in the look of the first picked edge's piece; the game's tint shows valid / invalid.
		UMaterialInterface* Top = CDO->GetLookMaterialFor(mLook, true, CDO->mTopMaterial);
		UMaterialInterface* Body = CDO->GetLookMaterialFor(mLook, false, CDO->mBodyMaterial);
		mPreviewMesh->SetMaterial(0, Top);
		if (mPreviewMesh->GetNumSections() > 1)
		{
			mPreviewMesh->SetMaterial(1, Body);
		}
		FLinearColor Primary(1.f, 1.f, 1.f), Secondary(1.f, 1.f, 1.f);
		if (mPicks.Num() > 0)
		{
			if (const ARMBuildableProcedural* Piece = Cast<ARMBuildableProcedural>(mPicks[0].Source.Get()))
			{
				Piece->GetPaintColours(Primary, Secondary);
			}
		}
		mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(Primary.R, Primary.G, Primary.B, 1.0));
		mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(Secondary.R, Secondary.G, Secondary.B, 1.0));
		RMHologramLook::Apply(mPreviewMesh, nullptr, GetStencilForHologramMaterialState(mPlacementMaterialState));
	}
	// The overlay keeps its own material (the game may give every mesh the hologram material).
	if (UMaterialInterface* Overlay = RMFill::OverlayMaterial())
	{
		if (mOverlayMesh->GetNumSections() > 0)
		{
			mOverlayMesh->SetMaterial(0, Overlay);
		}
		mOverlayMesh->SetRenderCustomDepth(false);
	}
}

void ARMFillHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || (mPicks.Num() == 0 && !mHasHover))
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	RMFill::FRMFillOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	const FLinearColor Green(0.f, 1.f, 0.f);
	const FLinearColor Red(1.f, 0.05f, 0.05f);
	const FLinearColor White(1.f, 1.f, 1.f);
	const FLinearColor Yellow(1.f, 0.85f, 0.f);
	const FLinearColor YellowAimed(1.f, 1.f, 0.6f);
	// The outline so far (picked parts and the straight lines joining them).
	if (mRing.Num() >= 2)
	{
		B.Polyline(mRing, mRing.Num() >= 3 && !mValid ? Red : Green);
		if (mClosedByCurves)
		{
			B.Line(mRing.Last(), mRing[0], Green);
		}
		else if (mValid)
		{
			B.Line(mCloseA, mCloseB, mAimClosing ? YellowAimed : Yellow); // click it to build
		}
	}
	else if (mPicks.Num() > 0)
	{
		B.Polyline(mPicks[0].Curve, Green);
	}
	// The aimed edge: white (a picked one too: clicking drops it).
	if (mHasHover)
	{
		B.Polyline(mHover.Curve, White);
	}
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Overlay = RMFill::OverlayMaterial())
	{
		mOverlayMesh->SetMaterial(0, Overlay);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}

void ARMFillHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableFill* Fill = Cast<ARMBuildableFill>(inBuildable))
	{
		Fill->SetLook(mLook);
		Fill->SetOutline(mLocalOutline, mHeight);
	}
}

AActor* ARMFillHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	if (Built)
	{
		// Built: start over.
		mPicks.Reset();
		mRing.Reset();
		mValid = false;
		mHasHover = false;
		mAimClosing = false;
		mPreviewKey = 0;
		mPreviewMesh->ClearAllMeshSections();
		mOverlayMesh->ClearAllMeshSections();
	}
	return Built;
}
