#include "Holograms/RMRoadSlopeHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMOverlayLines.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
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
	DOREPLIFETIME(ARMRoadSlopeHologram, mTargetX);
	DOREPLIFETIME(ARMRoadSlopeHologram, mSide);
	DOREPLIFETIME(ARMRoadSlopeHologram, mRun);
	DOREPLIFETIME(ARMRoadSlopeHologram, mAll);
}

bool ARMRoadSlopeHologram::IsValidHitResult(const FHitResult& InHit) const
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit);
	return Cast<ARMBuildableRoad>(hitResult.GetActor()) != nullptr || Cast<ARMBuildableIntersection>(hitResult.GetActor()) != nullptr
		|| Super::IsValidHitResult(hitResult);
}

void ARMRoadSlopeHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	ARMBuildableRoad* Hit = Cast<ARMBuildableRoad>(hitResult.GetActor());
	mTarget = Hit && !Hit->GetIsDismantled() && !Hit->IsActorBeingDestroyed() && !Hit->IsHidden() ? Hit : nullptr;
	ARMBuildableIntersection* HitX = Cast<ARMBuildableIntersection>(hitResult.GetActor());
	mTargetX = HitX && !HitX->GetIsDismantled() && !HitX->IsActorBeingDestroyed() && !HitX->IsHidden() ? HitX : nullptr;
	AActor* Target = mTarget ? static_cast<AActor*>(mTarget) : static_cast<AActor*>(mTargetX);
	SetActorLocation(Target ? Target->GetActorLocation() : FVector(hitResult.Location));
	SetActorRotation(FRotator::ZeroRotator);
	// Road: the side aimed at (the hit seen across the road at its nearest point).
	if (mTarget)
	{
		TArray<FRMFrame> Frames;
		mTarget->GetWorldFrames(Frames, 200.f);
		const FRMFrame* Best = nullptr;
		float BestDist = TNumericLimits<float>::Max();
		for (const FRMFrame& F : Frames)
		{
			const float D = FVector::DistSquared(F.Pos, hitResult.Location);
			if (D < BestDist)
			{
				BestDist = D;
				Best = &F;
			}
		}
		mSide = Best && FVector::DotProduct(FVector(hitResult.Location) - Best->Pos, Best->Right) > 0.f ? 1 : 0;
	}
	else if (mTargetX)
	{
		mRun = FMath::Max(0, mTargetX->RunAt(hitResult.Location)); // the curb nearest the aim
	}
	mAll = mSnapToGuideLines;
	if (Target)
	{
		const uint32 Key = mTarget ? mTarget->SlopeShapeKey() : mTargetX->SlopeShapeKey();
		if (mMeasuredFor.Get() != Target || mMeasuredKey != Key)
		{
			if (mTarget)
			{
				mTarget->MeasureSlopes(mMeasuredLeft, mMeasuredRight);
			}
			else
			{
				mTargetX->MeasureSlopes(mMeasuredLeft);
				mMeasuredRight.Reset();
			}
			mMeasuredFor = Target;
			mMeasuredKey = Key;
		}
	}
	RebuildOverlay();
}

bool ARMRoadSlopeHologram::TargetHasSlopes() const
{
	// The aimed side / curb (with the snap key too: it decides whether all are added or removed).
	return (CurrentMask() & (1u << (mTarget ? mSide : FMath::Min(mRun, 31)))) != 0;
}

uint32 ARMRoadSlopeHologram::RequestedMask() const
{
	if (mTarget)
	{
		return mAll ? 3u : (1u << mSide);
	}
	if (mTargetX)
	{
		TArray<int32> Runs;
		int32 NumRuns = 0;
		mTargetX->SampleRuns(Runs, NumRuns);
		const uint32 All = NumRuns >= 32 ? ~0u : ((1u << NumRuns) - 1u);
		return mAll ? All : (1u << FMath::Min(mRun, 31));
	}
	return 0;
}

uint32 ARMRoadSlopeHologram::CurrentMask() const
{
	return mTarget ? mTarget->GetSlopeSides() : (mTargetX ? mTargetX->GetSlopeRuns() : 0u);
}

uint32 ARMRoadSlopeHologram::NewMask() const
{
	return TargetHasSlopes() ? (CurrentMask() & ~RequestedMask()) : (CurrentMask() | RequestedMask());
}

void ARMRoadSlopeHologram::CheckValidPlacement()
{
	// No Super: nothing is placed.
	if (!HasTarget())
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
	else if (!TargetHasSlopes())
	{
		// The sides / curbs being added: none may be too deep somewhere (no half banks).
		TArray<FVector2D> L = mMeasuredLeft, R = mMeasuredRight;
		if (mTarget)
		{
			ARMBuildableRoad::MaskSlopeSides(L, R, uint8(RequestedMask()));
		}
		else
		{
			mTargetX->MaskSlopeRuns(L, RequestedMask());
		}
		if (ARMBuildableRoad::SlopeTooDeep(L, R))
		{
			AddConstructDisqualifier(URMCD_SlopeTooDeep::StaticClass());
		}
	}
}

