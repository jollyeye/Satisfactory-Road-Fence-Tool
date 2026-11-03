#include "Snapping/RMVanillaSnapping.h"

#if !WITH_EDITOR

#include "RoadMakerModule.h"
#include "RMRoadUtils.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "Hologram/FGHologram.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGPowerPoleHologram.h"
#include "Hologram/FGFloodlightHologram.h"
#include "Hologram/FGRailroadTrackHologram.h"
#include "Hologram/FGVehiclePathSegmentHologram.h"
#include "Patching/NativeHookManager.h"
#include "Components/SplineComponent.h"

namespace
{
	/** Reads the protected snap-to-guidelines flag (set while the snap key is held). */
	struct FRMSnapAccess : public AFGHologram
	{
		static bool IsSnapHeld(const AFGHologram* Hologram)
		{
			return static_cast<const FRMSnapAccess*>(Hologram)->mSnapToGuideLines;
		}
	};

	enum class ESnapKind : uint8 { None, PowerPole, StreetLight, PathStart };

	/** Correction between "facing" and the street light model's own forward axis (was 90 off in game). */
	constexpr float StreetLightYawOffset = -90.f;

	ESnapKind KindOf(const AFGHologram* Hologram)
	{
		const UClass* Build = Hologram ? Hologram->GetBuildClass().Get() : nullptr;
		if (!Build)
		{
			return ESnapKind::None;
		}
		if (Build->IsChildOf(AFGBuildablePowerPole::StaticClass()))
		{
			return ESnapKind::PowerPole;
		}
		if (Build->GetName().Contains(TEXT("StreetLight")))
		{
			return ESnapKind::StreetLight;
		}
		if (Hologram->IsA<AFGRailroadTrackHologram>() || Hologram->IsA<AFGVehiclePathSegmentHologram>())
		{
			return ESnapKind::PathStart;
		}
		return ESnapKind::None;
	}

	struct FFenceLinePoint
	{
		FVector Location;
		FVector Inward; // horizontal, towards the road centre
		float Distance = TNumericLimits<float>::Max();
	};

	/** Closest point on any nearby road's fence lines (both edges). */
	bool FindFenceLinePoint(UWorld* World, const FVector& Aim, float MaxDistance, FFenceLinePoint& Out)
	{
		TArray<ARMBuildableRoad*> Roads;
		RMRoadUtils::GatherNearby(World, Aim, MaxDistance + RM::FoundationSize * 2.f, Roads);
		const float Inset = GetDefault<ARMBuildableFence>()->mEdgeInset;
		bool bFound = false;
		for (ARMBuildableRoad* Road : Roads)
		{
			TArray<FRMFrame> Frames;
			Road->GetWorldFrames(Frames, 25.f);
			const float Offset = Road->GetRoadWidth() * 0.5f - Inset;
			for (const FRMFrame& F : Frames)
			{
				for (float Side : { 1.f, -1.f })
				{
					const FVector P = F.Pos + F.Right * (Offset * Side);
					const float D = FVector::Dist(P, Aim);
					if (D < Out.Distance && D <= MaxDistance)
					{
						Out.Location = P;
						Out.Inward = -F.Right * Side;
						Out.Distance = D;
						bFound = true;
					}
				}
			}
		}
		return bFound;
	}

	/** Street light facing; scrolling flips it. Per hologram, reset when the hologram goes away. */
	struct FLightState
	{
		int32 LastScroll = 0;
		bool bFacingAway = false;
	};
	TMap<TWeakObjectPtr<const AFGHologram>, FLightState> GLightStates;


	// --- Path / track alignment at road ends ------------------------------------------------------

	FHitResult MoveHit(const FHitResult& Hit, const FVector& Location);

	/**
	 * Per path/track hologram while the snap key forces alignment. The game owns the direction, so
	 * it is steered through the scroll rotation: each frame the spline's actual direction at the
	 * snapped point is measured and the scroll value corrected (a frame later) until it matches.
	 */
	struct FPathState
	{
		bool bStartStep = true;
		bool bFlipped = false;        // scrolling while the snap key is held turns it 180
		bool bHasLastSet = false;
		int32 LastSetScroll = 0;      // last value we wrote; anything else means the player scrolled
		float LastError = 0.f;
		float Sign = 1.f;             // learned: does more scroll turn it left or right
		bool bSetCustomEnd = false;
	};
	TMap<TWeakObjectPtr<const AFGHologram>, FPathState> GPathStates;

