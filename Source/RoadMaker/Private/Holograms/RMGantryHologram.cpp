#include "Holograms/RMGantryHologram.h"
#include "RMRoadUtils.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableGantry.h"
#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Holograms/RMOverlayLines.h"
#include "UI/RMScreenHint.h"
#include "HAL/PlatformTime.h"

namespace RMGantryTool
{
	constexpr float MinSpan = 300.f;
	constexpr float MaxSpan = 6000.f;
	constexpr float MaxStep = 300.f; // the two columns' feet at most 3 m apart in height
}

ARMGantryHologram::ARMGantryHologram()
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
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GantryOverlay"));
	mOverlayMesh->SetupAttachment(GetRootComponent());
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMGantryHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMGantryHologram, mStep);
	DOREPLIFETIME(ARMGantryHologram, mHalfSpan);
	DOREPLIFETIME(ARMGantryHologram, mBeamBottom);
}

int32 ARMGantryHologram::GetBaseCostMultiplier() const
{
	return ARMBuildableGantry::CostUnits(mHalfSpan, mBeamBottom);
}

bool ARMGantryHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	const FHitResult Hit = RMRoadUtils::ThroughMarking(hitResult);
	return mStep == 2 || Cast<ARMBuildableProcedural>(Hit.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMGantryHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

bool ARMGantryHologram::PairColumn(const FRMTunnelSide& Side, FVector& OutColumn) const
{
	int32 Seg;
	float T;
	if (Side.Line.Num() < 2 || (Side.Source == mFirst.Source && Side.Key == mFirst.Key)
		|| !RMOverlayLines::Nearest2D(Side.Line, mColumnA, Seg, T, OutColumn))
	{
		return false;
	}
	const FVector D = OutColumn - mColumnA;
	const float W = D.Size2D();
	return W >= RMGantryTool::MinSpan && W <= RMGantryTool::MaxSpan && FMath::Abs(D.Z) <= RMGantryTool::MaxStep
		&& FMath::Abs(FVector::DotProduct(D.GetSafeNormal2D(), mFirstFwd)) < 0.2f; // straight across
}

void ARMGantryHologram::ResetTool()
{
	mStep = 0;
	mFirst = FRMTunnelSide();
	mHasHover = false;
	mOnRoad = false;
	mBeamBottom = ARMBuildableGantry::MinClearance;
	mOverlayMesh->ClearAllMeshSections();
}

void ARMGantryHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	if (mStep > 0 && !mFirst.Source.IsValid())
	{
		ResetTool(); // the first road is gone
	}
	if (mStep < 2)
	{
		// The columns stand on fence lines: ColumnInset in from a road or walking path edge.
		mHasHover = ARMTunnelHologram::FindSideAt(hitResult, mHover, ARMBuildableGantry::ColumnInset, false);
		mOnRoad = false;
		FVector ColumnB;
		if (mHasHover)
		{
			int32 Seg;
			float T;
			RMOverlayLines::Nearest2D(mHover.Line, mStep == 1 ? mColumnA : FVector(hitResult.Location), Seg, T, mHoverColumn);
		}
		if (mStep == 1 && mHasHover && PairColumn(mHover, ColumnB))
		{
			// Across from column A to column B, level, at the lower foot (the other sinks into its road a little).
			mOnRoad = true;
			const FVector Across = (ColumnB - mColumnA).GetSafeNormal2D();
			const FVector Fwd = FVector::CrossProduct(Across, FVector::UpVector);
			FVector Mid = (mColumnA + ColumnB) * 0.5f;
			Mid.Z = FMath::Min(mColumnA.Z, ColumnB.Z);
			SetActorLocationAndRotation(Mid, FRotator(0.f, Fwd.Rotation().Yaw, 0.f));
			mHalfSpan = FMath::Max(100.f, FVector::Dist2D(mColumnA, ColumnB) * 0.5f);
		}
		else
		{
			SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
		}
	}
	else
	{
		// The beam is as high as the aim passes the centre of the gantry (closest point between the
		// view line and the vertical line through the gantry), in 2 m steps.
		const FVector Origin = GetActorLocation();
		FVector Start = hitResult.TraceStart;
		FVector Dir = (hitResult.TraceEnd - hitResult.TraceStart).GetSafeNormal();
		// The aim at the sky has no usable trace: use the player's camera.
		APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager)
		{
			Start = PC->PlayerCameraManager->GetCameraLocation();
			Dir = PC->PlayerCameraManager->GetCameraRotation().Vector();
		}
		const FVector W0 = Start - Origin;
		const float B = Dir.Z;
		const float D = FVector::DotProduct(Dir, W0);
		const float E = W0.Z;
		const float Denom = 1.f - B * B;
		float Height = -1.f;
		if (Denom > 0.01f)
		{
			Height = (E - B * D) / Denom;
			const float Steps = FMath::RoundToFloat(Height / ARMBuildableGantry::HeightStep);
			mBeamBottom = FMath::Clamp(Steps * ARMBuildableGantry::HeightStep, ARMBuildableGantry::MinClearance, ARMBuildableGantry::MaxClearance);
		}
		if (!FMath::IsNearlyEqual(mShownBottom, mBeamBottom, 0.5f))
		{
			UE_LOG(LogRoadMaker, Verbose, TEXT("Gantry height step: camera %s dir %s -> aim height %.0f, beam bottom %.0f"),
				*Start.ToCompactString(), *Dir.ToCompactString(), Height, mBeamBottom);
		}
	}
	RebuildPreview();
	RebuildOverlay();
	UpdateHint();
}

