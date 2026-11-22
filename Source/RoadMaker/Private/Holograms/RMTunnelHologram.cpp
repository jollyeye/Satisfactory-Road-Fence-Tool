#include "Holograms/RMTunnelHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableTunnel.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "UI/RMScreenHint.h"
#include "RMRoadUtils.h"
#include "RMGeometry.h"
#include "RoadMakerModule.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

// Named, not anonymous: unity builds put other files' helpers next to these.
namespace RMTunnelTool
{
	constexpr float MinWidth = 300.f;
	constexpr float MaxWidth = 6000.f;
	constexpr float Step = 100.f;        // a cross-section per metre of the first side ...
	constexpr float StraightTol = 3.f;   // ... thinned where both feet run straight (within 3 cm) ...
	constexpr float MaxGap = 2000.f;     // ... at most 20 m apart
	constexpr float OverlayPixels = 4.f;

	/** Closest point of a polyline to P seen from above: segment, fraction, point (3D, on the line). */
	bool Nearest2D(const TArray<FVector>& L, const FVector& P, int32& OutSeg, float& OutT, FVector& OutPoint)
	{
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; I + 1 < L.Num(); ++I)
		{
			const FVector2D AB(L[I + 1] - L[I]), AP(P - L[I]);
			const float Len2 = AB.SizeSquared();
			const float T = Len2 > 1e-4f ? FMath::Clamp(FVector2D::DotProduct(AP, AB) / Len2, 0.f, 1.f) : 0.f;
			const FVector C = FMath::Lerp(L[I], L[I + 1], T);
			const float D = FVector::DistSquared2D(C, P);
			if (D < Best)
			{
				Best = D;
				OutSeg = I;
				OutT = T;
				OutPoint = C;
			}
		}
		return Best < TNumericLimits<float>::Max();
	}

	/** Camera-facing lines, in actor space (as the Gap Fill tool draws them). */
	struct FOverlay
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
		static UMaterialInterface* M = [] { UMaterialInterface* L = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (L) { L->AddToRoot(); } return L; }();
		return M;
	}

	bool SameSide(const FRMTunnelSide& A, const FRMTunnelSide& B)
	{
		return A.Source.IsValid() && A.Source == B.Source && A.Key == B.Key;
	}
}

ARMTunnelHologram::ARMTunnelHologram()
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
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TunnelOverlay"));
	mOverlayMesh->SetupAttachment(mPreviewMesh);
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMTunnelHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMTunnelHologram, mLocalLeft);
	DOREPLIFETIME(ARMTunnelHologram, mLocalRight);
	DOREPLIFETIME(ARMTunnelHologram, mArcDeg);
}

void ARMTunnelHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

bool ARMTunnelHologram::IsValidHitResult(const FHitResult& InHit) const
{
	// Setting the arc: every aim counts (also at the sky). Else our roads and walls (the game may refuse them as a floor).
	const FHitResult Hit = RMRoadUtils::ThroughMarking(InHit);
	return mStep == 2 || Cast<ARMBuildableProcedural>(Hit.GetActor()) != nullptr || Super::IsValidHitResult(Hit);
}

bool ARMTunnelHologram::FindSide(const FHitResult& Hit, FRMTunnelSide& Out) const
{
	AActor* Actor = Hit.GetActor();
	if (!Actor || Actor->IsActorBeingDestroyed())
	{
		return false;
	}
	if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Actor))
	{
		if (Road->GetIsDismantled())
		{
			return false;
		}
		TArray<FRMFrame> Frames;
		Road->GetWorldFrames(Frames, RMTunnelTool::Step);
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
		// The shell's outer face flush with the road's edge.
		const float Across = FMath::Max(Road->GetRoadWidth() * 0.5f - ARMBuildableTunnel::Thickness * 0.5f, 0.f) * Side;
		Out = FRMTunnelSide();
		Out.Source = Road;
		Out.Key = Side > 0.f ? 1 : 0;
		for (const FRMFrame& F : Frames)
		{
			Out.Line.Add(F.Pos + F.Right * Across);
		}
		return true;
	}
	if (ARMBuildableFence* Wall = Cast<ARMBuildableFence>(Actor))
	{
		if (Wall->GetIsDismantled() || !Wall->IsWall())
		{
			return false;
		}
		TArray<FRMFrame> Frames;
		Wall->GetWorldFrames(Frames, RMTunnelTool::Step);
		if (Frames.Num() < 2)
		{
			return false;
		}
		// On the wall's top (walls stand upright: straight up from their foot line).
		Out = FRMTunnelSide();
		Out.Source = Wall;
		Out.Key = 2;
		for (const FRMFrame& F : Frames)
		{
			Out.Line.Add(F.Pos + FVector::UpVector * Wall->GetHeight());
		}
		return true;
	}
	return false;
}

