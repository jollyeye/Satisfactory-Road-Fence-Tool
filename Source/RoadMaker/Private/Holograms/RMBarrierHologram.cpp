#include "Holograms/RMBarrierHologram.h"
#include "RMRoadUtils.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Holograms/RMOverlayLines.h"
#include "Buildables/RMBuildableBarrier.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "UI/RMScreenHint.h"
#include "HAL/PlatformTime.h"

ARMBarrierHologram::ARMBarrierHologram()
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
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BarrierOverlay"));
	mOverlayMesh->SetupAttachment(GetRootComponent());
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMBarrierHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBarrierHologram, mStep);
	DOREPLIFETIME(ARMBarrierHologram, mArmLength);
}

bool ARMBarrierHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	const FHitResult Hit = RMRoadUtils::ThroughMarking(hitResult);
	return Cast<ARMBuildableProcedural>(Hit.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMBarrierHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

bool ARMBarrierHologram::PairEnd(const FRMTunnelSide& Side, FVector& OutEnd) const
{
	int32 Seg;
	float T;
	if (Side.Line.Num() < 2 || (Side.Source == mFirst.Source && Side.Key == mFirst.Key)
		|| !RMOverlayLines::Nearest2D(Side.Line, mPost, Seg, T, OutEnd))
	{
		return false;
	}
	const FVector D = OutEnd - mPost;
	const float W = D.Size2D() - ARMBuildableBarrier::PostHalf;
	return W >= ARMBuildableBarrier::MinArm && W <= ARMBuildableBarrier::MaxArm && FMath::Abs(D.Z) <= 300.f
		&& FMath::Abs(FVector::DotProduct(D.GetSafeNormal2D(), mFirstFwd)) < 0.2f; // straight across
}

void ARMBarrierHologram::ResetTool()
{
	mStep = 0;
	mFirst = FRMTunnelSide();
	mHasHover = false;
	mAcross = false;
	mOverlayMesh->ClearAllMeshSections();
}

void ARMBarrierHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	if (mStep > 0 && !mFirst.Source.IsValid())
	{
		ResetTool(); // the first road is gone
	}
	mHasHover = ARMTunnelHologram::FindSideAt(hitResult, mHover, ARMBuildableBarrier::ColumnInset, false);
	mAcross = false;
	if (mHasHover)
	{
		int32 Seg;
		float T;
		RMOverlayLines::Nearest2D(mHover.Line, mStep == 1 ? mPost : FVector(hitResult.Location), Seg, T, mHoverPoint);
	}
	FVector End;
	if (mStep == 1 && mHasHover && PairEnd(mHover, End))
	{
		// At the post, turned so its local Y points across to the other side.
		mAcross = true;
		const FVector Across = (End - mPost).GetSafeNormal2D();
		const FVector Fwd = FVector::CrossProduct(Across, FVector::UpVector);
		SetActorLocationAndRotation(mPost, FRotator(0.f, Fwd.Rotation().Yaw, 0.f));
		mArmLength = FMath::Clamp(FVector::Dist2D(mPost, End) - ARMBuildableBarrier::PostHalf, ARMBuildableBarrier::MinArm, ARMBuildableBarrier::MaxArm);
	}
	else
	{
		SetActorLocationAndRotation(mStep == 1 ? mPost : (mHasHover ? mHoverPoint : FVector(hitResult.Location)), FRotator::ZeroRotator);
	}
	RebuildPreview();
	RebuildOverlay();
	UpdateHint();
}

bool ARMBarrierHologram::DoMultiStepPlacement(bool isInputFromARelease)
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
			int32 Seg = 0;
			float T = 0.f;
			RMOverlayLines::Nearest2D(mFirst.Line, mHoverPoint, Seg, T, mPost);
			mFirstFwd = (mFirst.Line[Seg + 1] - mFirst.Line[Seg]).GetSafeNormal2D();
			mStep = 1;
		}
		return false;
	}
	if (mHasHover && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key)
	{
		ResetTool();
		return false;
	}
	return mAcross; // builds
}

