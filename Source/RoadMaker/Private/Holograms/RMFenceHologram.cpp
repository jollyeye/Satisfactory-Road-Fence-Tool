#include "Holograms/RMFenceHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "RMRoadUtils.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGDismantleInterface.h"

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
	mInwardSign = -Side;
	mOnRoadEdge = true;
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
	mOnRoadEdge = false;
	mInwardSign = 1.f;
	mReplaceTarget = nullptr;
	Super::SetHologramLocationAndRotation(hitResult);
	if (!mReplaceTarget)
	{
		SetHiddenTarget(nullptr);
	}
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
	mInwardSign = 1.f;
	mOnRoadEdge = true;
	SetWorldPoints(Points);
	return true;
}
