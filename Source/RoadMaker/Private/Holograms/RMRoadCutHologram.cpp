#include "Holograms/RMRoadCutHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"

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

void ARMRoadCutHologram::GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const
{
	Super::GetSupportedBuildModes_Implementation(out_buildmodes);
	if (mBuildModeWithFences)
	{
		out_buildmodes.AddUnique(TSubclassOf<UFGBuildGunModeDescriptor>(mBuildModeWithFences.Get()));
	}
}

bool ARMRoadCutHologram::IsWithFences() const
{
	return !mFenceTool && mBuildModeWithFences && IsCurrentBuildMode(mBuildModeWithFences);
}

bool ARMRoadCutHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	return Cast<ARMBuildableSpline>(hitResult.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMRoadCutHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	ARMBuildableSpline* Hit = Cast<ARMBuildableSpline>(hitResult.GetActor());
	const bool bRightKind = Hit && (mFenceTool ? Hit->IsA<ARMBuildableFence>() : Hit->IsA<ARMBuildableRoad>());
	mTarget = bRightKind && !Hit->GetIsDismantled() && !Hit->IsActorBeingDestroyed() && !Hit->IsHidden() ? Hit : nullptr;
	if (mTarget)
	{
		// Nearest point along the piece to the crosshair.
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
	return mTarget && mCutDist >= MinPieceLength() && mTarget->GetLength() - mCutDist >= MinPieceLength();
}

void ARMRoadCutHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so the game's floor / clearance checks do not apply.
	if (!mTarget)
	{
		AddConstructDisqualifier(mFenceTool ? URMCD_AimAtFence::StaticClass() : URMCD_AimAtRoad::StaticClass());
	}
	else if (!IsCutValid())
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
	}
	else if (!mTarget->CanCutAt(mCutDist))
	{
		AddConstructDisqualifier(URMCD_RiderTooShort::StaticClass());
	}
}

namespace
{
	/**
	 * Where a piece's spline crosses the plane through Cut facing along it: distance along the piece
	 * and the crossing's offset across (Right) and up from the cut point. The crossing nearest to the
	 * cut point; false if none.
	 */
	bool FindPlaneCrossing(const TArray<FRMFrame>& Frames, const FRMFrame& Cut, float& OutDist, float& OutAcross, float& OutUp)
	{
		bool bFound = false;
		float BestDistSq = TNumericLimits<float>::Max();
		for (int32 I = 1; I < Frames.Num(); ++I)
		{
			const float SA = FVector::DotProduct(Frames[I - 1].Pos - Cut.Pos, Cut.Fwd);
			const float SB = FVector::DotProduct(Frames[I].Pos - Cut.Pos, Cut.Fwd);
			if ((SA <= 0.f) == (SB <= 0.f))
			{
				continue;
			}
			const float Alpha = FMath::Clamp(SA / (SA - SB), 0.f, 1.f);
			const FVector P = FMath::Lerp(Frames[I - 1].Pos, Frames[I].Pos, Alpha);
			const float DSq = FVector::DistSquared(P, Cut.Pos);
			if (DSq < BestDistSq)
			{
				BestDistSq = DSq;
				OutDist = FMath::Lerp(Frames[I - 1].Dist, Frames[I].Dist, Alpha);
				OutAcross = FVector::DotProduct(P - Cut.Pos, Cut.Right);
				OutUp = FVector::DotProduct(P - Cut.Pos, Cut.Up);
				bFound = true;
			}
		}
		return bFound;
	}
}

