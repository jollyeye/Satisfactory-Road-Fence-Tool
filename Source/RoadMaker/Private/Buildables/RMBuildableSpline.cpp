#include "Buildables/RMBuildableSpline.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "FGDismantleInterface.h"
#include "FGRecipe.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Algo/Reverse.h"

ARMBuildableSpline::ARMBuildableSpline(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ARMBuildableSpline::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableSpline, mSplinePoints);
	DOREPLIFETIME_CONDITION(ARMBuildableSpline, mRevealFrom, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(ARMBuildableSpline, mRevealPoint, COND_InitialOnly);
}

void ARMBuildableSpline::SetSplinePoints(const TArray<FRMSplinePoint>& LocalPoints)
{
	mSplinePoints = LocalPoints;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableSpline::OnRep_SplinePoints()
{
	RebuildMesh();
}

void ARMBuildableSpline::GetLocalFrames(TArray<FRMFrame>& OutFrames, float Step) const
{
	RMGeometry::SampleSpline(mSplinePoints, Step, OutFrames);
}

void ARMBuildableSpline::GetWorldFrames(TArray<FRMFrame>& OutFrames, float Step) const
{
	GetLocalFrames(OutFrames, Step);
	// Without the actor's scale: a piece that was just built is squashed while its rise animation plays,
	// but its frames are where it will be (pieces placed on it at once, e.g. moved lane markings, need that).
	FTransform T = GetActorTransform();
	T.SetScale3D(FVector::OneVector);
	for (FRMFrame& F : OutFrames)
	{
		F.Pos = T.TransformPosition(F.Pos);
		F.Fwd = T.TransformVectorNoScale(F.Fwd);
		F.Right = T.TransformVectorNoScale(F.Right);
		F.Up = T.TransformVectorNoScale(F.Up);
	}
}

float ARMBuildableSpline::GetLength() const
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, 100.f);
	return Frames.Num() > 0 ? Frames.Last().Dist : 0.f;
}

bool ARMBuildableSpline::GetFrameAt(float Dist, FRMFrame& OutFrame) const
{
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 25.f);
	if (Frames.Num() < 2)
	{
		return false;
	}
	OutFrame = Frames.Last();
	for (int32 I = 1; I < Frames.Num(); ++I)
	{
		if (Frames[I].Dist >= Dist)
		{
			const FRMFrame& A = Frames[I - 1];
			const FRMFrame& B = Frames[I];
			const float Alpha = FMath::Clamp((Dist - A.Dist) / FMath::Max(B.Dist - A.Dist, KINDA_SMALL_NUMBER), 0.f, 1.f);
			OutFrame = A;
			OutFrame.Pos = FMath::Lerp(A.Pos, B.Pos, Alpha);
			OutFrame.Fwd = FMath::Lerp(A.Fwd, B.Fwd, Alpha).GetSafeNormal();
			OutFrame.Right = FMath::Lerp(A.Right, B.Right, Alpha).GetSafeNormal();
			OutFrame.Up = FMath::Lerp(A.Up, B.Up, Alpha).GetSafeNormal();
			OutFrame.Dist = Dist;
			break;
		}
	}
	return true;
}

namespace
{
	/** Spawns a copy of Source (class, recipe, paint, settings) along the given world curve. */
	ARMBuildableSpline* SpawnCutPiece(ARMBuildableSpline* Source, const TArray<FRMSplinePoint>& World, int32 CostMultiplier, ERMRevealFrom RevealFrom)
	{
		return Source->SpawnLike(Source->GetClass(), Source->GetBuiltWithRecipe(), World, CostMultiplier, RevealFrom);
	}
}

namespace
{
	/** The frame at distance D along Frames (sorted by Dist), interpolated between its neighbours. */
	FRMFrame RMFrameAtDist(const TArray<FRMFrame>& Frames, float D)
	{
		for (int32 I = 1; I < Frames.Num(); ++I)
		{
			if (Frames[I].Dist >= D)
			{
				const FRMFrame& A = Frames[I - 1];
				const FRMFrame& B = Frames[I];
				const float T = B.Dist > A.Dist ? FMath::Clamp((D - A.Dist) / (B.Dist - A.Dist), 0.f, 1.f) : 0.f;
				FRMFrame F;
				F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
				F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
				F.Right = FMath::Lerp(A.Right, B.Right, T).GetSafeNormal();
				F.Up = FMath::Lerp(A.Up, B.Up, T).GetSafeNormal();
				F.Dist = D;
				return F;
			}
		}
		return Frames.Num() > 0 ? Frames.Last() : FRMFrame();
	}

