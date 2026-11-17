#include "Holograms/RMFenceHologram.h"
#include "Buildables/RMBuildableSignWall.h"
#include "Holograms/RMHologramLook.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Buildables/RMBuildableFill.h"
#include "Algo/Reverse.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Equipment/FGBuildGunBuild.h"
#include "RMRoadUtils.h"
#include "EngineUtils.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGDismantleInterface.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

const ARMBuildableFence* ARMFenceHologram::GetFenceDefaults() const
{
	return mBuildClass ? mBuildClass->GetDefaultObject<ARMBuildableFence>() : GetDefault<ARMBuildableFence>();
}

bool ARMFenceHologram::TrySnapToRoadEnd(const FHitResult& hitResult)
{
	mOnRoadEdge = false;

	// Edge mode only makes sense before a free fence has been started.
	if (mStep != 0)
	{
		return SnapToFoundationGrid(hitResult);
	}
	// Snapping to roads, curbs and other fences works only while the snap key is held, except stacking:
	// a fence wall aimed at another fence wall always goes on top of it (upper half) or below it (lower half).
	const ARMBuildableFence* AimedFence = Cast<ARMBuildableFence>(hitResult.GetActor());
	const bool bStacking = AimedFence && AimedFence->IsWall() && GetFenceDefaults()->IsWall();
	if (!mSnapToGuideLines && !bStacking && (AimedFence || Cast<ARMBuildableRoad>(hitResult.GetActor())
		|| Cast<ARMBuildableIntersection>(hitResult.GetActor()) || Cast<ARMBuildableFill>(hitResult.GetActor())))
	{
		return false;
	}
	// With the snap key held, aiming at another fence of the same family (fence wall for fence
	// walls, road fence for road fences) puts the new one in its place, replaced on click, like the
	// game's walls. Fence walls also stack: aiming at the upper half of a fence wall puts the new one
	// right on top of it, the lower half right below it, same course.
	{
		ARMBuildableFence* Other = Cast<ARMBuildableFence>(hitResult.GetActor());
		const bool bOwnWall = GetFenceDefaults()->IsWall();
		// Replacing needs a different fence: another type, or the same type showing another look.
		const ERMFenceType OwnType = GetFenceDefaults()->mFenceType;
		const bool bSameFence = Other && Other->GetEffectiveType() == OwnType;
		if (Other && Other->IsWall() == bOwnWall && mSnapToGuideLines && !bSameFence)
		{
			TArray<FRMSplinePoint> Points;
			Other->GetWorldSplinePoints(Points);
			if (Points.Num() >= 2)
			{
				mInwardSign = Other->GetInwardSign();
				mOnRoadEdge = true;
				mReplaceTarget = Other;
				SetHiddenTarget(Other);
				SetWorldPoints(Points);
				return true;
			}
		}
		if (bOwnWall && Other && Other->IsWall())
		{
			TArray<FRMSplinePoint> Points;
			Other->GetWorldSplinePoints(Points);
			TArray<FRMFrame> Frames;
			Other->GetWorldFrames(Frames, 100.f);
			if (Points.Num() >= 2 && Frames.Num() >= 2)
			{
				float BaseZ = Frames[0].Pos.Z;
				float Best = TNumericLimits<float>::Max();
				for (const FRMFrame& F : Frames)
				{
					const float D = FVector::DistSquared2D(F.Pos, hitResult.Location);
					if (D < Best)
					{
						Best = D;
						BaseZ = F.Pos.Z;
					}
				}
				const float Wall = ARMBuildableFence::GetWallHeight();
				const float Shift = hitResult.Location.Z - BaseZ < Wall * 0.5f ? -Wall : Wall;
				for (FRMSplinePoint& P : Points)
				{
					P.Location.Z += Shift;
				}
				mInwardSign = Other->GetInwardSign();
				mOnRoadEdge = true;
				SetWorldPoints(Points);
				return true;
			}
		}
	}
	if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(hitResult.GetActor()))
	{
		return SnapToIntersectionCurb(X, hitResult.Location);
	}
	if (ARMBuildableFill* Fill = Cast<ARMBuildableFill>(hitResult.GetActor()))
	{
		return GetFenceDefaults()->IsWall() && SnapToFillSide(Fill, hitResult.Location); // walls only: a fill is no road
	}
	ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(hitResult.GetActor());
	if (!Road)
	{
		return SnapToFoundationGrid(hitResult);
	}

	TArray<FRMFrame> Frames;
	Road->GetWorldFrames(Frames, 50.f);
	if (Frames.Num() < 2)
	{
		return false;
	}

	// Which side: the sign of the hit point along the road's right vector at the closest sample.
	int32 Closest = 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		const float D = FVector::DistSquared(Frames[I].Pos, hitResult.Location);
		if (D < Best)
		{
			Best = D;
			Closest = I;
		}
	}
	const float Side = FVector::DotProduct(hitResult.Location - Frames[Closest].Pos, Frames[Closest].Right) >= 0.f ? 1.f : -1.f;

	// Edge curve: P(s) + d * N(s), with d just inside the edge so the fence stands on the asphalt.
	const float Offset = Side * (Road->GetRoadWidth() * 0.5f - GetFenceDefaults()->mEdgeInset);
	TArray<FRMSplinePoint> Edge;
	RMGeometry::OffsetCurve(Frames, Offset, 400.f, Edge);

	// The fence spline runs the same way as the road, so the road centre is on the fence's -Side side.
	// Fences already on this edge: the new one fills the free stretch under the aim, end to end with them.
	TArray<FRMFrame> Gap;
	if (ClipEdgeToFreeGap(Frames, Offset, Closest, Gap))
	{
		RMGeometry::OffsetCurve(Gap, Offset, 400.f, Edge);
	}

	mInwardSign = mFacingFlipped ? Side : -Side; // scrolled: the same edge, facing away from the road
	mOnRoadEdge = true;
	mOnRoadOrCurb = true;
	SetWorldPoints(Edge);
	return true;
}

