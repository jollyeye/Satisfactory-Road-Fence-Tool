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

		/**
		 * The outline of a road's top (both edges and both ends) as a selection outline: a dark line under
		 * coloured dashes that run along it ("marching ants"). Dash sizes are in screen pixels.
		 */
		void Outline(const TArray<FRMFrame>& Frames, float Width, float Pixels, const FLinearColor& Color)
		{
			if (Frames.Num() < 2)
			{
				return;
			}
			const float Half = Width * 0.5f;
			auto Edge = [&](const FRMFrame& F, float Side) { return F.Pos + F.Right * (Half * Side) + F.Up * 6.f; };
			// One closed loop: left edge forward, far end, right edge back, near end.
			TArray<FVector> Loop;
			for (const FRMFrame& F : Frames)
			{
				Loop.Add(Edge(F, -1.f));
			}
			for (int32 I = Frames.Num() - 1; I >= 0; --I)
			{
				Loop.Add(Edge(Frames[I], 1.f));
			}
			Loop.Add(Loop[0]);

			// Dash length from the size of a pixel at the outline's middle.
			FVector Mid = FVector::ZeroVector;
			for (const FVector& P : Loop)
			{
				Mid += P / Loop.Num();
			}
			const float PerPixel = WorldPerPixel * FMath::Max(FVector::Dist(Mid, Camera), 10.f);
			const float Dash = 14.f * PerPixel;
			const float Period = 2.f * Dash;
			const float Phase = FMath::Fmod(static_cast<float>(FPlatformTime::Seconds()) * 60.f * PerPixel, Period); // 60 px per second

			const FLinearColor Dark(0.02f, 0.02f, 0.02f);
			float S = 0.f; // distance along the loop
			for (int32 I = 1; I < Loop.Num(); ++I)
			{
				const FVector A = Loop[I - 1];
				const FVector B = Loop[I];
				const float Len = FVector::Dist(A, B);
				if (Len < 1.f)
				{
					continue;
				}
				Line(A, B, Pixels + 2.f, Dark);
				// The parts of this segment inside a dash: [k * Period - Phase, k * Period - Phase + Dash).
				float From = S;
				const float To = S + Len;
				while (From < To)
				{
					const float InPeriod = FMath::Fmod(From + Phase, Period);
					if (InPeriod < Dash)
					{
						const float End = FMath::Min(To, From + (Dash - InPeriod));
						Line(FMath::Lerp(A, B, (From - S) / Len), FMath::Lerp(A, B, (End - S) / Len), Pixels, Color);
						From = End;
					}
					else
					{
						From += Period - InPeriod;
					}
				}
				S = To;
			}
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
	PrimaryActorTick.bCanEverTick = true;
}

void ARMRoadMergeHologram::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (mFirst || mAimed)
	{
		RebuildOverlay();
	}
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

	const FLinearColor Cyan(0.f, 0.9f, 1.f);
	const FLinearColor White(1.f, 1.f, 1.f);
	const FLinearColor Red(1.f, 0.05f, 0.05f);
	TArray<FRMFrame> Frames;
	if (!mFirst)
	{
		// The road you would pick.
		mAimed->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mAimed->GetRoadWidth(), 4.f, White);
	}
	else if (!mAimed || mAimed == mFirst)
	{
		// Picked, waiting for the road beside it.
		mFirst->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mFirst->GetRoadWidth(), 4.f, mAimed == mFirst ? Cyan : White);
	}
	else
	{
		float Width = 0.f;
		TArray<FRMFrame> Centre;
		if (mFirst->CheckMerge(mAimed, &Centre, &Width) == ARMBuildableRoad::EMergeCheck::Ok)
		{
			// The merged road: one outline around both.
			B.Outline(Centre, Width, 5.f, Cyan);
		}
		else
		{
			mFirst->GetWorldFrames(Frames, 200.f);
			B.Outline(Frames, mFirst->GetRoadWidth(), 4.f, White);
			Frames.Reset();
			mAimed->GetWorldFrames(Frames, 200.f);
			B.Outline(Frames, mAimed->GetRoadWidth(), 4.f, Red);
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