	/** Which side of a section boundary a point is on (+ = after it): seen from above, across the road. */
	float RMBoundarySide(const FRMFrame& Bound, const FVector& P)
	{
		const FVector Normal = FVector(Bound.Fwd.X, Bound.Fwd.Y, 0.f).GetSafeNormal();
		return FVector::DotProduct(P - Bound.Pos, Normal);
	}
}

void ARMBuildableSpline::UpdateChunkLayout()
{
	mChunkFrames.Reset();
	mChunkBounds.Reset();
	GetLocalFrames(mChunkFrames, 1000.f);
	const float Length = mChunkFrames.Num() >= 2 ? mChunkFrames.Last().Dist : 0.f;
	mChunkCount = Length > SingleSectionMax ? FMath::CeilToInt(Length / SectionLength) : 1;
	mChunkLength = mChunkCount > 0 ? Length / mChunkCount : Length;
	if (mChunkCount > 1)
	{
		// Boundary frames from a fine sampling (1 m), so they sit on the curve within millimetres.
		TArray<FRMFrame> Fine;
		GetLocalFrames(Fine, 100.f);
		const float Scale = Fine.Num() >= 2 && Fine.Last().Dist > 0.f ? Fine.Last().Dist / Length : 1.f;
		for (int32 K = 1; K < mChunkCount; ++K)
		{
			mChunkBounds.Add(RMFrameAtDist(Fine, K * mChunkLength * Scale));
		}
	}
}

void ARMBuildableSpline::AddChunkBoundaries(TArray<FRMFrame>& Frames) const
{
	if (mChunkBounds.Num() == 0 || Frames.Num() < 2)
	{
		return;
	}
	for (const FRMFrame& Bound : mChunkBounds)
	{
		// The ring nearest the boundary is moved onto it when close, else one is added.
		int32 After = Frames.Num() - 1;
		for (int32 I = 1; I < Frames.Num(); ++I)
		{
			if (Frames[I].Dist >= Bound.Dist)
			{
				After = I;
				break;
			}
		}
		const float Step = FMath::Max(Frames[After].Dist - Frames[After - 1].Dist, 1.f);
		if (FMath::Abs(Frames[After].Dist - Bound.Dist) < Step * 0.1f && After < Frames.Num() - 1)
		{
			Frames[After] = Bound;
		}
		else if (FMath::Abs(Frames[After - 1].Dist - Bound.Dist) < Step * 0.1f && After - 1 > 0)
		{
			Frames[After - 1] = Bound;
		}
		else
		{
			Frames.Insert(Bound, After);
		}
	}
}

int32 ARMBuildableSpline::GetChunkOf(const FVector& LocalPos) const
{
	if (mChunkCount <= 1 || mChunkFrames.Num() < 2 || mChunkLength <= 0.f)
	{
		return 0;
	}
	// The nearest 10 m frame, then the exact spot along the segment to its neighbour.
	int32 Best = 0;
	float BestSq = TNumericLimits<float>::Max();
	for (int32 I = 0; I < mChunkFrames.Num(); ++I)
	{
		const float Sq = FVector::DistSquared(LocalPos, mChunkFrames[I].Pos);
		if (Sq < BestSq)
		{
			BestSq = Sq;
			Best = I;
		}
	}
	const FRMFrame& F = mChunkFrames[Best];
	const float Along = F.Dist + FVector::DotProduct(LocalPos - F.Pos, F.Fwd);
	int32 Chunk = FMath::Clamp(FMath::FloorToInt(Along / mChunkLength), 0, mChunkCount - 1);
	// Near a boundary the estimate can be one off (on the outside of a curve most): the boundary's own
	// cross plane decides, the same plane every detail level has a ring in (AddChunkBoundaries).
	if (mChunkBounds.Num() == mChunkCount - 1)
	{
		if (Chunk > 0 && RMBoundarySide(mChunkBounds[Chunk - 1], LocalPos) < 0.f)
		{
			--Chunk;
		}
		else if (Chunk < mChunkCount - 1 && RMBoundarySide(mChunkBounds[Chunk], LocalPos) >= 0.f)
		{
			++Chunk;
		}
	}
	return Chunk;
}

int32 ARMBuildableSpline::GetSectionCount() const
{
	const float Length = GetLength();
	return Length > SingleSectionMax ? FMath::CeilToInt(Length / SectionLength) : 1;
}

void ARMBuildableSpline::GetSectionRange(int32 Section, float& OutFrom, float& OutTo) const
{
	const float Length = GetLength();
	const int32 Count = GetSectionCount();
	const float Each = Length / Count;
	OutFrom = FMath::Clamp(Section, 0, Count - 1) * Each;
	OutTo = Section >= Count - 1 ? Length : OutFrom + Each;
}