bool ARMFenceHologram::SnapToFoundationGrid(const FHitResult& hitResult)
{
	FTransform Piece;
	FBox Box;
	if (!RMRoadUtils::GetFoundationPiece(hitResult, Piece, Box))
	{
		return false;
	}

	// Grid in the piece's local space, counted from its centre, so the edges and the centre of a
	// foundation are always grid points; the point sits on the top face.
	const FVector Scale = Piece.GetScale3D().GetAbs();
	const FVector Center = Box.GetCenter();
	const FVector Local = Piece.InverseTransformPosition(hitResult.Location);
	auto SnapAxis = [this](float Value, float Mid, float Min, float Max, float AxisScale)
	{
		const float Step = mFoundationGridStep / FMath::Max(AxisScale, KINDA_SMALL_NUMBER);
		return FMath::Clamp(Mid + FMath::RoundToFloat((Value - Mid) / Step) * Step, Min, Max);
	};
	const FVector LocalPoint(
		SnapAxis(Local.X, Center.X, Box.Min.X, Box.Max.X, Scale.X),
		SnapAxis(Local.Y, Center.Y, Box.Min.Y, Box.Max.Y, Scale.Y),
		Box.Max.Z);
	const FVector Point = Piece.TransformPosition(LocalPoint);

	if (mStep == 0)
	{
		// Start direction: the foundation axis closest to where you look, then the mouse wheel.
		FVector View = hitResult.TraceEnd - hitResult.TraceStart;
		View.Z = 0.f;
		View = View.GetSafeNormal();
		FVector BestAxis = FVector::ForwardVector;
		float BestDot = -2.f;
		for (const FVector& A : { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) })
		{
			FVector W = Piece.TransformVectorNoScale(A);
			W.Z = 0.f;
			W = W.GetSafeNormal();
			const float D = FVector::DotProduct(W, View);
			if (D > BestDot)
			{
				BestDot = D;
				BestAxis = W;
			}
		}
		mStart = Point;
		mStartSnapped = false;
		mStartDir = BestAxis.RotateAngleAxis(GetStepScrollYaw(), FVector::UpVector);
	}
	else
	{
		mEnd = Point;
		mEndSnapped = false;
	}
	UpdateCurve();
	return true;
}

void ARMFenceHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	// A scroll step while snapped to a road edge / curb (last frame) turns the fence round.
	const int32 Scroll = GetScrollRotateValue();
	if (mLastScroll != TNumericLimits<int32>::Min() && Scroll != mLastScroll && mOnRoadOrCurb)
	{
		mFacingFlipped = !mFacingFlipped;
	}
	mLastScroll = Scroll;
	mOnRoadEdge = false;
	mOnRoadOrCurb = false;
	mInwardSign = 1.f;
	mReplaceTarget = nullptr;
	Super::SetHologramLocationAndRotation(hitResult);
	if (!mReplaceTarget)
	{
		SetHiddenTarget(nullptr);
	}
}

void ARMFenceHologram::OnHologramLockStateChanged()
{
	Super::OnHologramLockStateChanged();
	mSnapBase = GetActorLocation();
	mSnapNudge = FVector::ZeroVector;
}

void ARMFenceHologram::SetHologramNudgeLocation()
{
	if (!mOnRoadEdge)
	{
		Super::SetHologramNudgeLocation();
		return;
	}
	if (IsHologramLocked())
	{
		SetActorLocation(mSnapBase + mSnapNudge);
	}
}

ENudgeFailReason ARMFenceHologram::NudgeHologram(const FVector& NudgeInput, const FHitResult& HitResult)
{
	if (!mOnRoadEdge || !IsHologramLocked())
	{
		return Super::NudgeHologram(NudgeInput, HitResult);
	}
	const FVector Step(FMath::Sign(NudgeInput.X), FMath::Sign(NudgeInput.Y), FMath::Sign(NudgeInput.Z));
	if (Step.IsNearlyZero())
	{
		return ENudgeFailReason::NFR_Disallowed;
	}
	// Forward / right are the world axes nearest to where the camera looks; Z is up / down.
	float CameraYaw = 0.f;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager)
			{
				CameraYaw = PC->PlayerCameraManager->GetCameraRotation().Yaw;
			}
		}
	}
	const float AxisYaw = FMath::RoundToFloat(CameraYaw / 90.f) * 90.f;
	const FVector Forward = FRotator(0.f, AxisYaw, 0.f).Vector();
	const FVector Right = FRotator(0.f, AxisYaw + 90.f, 0.f).Vector();
	mSnapNudge += (Forward * Step.X + Right * Step.Y + FVector::UpVector * Step.Z) * GetNudgeDistance();
	SetHologramNudgeLocation();
	return ENudgeFailReason::NFR_Success;
}

void ARMFenceHologram::SetHiddenTarget(AActor* Target)
{
	if (mHiddenTarget.Get() == Target)
	{
		return;
	}
	if (AActor* Old = mHiddenTarget.Get())
	{
		Old->SetActorHiddenInGame(false);
	}
	mHiddenTarget = Target;
	if (Target)
	{
		Target->SetActorHiddenInGame(true); // the hologram stands exactly where it is
	}
}

AActor* ARMFenceHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Target = mReplaceTarget;
	AActor* Built = Super::Construct(out_children, constructionID);
	// Replacing a fence: if the game's upgrade flow did not remove the old wall, remove it here
	// and give its cost back to the player (as dismantling would).
	ARMBuildableFence* Old = Cast<ARMBuildableFence>(Target);
	if (Built && Old && Old != Built && IsValid(Old) && !Old->IsActorBeingDestroyed() && !Old->GetIsDismantled()
		&& !Old->GetIsPendingDismantleRemoval() && Old->HasAuthority())
	{
		TArray<FInventoryStack> Refund;
		IFGDismantleInterface::Execute_GetDismantleRefund(Old, Refund, false);
		IFGDismantleInterface::Execute_PreUpgrade(Old);
		IFGDismantleInterface::Execute_Upgrade(Old, Built);
		IFGDismantleInterface::Execute_Dismantle(Old);
		if (AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(GetConstructionInstigator()))
		{
			if (UFGInventoryComponent* Inventory = Player->GetInventory())
			{
				for (const FInventoryStack& Stack : Refund)
				{
					Inventory->AddStack(Stack, true);
				}
			}
		}
	}
	SetHiddenTarget(nullptr);
	return Built;
}

void ARMFenceHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SetHiddenTarget(nullptr);
	Super::EndPlay(EndPlayReason);
}

bool ARMFenceHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mOnRoadEdge)
	{
		return true; // the whole edge is already known: build right away
	}
	return Super::DoMultiStepPlacement(isInputFromARelease);
}

void ARMFenceHologram::RebuildPreview()
{
	const ARMBuildableFence* Fence = GetFenceDefaults();
	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 50.f);
	mPreviewMesh->ClearAllMeshSections();
	if (Frames.Num() < 2)
	{
		return;
	}

	FRMMesh Main, Panel;
	ARMBuildableFence::BuildFenceMesh(Fence->mFenceType, Frames, 400.f, mInwardSign, Main, Panel, Fence->GetWallLayout());
	Main.ApplyToSection(mPreviewMesh, 0, false);
	Panel.ApplyToSection(mPreviewMesh, 1, false);
	UpdateHologramLook();
}

void ARMFenceHologram::UpdateHologramLook()
{
	const ARMBuildableFence* Fence = GetFenceDefaults();
	if (!Fence || mPreviewMesh->GetNumSections() == 0 || !Fence->GetEffectiveMainMaterial())
	{
		Super::UpdateHologramLook();
		return;
	}
	// The fence's own materials (wall look of this fence class, grid panel) and its paint (the
	// hologram's colours: the chosen swatch, or the one sampled with the middle mouse button); the game's
	// hologram tint and outline (stencil) show valid / warning / invalid on top.
	mPreviewMesh->SetMaterial(0, Fence->GetEffectiveMainMaterial());
	if (mPreviewMesh->GetNumSections() > 1 && Fence->mPanelMaterial)
	{
		mPreviewMesh->SetMaterial(1, Fence->mPanelMaterial);
	}
	Fence->ApplyPaintToMesh(mPreviewMesh, mCustomizationData);
	RMHologramLook::Apply(mPreviewMesh, nullptr, GetStencilForHologramMaterialState(mPlacementMaterialState));
}

void ARMFenceHologram::ApplyCustomizationData()
{
	Super::ApplyCustomizationData();
	UpdateHologramLook();
}

void ARMFenceHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.
	if (mStep == 1 && !mOnRoadEdge)
	{
		TArray<FRMFrame> Frames;
		GetPreviewFrames(Frames, 100.f);
		if (Frames.Num() < 2 || Frames.Last().Dist < 100.f)
		{
			AddConstructDisqualifier(URMCD_TooShort::StaticClass());
		}
	}
	// Fence walls cannot share their course with another fence wall (like the game's walls): touching
	// end to end, stacked edge to edge or overlapping in part is fine (yellow), the same curve is not.
	mTouchingOther = false;
	if (GetFenceDefaults()->IsWall() && OverlapsAnotherWall(TOptional<ERMFenceType>(), &mTouchingOther))
	{
		AddConstructDisqualifier(URMCD_FenceWallTaken::StaticClass());
	}
	// Road fences: with the snap key, not along the same kind of road fence (a different kind is
	// fine: it replaces it when aimed at, or stands beside it).
	if (!GetFenceDefaults()->IsWall() && mSnapToGuideLines && OverlapsAnotherWall(GetFenceDefaults()->mFenceType, &mTouchingOther))
	{
		AddConstructDisqualifier(URMCD_SameFenceTaken::StaticClass());
	}
}

