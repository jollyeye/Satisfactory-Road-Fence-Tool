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
	DOREPLIFETIME(ARMRoadCutHologram, mSplitLeft);
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
		const FRMFrame* Nearest = nullptr;
		for (const FRMFrame& F : Frames)
		{
			const float D = FVector::DistSquared2D(F.Pos, hitResult.Location);
			if (D < Best)
			{
				Best = D;
				mCutDist = F.Dist;
				Nearest = &F;
				SetActorLocation(F.Pos);
			}
		}
		if (mSplitTool && Nearest)
		{
			// Where across the road you aim, in whole foundations from its left edge (every 8 m).
			const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(mTarget);
			const float Half = Road->GetRoadWidth() * 0.5f;
			const float FromLeft = Half + FVector::DotProduct(hitResult.Location - Nearest->Pos, Nearest->Right);
			mSplitLeft = FMath::Clamp(FMath::RoundToInt(FromLeft / RM::FoundationSize), 1, FMath::Max(1, Road->GetWidthInFoundations() - 1));
		}
	}
	else
	{
		SetActorLocation(hitResult.Location);
	}
	// Snap key held: the cut goes in line with a nearby road end.
	if (mTarget && mFenceTool && mSnapToGuideLines)
	{
		SnapFenceCutToRoadEnd();
	}
	SetActorRotation(FRotator::ZeroRotator);
	RebuildOverlay();
}

void ARMRoadCutHologram::SnapFenceCutToRoadEnd()
{
	// A road end near the aimed point (e.g. where a road was cut): the fence is cut exactly in line
	// with it, so the fence pieces match the road pieces.
	FRMFrame Aim;
	if (!mTarget->GetFrameAt(mCutDist, Aim) || !GetWorld())
	{
		return;
	}
	TArray<FRMFrame> Frames;
	mTarget->GetWorldFrames(Frames, 25.f);
	float BestDelta = mEndSnapDistance;
	float BestDist = -1.f;
	for (TActorIterator<ARMBuildableRoad> It(GetWorld()); It; ++It)
	{
		ARMBuildableRoad* Road = *It;
		if (Road->GetIsDismantled() || Road->IsActorBeingDestroyed() || Road->IsHidden())
		{
			continue;
		}
		for (int32 E = 0; E < 2; ++E)
		{
			FVector Loc, Outward;
			Road->GetEndWorld(E, Loc, Outward);
			if (FVector::Dist(Loc, Aim.Pos) > Road->GetRoadWidth() * 0.5f + mEndSnapDistance + 200.f)
			{
				continue;
			}
			// The plane of the road's end face.
			FRMFrame End;
			End.Pos = Loc;
			End.Fwd = Outward.GetSafeNormal();
			End.Right = FVector::CrossProduct(FVector::UpVector, End.Fwd).GetSafeNormal();
			End.Up = FVector::UpVector;
			float Dist = 0.f, Across = 0.f, Up = 0.f;
			if (!FindPlaneCrossing(Frames, End, Dist, Across, Up))
			{
				continue;
			}
			// The fence must be by that road (on it or on its edge), not across the map.
			if (FMath::Abs(Across) > Road->GetRoadWidth() * 0.5f + 100.f || Up < -200.f || Up > 3000.f)
			{
				continue;
			}
			const float Delta = FMath::Abs(Dist - mCutDist);
			if (Delta <= BestDelta)
			{
				BestDelta = Delta;
				BestDist = Dist;
			}
		}
	}
	if (BestDist >= 0.f)
	{
		mCutDist = BestDist;
		FRMFrame Snapped;
		if (mTarget->GetFrameAt(mCutDist, Snapped))
		{
			SetActorLocation(Snapped.Pos);
		}
	}
}

bool ARMRoadCutHologram::IsCutValid() const
{
	if (mSplitTool)
	{
		const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(mTarget);
		return Road && Road->GetWidthInFoundations() >= 2 && mSplitLeft >= 1 && mSplitLeft < Road->GetWidthInFoundations();
	}
	return mTarget && mCutDist >= MinPieceLength() && mTarget->GetLength() - mCutDist >= MinPieceLength();
}

void ARMRoadCutHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so the game's floor / clearance checks do not apply.
	if (!mTarget)
	{
		AddConstructDisqualifier(mFenceTool ? URMCD_AimAtFence::StaticClass() : URMCD_AimAtRoad::StaticClass());
	}
	else if (mSplitTool)
	{
		const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(mTarget);
		if (!IsCutValid())
		{
			AddConstructDisqualifier(URMCD_RoadTooNarrow::StaticClass());
		}
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
		ARMBuildableFence* Fence = Cast<ARMBuildableFence>(Other);
		if (!Fence)
		{
			continue;
		}
		// Quick reject: no point of its spline can be near the cut (the actor sits at its first point).
		if (FVector::Dist(Other->GetActorLocation(), Cut.Pos) > Other->GetLength() + 3000.f)
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
		// Only this road's fences: road fences stand on it (inside its edges); fence walls stand
		// centred on its edge. Fences of a road beside it stay. Stacked walls (up to 30 m) included.
		const float MaxAcross = Fence->IsWall() ? Half + 40.f : Half;
		const bool bTake = FMath::Abs(Across) <= MaxAcross && Up >= -200.f && Up <= 3000.f;
		const float Min = 50.f;
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
	if (mSplitTool)
	{
		if (IsCutValid())
		{
			ARMBuildableRoad* Right = nullptr;
			First = Cast<ARMBuildableRoad>(mTarget)->SplitLengthwise(mSplitLeft, &Right);
			if (Right)
			{
				out_children.Add(Right);
			}
		}
	}
	else if (IsCutValid())
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
			// Upright pieces: vertical outline, level across.
			const bool bUpright = Piece->IsCutSectionUpright();
			const FVector Up = bUpright ? FVector::UpVector : F.Up;
			const FVector Right = bUpright ? FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal() : F.Right;
			const FVector TL = F.Pos - Right * Half + Up * (Top + 2.f);
			const FVector TR = F.Pos + Right * Half + Up * (Top + 2.f);
			const FVector BL = F.Pos - Right * Half + Up * Bottom;
			const FVector BR = F.Pos + Right * Half + Up * Bottom;
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

	if (mSplitTool)
	{
		// A line along the whole road where it is split, just above the surface.
		const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(mTarget);
		const bool bOk = IsCutValid();
		TArray<FRMFrame> Frames;
		mTarget->GetWorldFrames(Frames, 100.f);
		const float Across = -Road->GetRoadWidth() * 0.5f + mSplitLeft * RM::FoundationSize;
		// Bright cyan (red if refused) on a dark border: visible on yellow lane lines and asphalt alike.
		const FLinearColor Color = bOk ? FLinearColor(0.f, 0.9f, 1.f) : FLinearColor(1.f, 0.05f, 0.05f);
		const FLinearColor Border(0.f, 0.f, 0.f);
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (int32 I = 1; I < Frames.Num(); ++I)
			{
				B.Line(Frames[I - 1].Pos + Frames[I - 1].Right * Across + Frames[I - 1].Up * 4.f,
					Frames[I].Pos + Frames[I].Right * Across + Frames[I].Up * 4.f, Pass == 0 ? 11.f : 6.f, Pass == 0 ? Border : Color);
			}
		}
		static UMaterialInterface* SplitMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay"));
		mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
		if (SplitMaterial)
		{
			mOverlayMesh->SetMaterial(0, SplitMaterial);
		}
		mOverlayMesh->SetRenderCustomDepth(false);
		return;
	}

	// Orange where it is cut (red if the cut is refused); with fences, the extra pieces in white.
	const bool bValid = IsCutValid() && mTarget->CanCutAt(mCutDist);
	B.Section(mTarget, mCutDist, bValid ? FLinearColor(1.f, 0.35f, 0.f) : FLinearColor(1.f, 0.05f, 0.05f));
	if (bValid && IsWithFences())
	{
		// Searched again only when the cut point moves.
		if (mSideCutsTarget.Get() != mTarget || !FMath::IsNearlyEqual(mSideCutsDist, mCutDist))
		{
			mSideCutsTarget = mTarget;
			mSideCutsDist = mCutDist;
			mSideCutsCache.Reset();
			TArray<TPair<ARMBuildableSpline*, float>> Side;
			FindSideCuts(Side);
			for (const TPair<ARMBuildableSpline*, float>& S : Side)
			{
				mSideCutsCache.Add(TPair<TWeakObjectPtr<ARMBuildableSpline>, float>(S.Key, S.Value));
			}
		}
		for (const TPair<TWeakObjectPtr<ARMBuildableSpline>, float>& S : mSideCutsCache)
		{
			B.Section(S.Key.Get(), S.Value, FLinearColor(1.f, 1.f, 1.f));
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
