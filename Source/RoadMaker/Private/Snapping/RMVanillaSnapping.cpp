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
	 * Per path/track hologram while the snap key forces alignment. The game owns the direction and
	 * only lets it be turned through the scroll rotation, so the hologram is placed, its spline
	 * direction measured, and the scroll value solved for the wanted direction (how many degrees one
	 * scroll unit turns it is measured once with a probe placement), then placed again.
	 */
	struct FPathState
	{
		bool bStartStep = true;
		bool bFlipped = false;        // scroll while the snap key is held turns it 180
		float DegreesPerUnit = 0.f;   // measured; 0 = not yet
		bool bSetCustomEnd = false;
	};
	TMap<TWeakObjectPtr<const AFGHologram>, FPathState> GPathStates;

	/** Set while we place the hologram ourselves, so our own hook lets the game's code run. */
	bool GInSteerPlacement = false;

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

	/** Places the hologram again with the game's own code (our hook stands aside). */
	void PlaceAgain(AFGHologram* Hologram, const FHitResult& Hit)
	{
		TGuardValue<bool> Guard(GInSteerPlacement, true);
		Hologram->SetHologramLocationAndRotation(Hit);
	}

	/** The hologram was just placed at a road end with the snap key held: turn it along the road. */
	void SteerAlongRoad(AFGHologram* Hologram, const FHitResult& PlacedHit, const FHitResult& Hit, const FRMRoadEndRef& End)
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
			const float Known = State.DegreesPerUnit;
			State = FPathState();
			State.bStartStep = bStart;
			State.DegreesPerUnit = bStart ? Known : 0.f; // the end may react differently
		}

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
				PlaceAgain(Hologram, PlacedHit);
			}
			Want = FVector::DotProduct(PlacedHit.Location - Hologram->GetActorLocation(), Axis) >= 0.f ? Axis : -Axis;
		}
		if (State.bFlipped)
		{
			Want = -Want;
		}
		const float WantYaw = Want.Rotation().Yaw;

		FVector Have;
		if (!GetSplineDirection(Hologram, bStart, Have))
		{
			return;
		}
		const int32 Scroll0 = Hologram->GetScrollRotateValue();
		const float Error = FMath::FindDeltaAngleDegrees(Have.Rotation().Yaw, WantYaw);
		if (FMath::Abs(Error) < 0.5f)
		{
			return;
		}

		if (FMath::Abs(State.DegreesPerUnit) < 0.01f)
		{
			// Probe: how far does the direction turn for 45 scroll units?
			const int32 Probe = 45;
			Hologram->SetScrollRotateValue(Scroll0 + Probe);
			PlaceAgain(Hologram, PlacedHit);
			FVector Probed;
			const bool bOk = GetSplineDirection(Hologram, bStart, Probed);
			const float Turned = bOk ? FMath::FindDeltaAngleDegrees(Have.Rotation().Yaw, Probed.Rotation().Yaw) : 0.f;
			if (FMath::Abs(Turned) < 1.f)
			{
				// Scroll does not turn it here: put everything back as the game had it.
				Hologram->SetScrollRotateValue(Scroll0);
				PlaceAgain(Hologram, PlacedHit);
				UE_LOG(LogRoadMaker, Verbose, TEXT("Path align: scroll does not turn %s (start=%d)"), *Hologram->GetName(), bStart);
				return;
			}
			State.DegreesPerUnit = Turned / Probe;
			UE_LOG(LogRoadMaker, Display, TEXT("Path align: %s turns %.3f deg per scroll unit (start=%d)"), *Hologram->GetName(), State.DegreesPerUnit, bStart);
		}

		Hologram->SetScrollRotateValue(Scroll0 + FMath::RoundToInt(Error / State.DegreesPerUnit));
		PlaceAgain(Hologram, PlacedHit);
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
		if (Kind == ESnapKind::None || !Hologram->GetWorld() || GInSteerPlacement)
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
			const FHitResult Placed = MoveHit(Hit, Best);
			Scope(Hologram, Placed);
			// Holding the snap key: line it up with the road (square to the end edge).
			if (FRMSnapAccess::IsSnapHeld(Hologram))
			{
				SteerAlongRoad(Hologram, Placed, Hit, *BestEnd);
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

	// Scrolling while a path/track is held aligned at a road end flips it 180 instead of turning.
#define RM_HOOK_SCROLL(Class) \
	SUBSCRIBE_METHOD_VIRTUAL(Class::ScrollRotate, GetMutableDefault<Class>(), \
		[](auto& Scope, Class* Hologram, int32 Delta, int32 Step) \
		{ \
			FPathState* State = GPathStates.Find(Hologram); \
			if (State && Delta != 0 && FRMSnapAccess::IsSnapHeld(Hologram)) \
			{ \
				State->bFlipped = !State->bFlipped; \
				Scope.Cancel(); \
			} \
		})

	RM_HOOK_SCROLL(AFGRailroadTrackHologram);
	RM_HOOK_SCROLL(AFGVehiclePathSegmentHologram);

#undef RM_HOOK_SCROLL
}

#else

void RMVanillaSnapping::InstallHooks() {}

#endif