void ARMBarrierHologram::CheckValidPlacement()
{
	// No Super call on purpose (as the gantry): the vanilla floor check is for boxes.
	if (mStep == 1 && !mAcross && !(mHasHover && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key))
	{
		AddConstructDisqualifier(URMCD_GantryNotAcross::StaticClass());
	}
	else if (mStep == 0 && !mHasHover)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

void ARMBarrierHologram::UpdateHint()
{
	if (mStep == 1)
	{
		if (mHasHover && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key)
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierDropFirst", "Boom Barrier: click to drop this side"));
		}
		else if (mAcross)
		{
			RMScreenHint::Show(GetWorld(), FText::Format(NSLOCTEXT("RoadMaker", "BarrierBuildHint", "Boom Barrier: arm {0} m (click to build)"),
				FText::AsNumber(FMath::RoundToInt(mArmLength / 10.f) / 10.f)));
		}
		else
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierSecondSide", "Boom Barrier: aim at the other side (any road or walking path edge straight across)"));
		}
	}
	else
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierFirstSide", "Boom Barrier: click the side for the post (a road or walking path edge)"));
	}
}

void ARMBarrierHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || (mStep == 0 && !mHasHover))
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
		B.Polyline(mFirst.Line, FLinearColor(0.f, 1.f, 0.f)); // the post's side: green, and the post's arrow
		B.Arrow(mPost, FLinearColor(0.f, 1.f, 0.f));
	}
	if (mHasHover && !(mStep == 1 && mHover.Source == mFirst.Source && mHover.Key == mFirst.Key))
	{
		const bool bBad = mStep == 1 && !mAcross;
		const FLinearColor Color = bBad ? FLinearColor(1.f, 0.05f, 0.05f) : FLinearColor(1.f, 1.f, 1.f);
		B.Polyline(mHover.Line, Color);
		B.Arrow(mHoverPoint, Color);
	}
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

void ARMBarrierHologram::RebuildPreview()
{
	// The post where it will stand; the arm once the other side is known.
	const float Length = mAcross ? mArmLength : 0.f;
	if (FMath::IsNearlyEqual(mShownLength, Length, 0.5f) && mPreviewMesh->GetNumSections() > 0)
	{
		return;
	}
	mShownLength = Length;
	mPreviewMesh->ClearAllMeshSections();
	FRMMesh Mesh;
	ARMBuildableBarrier::BuildPostMesh(Mesh);
	if (Length > 0.f)
	{
		FRMMesh White, Red;
		ARMBuildableBarrier::BuildArmMesh(Length, White, Red);
		const FVector Hinge(0.f, 0.f, ARMBuildableBarrier::HingeHeight);
		for (const FRMMesh* Part : { &White, &Red })
		{
			const int32 Base = Mesh.Vertices.Num();
			for (int32 V = 0; V < Part->Vertices.Num(); ++V)
			{
				Mesh.AddVertex(Part->Vertices[V] + Hinge, Part->Normals[V], Part->UVs[V], Part->Tangents[V].TangentX);
			}
			for (int32 I = 0; I + 2 < Part->Triangles.Num(); I += 3)
			{
				Mesh.Triangles.Append({ Base + Part->Triangles[I], Base + Part->Triangles[I + 1], Base + Part->Triangles[I + 2] });
			}
		}
	}
	Mesh.ApplyToSection(mPreviewMesh, 0, false);
	UMaterialInterface* Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
		? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}

void ARMBarrierHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableBarrier* B = Cast<ARMBuildableBarrier>(inBuildable))
	{
		B->SetArmLength(mArmLength);
	}
}

AActor* ARMBarrierHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	if (Built)
	{
		ResetTool(); // the next one starts over
	}
	return Built;
}