bool ARMTunnelHologram::PairSides(const FRMTunnelSide& A, const FRMTunnelSide& B, TArray<FVector>& OutLeft, TArray<FVector>& OutRight)
{
	using namespace RMTunnelTool;
	OutLeft.Reset();
	OutRight.Reset();
	const TArray<FVector>& LA = A.Line;
	const TArray<FVector>& LB = B.Line;
	if (LA.Num() < 2 || LB.Num() < 2 || SameSide(A, B))
	{
		return false;
	}
	// For each point of the first side: the point of the other straight across from it, if any.
	TArray<FVector> Across;
	TArray<bool> Ok;
	for (int32 I = 0; I < LA.Num(); ++I)
	{
		const FVector Fwd = FVector(LA[FMath::Min(I + 1, LA.Num() - 1)] - LA[FMath::Max(I - 1, 0)]).GetSafeNormal2D();
		int32 Seg;
		float T;
		FVector P;
		bool bGood = !Fwd.IsNearlyZero() && Nearest2D(LB, LA[I], Seg, T, P);
		if (bGood)
		{
			const FVector D = P - LA[I];
			const float W = D.Size2D();
			bGood = W >= MinWidth && W <= MaxWidth && FMath::Abs(D.Z) < W
				&& FMath::Abs(FVector::DotProduct(D.GetSafeNormal2D(), Fwd)) < 0.15f; // straight across (past the other's end: slanted)
		}
		Across.Add(P);
		Ok.Add(bGood);
	}
	// The longest stretch where they run side by side.
	int32 BestFrom = 0, BestLen = 0;
	for (int32 I = 0; I < Ok.Num();)
	{
		if (!Ok[I])
		{
			++I;
			continue;
		}
		int32 J = I;
		while (J < Ok.Num() && Ok[J])
		{
			++J;
		}
		if (J - I > BestLen)
		{
			BestLen = J - I;
			BestFrom = I;
		}
		I = J;
	}
	if (BestLen < 2)
	{
		return false;
	}
	// Thinned: a cross-section only where a foot bends (or every 20 m).
	auto Deviates = [&](int32 From, int32 To)
	{
		for (int32 M = From + 1; M < To; ++M)
		{
			const float F = float(M - From) / float(To - From);
			if (FVector::Dist(FMath::Lerp(LA[From], LA[To], F), LA[M]) > StraightTol
				|| FVector::Dist(FMath::Lerp(Across[From], Across[To], F), Across[M]) > StraightTol)
			{
				return true;
			}
		}
		return false;
	};
	const int32 Last = BestFrom + BestLen - 1;
	int32 Kept = BestFrom;
	OutLeft.Add(LA[BestFrom]);
	OutRight.Add(Across[BestFrom]);
	for (int32 J = BestFrom + 2; J <= Last + 1; ++J)
	{
		const bool bEnd = J > Last;
		if (bEnd || Deviates(Kept, J) || FVector::Dist(LA[Kept], LA[J]) > MaxGap)
		{
			Kept = J - 1;
			OutLeft.Add(LA[Kept]);
			OutRight.Add(Across[Kept]);
		}
	}
	return OutLeft.Num() >= 2;
}

void ARMTunnelHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult Hit = RMRoadUtils::ThroughMarking(InHit);
	if (mStep > 0 && !mFirst.Source.IsValid())
	{
		ResetTool(); // the first side is gone (dismantled)
	}
	if (mStep < 2)
	{
		mHasHover = FindSide(Hit, mHover);
		mPaired = false;
		TArray<FVector> L, R;
		if (mStep == 1 && mHasHover && PairSides(mFirst, mHover, L, R))
		{
			mPaired = true;
			const FVector Origin = (L[0] + R[0]) * 0.5f;
			SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);
			mLocalLeft.Reset();
			mLocalRight.Reset();
			for (int32 I = 0; I < L.Num(); ++I)
			{
				mLocalLeft.Add(L[I] - Origin);
				mLocalRight.Add(R[I] - Origin);
			}
		}
		else
		{
			SetActorLocationAndRotation(Hit.Location, FRotator::ZeroRotator);
			mLocalLeft.Reset();
			mLocalRight.Reset();
		}
	}
	else if (mLocalLeft.Num() >= 2)
	{
		// The arc: as high as the aim passes over the middle of the tunnel (closest point between the view line and the
		// vertical line there), as an angle in 5 degree steps up to a semicircle.
		const int32 M = mLocalLeft.Num() / 2;
		const FVector Mid = GetActorLocation() + (mLocalLeft[M] + mLocalRight[M]) * 0.5f;
		const float Width = FVector::Dist(mLocalLeft[M], mLocalRight[M]);
		FVector Start = Hit.TraceStart;
		FVector Dir = (Hit.TraceEnd - Hit.TraceStart).GetSafeNormal();
		APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager)
		{
			Start = PC->PlayerCameraManager->GetCameraLocation();
			Dir = PC->PlayerCameraManager->GetCameraRotation().Vector();
		}
		const FVector W0 = Start - Mid;
		const float B = Dir.Z;
		const float Denom = 1.f - B * B;
		if (Denom > 0.01f)
		{
			const float Rise = (W0.Z - B * FVector::DotProduct(Dir, W0)) / Denom;
			const float Arc = FMath::RoundToFloat(ARMBuildableTunnel::ArcForRise(Width, Rise) / ARMBuildableTunnel::ArcStep) * ARMBuildableTunnel::ArcStep;
			mArcDeg = FMath::Clamp(Arc, ARMBuildableTunnel::MinArc, ARMBuildableTunnel::MaxArc);
		}
	}
	RebuildPreview();
	RebuildOverlay();
	UpdateHint();
}

void ARMTunnelHologram::UpdateHint()
{
	if (mStep == 2)
	{
		RMScreenHint::Show(GetWorld(), FText::Format(NSLOCTEXT("RoadMaker", "TunnelArcHint", "Arc: {0}° (aim up or down; click to build)"),
			FText::AsNumber(FMath::RoundToInt(mArcDeg))));
	}
	else if (mStep == 1)
	{
		if (mHasHover && RMTunnelTool::SameSide(mHover, mFirst))
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "TunnelDropFirst", "Tunnel: click to drop this side"));
		}
		else if (mHasHover && !mPaired)
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "TunnelNotBesideHint", "Tunnel: this side does not run beside the first one"), true);
		}
		else
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "TunnelSecondSide", "Tunnel: click the other side"));
		}
	}
	else
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "TunnelFirstSide", "Tunnel: click the first side (a road or walking path edge, or a fence wall)"));
	}
}

bool ARMTunnelHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	// One action per click, even if the game reports the click twice (press and release).
	const double Now = FPlatformTime::Seconds();
	if (Now - mLastClickTime < 0.25)
	{
		return false;
	}
	mLastClickTime = Now;
	if (mStep == 0)
	{
		if (mHasHover)
		{
			mFirst = mHover;
			mStep = 1;
		}
		return false;
	}
	if (mStep == 1)
	{
		if (mHasHover && RMTunnelTool::SameSide(mHover, mFirst))
		{
			ResetTool();
		}
		else if (mPaired)
		{
			mStep = 2;
			mArcDeg = ARMBuildableTunnel::MaxArc;
		}
		return false;
	}
	return mLocalLeft.Num() >= 2;
}

void ARMTunnelHologram::CheckValidPlacement()
{
	// No Super: the game's floor / clearance checks are for box-shaped buildings.
	if (mStep == 2)
	{
		return;
	}
	if (mStep == 1 && mHasHover && !mPaired && !RMTunnelTool::SameSide(mHover, mFirst))
	{
		AddConstructDisqualifier(URMCD_TunnelNotBeside::StaticClass());
	}
	else if (!mHasHover)
	{
		AddConstructDisqualifier(URMCD_TunnelPickSides::StaticClass());
	}
}

