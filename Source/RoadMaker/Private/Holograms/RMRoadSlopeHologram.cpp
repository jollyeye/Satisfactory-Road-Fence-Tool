#include "Holograms/RMRoadSlopeHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMOverlayLines.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "RMGeometry.h"
#include "UI/RMScreenHint.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

ARMRoadSlopeHologram::ARMRoadSlopeHologram()
{
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("SlopeOverlay"));
	if (RootComponent)
	{
		mOverlayMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mOverlayMesh);
	}
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMRoadSlopeHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadSlopeHologram, mTarget);
}

bool ARMRoadSlopeHologram::IsValidHitResult(const FHitResult& InHit) const
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit);
	return Cast<ARMBuildableRoad>(hitResult.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMRoadSlopeHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	ARMBuildableRoad* Hit = Cast<ARMBuildableRoad>(hitResult.GetActor());
	mTarget = Hit && !Hit->GetIsDismantled() && !Hit->IsActorBeingDestroyed() && !Hit->IsHidden() ? Hit : nullptr;
	SetActorLocation(mTarget ? mTarget->GetActorLocation() : FVector(hitResult.Location));
	SetActorRotation(FRotator::ZeroRotator);
	if (mTarget && !mTarget->HasSideSlopes())
	{
		const uint32 Key = mTarget->SlopeShapeKey();
		if (mMeasuredFor.Get() != mTarget || mMeasuredKey != Key)
		{
			mTarget->MeasureSlopes(mMeasuredLeft, mMeasuredRight);
			mMeasuredFor = mTarget;
			mMeasuredKey = Key;
		}
	}
	RebuildOverlay();
}

void ARMRoadSlopeHologram::CheckValidPlacement()
{
	// No Super: nothing is placed.
	if (!mTarget)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

AActor* ARMRoadSlopeHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The switch itself (server): measured there, saved with the road and sent to everyone.
	if (IsValid(mTarget) && !mTarget->GetIsDismantled() && !mTarget->IsActorBeingDestroyed())
	{
		mTarget->SetSideSlopes(!mTarget->HasSideSlopes());
	}
	mTarget = nullptr;
	mMeasuredFor = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return nullptr;
}

void ARMRoadSlopeHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

void ARMRoadSlopeHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	if (!mTarget)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeAimHint", "Side Slopes: aim at a road or walking path"));
		return;
	}
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const bool bRemove = mTarget->HasSideSlopes();
	FRMMesh Banks;
	mTarget->BuildSlopeMesh(bRemove ? mTarget->GetSlopeLeft() : mMeasuredLeft, bRemove ? mTarget->GetSlopeRight() : mMeasuredRight, Banks);
	if (bRemove)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeRemoveHint", "Side Slopes: click to remove this road's banks"));
	}
	else if (Banks.IsEmpty())
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeNoneHint", "Side Slopes: no banks needed here (ground level with the road, or over 8 m below). Click to turn them on anyway"));
	}
	else
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeAddHint", "Side Slopes: click to add banks down to the ground (cyan)"));
	}
	if (!PC || !PC->PlayerCameraManager || Banks.IsEmpty())
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	RMOverlayLines::FOverlay O;
	O.Base = GetActorLocation();
	O.Camera = PC->PlayerCameraManager->GetCameraLocation();
	O.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
	const FLinearColor Color = bRemove ? FLinearColor(1.f, 0.05f, 0.05f) : FLinearColor(0.f, 0.9f, 1.f);
	// A cage: every triangle's edges (each edge once), in world space, lifted off the surface by the overlay.
	const FTransform& T = mTarget->GetActorTransform();
	TSet<TPair<int32, int32>> Done;
	for (int32 I = 0; I + 2 < Banks.Triangles.Num(); I += 3)
	{
		for (int32 E = 0; E < 3; ++E)
		{
			int32 A = Banks.Triangles[I + E], B = Banks.Triangles[I + (E + 1) % 3];
			if (A > B)
			{
				Swap(A, B);
			}
			bool bSeen = false;
			Done.Add(TPair<int32, int32>(A, B), &bSeen);
			if (!bSeen)
			{
				O.Line(T.TransformPosition(Banks.Vertices[A]) - FVector(0.f, 0.f, 8.f), T.TransformPosition(Banks.Vertices[B]) - FVector(0.f, 0.f, 8.f), Color);
			}
		}
	}
	mOverlayMesh->CreateMeshSection_LinearColor(0, O.Vertices, O.Triangles, TArray<FVector>(), TArray<FVector2D>(), O.Colors, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* M = RMOverlayLines::OverlayMaterial())
	{
		mOverlayMesh->SetMaterial(0, M);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}