	bool IsStartStep(AFGHologram* Hologram)
	{
		if (AFGRailroadTrackHologram* Rail = Cast<AFGRailroadTrackHologram>(Hologram))
		{
			return Rail->GetCurrentBuildStep() == ESplineHologramBuildStep::SHBS_FindStart;
		}
		FProperty* Prop = FindFProperty<FProperty>(Hologram->GetClass(), TEXT("mBuildStep"));
		if (FEnumProperty* Enum = CastField<FEnumProperty>(Prop))
		{
			return Enum->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Hologram)) == 0;
		}
		if (FByteProperty* Byte = CastField<FByteProperty>(Prop))
		{
			return Byte->GetPropertyValue_InContainer(Hologram) == 0;
		}
		return true;
	}

	void SetBoolField(UObject* Object, const TCHAR* Name, bool bValue)
	{
		if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(Object->GetClass(), Name))
		{
			Prop->SetPropertyValue_InContainer(Object, bValue);
		}
	}

	/** Travel direction of the hologram's spline at its first or last point (world, horizontal). */
	bool GetSplineDirection(AFGHologram* Hologram, bool bAtStart, FVector& OutDir)
	{
		TInlineComponentArray<USplineComponent*> Splines(Hologram);
		USplineComponent* Best = nullptr;
		for (USplineComponent* Spline : Splines)
		{
			if (Spline && Spline->GetNumberOfSplinePoints() >= 2
				&& (!Best || Spline->GetNumberOfSplinePoints() > Best->GetNumberOfSplinePoints()))
			{
				Best = Spline;
			}
		}
		if (!Best)
		{
			return false;
		}
		const int32 Index = bAtStart ? 0 : Best->GetNumberOfSplinePoints() - 1;
		OutDir = Best->GetDirectionAtSplinePoint(Index, ESplineCoordinateSpace::World);
		OutDir.Z = 0.f;
		return OutDir.Normalize();
	}

	/** Snap key released or snap point left: stop steering. */
	void ReleasePathSnap(AFGHologram* Hologram)
	{
		if (FPathState* State = GPathStates.Find(Hologram))
		{
			if (State->bSetCustomEnd)
			{
				SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), false);
			}
			GPathStates.Remove(Hologram);
		}
	}

	/** After the game placed the point: measure and correct the direction for the next frame. */
	void SteerAlongRoad(AFGHologram* Hologram, const FHitResult& Hit, const FVector& Point, const FRMRoadEndRef& End)
	{
		const FVector Axis = FVector(End.Outward.X, End.Outward.Y, 0.f).GetSafeNormal();
		if (Axis.IsNearlyZero())
		{
			return;
		}
		const bool bStart = IsStartStep(Hologram);
		FPathState& State = GPathStates.FindOrAdd(Hologram);
		if (State.bStartStep != bStart)
		{
			State = FPathState();
			State.bStartStep = bStart;
		}

		// Player scrolled while holding the key: flip 180 instead.
		const int32 Scroll = Hologram->GetScrollRotateValue();
		if (State.bHasLastSet && Scroll != State.LastSetScroll)
		{
			State.bFlipped = !State.bFlipped;
		}

		// Base direction: start = along the road the way you look; end = the way the path travels.
		FVector Want;
		if (bStart)
		{
			const FVector View = Hit.TraceEnd - Hit.TraceStart;
			Want = FVector::DotProduct(View, Axis) >= 0.f ? Axis : -Axis;
		}
		else
		{
			if (!State.bSetCustomEnd)
			{
				SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), true);
				State.bSetCustomEnd = true;
			}
			const FVector Start = Hologram->GetActorLocation();
			Want = FVector::DotProduct(Point - Start, Axis) >= 0.f ? Axis : -Axis;
		}
		if (State.bFlipped)
		{
			Want = -Want;
		}

		FVector Have;
		if (!GetSplineDirection(Hologram, bStart, Have))
		{
			return;
		}
		const float Error = FMath::FindDeltaAngleDegrees(Have.Rotation().Yaw, Want.Rotation().Yaw);
		// Learn which way the scroll turns: if the last correction made things worse, reverse.
		if (State.bHasLastSet && FMath::Abs(State.LastError) > 2.f && FMath::Abs(Error) > FMath::Abs(State.LastError) + 1.f)
		{
			State.Sign = -State.Sign;
		}
		State.LastError = Error;
		int32 NewScroll = Scroll;
		if (FMath::Abs(Error) > 0.5f)
		{
			NewScroll = Scroll + FMath::RoundToInt(Error * State.Sign);
			Hologram->SetScrollRotateValue(NewScroll);
		}
		State.LastSetScroll = Hologram->GetScrollRotateValue();
		State.bHasLastSet = true;
	}

	FHitResult MoveHit(const FHitResult& Hit, const FVector& Location)
	{
		FHitResult Moved = Hit;
		Moved.Location = Location;
		Moved.ImpactPoint = Location;
		Moved.Normal = FVector::UpVector;
		Moved.ImpactNormal = FVector::UpVector;
		return Moved;
	}

	/** Shared before-hook body; THologram is the hooked class, which the scope must be called with. */
	template <typename TScope, typename THologram>
	bool HandleSetLocation(TScope& Scope, THologram* Hologram, const FHitResult& Hit)
	{
		const ESnapKind Kind = KindOf(Hologram);
		if (Kind == ESnapKind::None || !Hologram->GetWorld())
		{
			return false;
		}

		if (Kind == ESnapKind::PathStart)
		{
			// Points near a road end or intersection mouth snap to the closest of the points every
			// 2 m across that end, level with the road top. Joined ends count too: a path usually
			// starts where two roads (or a road and an intersection) meet.
			TArray<FRMRoadEndRef> Ends;
			RMRoadUtils::FindFreeEnds(Hologram->GetWorld(), Hit.Location, 1200.f, 6, nullptr, Ends, true);
			FVector Best = FVector::ZeroVector;
			const FRMRoadEndRef* BestEnd = nullptr;
			float BestDist = 600.f;
			for (const FRMRoadEndRef& End : Ends)
			{
				for (const FVector& P : End.GetPointsAcross(200.f))
				{
					const float D = FVector::Dist(P, Hit.Location);
					if (D < BestDist)
					{
						BestDist = D;
						Best = P;
						BestEnd = &End;
					}
				}
			}
			if (!BestEnd)
			{
				ReleasePathSnap(Hologram);
				return false;
			}
			Scope(Hologram, MoveHit(Hit, Best));
			// Holding the snap key: line it up with the road (square to the end edge).
			if (FRMSnapAccess::IsSnapHeld(Hologram))
			{
				SteerAlongRoad(Hologram, Hit, Best, *BestEnd);
			}
			else
			{
				ReleasePathSnap(Hologram);
			}
			return true;
		}

		// Poles and lights: only while the snap key is held, like signals on a railway.
		if (!FRMSnapAccess::IsSnapHeld(Hologram))
		{
			return false;
		}
		FFenceLinePoint Line;
		if (!FindFenceLinePoint(Hologram->GetWorld(), Hit.Location, 800.f, Line))
		{
			return false;
		}
		Scope(Hologram, MoveHit(Hit, Line.Location));

		if (Kind == ESnapKind::StreetLight)
		{
			FLightState& State = GLightStates.FindOrAdd(Hologram);
			const int32 Scroll = Hologram->GetScrollRotateValue();
			if (Scroll != State.LastScroll)
			{
				State.bFacingAway = !State.bFacingAway;
				State.LastScroll = Scroll;
			}
			const FVector Facing = State.bFacingAway ? -Line.Inward : Line.Inward;
			// The street light model's arm points along its local Y axis, not X: turn by 90 degrees.
			Hologram->SetActorRotation(FRotator(0.f, Facing.Rotation().Yaw + StreetLightYawOffset, 0.f));
		}
		return true;
	}
}

void RMVanillaSnapping::InstallHooks()
{
	// Each class with its own SetHologramLocationAndRotation is hooked once; AFGBuildableHologram's
	// version covers the holograms that do not override it (e.g. street lights).
#define RM_HOOK_SET_LOCATION(Class) \
	SUBSCRIBE_METHOD_VIRTUAL(Class::SetHologramLocationAndRotation, GetMutableDefault<Class>(), \
		[](auto& Scope, Class* Hologram, const FHitResult& Hit) { HandleSetLocation(Scope, Hologram, Hit); })

	RM_HOOK_SET_LOCATION(AFGBuildableHologram);
	RM_HOOK_SET_LOCATION(AFGPowerPoleHologram);
	RM_HOOK_SET_LOCATION(AFGFloodlightHologram);
	RM_HOOK_SET_LOCATION(AFGRailroadTrackHologram);
	RM_HOOK_SET_LOCATION(AFGVehiclePathSegmentHologram);

#undef RM_HOOK_SET_LOCATION
}

#else

void RMVanillaSnapping::InstallHooks() {}

#endif