bool ARMFenceHologram::ClipEdgeToFreeGap(const TArray<FRMFrame>& Frames, float Offset, int32 Aim, TArray<FRMFrame>& OutGap) const
{
	OutGap.Reset();
	UWorld* World = GetWorld();
	const int32 Num = Frames.Num();
	if (!World || Num < 2 || !Frames.IsValidIndex(Aim))
	{
		return false;
	}
	// The edge line, sample by sample (the same 50 cm samples as the road's frames).
	TArray<FVector> EdgePts;
	EdgePts.Reserve(Num);
	FBox Box(ForceInit);
	for (const FRMFrame& F : Frames)
	{
		EdgePts.Add(F.Pos + F.Right * Offset);
		Box += EdgePts.Last();
	}
	Box = Box.ExpandBy(FVector(200.f, 200.f, 200.f));

	// The fences of the same family near this edge, as 50 cm samples, with their two ends.
	const bool bOwnWall = GetFenceDefaults()->IsWall();
	const ERMFenceType OwnType = GetFenceDefaults()->mFenceType;
	TArray<TArray<FRMFrame>> Others;
	TArray<FVector> Ends;
	for (TActorIterator<ARMBuildableFence> It(World); It; ++It)
	{
		ARMBuildableFence* Other = *It;
		const bool bCandidate = Other && (bOwnWall ? Other->IsWall() : (!Other->IsWall() && Other->GetEffectiveType() == OwnType));
		if (!bCandidate || Other->GetIsDismantled() || Other->IsActorBeingDestroyed() || !Other->GetComponentsBoundingBox(true).Intersect(Box))
		{
			continue;
		}
		TArray<FRMFrame> OF;
		Other->GetWorldFrames(OF, 50.f);
		if (OF.Num() >= 2)
		{
			Ends.Add(OF[0].Pos);
			Ends.Add(OF.Last().Pos);
			Others.Add(MoveTemp(OF));
		}
	}
	if (Others.Num() == 0)
	{
		return false;
	}
	// A sample is taken when a fence runs on it: within 40 cm sideways, the same course (not one stacked above).
	auto Taken = [&](int32 I)
	{
		const FVector& P = EdgePts[I];
		for (const TArray<FRMFrame>& OF : Others)
		{
			const FVector& A = OF[0].Pos;
			const FVector& B = OF.Last().Pos;
			for (const FRMFrame& F : OF)
			{
				if (FVector::DistSquared2D(F.Pos, P) < 40.f * 40.f && FMath::Abs(F.Pos.Z - P.Z) < 150.f)
				{
					// Not past either end of that fence (its end samples reach 25 cm beyond the end otherwise).
					const bool bPastA = FVector::DotProduct(P - A, OF[0].Fwd) < 0.f;
					const bool bPastB = FVector::DotProduct(P - B, OF.Last().Fwd) > 0.f;
					if (!bPastA && !bPastB)
					{
						return true;
					}
				}
			}
		}
		return false;
	};
	if (Taken(Aim))
	{
		return false;
	}
	int32 Lo = Aim, Hi = Aim;
	while (Lo > 0 && !Taken(Lo - 1))
	{
		--Lo;
	}
	while (Hi < Num - 1 && !Taken(Hi + 1))
	{
		++Hi;
	}
	if (Lo == 0 && Hi == Num - 1)
	{
		return false;
	}
	// Exact ends: where the neighbouring fence's end lies along the road, between the taken and the free sample.
	auto ExactDist = [&](int32 Free, int32 TakenI)
	{
		const FVector& P = EdgePts[Free];
		const FVector* Best = nullptr;
		float BestD = TNumericLimits<float>::Max();
		for (const FVector& E : Ends)
		{
			const float D = FVector::DistSquared(E, P);
			if (D < BestD)
			{
				BestD = D;
				Best = &E;
			}
		}
		const float DFree = Frames[Free].Dist;
		const float DTaken = Frames[TakenI].Dist;
		if (!Best || BestD > 150.f * 150.f)
		{
			return (DFree + DTaken) * 0.5f;
		}
		// Edge distance to centre-line distance (they differ on curves).
		const FVector EdgeStep = EdgePts[TakenI] - P;
		const float EdgeLen = FMath::Max(EdgeStep.Size2D(), 1.f);
		const FVector ToEnd = *Best - P;
		const float Along = FVector::DotProduct(FVector(ToEnd.X, ToEnd.Y, 0.f), EdgeStep.GetSafeNormal2D());
		const float T = FMath::Clamp(Along / EdgeLen, 0.f, 1.f);
		return FMath::Lerp(DFree, DTaken, T);
	};
	const float D0 = Lo > 0 ? ExactDist(Lo, Lo - 1) : Frames[0].Dist;
	const float D1 = Hi < Num - 1 ? ExactDist(Hi, Hi + 1) : Frames.Last().Dist;
	if (D1 - D0 < 100.f)
	{
		return false;
	}
	auto FrameAt = [&](float D)
	{
		int32 I = 0;
		while (I < Num - 2 && Frames[I + 1].Dist < D)
		{
			++I;
		}
		const FRMFrame& A = Frames[I];
		const FRMFrame& B = Frames[I + 1];
		const float T = FMath::Clamp((D - A.Dist) / FMath::Max(B.Dist - A.Dist, 0.01f), 0.f, 1.f);
		FRMFrame F;
		F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
		F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
		F.Right = FMath::Lerp(A.Right, B.Right, T).GetSafeNormal();
		F.Up = FMath::Lerp(A.Up, B.Up, T).GetSafeNormal();
		F.Dist = D;
		return F;
	};
	OutGap.Add(FrameAt(D0));
	for (const FRMFrame& F : Frames)
	{
		if (F.Dist > D0 + 1.f && F.Dist < D1 - 1.f)
		{
			OutGap.Add(F);
		}
	}
	OutGap.Add(FrameAt(D1));
	for (FRMFrame& F : OutGap)
	{
		F.Dist -= D0;
	}
	return OutGap.Num() >= 2;
}

