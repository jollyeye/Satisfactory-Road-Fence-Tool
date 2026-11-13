#include "Holograms/RMSplineHologram.h"
#include "RMRoadUtils.h"
#include "Buildables/RMBuildableSpline.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Holograms/RMHologramLook.h"
#include "Equipment/FGBuildGunBuild.h"

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

	mCanLockHologram = true;
	mCanNudgeHologram = true;
}

void ARMSplineHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMSplineHologram, mLocalPoints);
}

void ARMSplineHologram::GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const
{
	Super::GetSupportedBuildModes_Implementation(out_buildmodes);
	if (mBuildModeStraight)
	{
		out_buildmodes.AddUnique(TSubclassOf<UFGBuildGunModeDescriptor>(mBuildModeStraight.Get()));
	}
	if (mBuildModeKeepAngle)
	{
		out_buildmodes.AddUnique(TSubclassOf<UFGBuildGunModeDescriptor>(mBuildModeKeepAngle.Get()));
	}
	if (mBuildModeSmooth)
	{
		out_buildmodes.AddUnique(TSubclassOf<UFGBuildGunModeDescriptor>(mBuildModeSmooth.Get()));
	}
}

bool ARMSplineHologram::IsKeepAngleMode() const
{
	return mBuildModeKeepAngle && IsCurrentBuildMode(mBuildModeKeepAngle);
}

bool ARMSplineHologram::IsStraightMode() const
{
	return mBuildModeStraight && IsCurrentBuildMode(mBuildModeStraight);
}

bool ARMSplineHologram::IsSmoothMode() const
{
	return mBuildModeSmooth && IsCurrentBuildMode(mBuildModeSmooth);
}

namespace
{
	/** Rise per horizontal distance of a direction. */
	float SlopeOf(const FVector& Dir)
	{
		const float Flat = FVector(Dir.X, Dir.Y, 0.f).Size();
		return Flat > KINDA_SMALL_NUMBER ? Dir.Z / Flat : 0.f;
	}

	/** Sets the height part of a tangent from a slope, keeping its horizontal part. */
	void SetTangentSlope(FVector& Tangent, float Slope)
	{
		Tangent.Z = FVector(Tangent.X, Tangent.Y, 0.f).Size() * Slope;
	}
}

bool ARMSplineHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	// Our own pieces are always valid to aim at (roads on roads, fences on roads, snapping to ends).
	if (Cast<ARMBuildableProcedural>(hitResult.GetActor()))
	{
		return true;
	}
	return Super::IsValidHitResult(hitResult);
}

bool ARMSplineHologram::TrySnapToActor(const FHitResult& hitResult)
{
	// Snapping is done in SetHologramLocationAndRotation every frame instead.
	return false;
}

float ARMSplineHologram::GetStepScrollYaw() const
{
	return static_cast<float>(GetScrollRotateValue() - mScrollAtStepStart);
}

void ARMSplineHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	mAimOrigin = hitResult.TraceStart;
	mAimDir = (hitResult.TraceEnd - hitResult.TraceStart).GetSafeNormal();
	if (mStep == 1 && IsKeepAngleMode())
	{
		// Straight (keep angle): end snaps are ignored; the length comes from the aim (UpdateCurve).
		mEndSnapped = false;
		mEnd = hitResult.Location;
		UpdateCurve();
		return;
	}
	if (TrySnapToRoadEnd(hitResult))
	{
		return;
	}

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
		// Mouse wheel turns the start direction.
		mStartDir = View.RotateAngleAxis(GetStepScrollYaw(), FVector::UpVector);
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
		const FVector StubEnd = mStart + mStartDir * GetStubLength();
		RMGeometry::MakeFixedEnds(mStart, mStartDir, StubEnd, mStartDir, World);
	}
	else if (IsKeepAngleMode())
	{
		// Straight on out of the start at its slope: the aim (seen from above) only sets the length.
		FVector Flat = FVector(mStartDir.X, mStartDir.Y, 0.f);
		if (!Flat.Normalize())
		{
			Flat = FVector::ForwardVector;
		}
		const float Slope = mStartSnapped ? SlopeOf(mStartDir) : 0.f;
		const FVector Line = (Flat + FVector(0.f, 0.f, Slope)).GetSafeNormal();
		// Distance along the road's own line (sloped too): the point of that line nearest the aim
		// ray, so aiming up along a climbing road lengthens it; else the aim point seen from above.
		float Along = FVector::DotProduct(FVector(mEnd.X - mStart.X, mEnd.Y - mStart.Y, 0.f), Flat);
		if (!mAimDir.IsNearlyZero())
		{
			const FVector W = mStart - mAimOrigin;
			const float B = FVector::DotProduct(Line, mAimDir);
			const float Denom = 1.f - B * B;
			if (Denom > 1e-4f)
			{
				const float T = (B * FVector::DotProduct(mAimDir, W) - FVector::DotProduct(Line, W)) / Denom;
				Along = T * FVector::DotProduct(Line, Flat); // horizontal part of the distance along the line
			}
		}
		Along = FMath::Max(100.f, Along);
		// Not past the longest road allowed, and stopping exactly on it within 50 cm (to get the most).
		const float MaxLength = GetMaxPlaceLength();
		if (MaxLength > 0.f)
		{
			const float MaxAlong = MaxLength / FMath::Sqrt(1.f + Slope * Slope);
			if (Along > MaxAlong - 50.f)
			{
				Along = MaxAlong - 1.f; // a hair under, so rounding never makes it "too long"
			}
		}
		const FVector End = mStart + Flat * Along + FVector(0.f, 0.f, Slope * Along);
		const FVector Dir = (Flat + FVector(0.f, 0.f, Slope)).GetSafeNormal();
		RMGeometry::MakeFixedEnds(mStart, Dir, End, Dir, World);
	}
	else if (mEndSnapped)
	{
		RMGeometry::MakeQuadratic(mStart, mStartDir, mEnd, mEndDir, World);
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
		// Quadratic Bezier: the end heads along the chord (start to end), turned by the mouse wheel.
		FVector EndDir = FVector(mEnd.X - mStart.X, mEnd.Y - mStart.Y, 0.f).GetSafeNormal();
		if (EndDir.IsNearlyZero())
		{
			EndDir = mStartDir;
		}
		const float Yaw = mStep == 1 ? GetStepScrollYaw() : 0.f;
		RMGeometry::MakeQuadratic(mStart, mStartDir, mEnd, EndDir.RotateAngleAxis(Yaw, FVector::UpVector), World);
	}

	if (IsSmoothMode() && World.Num() >= 2)
	{
		// Leave and arrive with the slope of what the ends snap to; free ends are level. With level
		// ends the height follows an S-curve (eases out of the lower road, into the upper one).
		SetTangentSlope(World[0].Tangent, mStartSnapped ? SlopeOf(mStartDir) : 0.f);
		SetTangentSlope(World.Last().Tangent, mEndSnapped ? SlopeOf(mEndDir) : 0.f);
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
	UpdateHologramLook();
}

void ARMSplineHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateHologramLook();
}

void ARMSplineHologram::UpdateHologramLook()
{
	UMaterialInterface* Material = mPreviewMaterial;
	if (!Material)
	{
		Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
			? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	}
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}


bool ARMSplineHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mStep == 0)
	{
		// Lock in the start (and its snap) and start dragging the end.
		mStep = 1;
		mScrollAtStepStart = GetScrollRotateValue();
		mEnd = mStart + mStartDir * RM::FoundationSize;
		UpdateCurve();
		// A lock carried over from the start point now nudges the end, from where it is.
		ResetNudge();
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

bool ARMSplineHologram::CanNudgeHologram() const
{
	return IsHologramLocked();
}

void ARMSplineHologram::OnHologramLockStateChanged()
{
	Super::OnHologramLockStateChanged();
	ResetNudge();
}

FVector ARMSplineHologram::GetPlacedPointAxis() const
{
	FVector Dir = mStartDir;
	if (mStep != 0)
	{
		// The end's travel direction (last tangent of the curve), else start to end.
		Dir = mLocalPoints.Num() >= 2 ? GetActorTransform().TransformVectorNoScale(mLocalPoints.Last().Tangent) : mEnd - mStart;
	}
	Dir.Z = 0.f;
	return Dir.Normalize() ? Dir : FVector::ForwardVector;
}

void ARMSplineHologram::ResetNudge()
{
	mNudgeBase = mStep == 0 ? mStart : mEnd;
	mNudgeAxis = GetPlacedPointAxis();
	SetNudgeOffset(FVector::ZeroVector);
}

FTransform ARMSplineHologram::GetNudgeSpaceTransform() const
{
	// Forward/back/left/right relative to the point's direction when the nudge started (kept fixed,
	// so moving the point does not turn the axes).
	return FTransform(FRotator(0.f, mNudgeAxis.Rotation().Yaw, 0.f), mNudgeBase);
}

ENudgeFailReason ARMSplineHologram::NudgeHologram(const FVector& NudgeInput, const FHitResult& HitResult)
{
	if (!CanNudgeHologram())
	{
		return Super::NudgeHologram(NudgeInput, HitResult);
	}
	// The game turns the input into the world axis nearest to the camera; we keep the point's axes:
	// X = forward / back along the point's direction, Y = right / left, Z = up / down.
	const FVector Step(FMath::Sign(NudgeInput.X), FMath::Sign(NudgeInput.Y), FMath::Sign(NudgeInput.Z));
	if (Step.IsNearlyZero())
	{
		return ENudgeFailReason::NFR_Disallowed;
	}
	SetNudgeOffset(GetHologramNudgeOffset() + Step * GetNudgeDistance());
	SetHologramNudgeLocation();
	return ENudgeFailReason::NFR_Success;
}

void ARMSplineHologram::SetHologramNudgeLocation()
{
	if (!IsHologramLocked())
	{
		return;
	}
	const FVector Base = mNudgeBase;
	const FVector Offset = GetNudgeSpaceTransform().GetRotation().RotateVector(GetHologramNudgeOffset());
	if (mStep == 0)
	{
		mStart = Base + Offset;
	}
	else
	{
		mEnd = Base + Offset;
	}
	UpdateCurve();
}