void ARMBuildableSpline::PrepareReveal()
{
	GetLocalFrames(mRevealFrames, 100.f);
	mRevealFromEnd = mRevealFrom == ERMRevealFrom::End;
	mRevealPointAlong = -1.f;
	if (mRevealFrom == ERMRevealFrom::FromPoint && mRevealFrames.Num() >= 2)
	{
		const FVector Local = GetActorTransform().InverseTransformPosition(mRevealPoint);
		float BestSq = TNumericLimits<float>::Max();
		for (const FRMFrame& F : mRevealFrames)
		{
			const float Sq = FVector::DistSquared(Local, F.Pos);
			if (Sq < BestSq)
			{
				BestSq = Sq;
				mRevealPointAlong = F.Dist;
			}
		}
	}
	if (mRevealFrames.Num() >= 2 && (mRevealFrom == ERMRevealFrom::NearestPlayer || mRevealFrom == ERMRevealFrom::NearestPoint))
	{
		FVector From = mRevealPoint;
		bool bHave = mRevealFrom == ERMRevealFrom::NearestPoint;
		if (!bHave)
		{
			// The end nearer the local player (each player sees it from their own side).
			const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			if (const APawn* Pawn = PC ? PC->GetPawn() : nullptr)
			{
				From = Pawn->GetActorLocation();
				bHave = true;
			}
		}
		if (bHave)
		{
			const FTransform& T = GetActorTransform();
			mRevealFromEnd = FVector::Dist(From, T.TransformPosition(mRevealFrames.Last().Pos)) < FVector::Dist(From, T.TransformPosition(mRevealFrames[0].Pos));
		}
	}
}

float ARMBuildableSpline::RevealDistance(const FVector& LocalPos) const
{
	// Distance along the curve of the nearest centre-line point.
	if (mRevealFrames.Num() == 0)
	{
		return 0.f;
	}
	float BestSq = TNumericLimits<float>::Max();
	float Along = 0.f;
	for (const FRMFrame& F : mRevealFrames)
	{
		const float Sq = FVector::DistSquared(LocalPos, F.Pos);
		if (Sq < BestSq)
		{
			BestSq = Sq;
			Along = F.Dist;
		}
	}
	if (mRevealPointAlong >= 0.f)
	{
		return FMath::Abs(Along - mRevealPointAlong);
	}
	return mRevealFromEnd ? mRevealFrames.Last().Dist - Along : Along;
}

float ARMBuildableSpline::GetBuildAnimDuration() const
{
	return FMath::Clamp(GetLength() / 3000.f, 1.f, 4.f);
}

ARMBuildableSpline* ARMBuildableSpline::SpawnLike(TSubclassOf<ARMBuildableSpline> Class, TSubclassOf<UFGRecipe> Recipe,
	const TArray<FRMSplinePoint>& World, int32 CostMultiplier, ERMRevealFrom RevealFrom, const FVector& RevealPoint) const
{
	UWorld* W = GetWorld();
	if (!W || World.Num() < 2 || !Class)
	{
		return nullptr;
	}
	// Same convention as the holograms: actor at the first point, yawed along its direction.
	const FVector Dir = FVector(World[0].Tangent.X, World[0].Tangent.Y, 0.f).GetSafeNormal();
	const FRotator Yaw(0.f, Dir.IsNearlyZero() ? 0.f : Dir.Rotation().Yaw, 0.f);
	const FTransform T(Yaw, World[0].Location);
	TArray<FRMSplinePoint> Local;
	for (const FRMSplinePoint& P : World)
	{
		Local.Add(FRMSplinePoint(T.InverseTransformPosition(P.Location), T.InverseTransformVectorNoScale(P.Tangent)));
	}
	ARMBuildableSpline* New = W->SpawnActorDeferred<ARMBuildableSpline>(Class, T, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!New)
	{
		return nullptr;
	}
	New->SetBuiltWithRecipe(Recipe);
	New->SetCostMultiplier(CostMultiplier);
	New->SetSplinePoints(Local);
	New->SetCustomizationData_Native(GetCustomizationData_Native());
	CopyCutSettingsTo(New);
	New->SetRevealFrom(RevealFrom, RevealPoint);
	UGameplayStatics::FinishSpawningActor(New, T);
	return New;
}

