#include "Buildables/RMBuildableSpline.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "FGDismantleInterface.h"
#include "FGRecipe.h"

ARMBuildableSpline::ARMBuildableSpline(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ARMBuildableSpline::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableSpline, mSplinePoints);
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
	ARMBuildableSpline* SpawnCutPiece(ARMBuildableSpline* Source, const TArray<FRMSplinePoint>& World, int32 CostMultiplier)
	{
		return Source->SpawnLike(Source->GetClass(), Source->GetBuiltWithRecipe(), World, CostMultiplier);
	}
}

ARMBuildableSpline* ARMBuildableSpline::SpawnLike(TSubclassOf<ARMBuildableSpline> Class, TSubclassOf<UFGRecipe> Recipe,
	const TArray<FRMSplinePoint>& World, int32 CostMultiplier) const
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
	ARMBuildableSpline* First = SpawnCutPiece(this, FirstPoints, FirstCost);
	ARMBuildableSpline* Second = SpawnCutPiece(this, SecondPoints, SecondCost);
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