void ARMRoadCutHologram::FindSideCuts(TArray<TPair<ARMBuildableSpline*, float>>& Out) const
{
	Out.Reset();
	ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(mTarget);
	FRMFrame Cut;
	if (!Road || !GetWorld() || !Road->GetFrameAt(mCutDist, Cut))
	{
		return;
	}
	const float Half = Road->GetRoadWidth() * 0.5f;
	for (TActorIterator<ARMBuildableSpline> It(GetWorld()); It; ++It)
	{
		ARMBuildableSpline* Other = *It;
		if (Other == Road || Other->GetIsDismantled() || Other->IsActorBeingDestroyed() || Other->IsHidden())
		{
			continue;
		}
		ARMBuildableRoad* OtherRoad = Cast<ARMBuildableRoad>(Other);
		ARMBuildableFence* Fence = Cast<ARMBuildableFence>(Other);
		if (!OtherRoad && !Fence)
		{
			continue;
		}
		// Quick reject: nowhere near the cut.
		if (!Other->GetComponentsBoundingBox().ExpandBy(500.f).IsInside(Cut.Pos))
		{
			continue;
		}
		TArray<FRMFrame> Frames;
		Other->GetWorldFrames(Frames, 25.f);
		float Dist = 0.f, Across = 0.f, Up = 0.f;
		if (!FindPlaneCrossing(Frames, Cut, Dist, Across, Up))
		{
			continue;
		}
		bool bTake = false;
		if (Fence)
		{
			// On the road's sides (edge fences, centred or inset), stacked walls included.
			bTake = FMath::Abs(Across) <= Half + 150.f && Up >= -200.f && Up <= 3000.f;
		}
		else
		{
			// A road built beside it, edge to edge, at the same height.
			const float Beside = Half + OtherRoad->GetRoadWidth() * 0.5f;
			bTake = FMath::Abs(FMath::Abs(Across) - Beside) <= 100.f && FMath::Abs(Up) <= 150.f;
		}
		const float Min = Fence ? 50.f : 100.f;
		if (bTake && Dist >= Min && Other->GetLength() - Dist >= Min && Other->CanCutAt(Dist))
		{
			Out.Add(TPair<ARMBuildableSpline*, float>(Other, Dist));
		}
	}
}

AActor* ARMRoadCutHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The cut itself (server): new pieces replace the old ones. The first one is handed back as
	// "what was built".
	ARMBuildableSpline* First = nullptr;
	if (IsCutValid())
	{
		if (IsWithFences())
		{
			TArray<TPair<ARMBuildableSpline*, float>> Side;
			FindSideCuts(Side);
			for (const TPair<ARMBuildableSpline*, float>& S : Side)
			{
				ARMBuildableSpline* B = nullptr;
				if (ARMBuildableSpline* A = S.Key->CutAt(S.Value, &B))
				{
					out_children.Add(A);
					out_children.Add(B);
				}
			}
		}
		ARMBuildableSpline* Second = nullptr;
		First = mTarget->CutAt(mCutDist, &Second);
		if (Second)
		{
			out_children.Add(Second);
		}
	}
	mTarget = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return First;
}

namespace
{
	/** Camera-facing lines for the cut outlines, in actor space. */
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

		/** Outline of a piece's cross-section at Dist along it. */
		void Section(const ARMBuildableSpline* Piece, float Dist, const FLinearColor& Color)
		{
			FRMFrame F;
			if (!Piece || !Piece->GetFrameAt(Dist, F))
			{
				return;
			}
			float Half = 0.f, Bottom = 0.f, Top = 0.f;
			Piece->GetCutSection(Half, Bottom, Top);
			const FVector Up = F.Up;
			const FVector TL = F.Pos - F.Right * Half + Up * (Top + 2.f);
			const FVector TR = F.Pos + F.Right * Half + Up * (Top + 2.f);
			const FVector BL = F.Pos - F.Right * Half + Up * Bottom;
			const FVector BR = F.Pos + F.Right * Half + Up * Bottom;
			Line(TL, TR, 4.f, Color);
			Line(TR, BR, 4.f, Color);
			Line(BR, BL, 4.f, Color);
			Line(BL, TL, 4.f, Color);
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
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	FRMCutOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	// Orange where it is cut (red if the cut is refused); with fences, the extra pieces in white.
	const bool bValid = IsCutValid() && mTarget->CanCutAt(mCutDist);
	B.Section(mTarget, mCutDist, bValid ? FLinearColor(1.f, 0.35f, 0.f) : FLinearColor(1.f, 0.05f, 0.05f));
	if (bValid && IsWithFences())
	{
		TArray<TPair<ARMBuildableSpline*, float>> Side;
		FindSideCuts(Side);
		for (const TPair<ARMBuildableSpline*, float>& S : Side)
		{
			B.Section(S.Key, S.Value, FLinearColor(1.f, 1.f, 1.f));
		}
	}

	static UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay"));
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (Material)
	{
		mOverlayMesh->SetMaterial(0, Material);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}