bool ARMGantryHologram::DoMultiStepPlacement(bool isInputFromARelease)
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
			// Column A: on the side's fence line where the aim is; the side's direction there.
			mFirst = mHover;
			int32 Seg = 0;
			float T = 0.f;
			RMOverlayLines::Nearest2D(mFirst.Line, GetActorLocation(), Seg, T, mColumnA);
			mFirstFwd = (mFirst.Line[Seg + 1] - mFirst.Line[Seg]).GetSafeNormal2D();
			mStep = 1;
		}
		return false;
	}
	if (mStep == 1)
	{
		if (mHasHover && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key)
		{
			ResetTool();
		}
		else if (mOnRoad)
		{
			mStep = 2;
			mBeamBottom = ARMBuildableGantry::MinClearance;
			UE_LOG(LogRoadMaker, Verbose, TEXT("Gantry: both sides picked (span %.0f), now setting the height"), mHalfSpan * 2.f);
		}
		return false;
	}
	return true;
}

void ARMGantryHologram::CheckValidPlacement()
{
	// No Super call on purpose (see the intersection hologram): the vanilla floor check is for boxes.
	if (mStep == 1 && mHasHover && !mOnRoad && !(mHover.Source == mFirst.Source && mHover.Key == mFirst.Key))
	{
		AddConstructDisqualifier(URMCD_GantryNotAcross::StaticClass());
	}
	else if (mStep < 2 && !mHasHover)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

void ARMGantryHologram::UpdateHint()
{
	if (mStep == 2)
	{
		RMScreenHint::Show(GetWorld(), FText::Format(NSLOCTEXT("RoadMaker", "GantryHeightHint", "Sign Gantry: beam {0} m above the road (aim up or down; click to build)"),
			FText::AsNumber(FMath::RoundToInt(mBeamBottom / 100.f))));
	}
	else if (mStep == 1)
	{
		if (mHasHover && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key)
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "GantryDropFirst", "Sign Gantry: click to drop this side"));
		}
		else if (mHasHover && !mOnRoad)
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "GantryNotAcrossHint", "Sign Gantry: this edge is not straight across from the first column"), true);
		}
		else
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "GantrySecondSide", "Sign Gantry: click the other side (any road or walking path edge across)"));
		}
	}
	else
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "GantryFirstSide", "Sign Gantry: click the first side (a road or walking path edge)"));
	}
}

void ARMGantryHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || mStep == 2 || (mStep == 0 && !mHasHover))
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	RMOverlayLines::FOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
	if (mStep == 1)
	{
		B.Polyline(mFirst.Line, FLinearColor(0.f, 1.f, 0.f)); // the first side: green, and its column's arrow
		B.Arrow(mColumnA, FLinearColor(0.f, 1.f, 0.f));
	}
	if (mHasHover)
	{
		const bool bBad = mStep == 1 && !mOnRoad && !(mHover.Source == mFirst.Source && mHover.Key == mFirst.Key);
		const FLinearColor Color = bBad ? FLinearColor(1.f, 0.05f, 0.05f) : FLinearColor(1.f, 1.f, 1.f);
		B.Polyline(mHover.Line, Color);
		// Where the column goes on it: the white arrow (as the intersection tool shows its picks).
		if (mStep == 0 || !(mHover.Source == mFirst.Source && mHover.Key == mFirst.Key))
		{
			B.Arrow(mHoverColumn, Color);
		}
	}
	// The overlay hangs under the hologram, which turns with the gantry: its points are made in world space.
	const FTransform T = mOverlayMesh->GetComponentTransform();
	for (FVector& V : B.Vertices)
	{
		V = T.InverseTransformPosition(V + B.Base);
	}
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Overlay = RMOverlayLines::OverlayMaterial())
	{
		mOverlayMesh->SetMaterial(0, Overlay);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}

void ARMGantryHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableGantry* G = Cast<ARMBuildableGantry>(inBuildable))
	{
		G->SetShape(mHalfSpan, mBeamBottom);
		G->SetCostMultiplier(GetBaseCostMultiplier());
	}
}

AActor* ARMGantryHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	if (Built)
	{
		ResetTool(); // next one starts over
	}
	return Built;
}

void ARMGantryHologram::RebuildPreview()
{
	// The gantry shows once both columns are known (and while setting the height).
	const bool bShow = mStep == 2 || (mStep == 1 && mOnRoad);
	mPreviewMesh->SetVisibility(bShow, false);
	for (UStaticMeshComponent* Part : mParts)
	{
		if (Part)
		{
			Part->SetVisibility(bShow);
		}
	}
	if (FMath::IsNearlyEqual(mShownHalfSpan, mHalfSpan, 0.5f) && FMath::IsNearlyEqual(mShownBottom, mBeamBottom, 0.5f))
	{
		return;
	}
	mShownHalfSpan = mHalfSpan;
	mShownBottom = mBeamBottom;
	mPreviewMesh->ClearAllMeshSections();
	if (mParts.Num() == 0)
	{
		ARMBuildableGantry::CreateParts(this, mPreviewMesh, mParts);
		// The sampled / chosen paint (middle click copies it) was applied before the parts existed.
		if (mParts.Num() > 0)
		{
			ApplyCustomizationData();
		}
	}
	if (mParts.Num() > 0)
	{
		// The real parts; the boxes only if a game mesh is missing.
		ARMBuildableGantry::PlaceParts(mParts, mHalfSpan, mBeamBottom, 1.f);
	}
	else
	{
		FRMMesh Steel;
		ARMBuildableGantry::BuildGantryMesh(mHalfSpan, mBeamBottom, Steel);
		Steel.ApplyToSection(mPreviewMesh, 0, false);
	}
	UpdateHologramLook();
}

void ARMGantryHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateHologramLook();
}

void ARMGantryHologram::UpdateHologramLook()
{
	const uint8 Stencil = GetStencilForHologramMaterialState(mPlacementMaterialState);
	if (mParts.Num() > 0)
	{
		// Their own materials (the game may have put its hologram material on every mesh), with the
		// game's valid / invalid tint and outline on top, like the road hologram.
		for (UStaticMeshComponent* Part : mParts)
		{
			if (Part)
			{
				Part->EmptyOverrideMaterials();
				Part->SetRenderCustomDepth(true);
				Part->SetCustomDepthStencilValue(Stencil);
				Part->SetCastShadow(false);
			}
		}
		return;
	}
	UMaterialInterface* Material = mPreviewMaterial;
	if (!Material)
	{
		Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
			? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	}
	RMHologramLook::Apply(mPreviewMesh, Material, Stencil);
}
