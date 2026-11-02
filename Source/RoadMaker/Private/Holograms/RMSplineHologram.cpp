#include "Holograms/RMSplineHologram.h"
#include "Buildables/RMBuildableSpline.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"

ARMSplineHologram::ARMSplineHologram()
{
	mPreviewMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMesh"));
	if (RootComponent)
	{
		mPreviewMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mPreviewMesh);
	}
	mPreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMesh->SetCastShadow(false);
}

void ARMSplineHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMSplineHologram, mLocalPoints);
}

void ARMSplineHologram::GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGHologramBuildModeDescriptor>>& out_buildmodes) const
{
	Super::GetSupportedBuildModes_Implementation(out_buildmodes);
	if (mBuildModeStraight)
	{
		out_buildmodes.AddUnique(mBuildModeStraight);
	}
}

bool ARMSplineHologram::IsStraightMode() const
{
	return mBuildModeStraight && IsCurrentBuildMode(mBuildModeStraight);
}

void ARMSplineHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	// Reaching here means TrySnapToActor did not snap this frame.
	const FVector P = hitResult.Location + FVector(0.f, 0.f, GetSurfaceOffset());
	FVector View = (hitResult.TraceEnd - hitResult.TraceStart);
	View.Z = 0.f;
	View = View.GetSafeNormal();
	if (View.IsNearlyZero())
	{
		View = FVector::ForwardVector;
	}

	if (mStep == 0)
	{
		mStartSnapped = false;
		mStart = P;
		mStartDir = View;
	}
	else
	{
		mEndSnapped = false;
		mEnd = P;
	}
	UpdateCurve();
}

void ARMSplineHologram::UpdateCurve()
{
	TArray<FRMSplinePoint> World;

	if (mStep == 0)
	{
		// Short stub so the player sees width and direction before the first click.
		const FVector StubEnd = mStart + mStartDir * (RM::FoundationSize * 0.5f);
		RMGeometry::MakeFixedEnds(mStart, mStartDir, StubEnd, mStartDir, World);
	}
	else if (mEndSnapped)
	{
		RMGeometry::MakeFixedEnds(mStart, mStartDir, mEnd, mEndDir, World);
	}
	else if (IsStraightMode())
	{
		FVector End = mEnd;
		if (mStartSnapped)
		{
			// Keep going exactly in the direction of the road we started from.
			const FVector Dir = FVector(mStartDir.X, mStartDir.Y, 0.f).GetSafeNormal();
			const float Along = FMath::Max(0.f, FVector::DotProduct(End - mStart, Dir));
			End = FVector(mStart.X, mStart.Y, End.Z) + Dir * Along;
		}
		FVector Dir = FVector(End.X - mStart.X, End.Y - mStart.Y, 0.f).GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			Dir = mStartDir;
		}
		RMGeometry::MakeFixedEnds(mStart, Dir, End, Dir, World);
	}
	else
	{
		RMGeometry::MakeArc(mStart, mStartDir, mEnd, World);
	}

	SetWorldPoints(World);
}

void ARMSplineHologram::SetWorldPoints(const TArray<FRMSplinePoint>& WorldPoints)
{
	if (WorldPoints.Num() < 2)
	{
		mLocalPoints.Reset();
		RebuildPreview();
		return;
	}

	// The buildable spawns at the hologram's transform, so points are stored relative to it.
	const FVector Origin = WorldPoints[0].Location;
	const FVector Dir = FVector(WorldPoints[0].Tangent.X, WorldPoints[0].Tangent.Y, 0.f).GetSafeNormal();
	const FRotator Yaw(0.f, Dir.IsNearlyZero() ? 0.f : Dir.Rotation().Yaw, 0.f);
	SetActorLocationAndRotation(Origin, Yaw);

	const FTransform T(Yaw, Origin);
	mLocalPoints.Reset();
	for (const FRMSplinePoint& P : WorldPoints)
	{
		mLocalPoints.Add(FRMSplinePoint(T.InverseTransformPosition(P.Location), T.InverseTransformVectorNoScale(P.Tangent)));
	}
	RebuildPreview();

	if (mPreviewMaterial)
	{
		for (int32 I = 0; I < mPreviewMesh->GetNumSections(); ++I)
		{
			mPreviewMesh->SetMaterial(I, mPreviewMaterial);
		}
	}
}

bool ARMSplineHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mStep == 0)
	{
		// Lock in the start (and its snap) and start dragging the end.
		mStep = 1;
		mEnd = mStart + mStartDir * RM::FoundationSize;
		UpdateCurve();
		return false;
	}
	return true;
}

void ARMSplineHologram::GetPreviewFrames(TArray<FRMFrame>& OutFrames, float Step) const
{
	RMGeometry::SampleSpline(mLocalPoints, Step, OutFrames);
}

int32 ARMSplineHologram::GetBaseCostMultiplier() const
{
	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 100.f);
	const float Length = Frames.Num() > 0 ? Frames.Last().Dist : 0.f;
	const int32 Units = FMath::Max(1, FMath::CeilToInt(Length / GetCostUnitLength()));
	return Units * FMath::Max(1, GetCostWidthFactor());
}

void ARMSplineHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableSpline* Spline = Cast<ARMBuildableSpline>(inBuildable))
	{
		Spline->SetSplinePoints(mLocalPoints);
		Spline->SetCostMultiplier(GetBaseCostMultiplier());
	}
}