int32 ARMTunnelHologram::GetBaseCostMultiplier() const
{
	return ARMBuildableTunnel::CostUnits(mLocalLeft, mLocalRight, mArcDeg);
}

void ARMTunnelHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableTunnel* Tunnel = Cast<ARMBuildableTunnel>(inBuildable))
	{
		Tunnel->SetShape(mLocalLeft, mLocalRight, mArcDeg);
		Tunnel->SetCostMultiplier(GetBaseCostMultiplier());
	}
}

AActor* ARMTunnelHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	if (Built)
	{
		UE_LOG(LogRoadMaker, Verbose, TEXT("Tunnel built: %d cross-sections, arc %.0f"), mLocalLeft.Num(), mArcDeg);
		ResetTool();
	}
	return Built;
}

void ARMTunnelHologram::ResetTool()
{
	mStep = 0;
	mFirst = FRMTunnelSide();
	mHasHover = false;
	mPaired = false;
	mArcDeg = ARMBuildableTunnel::MaxArc;
	mLocalLeft.Reset();
	mLocalRight.Reset();
	mPreviewKey = 0;
	mPreviewMesh->ClearAllMeshSections();
	mOverlayMesh->ClearAllMeshSections();
}

void ARMTunnelHologram::RebuildPreview()
{
	const bool bShow = mLocalLeft.Num() >= 2 && (mStep == 2 || mPaired);
	uint32 Key = bShow ? GetTypeHash(mLocalLeft.Num()) + 1 : 0;
	if (bShow)
	{
		for (int32 I = 0; I < mLocalLeft.Num(); ++I)
		{
			Key = HashCombine(Key, HashCombine(GetTypeHash(mLocalLeft[I]), GetTypeHash(mLocalRight[I])));
		}
		Key = HashCombine(Key, GetTypeHash(mArcDeg));
	}
	if (Key == mPreviewKey)
	{
		return;
	}
	mPreviewKey = Key;
	mPreviewMesh->ClearAllMeshSections();
	if (bShow)
	{
		FRMMesh Shell;
		ARMBuildableTunnel::BuildTunnelMesh(mLocalLeft, mLocalRight, mArcDeg, ARMBuildableTunnel::Segments, Shell);
		Shell.ApplyToSection(mPreviewMesh, 0, false);
	}
	UpdateLook();
}

void ARMTunnelHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateLook();
}

void ARMTunnelHologram::UpdateLook()
{
	const ARMBuildableTunnel* CDO = mBuildClass ? Cast<ARMBuildableTunnel>(mBuildClass->GetDefaultObject()) : nullptr;
	if (CDO && mPreviewMesh->GetNumSections() > 0)
	{
		// The real concrete blocks (light, unpainted), with the game's valid / invalid tint on top.
		mPreviewMesh->SetMaterial(0, CDO->mShellMaterial);
		const FVector4 Light(0.8, 0.8, 0.78, 1.0);
		mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, Light);
		mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, Light);
		RMHologramLook::Apply(mPreviewMesh, nullptr, GetStencilForHologramMaterialState(mPlacementMaterialState));
	}
	if (UMaterialInterface* Overlay = RMTunnelTool::OverlayMaterial())
	{
		if (mOverlayMesh->GetNumSections() > 0)
		{
			mOverlayMesh->SetMaterial(0, Overlay);
		}
		mOverlayMesh->SetRenderCustomDepth(false);
	}
}

void ARMTunnelHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || mStep == 2 || (mStep == 0 && !mHasHover))
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	RMTunnelTool::FOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
	if (mStep == 1)
	{
		B.Polyline(mFirst.Line, FLinearColor(0.f, 1.f, 0.f)); // the first side: green
	}
	if (mHasHover)
	{
		// The aimed side: white, red if it cannot pair with the first.
		const bool bBad = mStep == 1 && !mPaired && !RMTunnelTool::SameSide(mHover, mFirst);
		B.Polyline(mHover.Line, bBad ? FLinearColor(1.f, 0.05f, 0.05f) : FLinearColor(1.f, 1.f, 1.f));
	}
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Overlay = RMTunnelTool::OverlayMaterial())
	{
		mOverlayMesh->SetMaterial(0, Overlay);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}