EHologramMaterialState ARMFenceHologram::GetHologramMaterialState() const
{
	const EHologramMaterialState State = Super::GetHologramMaterialState();
	return State == EHologramMaterialState::HMS_OK && mTouchingOther ? EHologramMaterialState::HMS_WARNING : State;
}

bool ARMFenceHologram::OverlapsAnotherWall(TOptional<ERMFenceType> SameRoadFence, bool* OutTouching) const
{
	UWorld* World = GetWorld();
	TArray<FRMFrame> Local;
	GetPreviewFrames(Local, 100.f);
	if (!World || Local.Num() < 2)
	{
		return false;
	}
	FTransform T = GetActorTransform();
	T.SetScale3D(FVector::OneVector);
	const float Length = Local.Last().Dist;
	const float Wall = ARMBuildableFence::GetWallHeight();
	TArray<FVector> Samples; // along the new wall, leaving its two ends free (walls may meet end to end)
	FBox Box(ForceInit);
	for (const FRMFrame& F : Local)
	{
		const FVector P = T.TransformPosition(F.Pos);
		Box += P;
		if (F.Dist > 50.f && F.Dist < Length - 50.f)
		{
			Samples.Add(P);
		}
	}
	if (Samples.Num() == 0)
	{
		Samples.Add(T.TransformPosition(Local[Local.Num() / 2].Pos));
	}
	Box = Box.ExpandBy(FVector(100.f, 100.f, Wall + 100.f));
	for (TActorIterator<ARMBuildableFence> It(World); It; ++It)
	{
		ARMBuildableFence* Other = *It;
		if (!Other)
		{
			continue;
		}
		const bool bCandidate = SameRoadFence.IsSet() ? (!Other->IsWall() && Other->GetEffectiveType() == SameRoadFence.GetValue()) : Other->IsWall();
		if (!Other || !bCandidate || Other == mReplaceTarget || Other->GetIsDismantled() || Other->IsActorBeingDestroyed()
			|| !Other->GetComponentsBoundingBox(true).Intersect(Box))
		{
			continue;
		}
		TArray<FRMFrame> Frames;
		Other->GetWorldFrames(Frames, 50.f);
		if (Frames.Num() < 2)
		{
			continue;
		}
		int32 Shared = 0; // samples of the new wall (1 m apart) lying on the other one
		for (const FVector& P : Samples)
		{
			const FRMFrame* Near = nullptr;
			float Best = TNumericLimits<float>::Max();
			for (const FRMFrame& F : Frames)
			{
				const float D = FVector::DistSquared2D(F.Pos, P);
				if (D < Best)
				{
					Best = D;
					Near = &F;
				}
			}
			if (!Near)
			{
				continue;
			}
			const FVector Rel = P - Near->Pos;
			const float Along = FMath::Abs(FVector::DotProduct(FVector(Rel.X, Rel.Y, 0.f), FVector(Near->Fwd.X, Near->Fwd.Y, 0.f).GetSafeNormal()));
			const float Across = FMath::Abs(FVector::DotProduct(FVector(Rel.X, Rel.Y, 0.f), FVector(Near->Right.X, Near->Right.Y, 0.f).GetSafeNormal()));
			// Both go up from their base line: walls Wall high; road fences count as the same when their bases are within 1 m.
			const float Overlap = (SameRoadFence.IsSet() ? 120.f : Wall) - FMath::Abs(Rel.Z);
			if (Along < 30.f && Across < 40.f && Overlap > 20.f)
			{
				++Shared;
			}
		}
		if (Shared == 0)
		{
			continue;
		}
		// The same curve: they run together for most of the shorter one (its two free ends left out).
		const float SharedLength = Shared * 100.f;
		const float Shorter = FMath::Max(FMath::Min(Length, Frames.Last().Dist) - 100.f, 100.f);
		if (SharedLength >= 0.75f * Shorter)
		{
			return true;
		}
		if (OutTouching)
		{
			*OutTouching = true;
		}
	}
	return false;
}

void ARMFenceHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMFenceHologram, mInwardSign);
	DOREPLIFETIME(ARMFenceHologram, mReplaceTarget);
}

void ARMFenceHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	if (ARMBuildableFence* Fence = Cast<ARMBuildableFence>(inBuildable))
	{
		Fence->SetInwardSign(mInwardSign);
	}
	Super::ConfigureActor(inBuildable);
}

bool ARMFenceHologram::SnapToFillSide(ARMBuildableFill* Fill, const FVector& AimPoint)
{
	TArray<FVector> Ring;
	Fill->GetWorldOutline(Ring);
	const int32 N = Ring.Num();
	if (N < 3)
	{
		return false;
	}
	// Wound so the inside is on each edge's +Right side (as an intersection's curbs).
	float Area = 0.f;
	for (int32 I = 0; I < N; ++I)
	{
		Area += Ring[I].X * Ring[(I + 1) % N].Y - Ring[(I + 1) % N].X * Ring[I].Y;
	}
	if (Area < 0.f)
	{
		Algo::Reverse(Ring);
	}
	// Corners: where the outline turns more than 35 degrees.
	auto Dir = [&Ring, N](int32 I) { return FVector(Ring[(I + 1) % N] - Ring[I]).GetSafeNormal2D(); };
	TArray<int32> Corners;
	for (int32 I = 0; I < N; ++I)
	{
		if (FVector::DotProduct(Dir((I + N - 1) % N), Dir(I)) < FMath::Cos(FMath::DegreesToRadians(35.f)))
		{
			Corners.Add(I);
		}
	}
	// The edge nearest the aim, and the side (corner to corner) it belongs to.
	int32 Nearest = 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 I = 0; I < N; ++I)
	{
		const FVector A = Ring[I], B = Ring[(I + 1) % N];
		const FVector AB(B.X - A.X, B.Y - A.Y, 0.f);
		const float T = FMath::Clamp(FVector::DotProduct(FVector(AimPoint.X - A.X, AimPoint.Y - A.Y, 0.f), AB) / FMath::Max(AB.SizeSquared(), 1.f), 0.f, 1.f);
		const float D = FVector::DistSquared2D(A + (B - A) * T, AimPoint);
		if (D < Best)
		{
			Best = D;
			Nearest = I;
		}
	}
	int32 First = 0, Last = N; // no corners: all the way round
	if (Corners.Num() > 0)
	{
		First = Corners.Last();
		for (int32 C : Corners)
		{
			if (C <= Nearest)
			{
				First = C;
			}
		}
		Last = Corners[0] + N;
		for (int32 C : Corners)
		{
			if (C > Nearest)
			{
				Last = C;
				break;
			}
		}
		if (First > Nearest)
		{
			First -= N; // the side runs over the start of the outline
		}
		if (Last <= First)
		{
			Last += N;
		}
	}
	const float Inset = GetFenceDefaults()->mEdgeInset;
	TArray<FVector> Inside;
	for (int32 K = First; K <= Last; ++K)
	{
		const int32 I = ((K % N) + N) % N;
		const FVector Prev = Ring[((I - 1) % N + N) % N];
		const FVector Next = Ring[(I + 1) % N];
		// At the side's ends, the direction of the side itself (not of the corner).
		const FVector Fwd = (K == First ? FVector(Next - Ring[I]) : K == Last ? FVector(Ring[I] - Prev) : FVector(Next - Prev)).GetSafeNormal2D();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
		Inside.Add(Ring[I] + Right * Inset);
	}
	if (Inside.Num() < 2)
	{
		return false;
	}
	TArray<FRMSplinePoint> Points;
	RMGeometry::SplineThroughPoints(Inside, Points);
	mInwardSign = mFacingFlipped ? -1.f : 1.f; // scrolled: facing away from the fill
	mOnRoadEdge = true;
	mOnRoadOrCurb = true;
	SetWorldPoints(Points);
	return true;
}