AActor* ARMRoadSlopeHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The switch itself (server): measured there, saved with the road and sent to everyone. The road is handed back
	// as "what was built": the build gun reads the returned actor and crashes on none.
	AActor* Built = nullptr;
	ARMBuildableRoad* Road = mTarget;
	ARMBuildableIntersection* Junction = mTargetX;
	if (IsValid(Road) && !Road->GetIsDismantled() && !Road->IsActorBeingDestroyed())
	{
		Road->SetSlopeSides(uint8(NewMask()));
		Built = Road;
	}
	else if (IsValid(Junction) && !Junction->GetIsDismantled() && !Junction->IsActorBeingDestroyed())
	{
		Junction->SetSlopeRuns(NewMask());
		Built = Junction;
	}
	mTarget = nullptr;
	mTargetX = nullptr;
	mMeasuredFor = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return Built;
}

void ARMRoadSlopeHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

void ARMRoadSlopeHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	if (!HasTarget())
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeAimHint", "Side Slopes: aim at a road, walking path or intersection"));
		return;
	}
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const bool bRemove = TargetHasSlopes();
	FRMMesh Banks;
	// Only the sides / curbs the click switches (the others' samples flattened).
	TArray<FVector2D> PreviewLeft, PreviewRight;
	if (mTarget)
	{
		PreviewLeft = bRemove ? mTarget->GetSlopeLeft() : mMeasuredLeft;
		PreviewRight = bRemove ? mTarget->GetSlopeRight() : mMeasuredRight;
		ARMBuildableRoad::MaskSlopeSides(PreviewLeft, PreviewRight, uint8(RequestedMask()));
		mTarget->BuildSlopeMesh(PreviewLeft, PreviewRight, Banks, true); // the banks only: no caps or floor in the cage
	}
	else
	{
		PreviewLeft = bRemove ? mTargetX->GetSlopeSamples() : mMeasuredLeft;
		mTargetX->MaskSlopeRuns(PreviewLeft, RequestedMask());
		mTargetX->BuildSlopeMesh(PreviewLeft, Banks, true);
	}
	const bool bTooDeep = !bRemove && ARMBuildableRoad::SlopeTooDeep(PreviewLeft, PreviewRight);
	if (bRemove && mAll)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeRemoveHint", "Side Slopes: click to remove the banks"));
	}
	else if (bRemove && mTarget)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeRemoveSideHint", "Side Slopes: click to remove the bank on this side (red; hold the snap key for both sides)"));
	}
	else if (bRemove)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeRemoveCurbHint", "Side Slopes: click to remove the bank on this curb (red; hold the snap key for all curbs)"));
	}
	else if (bTooDeep)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeTooDeepHint", "Side Slopes: the ground is more than 8 m below where the red lines are: no banks here"), true);
	}
	else if (Banks.IsEmpty())
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeNoneHint", "Side Slopes: no banks needed here (the ground is level with it). Click to turn them on anyway"));
	}
	else if (mAll)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeAddHint", "Side Slopes: click to add banks down to the ground (cyan)"));
	}
	else if (mTarget)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeAddSideHint", "Side Slopes: click to add a bank on this side, down to the ground (cyan; hold the snap key for both sides)"));
	}
	else
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SlopeAddCurbHint", "Side Slopes: click to add a bank on this curb, down to the ground (cyan; hold the snap key for all curbs)"));
	}
	if (!PC || !PC->PlayerCameraManager || (Banks.IsEmpty() && !bTooDeep))
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
	const FTransform& T = mTarget ? mTarget->GetActorTransform() : mTargetX->GetActorTransform();
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
	// Too deep: a red line 8 m down from the edge at each such sample.
	if (bTooDeep && mTargetX)
	{
		TArray<FVector> Points;
		mTargetX->GetSlopeWorldPoints(Points);
		for (int32 I = 0; I < Points.Num() && I < mMeasuredLeft.Num(); ++I)
		{
			if (PreviewLeft.IsValidIndex(I) && PreviewLeft[I].Y < 0.f)
			{
				O.Line(Points[I], Points[I] - FVector(0.f, 0.f, ARMBuildableRoad::SlopeMaxDrop), FLinearColor(1.f, 0.05f, 0.05f));
			}
		}
	}
	else if (bTooDeep)
	{
		const float H = mTarget->GetRoadWidth() * 0.5f;
		for (int32 SideIndex = 0; SideIndex < 2; ++SideIndex)
		{
			for (const FVector2D& S : SideIndex == 0 ? PreviewLeft : PreviewRight)
			{
				FRMFrame F;
				if (S.Y < 0.f && mTarget->GetFrameAt(float(S.X), F))
				{
					const FVector Edge = F.Pos + F.Right * (SideIndex == 0 ? -H : H);
					O.Line(Edge, Edge - FVector(0.f, 0.f, ARMBuildableRoad::SlopeMaxDrop), FLinearColor(1.f, 0.05f, 0.05f));
				}
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
