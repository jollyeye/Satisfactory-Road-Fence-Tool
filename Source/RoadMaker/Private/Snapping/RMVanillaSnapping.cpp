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
#include "Components/SplineMeshComponent.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "FGConstructDisqualifier.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Algo/Reverse.h"
#include "UObject/UnrealType.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "Buildables/RMBuildableIntersection.h"

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

	/** Closest point on the edge of any nearby road (both sides), like the game's own edge snapping. */
	bool FindFenceLinePoint(UWorld* World, const FVector& Aim, float MaxDistance, FFenceLinePoint& Out)
	{
		TArray<ARMBuildableRoad*> Roads;
		RMRoadUtils::GatherNearby(World, Aim, MaxDistance + RM::FoundationSize * 2.f, Roads);
		bool bFound = false;
		for (ARMBuildableRoad* Road : Roads)
		{
			TArray<FRMFrame> Frames;
			Road->GetWorldFrames(Frames, 25.f);
			const float Offset = Road->GetRoadWidth() * 0.5f; // exactly on the road edge
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
		bool bLaneMode = false;                 // one-click lane path: we put the hologram in its end step
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

	/** Vehicle path hologram build step (0 = start point, 1 = end point). */
	void SetBuildStep(AFGHologram* Hologram, uint8 Step)
	{
		FProperty* Prop = FindFProperty<FProperty>(Hologram->GetClass(), TEXT("mBuildStep"));
		if (FEnumProperty* Enum = CastField<FEnumProperty>(Prop))
		{
			Enum->GetUnderlyingProperty()->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Hologram), static_cast<int64>(Step));
		}
		else if (FByteProperty* Byte = CastField<FByteProperty>(Prop))
		{
			Byte->SetPropertyValue_InContainer(Hologram, Step);
		}
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

	/** Protected vehicle path hologram functions that rebuild its spline and redraw it. */
	struct FRMPathAccess : public AFGVehiclePathSegmentHologram
	{
		static void Redraw(AFGVehiclePathSegmentHologram* Path)
		{
			FRMPathAccess* Self = static_cast<FRMPathAccess*>(Path);
			Self->RouteHologramSpline();
			Self->UpdateHologramVisualState();
			Self->UpdatePathVisualizationHandle();
		}

		/** The game's own "snap this end to that node": joins the new path to the existing network. */
		static bool SnapToNode(AFGVehiclePathSegmentHologram* Path, AFGVehiclePathNode* Node)
		{
			return Node && static_cast<FRMPathAccess*>(Path)->TrySnapToPathNode(Node);
		}

		/** Removes the path meshes drawn in the end step (the start step never touches them). */
		static void RemovePathMeshes(AFGVehiclePathSegmentHologram* Path)
		{
			FRMPathAccess* Self = static_cast<FRMPathAccess*>(Path);
			for (USplineMeshComponent* Mesh : Self->mSplineMeshComponents)
			{
				if (Mesh)
				{
					Mesh->DestroyComponent();
				}
			}
			Self->mSplineMeshComponents.Empty();
		}
	};

	/** Writes a vehicle path hologram's spline points (world space in) and has the game redraw it. */
	void SetPathSplineWorld(AFGHologram* Hologram, const TArray<FVector>& Points, const TArray<FVector>& Tangents)
	{
		FArrayProperty* Data = FindFProperty<FArrayProperty>(Hologram->GetClass(), TEXT("mSplineData"));
		if (!Data)
		{
			return;
		}
		TArray<FSplinePointData>* Array = Data->ContainerPtrToValuePtr<TArray<FSplinePointData>>(Hologram);
		const FTransform T = Hologram->GetActorTransform();
		Array->Reset();
		for (int32 I = 0; I < Points.Num(); ++I)
		{
			FSplinePointData P;
			P.Location = T.InverseTransformPosition(Points[I]);
			P.ArriveTangent = T.InverseTransformVector(Tangents[I]);
			P.LeaveTangent = P.ArriveTangent;
			Array->Add(P);
		}
		// Protected UFUNCTION: called through reflection; rebuilds the spline and its visuals.
		if (UFunction* Rebuild = Hologram->FindFunction(TEXT("OnRep_SplineData")))
		{
			Hologram->ProcessEvent(Rebuild, nullptr);
		}
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
			Redraw(Rail);
		}

		/** Replaces the track's spline points (world space in) and rebuilds preview and clearance. */
		static void SetSplineWorld(AFGRailroadTrackHologram* Rail, const TArray<FVector>& Points, const TArray<FVector>& Tangents)
		{
			FRMRailAccess* Self = static_cast<FRMRailAccess*>(Rail);
			const FTransform T = Rail->GetActorTransform();
			Self->mSplineData.Reset();
			for (int32 I = 0; I < Points.Num(); ++I)
			{
				FSplinePointData P;
				P.Location = T.InverseTransformPosition(Points[I]);
				P.ArriveTangent = T.InverseTransformVector(Tangents[I]);
				P.LeaveTangent = P.ArriveTangent;
				Self->mSplineData.Add(P);
			}
			// The game's track meshes are laid along the spline component by distance; a closed loop
			// or a stale distance table would wrap them back around like a ring.
			if (USplineComponent* Spline = Self->mSplineComponent)
			{
				Spline->SetClosedLoop(false, false);
			}
			Self->UpdateSplineComponent();
			if (USplineComponent* Spline = Self->mSplineComponent)
			{
				Spline->SetClosedLoop(false, false);
				Spline->UpdateSpline();
			}
			Self->UpdateClearanceData();
			Redraw(Rail);
		}

		/** Rebuilds what is drawn from the spline points (protected UFUNCTION, through reflection). */
		static void Redraw(AFGRailroadTrackHologram* Rail)
		{
			if (UFunction* Rebuild = Rail->FindFunction(TEXT("OnRep_SplineData")))
			{
				Rail->ProcessEvent(Rebuild, nullptr);
			}
		}

		/** Back to the start step: drop the lane track's snapped connections. */
		static void ResetToStart(AFGRailroadTrackHologram* Rail)
		{
			FRMRailAccess* Self = static_cast<FRMRailAccess*>(Rail);
			Self->mBuildStep = ESplineHologramBuildStep::SHBS_FindStart;
		}
	};


	/** Sets one of the rail hologram's snapped connection slots (0 = start, 1 = end); private, by reflection. */
	void SetRailSnappedConnection(AFGHologram* Hologram, int32 Index, UFGRailroadTrackConnectionComponent* Connection)
	{
		FObjectPropertyBase* Prop = FindFProperty<FObjectPropertyBase>(Hologram->GetClass(), TEXT("mSnappedConnectionComponents"));
		if (Prop && Index < Prop->ArrayDim)
		{
			Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Hologram, Index), Connection);
		}
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
			if (State->bLaneMode)
			{
				// The player never clicked a start: back to the start step, and drop the lane path the
				// game is still drawing (otherwise it stays on screen next to the normal hologram).
				SetBuildStep(Hologram, 0);
				SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), false);
				if (AFGRailroadTrackHologram* Rail = Cast<AFGRailroadTrackHologram>(Hologram))
				{
					FRMRailAccess::ResetToStart(Rail);
					SetRailSnappedConnection(Hologram, 0, nullptr);
					SetRailSnappedConnection(Hologram, 1, nullptr);
					// Replace the lane track with a short stub so nothing of it stays on screen.
					const FVector P = Rail->GetActorLocation();
					const FVector F = Rail->GetActorForwardVector() * 100.f;
					FRMRailAccess::SetSplineWorld(Rail, { P, P + F }, { F, F });
				}
				if (AFGVehiclePathSegmentHologram* Path = Cast<AFGVehiclePathSegmentHologram>(Hologram))
				{
					// Remove the lane path's meshes, then let the game redraw its start-step look.
					FRMPathAccess::RemovePathMeshes(Path);
					FRMPathAccess::Redraw(Path);
				}
			}
			GPathStates.Remove(Hologram);
		}
	}

	/** Height (cm) path/rail snap points sit above the asphalt. */
	constexpr float PathSnapLift = 10.f;

	/** How far (cm) snap points move onto the road at a free end that drops off. */
	constexpr float PathEndInset = 500.f;

	/** True if there is something to drive on just beyond a road end (within 1.5 m below its top). */
	bool HasFloorBeyond(UWorld* World, const FVector& EndPoint, const FVector& Outward)
	{
		if (!World)
		{
			return false;
		}
		const FVector Flat = FVector(Outward.X, Outward.Y, 0.f).GetSafeNormal();
		for (const float Ahead : { 150.f, 400.f })
		{
			const FVector From = EndPoint + Flat * Ahead + FVector(0.f, 0.f, 100.f);
			const FVector To = From - FVector(0.f, 0.f, 250.f);
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * True if an existing vehicle path (for path holograms) or railway track (for track holograms)
	 * is within 4 m: then the game's own snapping to it takes priority over road snap points.
	 */
	bool HasVanillaSnapNear(AFGHologram* Hologram, const FVector& Location)
	{
		UWorld* World = Hologram ? Hologram->GetWorld() : nullptr;
		if (!World)
		{
			return false;
		}
		const bool bRail = Hologram->IsA<AFGRailroadTrackHologram>();
		TArray<FOverlapResult> Overlaps;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMVanillaSnapNear), false, Hologram);
		World->OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity,
			FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllObjects), FCollisionShape::MakeSphere(400.f), Params);
		for (const FOverlapResult& O : Overlaps)
		{
			const AActor* A = O.GetActor();
			if (!A || A->IsA<AFGHologram>())
			{
				continue;
			}
			const FString Name = A->GetClass()->GetName();
			if (bRail ? Name.Contains(TEXT("RailroadTrack")) : Name.Contains(TEXT("VehiclePath")))
			{
				return true;
			}
		}
		return false;
	}

	/** Rise per horizontal distance of a road end, in its outward direction (0 for intersection mouths). */
	float RoadSlope(const FRMRoadEndRef& End)
	{
		const float Flat = FVector(End.Outward.X, End.Outward.Y, 0.f).Size();
		return Flat > KINDA_SMALL_NUMBER ? End.Outward.Z / Flat : 0.f;
	}


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
	template <typename TPlace, typename THologram>
	void PlacePathEnd(TPlace&& Place, THologram* Hologram, const FHitResult& Placed, const FVector& Start, const FVector& Axis, FPathState& State)
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
		Place(Placed);

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

	/**
	 * Like MoveHit, but the trace itself also runs straight down through the point: the rail tool
	 * traces again from the hit's trace (seen in the log: its end landed under the crosshair, not on
	 * the point given), so the trace must lead to the point too.
	 */
	FHitResult MoveHitVertical(const FHitResult& Hit, const FVector& Location)
	{
		FHitResult Moved = MoveHit(Hit, Location);
		Moved.TraceStart = Location + FVector(0.f, 0.f, 300.f);
		Moved.TraceEnd = Location - FVector(0.f, 0.f, 300.f);
		Moved.Distance = 300.f;
		Moved.Time = 0.5f;
		return Moved;
	}

	/** Shared before-hook body; THologram is the hooked class, which the scope must be called with. */
	/**
	 * One-click lane path: holding the vehicle path and aiming at a lane of a road (before the first
	 * click), the hologram is placed along the whole lane, end to end, and one click builds it.
	 *  - Lane: 2-3 lane roads use the lane centre (between the edge lines), 1-lane roads the nearest
	 *    2 m point, the same points the road-end snapping uses.
	 *  - Direction: the road's build direction, unless a path on a neighbouring road already ends or
	 *    starts at one of the lane points: then this one continues that flow and joins its node.
	 *    Scroll flips it.
	 *  - Ends that drop off move 5 m onto the road (the game needs floor under the whole vehicle).
	 * The start is placed by the game's own code in its start step, then the hologram is switched to
	 * its end step and the end is placed like a road-end snap (heading through the scroll value).
	 */
	template <typename TScope, typename THologram>
	bool PlaceLanePath(TScope& Scope, THologram* Hologram, const FHitResult& Hit, ARMBuildableRoad* Road)
	{
		UWorld* World = Hologram->GetWorld();
		TArray<FRMFrame> Frames;
		Road->GetWorldFrames(Frames, 100.f);
		if (!World || Frames.Num() < 2)
		{
			return false;
		}

		// Lane under the cursor.
		int32 NearIdx = 0;
		float NearDist = TNumericLimits<float>::Max();
		for (int32 I = 0; I < Frames.Num(); ++I)
		{
			const float D = FVector::DistSquared(Frames[I].Pos, Hit.Location);
			if (D < NearDist)
			{
				NearDist = D;
				NearIdx = I;
			}
		}
		const float U = FVector::DotProduct(Hit.Location - Frames[NearIdx].Pos, Frames[NearIdx].Right);
		const float Width = Road->GetRoadWidth();
		const int32 Lanes = FMath::Clamp<int32>(Road->GetMarkings().Lanes, 1, 3);
		TArray<float> Offsets;
		if (Lanes <= 1)
		{
			const int32 Count = FMath::FloorToInt(Width * 0.5f / 200.f);
			for (int32 K = -Count; K <= Count; ++K)
			{
				Offsets.Add(K * 200.f);
			}
		}
		else
		{
			const float Inset = Road->GetMarkings().bEdgeLines ? Road->mEdgeLineInset : 0.f; // no side lines: full width
			const float Inner = FMath::Max(Width - 2.f * Inset, 1.f);
			for (int32 I = 0; I < Lanes; ++I)
			{
				Offsets.Add(-Width * 0.5f + Inset + Inner * (I + 0.5f) / Lanes);
			}
		}
		int32 Lane = 0;
		for (int32 I = 1; I < Offsets.Num(); ++I)
		{
			if (FMath::Abs(Offsets[I] - U) < FMath::Abs(Offsets[Lane] - U))
			{
				Lane = I;
			}
		}
		const float Off = Offsets[Lane];
		const FRMFrame& F0 = Frames[0];
		const FRMFrame& FN = Frames.Last();
		FVector A = F0.Pos + F0.Right * Off;   // at road end 0
		FVector B = FN.Pos + FN.Right * Off;   // at road end 1
		bool bJoinedA = false, bJoinedB = false;

		// Existing paths at the lane points: continue their flow and use their nodes.
		int32 Suggest = 0; // +1: start->end of the road, -1: the other way
		AFGVehiclePathNode* NodeA = nullptr;
		AFGVehiclePathNode* NodeB = nullptr;
		auto NodeNear = [](AFGVehiclePathNode* Node, const FVector& P, FVector& Out, AFGVehiclePathNode*& OutNode)
		{
			if (Node && FVector::Dist(Node->GetActorLocation(), P) < 150.f)
			{
				Out = Node->GetActorLocation();
				OutNode = Node;
				return true;
			}
			return false;
		};
		for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It)
		{
			FVector Snap;
			if (!bJoinedA && NodeNear(It->GetEndNode(), A, Snap, NodeA))        { Suggest = Suggest ? Suggest : 1;  A = Snap; bJoinedA = true; }
			else if (!bJoinedA && NodeNear(It->GetStartNode(), A, Snap, NodeA)) { Suggest = Suggest ? Suggest : -1; A = Snap; bJoinedA = true; }
			if (!bJoinedB && NodeNear(It->GetStartNode(), B, Snap, NodeB))      { Suggest = Suggest ? Suggest : 1;  B = Snap; bJoinedB = true; }
			else if (!bJoinedB && NodeNear(It->GetEndNode(), B, Snap, NodeB))   { Suggest = Suggest ? Suggest : -1; B = Snap; bJoinedB = true; }
		}

		FPathState& State = GPathStates.FindOrAdd(Hologram);
		if (!State.bLaneMode || State.Owner.Get() != Road || State.EndIndex != Lane)
		{
			State.bFlipped = false; // new road or lane: start from the suggested direction
		}
		State.bLaneMode = true;
		State.Owner = Road;
		State.EndIndex = Lane;
		State.bStartStep = false;
		State.StartDir = FVector::ZeroVector;

		bool bForward = Suggest >= 0;
		if (State.bFlipped)
		{
			bForward = !bForward;
		}

		// The lane as road distance range: ends without a road or path joined and nothing to drive
		// on beyond them start 5 m onto the road (the game needs floor under the whole vehicle).
		const float Total = FN.Dist;
		const bool bInset0 = !bJoinedA && !RMRoadUtils::IsEndOccupied(World, Road, 0) && !HasFloorBeyond(World, A, -F0.Fwd);
		const bool bInset1 = !bJoinedB && !RMRoadUtils::IsEndOccupied(World, Road, 1) && !HasFloorBeyond(World, B, FN.Fwd);
		const float S0 = bInset0 ? FMath::Min(PathEndInset, Total * 0.3f) : 0.f;
		const float S1 = bInset1 ? Total - FMath::Min(PathEndInset, Total * 0.3f) : Total;

		// Lane points every ~4 m along the road, following its curve exactly (the game's own path
		// would be a single curve between the ends and leave the lane on bends).
		auto LaneAt = [&Frames, Off](float D, FVector& OutPos, FVector& OutDir)
		{
			int32 Seg = 0;
			while (Seg + 2 < Frames.Num() && Frames[Seg + 1].Dist < D)
			{
				++Seg;
			}
			const FRMFrame& Fa = Frames[Seg];
			const FRMFrame& Fb = Frames[Seg + 1];
			const float T = Fb.Dist > Fa.Dist ? FMath::Clamp((D - Fa.Dist) / (Fb.Dist - Fa.Dist), 0.f, 1.f) : 0.f;
			const FVector Right = FMath::Lerp(Fa.Right, Fb.Right, T).GetSafeNormal();
			OutPos = FMath::Lerp(Fa.Pos, Fb.Pos, T) + Right * Off;
			OutDir = FMath::Lerp(Fa.Fwd, Fb.Fwd, T).GetSafeNormal();
		};
		const int32 Keys = FMath::Max(1, FMath::CeilToInt((S1 - S0) / 400.f));
		TArray<FVector> Points, Dirs;
		for (int32 K = 0; K <= Keys; ++K)
		{
			FVector P, D;
			LaneAt(S0 + (S1 - S0) * K / Keys, P, D);
			Points.Add(P + FVector::UpVector * PathSnapLift);
			Dirs.Add(D);
		}
		if (bJoinedA)
		{
			Points[0] = A; // exactly on the existing path's node
		}
		if (bJoinedB)
		{
			Points.Last() = B;
		}
		TArray<FVector> Tangents;
		for (int32 K = 0; K < Points.Num(); ++K)
		{
			const FVector& Prev = Points[FMath::Max(K - 1, 0)];
			const FVector& Next = Points[FMath::Min(K + 1, Points.Num() - 1)];
			const float Span = FVector::Dist(Prev, Next) / (K == 0 || K == Points.Num() - 1 ? 1.f : 2.f);
			Tangents.Add(Dirs[K] * Span);
		}
		if (!bForward)
		{
			Algo::Reverse(Points);
			Algo::Reverse(Tangents);
			for (FVector& T : Tangents)
			{
				T = -T;
			}
		}
		const FVector StartP = Points[0];
		const FVector EndP = Points.Last();
		AFGVehiclePathNode* StartNode = bForward ? NodeA : NodeB;
		AFGVehiclePathNode* EndNode = bForward ? NodeB : NodeA;
		AFGVehiclePathSegmentHologram* Path = Cast<AFGVehiclePathSegmentHologram>(Hologram);
		const FVector StartDir = Tangents[0].GetSafeNormal();
		const FVector EndDir = Tangents.Last().GetSafeNormal();

		// Start: the game's own start step (so it can join an existing node there), then the end
		// step; finally the lane points replace the game's single curve, which is what gets built.
		SetBuildStep(Hologram, 0);
		Scope(Hologram, MoveHit(Hit, StartP));
		if (Path && StartNode)
		{
			FRMPathAccess::SnapToNode(Path, StartNode); // joins the existing path there
		}
		if (FVector* Exit = VectorField(Hologram, TEXT("mStartPointExitDirection")))
		{
			*Exit = StartDir;
		}
		SetBuildStep(Hologram, 1);
		const FVector AxisB = FVector(EndDir.X, EndDir.Y, 0.f).GetSafeNormal();
		const bool bWasFlipped = State.bFlipped;
		State.bFlipped = false; // the direction is already decided above
		PlacePathEnd([Hologram](const FHitResult& H)
			{
				TGuardValue<bool> Guard(GInPathReplace, true);
				Hologram->SetHologramLocationAndRotation(H);
			}, Hologram, MoveHit(Hit, EndP), EndP - AxisB * 100.f, AxisB, State);
		State.bFlipped = bWasFlipped;
		if (Path && EndNode)
		{
			FRMPathAccess::SnapToNode(Path, EndNode); // joins the existing path there
		}
		SetPathSplineWorld(Hologram, Points, Tangents);
		return true;
	}

	/**
	 * One-click lane tracks are switched off for now: the game's rail tool rebuilds its curve from its
	 * own rules every frame (end point and heading included), so the lane track looped or went off in
	 * another direction. Road-end snapping for tracks (alignment and slope) is unaffected.
	 */
	constexpr bool bEnableLaneTracks = true;

	/** Rail hologram spline step (ESplineHologramBuildStep: 0 = find start, 2 = place end). */
	void SetRailStep(AFGHologram* Hologram, uint8 Step)
	{
		SetBuildStep(Hologram, Step); // same property name (mBuildStep) on the spline hologram
	}


	/** A free track end (connection with nothing attached) within 1.5 m of a point. */
	UFGRailroadTrackConnectionComponent* FindFreeTrackEnd(UWorld* World, const FVector& Point)
	{
		for (TActorIterator<AFGBuildableRailroadTrack> It(World); It; ++It)
		{
			for (int32 C = 0; C < 2; ++C)
			{
				UFGRailroadTrackConnectionComponent* Conn = It->GetConnection(C);
				if (Conn && !Conn->IsConnected() && FVector::Dist(Conn->GetConnectorLocation(), Point) < 150.f)
				{
					return Conn;
				}
			}
		}
		return nullptr;
	}

	/**
	 * One-click lane track (snap key held, before the first click): the rail hologram is laid along
	 * the whole lane, end to end, and one click builds it. Same lanes as vehicle paths; rails run both
	 * ways, so the direction only follows the road (scroll flips it). Free track ends at the lane ends
	 * are connected to. The start is placed by the game's own start step, then the hologram is put
	 * in its end step, the end placed, and the lane points replace the game's single curve.
	 */
	template <typename TScope, typename THologram>
	bool PlaceLaneTrack(TScope& Scope, THologram* Hologram, const FHitResult& Hit, ARMBuildableRoad* Road)
	{
		UWorld* World = Hologram->GetWorld();
		AFGRailroadTrackHologram* Rail = Cast<AFGRailroadTrackHologram>(Hologram);
		TArray<FRMFrame> Frames;
		Road->GetWorldFrames(Frames, 100.f);
		if (!World || !Rail || Frames.Num() < 2)
		{
			return false;
		}

		// Lane under the cursor (same rules as vehicle paths).
		int32 NearIdx = 0;
		float NearDist = TNumericLimits<float>::Max();
		for (int32 I = 0; I < Frames.Num(); ++I)
		{
			const float D = FVector::DistSquared(Frames[I].Pos, Hit.Location);
			if (D < NearDist)
			{
				NearDist = D;
				NearIdx = I;
			}
		}
		const float U = FVector::DotProduct(Hit.Location - Frames[NearIdx].Pos, Frames[NearIdx].Right);
		const float Width = Road->GetRoadWidth();
		const int32 Lanes = FMath::Clamp<int32>(Road->GetMarkings().Lanes, 1, 3);
		TArray<float> Offsets;
		if (Lanes <= 1)
		{
			const int32 Count = FMath::FloorToInt(Width * 0.5f / 200.f);
			for (int32 K = -Count; K <= Count; ++K)
			{
				Offsets.Add(K * 200.f);
			}
		}
		else
		{
			const float Inset = Road->GetMarkings().bEdgeLines ? Road->mEdgeLineInset : 0.f;
			const float Inner = FMath::Max(Width - 2.f * Inset, 1.f);
			for (int32 I = 0; I < Lanes; ++I)
			{
				Offsets.Add(-Width * 0.5f + Inset + Inner * (I + 0.5f) / Lanes);
			}
		}
		int32 Lane = 0;
		for (int32 I = 1; I < Offsets.Num(); ++I)
		{
			if (FMath::Abs(Offsets[I] - U) < FMath::Abs(Offsets[Lane] - U))
			{
				Lane = I;
			}
		}
		const float Off = Offsets[Lane];

		FPathState& State = GPathStates.FindOrAdd(Hologram);
		if (!State.bLaneMode || State.Owner.Get() != Road || State.EndIndex != Lane)
		{
			State.bFlipped = false;
		}
		if (!State.bLaneMode)
		{
			State.SavedScroll = Hologram->GetScrollRotateValue();
		}
		State.bLaneMode = true;
		State.bSetCustomEnd = true; // 1 degree rotation steps, scroll restored on leaving
		State.Owner = Road;
		State.EndIndex = Lane;
		State.bStartStep = false;

		// Lane points every ~4 m, right on the asphalt.
		const float Total = Frames.Last().Dist;
		const int32 Keys = FMath::Max(1, FMath::CeilToInt(Total / 400.f));
		TArray<FVector> Points, Tangents;
		for (int32 K = 0; K <= Keys; ++K)
		{
			const float D = Total * K / Keys;
			int32 Seg = 0;
			while (Seg + 2 < Frames.Num() && Frames[Seg + 1].Dist < D)
			{
				++Seg;
			}
			const FRMFrame& Fa = Frames[Seg];
			const FRMFrame& Fb = Frames[Seg + 1];
			const float T = Fb.Dist > Fa.Dist ? FMath::Clamp((D - Fa.Dist) / (Fb.Dist - Fa.Dist), 0.f, 1.f) : 0.f;
			const FVector Right = FMath::Lerp(Fa.Right, Fb.Right, T).GetSafeNormal();
			Points.Add(FMath::Lerp(Fa.Pos, Fb.Pos, T) + Right * Off);
			Tangents.Add(FMath::Lerp(Fa.Fwd, Fb.Fwd, T).GetSafeNormal());
		}
		for (int32 K = 0; K < Points.Num(); ++K)
		{
			const FVector& Prev = Points[FMath::Max(K - 1, 0)];
			const FVector& Next = Points[FMath::Min(K + 1, Points.Num() - 1)];
			Tangents[K] *= FVector::Dist(Prev, Next) / (K == 0 || K == Points.Num() - 1 ? 1.f : 2.f);
		}
		if (State.bFlipped)
		{
			Algo::Reverse(Points);
			Algo::Reverse(Tangents);
			for (FVector& T : Tangents)
			{
				T = -T;
			}
		}

		// Existing free track ends at the lane ends: start/end exactly there and connect.
		UFGRailroadTrackConnectionComponent* StartConn = FindFreeTrackEnd(World, Points[0]);
		UFGRailroadTrackConnectionComponent* EndConn = FindFreeTrackEnd(World, Points.Last());
		if (StartConn)
		{
			Points[0] = StartConn->GetConnectorLocation();
		}
		if (EndConn)
		{
			Points.Last() = EndConn->GetConnectorLocation();
		}
		const FVector StartDir = Tangents[0].GetSafeNormal();
		const FVector EndDir = Tangents.Last().GetSafeNormal();
		// The rail tool's own placement is not run here: each frame it writes its curve into only the
		// first two spline points and leaves the rest, mixing it into the lane (loops, invalid). So the
		// hologram is set up directly: end step, start at the lane start, lane points, connections.
		Scope.Cancel();
		Hologram->SetActorLocationAndRotation(Points[0], FRotator::ZeroRotator);
		SetRailStep(Hologram, 2);
		if (FVector* SnappedLoc = VectorField(Hologram, TEXT("mSnappedStartLocation")))
		{
			*SnappedLoc = Points[0];
		}
		if (FVector* SnappedTan = VectorField(Hologram, TEXT("mSnappedStartTangent")))
		{
			*SnappedTan = StartDir;
		}
		if (FVector* HitLoc = VectorField(Hologram, TEXT("mHitLocation")))
		{
			*HitLoc = Points.Last();
		}
		// The rail tool re-routes the track from its start and "hit" values after placement (its check
		// then drew a ring when given the travel direction): its end tangent points out of the track
		// end, i.e. against the travel direction.
		if (FVector* HitTan = VectorField(Hologram, TEXT("mHitTangent")))
		{
			*HitTan = -EndDir;
		}
		SetBoolField(Hologram, TEXT("mUseCustomEndRotation"), true);
		SetRailSnappedConnection(Hologram, 0, StartConn);
		SetRailSnappedConnection(Hologram, 1, EndConn);
		FRMRailAccess::SetSplineWorld(Rail, Points, Tangents);
		return true;
	}

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
			// Only the piece you aim at: a road gives its own ends' points, an intersection its own.
			// The game's own snapping comes first (aiming at or near an existing path / track).
			AActor* Aimed = Hit.GetActor();
			const bool bAimRoad = Cast<ARMBuildableRoad>(Aimed) != nullptr;
			const bool bAimIntersection = Cast<ARMBuildableIntersection>(Aimed) != nullptr;

			// One-click lane path (vehicle paths, before the first click): aiming at a road's lane.
			if (Hologram->template IsA<AFGVehiclePathSegmentHologram>())
			{
				FPathState* Lane = GPathStates.Find(Hologram);
				const bool bLaneMode = Lane && Lane->bLaneMode;
				// Only while the snap key is held, so the normal placement stays available.
				if (bAimRoad && FRMSnapAccess::IsSnapHeld(Hologram) && (bLaneMode || IsStartStep(Hologram)))
				{
					if (PlaceLanePath(Scope, Hologram, Hit, Cast<ARMBuildableRoad>(Aimed)))
					{
						return true;
					}
				}
				else if (bLaneMode)
				{
					ReleasePathSnap(Hologram); // looked away from the road: back to a normal start
				}
			}
			else if (bEnableLaneTracks && Hologram->template IsA<AFGRailroadTrackHologram>())
			{
				FPathState* Lane = GPathStates.Find(Hologram);
				const bool bLaneMode = Lane && Lane->bLaneMode;
				if (bAimRoad && FRMSnapAccess::IsSnapHeld(Hologram) && (bLaneMode || IsStartStep(Hologram)))
				{
					if (PlaceLaneTrack(Scope, Hologram, Hit, Cast<ARMBuildableRoad>(Aimed)))
					{
						return true;
					}
				}
				else if (bLaneMode)
				{
					ReleasePathSnap(Hologram);
				}
			}
			if ((!bAimRoad && !bAimIntersection) || HasVanillaSnapNear(Hologram, Hit.Location))
			{
				ReleasePathSnap(Hologram);
				return false;
			}
			TArray<FRMRoadEndRef> Ends;
			RMRoadUtils::FindFreeEnds(Hologram->GetWorld(), Hit.Location, 1200.f, 16, nullptr, Ends, true);
			FVector Best = FVector::ZeroVector;
			const FRMRoadEndRef* BestEnd = nullptr;
			float BestDist = 600.f;
			for (const FRMRoadEndRef& End : Ends)
			{
				const AActor* Owner = End.Road ? static_cast<const AActor*>(End.Road) : static_cast<const AActor*>(End.Intersection);
				if (Owner != Aimed)
				{
					continue;
				}
				for (const FVector& P : End.GetSnapPoints(Hologram->GetWorld()))
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

			// The game's path check needs floor under the whole vehicle at both ends of a path (seen
			// in the log: "NoFloor" at the last point). At a free end that drops off (raised road,
			// nothing beyond it) the points move 5 m onto the road, so a vehicle standing there is on
			// the asphalt. Ends joined to another piece, or with floor right beyond them (ground,
			// foundations), keep their points on the end so paths still meet there.
			const bool bFreeEnd = BestEnd->Road
				? !RMRoadUtils::IsEndOccupied(Hologram->GetWorld(), BestEnd->Road, BestEnd->EndIndex)
				: !RMRoadUtils::IsMouthOccupied(Hologram->GetWorld(), BestEnd->Intersection, BestEnd->Location);
			// Both adjustments are for vehicle paths only: rail tracks are not laid onto the surface
			// (a lift would leave them floating) and have no vehicle floor check.
			const bool bVehiclePath = Hologram->IsA<AFGVehiclePathSegmentHologram>();
			if (bVehiclePath && bFreeEnd && !HasFloorBeyond(Hologram->GetWorld(), Best, Along3))
			{
				Best -= Along3 * PathEndInset;
			}

			// A little above the asphalt (along the road's up): the game's "vehicles collide with
			// the terrain" check tests against the ground, which near the high end of a road on a
			// hill can sit right at the road surface.
			if (bVehiclePath)
			{
				Best += SurfaceUp * PathSnapLift;
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
				PlacePathEnd([&](const FHitResult& H) { Scope(Hologram, H); }, Hologram, Placed, Start, Axis, State);
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

	// Lane tracks: after placing the hologram the game runs a post-placement step in which the rail
	// tool routes its own track again from the crosshair hit; in lane mode that second track showed up
	// next to the lane track (a ring) and failed the checks. Skipped while a lane track is shown.
	SUBSCRIBE_METHOD_VIRTUAL(AFGRailroadTrackHologram::PostHologramPlacement, GetMutableDefault<AFGRailroadTrackHologram>(),
		[](auto& Scope, AFGRailroadTrackHologram* Hologram, const FHitResult& HitResult, bool bCallForChildren)
		{
			const FPathState* State = GPathStates.Find(Hologram);
			if (State && State->bLaneMode)
			{
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