bool ARMFenceHologram::SnapToIntersectionCurb(ARMBuildableIntersection* Intersection, const FVector& AimPoint)
{
	TArray<TArray<FVector>> Curbs;
	Intersection->GetWorldCurbs(Curbs);

	// The curb with a point closest to where the player aims.
	int32 BestCurb = INDEX_NONE;
	float Best = TNumericLimits<float>::Max();
	for (int32 C = 0; C < Curbs.Num(); ++C)
	{
		for (const FVector& P : Curbs[C])
		{
			const float D = FVector::DistSquared2D(P, AimPoint);
			if (D < Best)
			{
				Best = D;
				BestCurb = C;
			}
		}
	}
	if (BestCurb == INDEX_NONE)
	{
		return false;
	}

	// Curbs run counter-clockwise, so the junction centre is on their +Right side: move the fence
	// that way by the inset so it stands on the slab, and face it that way.
	const TArray<FVector>& Curb = Curbs[BestCurb];
	const float Inset = GetFenceDefaults()->mEdgeInset;
	TArray<FVector> Inside;
	for (int32 I = 0; I < Curb.Num(); ++I)
	{
		const FVector Prev = Curb[FMath::Max(I - 1, 0)];
		const FVector Next = Curb[FMath::Min(I + 1, Curb.Num() - 1)];
		const FVector Fwd = FVector(Next.X - Prev.X, Next.Y - Prev.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
		Inside.Add(Curb[I] + Right * Inset);
	}

	TArray<FRMSplinePoint> Points;
	RMGeometry::SplineThroughPoints(Inside, Points);
	mInwardSign = mFacingFlipped ? -1.f : 1.f; // scrolled: facing away from the intersection
	mOnRoadEdge = true;
	mOnRoadOrCurb = true;
	SetWorldPoints(Points);
	return true;
}

float ARMFenceHologram::GetCostUnitLength() const
{
	return mBuildClass && mBuildClass->IsChildOf(ARMBuildableSignWall::StaticClass()) ? 800.f : 400.f;
}