ARMBuildableSpline* ARMBuildableSpline::CutAt(float Dist, ARMBuildableSpline** OutSecond)
{
	if (OutSecond)
	{
		*OutSecond = nullptr;
	}
	if (!HasAuthority() || GetIsDismantled() || IsActorBeingDestroyed())
	{
		return nullptr;
	}
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 25.f);
	if (Frames.Num() < 3)
	{
		return nullptr;
	}
	const float Total = Frames.Last().Dist;
	if (Dist <= 1.f || Dist >= Total - 1.f)
	{
		return nullptr;
	}

	// The exact cut frame, between the two samples around Dist.
	FRMFrame Cut = Frames.Last();
	for (int32 I = 1; I < Frames.Num(); ++I)
	{
		if (Frames[I].Dist >= Dist)
		{
			const FRMFrame& A = Frames[I - 1];
			const FRMFrame& B = Frames[I];
			const float Alpha = (Dist - A.Dist) / FMath::Max(B.Dist - A.Dist, KINDA_SMALL_NUMBER);
			Cut = A;
			Cut.Pos = FMath::Lerp(A.Pos, B.Pos, Alpha);
			Cut.Fwd = FMath::Lerp(A.Fwd, B.Fwd, Alpha).GetSafeNormal();
			Cut.Right = FMath::Lerp(A.Right, B.Right, Alpha).GetSafeNormal();
			Cut.Up = FMath::Lerp(A.Up, B.Up, Alpha).GetSafeNormal();
			Cut.Dist = Dist;
			break;
		}
	}
	TArray<FRMFrame> FirstFrames;
	TArray<FRMFrame> SecondFrames;
	SecondFrames.Add(Cut);
	for (const FRMFrame& F : Frames)
	{
		if (F.Dist < Dist - 0.5f)
		{
			FirstFrames.Add(F);
		}
		else if (F.Dist > Dist + 0.5f)
		{
			SecondFrames.Add(F);
		}
	}
	FirstFrames.Add(Cut);
	// Second piece measured from its own start.
	for (FRMFrame& F : SecondFrames)
	{
		F.Dist -= Dist;
	}

	// Each piece is refitted through its own part of the curve (keys every 2 m, exact end points and
	// end directions), so both follow the original piece.
	TArray<FRMSplinePoint> FirstPoints, SecondPoints;
	RMGeometry::OffsetCurve(FirstFrames, 0.f, 200.f, FirstPoints);
	RMGeometry::OffsetCurve(SecondFrames, 0.f, 200.f, SecondPoints);
	if (FirstPoints.Num() < 2 || SecondPoints.Num() < 2)
	{
		return nullptr;
	}
	if (!CanCutAt(Dist))
	{
		return nullptr;
	}
	const int32 FirstCost = FMath::Max(1, FMath::RoundToInt(mCostMultiplier * Dist / Total));
	const int32 SecondCost = FMath::Max(1, mCostMultiplier - FirstCost);
	// Build animations from the outer ends, meeting at the cut.
	ARMBuildableSpline* First = SpawnCutPiece(this, FirstPoints, FirstCost, ERMRevealFrom::Start);
	ARMBuildableSpline* Second = SpawnCutPiece(this, SecondPoints, SecondCost, ERMRevealFrom::End);
	if (!First || !Second)
	{
		if (First)
		{
			First->Destroy();
		}
		if (Second)
		{
			Second->Destroy();
		}
		return nullptr;
	}
	BeforeCutDismantle(Dist);

	// Out of the way at once (the dismantle takes a moment): aiming must find the new pieces.
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	IFGDismantleInterface::Execute_Dismantle(this);
	if (OutSecond)
	{
		*OutSecond = Second;
	}
	return First;
}

namespace
{
	/** Distance (seen from above) from Q to the segment of Curve ending at point I. */
	float RMJoinSegDist2D(const TArray<FRMFrame>& Curve, int32 I, const FVector& Q)
	{
		const FVector A(Curve[I - 1].Pos.X, Curve[I - 1].Pos.Y, 0.f);
		const FVector B(Curve[I].Pos.X, Curve[I].Pos.Y, 0.f);
		return FVector::Dist(Q, FMath::ClosestPointOnSegment(Q, A, B));
	}

