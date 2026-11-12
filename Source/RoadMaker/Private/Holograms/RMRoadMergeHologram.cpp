#include "Holograms/RMRoadMergeHologram.h"
#include "RMRoadUtils.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

namespace
{
	/** Camera-facing lines for the merge outlines, in actor space. */
	struct FRMMergeOverlay
	{
		FVector Base;
		FVector Camera;
		float WorldPerPixel = 0.f;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FLinearColor> Colors;

		void Line(FVector A, FVector B, float Pixels, const FLinearColor& Color)
		{
			const FVector Dir = (B - A).GetSafeNormal();
			const float HA = Pixels * WorldPerPixel * FMath::Max(FVector::Dist(A, Camera), 10.f) * 0.5f;
			const float HB = Pixels * WorldPerPixel * FMath::Max(FVector::Dist(B, Camera), 10.f) * 0.5f;
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

		/** The outline of a road's top seen from above: both edges and both ends, one solid line. */
		void Outline(const TArray<FRMFrame>& Frames, float Width, float Pixels, const FLinearColor& Color)
		{
			if (Frames.Num() < 2)
			{
				return;
			}
			const float Half = Width * 0.5f;
			auto Edge = [&](const FRMFrame& F, float Side) { return F.Pos + F.Right * (Half * Side) + F.Up * 6.f; };
			for (int32 I = 1; I < Frames.Num(); ++I)
			{
				Line(Edge(Frames[I - 1], -1.f), Edge(Frames[I], -1.f), Pixels, Color);
				Line(Edge(Frames[I - 1], 1.f), Edge(Frames[I], 1.f), Pixels, Color);
			}
			Line(Edge(Frames[0], -1.f), Edge(Frames[0], 1.f), Pixels, Color);
			Line(Edge(Frames.Last(), -1.f), Edge(Frames.Last(), 1.f), Pixels, Color);
		}
	};

	bool MergeUsable(const ARMBuildableRoad* Road)
	{
		return Road && !Road->GetIsDismantled() && !Road->IsActorBeingDestroyed() && !Road->IsHidden();
	}
}

ARMRoadMergeHologram::ARMRoadMergeHologram()
{
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MergeOverlay"));
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

void ARMRoadMergeHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadMergeHologram, mFirst);
	DOREPLIFETIME(ARMRoadMergeHologram, mAimed);
}

bool ARMRoadMergeHologram::IsValidHitResult(const FHitResult& InHit) const
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit);
	return Cast<ARMBuildableRoad>(hitResult.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMRoadMergeHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	ARMBuildableRoad* Hit = Cast<ARMBuildableRoad>(hitResult.GetActor());
	mAimed = MergeUsable(Hit) ? Hit : nullptr;
	if (mFirst && !MergeUsable(mFirst))
	{
		mFirst = nullptr; // gone meanwhile
	}
	SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
	RebuildOverlay();
}

bool ARMRoadMergeHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	// One action per click, even if the game reports the click twice (press and release).
	const double Now = FPlatformTime::Seconds();
	const bool bNewClick = Now - mLastClickTime > 0.25;
	if (!mFirst)
	{
		if (bNewClick && mAimed)
		{
			mLastClickTime = Now;
			mFirst = mAimed; // picked
			RebuildOverlay();
		}
		return false;
	}
	if (mAimed == mFirst)
	{
		if (bNewClick)
		{
			mLastClickTime = Now;
			mFirst = nullptr; // dropped
			RebuildOverlay();
		}
		return false;
	}
	// The road beside it: merge (only when valid; CheckValidPlacement blocks the click otherwise).
	return mAimed && mFirst->CheckMerge(mAimed) == ARMBuildableRoad::EMergeCheck::Ok;
}

void ARMRoadMergeHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so the game's floor / clearance checks do not apply.
	if (!mFirst)
	{
		if (!mAimed)
		{
			AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
		}
		return; // clicking picks the aimed road
	}
	if (!mAimed)
	{
		AddConstructDisqualifier(URMCD_MergePickSecond::StaticClass());
		return;
	}
	if (mAimed == mFirst)
	{
		return; // clicking drops it
	}
	switch (mFirst->CheckMerge(mAimed))
	{
	case ARMBuildableRoad::EMergeCheck::Ok:
		break;
	case ARMBuildableRoad::EMergeCheck::TooWide:
		AddConstructDisqualifier(URMCD_MergeTooWide::StaticClass());
		break;
	case ARMBuildableRoad::EMergeCheck::LengthMismatch:
		AddConstructDisqualifier(URMCD_MergeLength::StaticClass());
		break;
	default:
		AddConstructDisqualifier(URMCD_MergeNotBeside::StaticClass());
		break;
	}
}

AActor* ARMRoadMergeHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The merge itself (server): the new road replaces the two; it is handed back as "what was built".
	ARMBuildableRoad* Merged = nullptr;
	if (MergeUsable(mFirst) && MergeUsable(mAimed) && mFirst != mAimed)
	{
		Merged = mFirst->MergeWith(mAimed);
	}
	mFirst = nullptr;
	mAimed = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return Merged;
}

void ARMRoadMergeHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || (!mFirst && !mAimed))
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	FRMMergeOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	const FLinearColor Cyan(0.f, 1.f, 0.f); // #00FF00 green (the "ok" colour of Join / Merge)
	const FLinearColor White(1.f, 1.f, 1.f);
	const FLinearColor Red(1.f, 0.05f, 0.05f);
	TArray<FRMFrame> Frames;
	if (!mFirst)
	{
		// The road you would pick.
		mAimed->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mAimed->GetRoadWidth(), 8.f, White);
	}
	else if (!mAimed || mAimed == mFirst)
	{
		// Picked, waiting for the road beside it.
		mFirst->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mFirst->GetRoadWidth(), 8.f, mAimed == mFirst ? Cyan : White);
	}
	else
	{
		float Width = 0.f;
		TArray<FRMFrame> Centre;
		if (mFirst->CheckMerge(mAimed, &Centre, &Width) == ARMBuildableRoad::EMergeCheck::Ok)
		{
			// The merged road: one outline around both.
			B.Outline(Centre, Width, 10.f, Cyan);
		}
		else
		{
			mFirst->GetWorldFrames(Frames, 200.f);
			B.Outline(Frames, mFirst->GetRoadWidth(), 8.f, White);
			Frames.Reset();
			mAimed->GetWorldFrames(Frames, 200.f);
			B.Outline(Frames, mAimed->GetRoadWidth(), 8.f, Red);
		}
	}

	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* Material = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (M) { M->AddToRoot(); } return M; }();
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (Material)
	{
		mOverlayMesh->SetMaterial(0, Material);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}
