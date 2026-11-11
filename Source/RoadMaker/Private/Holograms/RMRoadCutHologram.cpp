#include "Holograms/RMRoadCutHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

ARMRoadCutHologram::ARMRoadCutHologram()
{
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("CutOverlay"));
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

void ARMRoadCutHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadCutHologram, mTarget);
	DOREPLIFETIME(ARMRoadCutHologram, mCutDist);
}

bool ARMRoadCutHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	return Cast<ARMBuildableRoad>(hitResult.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMRoadCutHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	mTarget = Cast<ARMBuildableRoad>(hitResult.GetActor());
	if (mTarget && (mTarget->GetIsDismantled() || mTarget->IsActorBeingDestroyed() || mTarget->IsHidden()))
	{
		mTarget = nullptr;
	}
	if (mTarget)
	{
		// Nearest point along the road to the crosshair.
		TArray<FRMFrame> Frames;
		mTarget->GetWorldFrames(Frames, 25.f);
		float Best = TNumericLimits<float>::Max();
		for (const FRMFrame& F : Frames)
		{
			const float D = FVector::DistSquared2D(F.Pos, hitResult.Location);
			if (D < Best)
			{
				Best = D;
				mCutDist = F.Dist;
				SetActorLocation(F.Pos);
			}
		}
	}
	else
	{
		SetActorLocation(hitResult.Location);
	}
	SetActorRotation(FRotator::ZeroRotator);
	RebuildOverlay();
}

bool ARMRoadCutHologram::IsCutValid() const
{
	return mTarget && mCutDist >= mMinPieceLength && mTarget->GetLength() - mCutDist >= mMinPieceLength;
}

void ARMRoadCutHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so the game's floor / clearance checks do not apply.
	if (!mTarget)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
	else if (!IsCutValid())
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
	}
	else
	{
		TArray<TPair<AActor*, float>> Splits;
		if (mTarget->PlanRiderCuts(mCutDist, Splits) > 0)
		{
			AddConstructDisqualifier(URMCD_RiderTooShort::StaticClass());
		}
	}
}

AActor* ARMRoadCutHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The cut itself (server): two new roads replace the old one. The first one is handed back as
	// "what was built".
	ARMBuildableRoad* Second = nullptr;
	ARMBuildableRoad* First = IsCutValid() ? mTarget->CutAt(mCutDist, &Second) : nullptr;
	if (Second)
	{
		out_children.Add(Second);
	}
	mTarget = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return First;
}

namespace
{
	/** Camera-facing lines for the cut outline, in actor space. */
	struct FRMCutOverlay
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
	};
}

void ARMRoadCutHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || !mTarget)
	{
		return;
	}
	// Frame at the cut.
	TArray<FRMFrame> Frames;
	mTarget->GetWorldFrames(Frames, 25.f);
	const FRMFrame* Cut = nullptr;
	for (const FRMFrame& F : Frames)
	{
		if (!Cut || FMath::Abs(F.Dist - mCutDist) < FMath::Abs(Cut->Dist - mCutDist))
		{
			Cut = &F;
		}
	}
	if (!Cut)
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	FRMCutOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	// Cross-section outline: top edge across the road, sides and bottom (orange; red if too close
	// to an end).
	const FLinearColor Color = IsCutValid() ? FLinearColor(1.f, 0.35f, 0.f) : FLinearColor(1.f, 0.05f, 0.05f);
	const float Half = mTarget->GetRoadWidth() * 0.5f;
	const FVector Down = -Cut->Up * FMath::Max(mTarget->GetRoadHeight(), 10.f);
	const FVector TL = Cut->Pos - Cut->Right * Half + Cut->Up * 2.f;
	const FVector TR = Cut->Pos + Cut->Right * Half + Cut->Up * 2.f;
	B.Line(TL, TR, 4.f, Color);
	B.Line(TR, TR + Down, 4.f, Color);
	B.Line(TR + Down, TL + Down, 4.f, Color);
	B.Line(TL + Down, TL, 4.f, Color);

	static UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay"));
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (Material)
	{
		mOverlayMesh->SetMaterial(0, Material);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}
