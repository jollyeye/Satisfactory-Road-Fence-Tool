#include "Buildables/RMBuildableSpline.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "FGDismantleInterface.h"
#include "FGRecipe.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

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

void ARMBuildableSpline::UpdateChunkLayout()
{
	mChunkFrames.Reset();
	GetLocalFrames(mChunkFrames, 1000.f);
	const float Length = mChunkFrames.Num() >= 2 ? mChunkFrames.Last().Dist : 0.f;
	mChunkCount = Length > SingleSectionMax ? FMath::CeilToInt(Length / SectionLength) : 1;
	mChunkLength = mChunkCount > 0 ? Length / mChunkCount : Length;
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
	return FMath::Clamp(FMath::FloorToInt(Along / mChunkLength), 0, mChunkCount - 1);
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