	/**
	 * Largest distance (seen from above) from a point of Path to the polyline Curve. Both run the same
	 * way, so each point is looked for near where the last one was (fast for 1 km pieces); only a
	 * point not found close by searches the whole curve.
	 */
	float RMJoinMaxDeviation2D(const TArray<FRMFrame>& Path, const TArray<FRMFrame>& Curve)
	{
		float Worst = 0.f;
		int32 Near = 1;
		for (const FRMFrame& P : Path)
		{
			const FVector Q(P.Pos.X, P.Pos.Y, 0.f);
			float Best = TNumericLimits<float>::Max();
			const int32 Lo = FMath::Max(1, Near - 5);
			const int32 Hi = FMath::Min(Curve.Num() - 1, Near + 30);
			for (int32 I = Lo; I <= Hi; ++I)
			{
				const float D = RMJoinSegDist2D(Curve, I, Q);
				if (D < Best)
				{
					Best = D;
					Near = I;
				}
			}
			if (Best > 25.f)
			{
				for (int32 I = 1; I < Curve.Num(); ++I)
				{
					const float D = RMJoinSegDist2D(Curve, I, Q);
					if (D < Best)
					{
						Best = D;
						Near = I;
					}
				}
			}
			Worst = FMath::Max(Worst, Best);
		}
		return Worst;
	}
}

ERMJoinCourse ARMBuildableSpline::JoinCourse(const ARMBuildableSpline* Other, int32 MyEnd, int32 OtherEnd, float MaxLength, TArray<FRMFrame>* OutFrames) const
{
	if (!Other || Other == this)
	{
		return ERMJoinCourse::NotJoined;
	}
	TArray<FRMFrame> A, B;
	GetWorldFrames(A, 100.f);
	Other->GetWorldFrames(B, 100.f);
	if (A.Num() < 2 || B.Num() < 2)
	{
		return ERMJoinCourse::NotJoined;
	}
	// Order: this road's outer end -> joint -> the other road's outer end.
	// A course turned around runs the other way: its frames face the other way too.
	auto Turn = [](TArray<FRMFrame>& Frames)
	{
		Algo::Reverse(Frames);
		for (FRMFrame& F : Frames)
		{
			F.Fwd = -F.Fwd;
			F.Right = -F.Right;
		}
	};
	if (MyEnd == 0)
	{
		Turn(A);
	}
	if (OtherEnd == 1)
	{
		Turn(B);
	}
	const FVector Joint = A.Last().Pos;
	if (FVector::Dist2D(Joint, B[0].Pos) > 50.f || FMath::Abs(Joint.Z - B[0].Pos.Z) > 50.f)
	{
		return ERMJoinCourse::NotJoined;
	}
	const FVector DirA = FVector(A.Last().Pos - A[A.Num() - 2].Pos).GetSafeNormal2D();
	const FVector DirB = FVector(B[1].Pos - B[0].Pos).GetSafeNormal2D();
	if (FVector::DotProduct(DirA, DirB) < 0.998f) // about 3.5 degrees
	{
		return ERMJoinCourse::Kink;
	}
	// The joined course, measured again from its start.
	TArray<FRMFrame> Path = A;
	for (int32 I = 1; I < B.Num(); ++I)
	{
		Path.Add(B[I]);
	}
	float Dist = 0.f;
	for (int32 I = 0; I < Path.Num(); ++I)
	{
		Dist += I > 0 ? FVector::Dist(Path[I - 1].Pos, Path[I].Pos) : 0.f;
		Path[I].Dist = Dist;
	}
	if (Dist > MaxLength)
	{
		return ERMJoinCourse::TooLong;
	}
	// The curve a single road would have between the outer ends (quadratic as the road tool makes it,
	// or the straight-mode curve): the two roads must lie on it.
	const FVector StartDir = FVector(Path[1].Pos - Path[0].Pos).GetSafeNormal2D();
	const FVector EndDir = FVector(Path.Last().Pos - Path[Path.Num() - 2].Pos).GetSafeNormal2D();
	bool bMatches = false;
	for (int32 Kind = 0; Kind < 2 && !bMatches; ++Kind)
	{
		TArray<FRMSplinePoint> Points;
		if (Kind == 0)
		{
			RMGeometry::MakeQuadratic(Path[0].Pos, StartDir, Path.Last().Pos, EndDir, Points);
		}
		else
		{
			RMGeometry::MakeFixedEnds(Path[0].Pos, StartDir, Path.Last().Pos, EndDir, Points);
		}
		TArray<FRMFrame> Curve;
		RMGeometry::SampleSpline(Points, 100.f, Curve);
		bMatches = Curve.Num() >= 2 && RMJoinMaxDeviation2D(Path, Curve) <= 50.f;
	}
	if (!bMatches)
	{
		return ERMJoinCourse::Curve;
	}
	if (OutFrames)
	{
		if (MyEnd == 0)
		{
			// Built from this piece's outer end so far; turned to run the way this piece runs.
			Turn(Path);
			for (int32 I = 0; I < Path.Num(); ++I)
			{
				Path[I].Dist = Dist - Path[I].Dist;
			}
		}
		*OutFrames = MoveTemp(Path);
	}
	return ERMJoinCourse::Ok;
}

