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
	 * Vehicle paths snapped to a road-end point run along the road, like they do when snapping to
	 * another path's node: the hologram keeps its start and end directions in mStartPointExitDirection
	 * and mEndPointEntryDirection (with mUseCustomEndRotation), so those are written directly.
	 * Scrolling while snapped flips the direction 180 instead of turning it.
	 */
	struct FPathState
	{
		TWeakObjectPtr<const UObject> Owner; // road or intersection of the snapped end
		int32 EndIndex = -1;
		bool bStartStep = true;
		bool bFlipped = false;
		bool bSetCustomEnd = false;
		FVector StartDir = FVector::ZeroVector; // last aligned start direction, kept through the click
		int32 SavedScroll = 0;                  // the player's scroll before we steered the end
		float EndYawOffset = 0.f;               // learned: 0 or 180 between scroll heading and travel
		bool bEndOffsetLearned = false;
		int32 EndTrim = 0;                      // learned: whole degrees the game's heading is off
	};

	/** Set while we place the hologram again ourselves, so our hook lets the game's code run. */
	bool GInPathReplace = false;
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

	FVector* VectorField(UObject* Object, const TCHAR* Name)
	{
		FStructProperty* Prop = FindFProperty<FStructProperty>(Object->GetClass(), Name);
		return Prop && Prop->Struct == TBaseStructure<FVector>::Get() ? Prop->ContainerPtrToValuePtr<FVector>(Object) : nullptr;
	}

	/** Left the road-end snap points: give the end direction back to the game. */
	void ReleasePathSnap(AFGHologram* Hologram)
	{
		if (FPathState* State = GPathStates.Find(Hologram))
		{
			if (State->bSetCustomEnd)
			{
				SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), false);
				Hologram->SetScrollRotateValue(State->SavedScroll);
			}
			GPathStates.Remove(Hologram);
		}
	}

	/** Rise per horizontal distance of a road end, in its outward direction (0 for intersection mouths). */
	float RoadSlope(const FRMRoadEndRef& End)
	{
		const float Flat = FVector(End.Outward.X, End.Outward.Y, 0.f).Size();
		return Flat > KINDA_SMALL_NUMBER ? End.Outward.Z / Flat : 0.f;
	}

	/** Protected rail hologram members: its spline points and the update that rebuilds the preview. */
	struct FRMRailAccess : public AFGRailroadTrackHologram
	{
		/**
		 * Gives the spline point at the start (or end) the road's slope: the horizontal direction the
		 * game chose stays, only the height part of the tangents changes. Then the preview, spline and
		 * clearance are rebuilt from the edited points (the built track uses the same points).
		 */
		static void ApplySlope(AFGRailroadTrackHologram* Rail, bool bAtStart, float Slope)
		{
			FRMRailAccess* Self = static_cast<FRMRailAccess*>(Rail);
			if (Self->mSplineData.Num() < 2)
			{
				return;
			}
			FSplinePointData& P = bAtStart ? Self->mSplineData[0] : Self->mSplineData.Last();
			auto Tilt = [Slope](FVector& T)
			{
				const float Flat = FVector(T.X, T.Y, 0.f).Size();
				T.Z = Flat * Slope;
			};
			Tilt(P.ArriveTangent);
			Tilt(P.LeaveTangent);
			Self->UpdateSplineComponent();
			Self->UpdateClearanceData();
		}
	};

	/**
	 * Rail track on a road-end snap point. The track's start direction (and its end direction once
	 * custom end rotation is on) is the scroll value used as a world heading, rounded to the rotation
	 * step (seen in the log), so the scroll is set to the road's heading; the step is 1 degree here
	 * (GetRotationStep hook). Start: along the road the way you look. End: the way the track travels.
	 */
	template <typename TScope, typename THologram>
	void PlaceRail(TScope& Scope, THologram* Hologram, const FHitResult& Hit, const FHitResult& Placed, const FRMRoadEndRef& End, const FVector& Axis)
	{
		const UObject* Owner = End.Road ? static_cast<const UObject*>(End.Road) : static_cast<const UObject*>(End.Intersection);
		const bool bStart = IsStartStep(Hologram);
		FPathState& State = GPathStates.FindOrAdd(Hologram);
		if (State.Owner.Get() != Owner || State.EndIndex != End.EndIndex || State.bStartStep != bStart)
		{
			State.bFlipped = false;
		}
		State.Owner = Owner;
		State.EndIndex = End.EndIndex;
		State.bStartStep = bStart;
		if (!State.bSetCustomEnd)
		{
			State.SavedScroll = Hologram->GetScrollRotateValue();
			State.bSetCustomEnd = true; // "we changed the scroll"; undone in ReleasePathSnap
		}

		FVector Dir;
		if (bStart)
		{
			const FVector View = Hit.TraceEnd - Hit.TraceStart;
			Dir = FVector::DotProduct(View, Axis) >= 0.f ? Axis : -Axis;
		}
		else
		{
			const FVector* StartField = VectorField(Hologram, TEXT("mSnappedStartLocation"));
			const FVector Start = StartField && !StartField->IsNearlyZero() ? *StartField : Hologram->GetActorLocation();
			Dir = FVector::DotProduct(Placed.Location - Start, Axis) >= 0.f ? Axis : -Axis;
			SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), true);
		}
		if (State.bFlipped)
		{
			Dir = -Dir;
		}
		Hologram->SetScrollRotateValue(FMath::RoundToInt(Dir.Rotation().Yaw));
		Scope(Hologram, Placed);

		// The road's slope at this end, along the direction the track travels: the game only turns
		// the track by heading, so the height part is added to its spline point afterwards. At the
		// start it is also kept in mHitTangent, which the game copies when the start is clicked.
		const float Slope = RoadSlope(End) * (FVector::DotProduct(Dir, Axis) >= 0.f ? 1.f : -1.f);
		if (AFGRailroadTrackHologram* Rail = Cast<AFGRailroadTrackHologram>(Hologram))
		{
			FRMRailAccess::ApplySlope(Rail, bStart, Slope);
			if (bStart)
			{
				if (FVector* HitTangent = VectorField(Hologram, TEXT("mHitTangent")))
				{
					const FVector Flat = FVector(HitTangent->X, HitTangent->Y, 0.f);
					*HitTangent = (Flat + FVector(0.f, 0.f, Flat.Size() * Slope)).GetSafeNormal() * FMath::Max(HitTangent->Size(), 1.f);
				}
			}
		}
	}

	/** Travel direction at the end of the path hologram's own (unprojected) spline. */
	bool GetPathEndDirection(AFGHologram* Hologram, FVector& OutDir)
	{
		TInlineComponentArray<USplineComponent*> Splines(Hologram);
		USplineComponent* Use = nullptr;
		for (USplineComponent* S : Splines)
		{
			if (S && S->GetNumberOfSplinePoints() >= 2 && (!Use || S->GetName().Contains(TEXT("Temp"))))
			{
				Use = S;
			}
		}
		if (!Use)
		{
			return false;
		}
		OutDir = Use->GetDirectionAtSplinePoint(Use->GetNumberOfSplinePoints() - 1, ESplineCoordinateSpace::World);
		OutDir.Z = 0.f;
		return OutDir.Normalize();
	}

	/**
	 * Path end on a road-end snap point. With the game's custom end rotation on, the end direction is
	 * the scroll value used as a world heading (seen in the log: scroll 0 = +X), so the scroll is set
	 * to the road's heading; the result is checked and turned 180 once if it arrives backwards.
	 */
	template <typename TScope, typename THologram>
	void PlacePathEnd(TScope& Scope, THologram* Hologram, const FHitResult& Placed, const FVector& Start, const FVector& Axis, FPathState& State)
	{
		if (!State.bSetCustomEnd)
		{
			State.SavedScroll = Hologram->GetScrollRotateValue();
			State.bSetCustomEnd = true;
		}
		FVector Travel = FVector::DotProduct(Placed.Location - Start, Axis) >= 0.f ? Axis : -Axis;
		if (State.bFlipped)
		{
			Travel = -Travel;
		}
		const float WantYaw = Travel.Rotation().Yaw;
		SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), true);
		const float Heading = FRotator::NormalizeAxis(WantYaw + State.EndYawOffset);
		Hologram->SetScrollRotateValue(FMath::RoundToInt(Heading) + State.EndTrim);
		Scope(Hologram, Placed);

		// Fine trim: compare the heading the game actually used with the one wanted (whole degrees,
		// the rotation step is 1 here, see the GetRotationStep hook).
		if (const FVector* Entry = VectorField(Hologram, TEXT("mEndPointEntryDirection")))
		{
			const float Err = FMath::FindDeltaAngleDegrees(Heading, Entry->Rotation().Yaw);
			if (FMath::Abs(Err) > 0.6f && FMath::Abs(Err) < 30.f)
			{
				State.EndTrim -= FMath::RoundToInt(Err);
				State.EndTrim = FMath::Clamp(State.EndTrim, -20, 20);
			}
		}

		FVector Have;
		if (!State.bEndOffsetLearned && GetPathEndDirection(Hologram, Have))
		{
			State.bEndOffsetLearned = true;
		}
		else
		{
			return;
		}
		if (FVector::DotProduct(Have, Travel) < 0.f)
		{
			// Arrives backwards: the game's heading is the other way round. Learn it and redo.
			State.EndYawOffset = State.EndYawOffset == 0.f ? 180.f : 0.f;
			Hologram->SetScrollRotateValue(FMath::RoundToInt(FRotator::NormalizeAxis(WantYaw + State.EndYawOffset)));
			TGuardValue<bool> Guard(GInPathReplace, true);
			Hologram->SetHologramLocationAndRotation(Placed);
			UE_LOG(LogRoadMaker, Display, TEXT("Path end: heading offset now %.0f"), State.EndYawOffset);
		}
	}

	/** Turns the path's start arrow to the direction we set (it is only a visual). */
	void PointStartArrow(AFGHologram* Hologram, const FVector& Dir)
	{
		FObjectPropertyBase* Prop = FindFProperty<FObjectPropertyBase>(Hologram->GetClass(), TEXT("mDirectionalArrowComponent"));
		USceneComponent* Arrow = Prop ? Cast<USceneComponent>(Prop->GetObjectPropertyValue_InContainer(Hologram)) : nullptr;
		if (Arrow)
		{
			Arrow->SetWorldRotation(FRotator(0.f, Dir.Rotation().Yaw, 0.f));
		}
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
		if (Kind == ESnapKind::None || !Hologram->GetWorld() || GInPathReplace)
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

			// Road surface at this end: along the road (with its slope) and its up normal.
			const FVector Along3 = BestEnd->Outward.GetSafeNormal();          // out of the road
			const FVector Across = FVector::CrossProduct(FVector::UpVector, Along3).GetSafeNormal();
			FVector SurfaceUp = FVector::CrossProduct(Across, Along3).GetSafeNormal();
			if (SurfaceUp.Z < 0.f)
			{
				SurfaceUp = -SurfaceUp;
			}
			if (SurfaceUp.IsNearlyZero())
			{
				SurfaceUp = FVector::UpVector;
			}

			// At a free end the points move 1 m onto the road: the game lays vehicle paths onto the
			// surface below them, and right on the edge of a raised end part of it would drop to the
			// ground ("vehicles collide with the terrain"). Joined ends stay on the joint so paths
			// on neighbouring roads still meet.
			const bool bFreeEnd = BestEnd->Road
				? !RMRoadUtils::IsEndOccupied(Hologram->GetWorld(), BestEnd->Road, BestEnd->EndIndex)
				: !RMRoadUtils::IsMouthOccupied(Hologram->GetWorld(), BestEnd->Intersection, BestEnd->Location);
			if (bFreeEnd)
			{
				Best -= Along3 * 100.f;
			}

			// Keep the road's tilt in the hit normal: rail tracks take their slope from it.
			FHitResult Placed = MoveHit(Hit, Best);
			Placed.Normal = SurfaceUp;
			Placed.ImpactNormal = SurfaceUp;
			AFGVehiclePathSegmentHologram* Path = Cast<AFGVehiclePathSegmentHologram>(Hologram);
			const FVector Axis = FVector(BestEnd->Outward.X, BestEnd->Outward.Y, 0.f).GetSafeNormal();
			if (!Path || Axis.IsNearlyZero())
			{
				if (!Path && !Axis.IsNearlyZero())
				{
					PlaceRail(Scope, Hologram, Hit, Placed, *BestEnd, Axis);
				}
				else
				{
					Scope(Hologram, Placed);
				}
				return true;
			}

			// Same snap point as last frame keeps its 180 flip; a new point starts unflipped.
			const UObject* Owner = BestEnd->Road ? static_cast<const UObject*>(BestEnd->Road) : static_cast<const UObject*>(BestEnd->Intersection);
			const bool bStart = IsStartStep(Hologram);
			FPathState& State = GPathStates.FindOrAdd(Hologram);
			if (State.Owner.Get() != Owner || State.EndIndex != BestEnd->EndIndex || State.bStartStep != bStart)
			{
				State.bFlipped = false;
			}
			State.Owner = Owner;
			State.EndIndex = BestEnd->EndIndex;
			State.bStartStep = bStart;

			if (bStart)
			{
				const FVector View = Hit.TraceEnd - Hit.TraceStart;
				FVector Dir = FVector::DotProduct(View, Axis) >= 0.f ? Axis : -Axis;
				if (State.bFlipped)
				{
					Dir = -Dir;
				}
				Scope(Hologram, Placed);
				// Follow the road's slope too (the direction above is only the heading).
				const float Slope = RoadSlope(*BestEnd) * (FVector::DotProduct(Dir, Axis) >= 0.f ? 1.f : -1.f);
				const FVector Dir3 = (Dir + FVector(0.f, 0.f, Slope)).GetSafeNormal();
				if (FVector* Exit = VectorField(Hologram, TEXT("mStartPointExitDirection")))
				{
					*Exit = Dir3;
				}
				State.StartDir = Dir3;
				PointStartArrow(Hologram, Dir);
			}
			else
			{
				const FVector* StartField = VectorField(Hologram, TEXT("mStartLocation"));
				const FVector Start = StartField && !StartField->IsNearlyZero() ? *StartField : Hologram->GetActorLocation();
				PlacePathEnd(Scope, Hologram, Placed, Start, Axis, State);
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

	// Scrolling while a vehicle path sits on a road-end snap point flips it 180 instead of turning.
	SUBSCRIBE_METHOD_VIRTUAL(AFGVehiclePathSegmentHologram::ScrollRotate, GetMutableDefault<AFGVehiclePathSegmentHologram>(),
		[](auto& Scope, AFGVehiclePathSegmentHologram* Hologram, int32 Delta, int32 Step)
		{
			FPathState* State = GPathStates.Find(Hologram);
			if (State)
			{
				if (Delta != 0)
				{
					State->bFlipped = !State->bFlipped;
				}
				Scope.Cancel();
			}
		});

	// The path end turns in rotation-step increments (10 degrees); on a road-end snap point use 1
	// degree so the end can line up with the road.
	SUBSCRIBE_METHOD_VIRTUAL(AFGVehiclePathSegmentHologram::GetRotationStep, GetMutableDefault<AFGVehiclePathSegmentHologram>(),
		[](auto& Scope, const AFGVehiclePathSegmentHologram* Hologram)
		{
			const FPathState* State = GPathStates.Find(Hologram);
			if (State && State->bSetCustomEnd)
			{
				Scope.Override(1);
			}
		});

	// Rail tracks on road-end snap points: scroll flips 180, and turning uses 1 degree steps.
	SUBSCRIBE_METHOD_VIRTUAL(AFGRailroadTrackHologram::ScrollRotate, GetMutableDefault<AFGRailroadTrackHologram>(),
		[](auto& Scope, AFGRailroadTrackHologram* Hologram, int32 Delta, int32 Step)
		{
			if (FPathState* State = GPathStates.Find(Hologram))
			{
				if (Delta != 0)
				{
					State->bFlipped = !State->bFlipped;
				}
				Scope.Cancel();
			}
		});

	SUBSCRIBE_METHOD_VIRTUAL(AFGRailroadTrackHologram::GetRotationStep, GetMutableDefault<AFGRailroadTrackHologram>(),
		[](auto& Scope, const AFGRailroadTrackHologram* Hologram)
		{
			if (GPathStates.Contains(Hologram))
			{
				Scope.Override(1);
			}
		});

	// First click on a road-end snap point: make sure the start keeps our direction.
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGVehiclePathSegmentHologram::DoMultiStepPlacement, GetMutableDefault<AFGVehiclePathSegmentHologram>(),
		[](bool Result, AFGVehiclePathSegmentHologram* Hologram, bool bIsInputFromARelease)
		{
			FPathState* State = GPathStates.Find(Hologram);
			if (State && State->bStartStep && !State->StartDir.IsNearlyZero() && !IsStartStep(Hologram))
			{
				if (FVector* Exit = VectorField(Hologram, TEXT("mStartPointExitDirection")))
				{
					*Exit = State->StartDir;
				}
				State->bStartStep = false;
				State->bFlipped = false;
				State->Owner = nullptr;
			}
		});


}

#else

void RMVanillaSnapping::InstallHooks() {}

#endif
