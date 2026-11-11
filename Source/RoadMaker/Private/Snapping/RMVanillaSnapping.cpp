#include "Snapping/RMVanillaSnapping.h"

#if !WITH_EDITOR

#include "RoadMakerModule.h"
#include "RMRoadUtils.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/RMBuildableGantry.h"
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
#include "FGInventoryComponent.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Algo/Reverse.h"
#include "UObject/UnrealType.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Hologram/FGFoundationHologram.h"
#include "Hologram/FGStandaloneSignHologram.h"
#include "Hologram/FGSignPoleHologram.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"

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

	enum class ESnapKind : uint8 { None, PowerPole, StreetLight, PathStart, Foundation, Sign };

	/** Correction between "facing" and the street light model's own forward axis (was 90 off in game). */
	constexpr float StreetLightYawOffset = -90.f;

	ESnapKind KindOf(const AFGHologram* Hologram)
	{
		const UClass* Build = Hologram ? Hologram->GetBuildClass().Get() : nullptr;
		if (!Build)
		{
			return ESnapKind::None;
		}
		if (Hologram->IsA<AFGStandaloneSignHologram>())
		{
			return ESnapKind::Sign;
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
		if (Hologram->IsA<AFGFoundationHologram>() && Build->IsChildOf(AFGBuildableFoundation::StaticClass()))
		{
			return ESnapKind::Foundation; // foundations, frames and ramps (normal and inverted)
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
		const int32 Lanes = FMath::Clamp<int32>(Road->GetMarkings().Lanes, 1, 4);
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
	 * Sets a rail hologram up directly in its end step with the given track points. The rail tool's
	 * own placement is not run: each frame it writes its curve into only the first two spline points
	 * and leaves the rest (loops, invalid), and its post-placement re-route is skipped in this mode.
	 */
	void SetupRailDirect(AFGRailroadTrackHologram* Rail, const TArray<FVector>& Points, const TArray<FVector>& Tangents,
		UFGRailroadTrackConnectionComponent* StartConn, UFGRailroadTrackConnectionComponent* EndConn)
	{
		const FVector StartDir = Tangents[0].GetSafeNormal();
		const FVector EndDir = Tangents.Last().GetSafeNormal();
		Rail->SetActorLocationAndRotation(Points[0], FRotator::ZeroRotator);
		SetRailStep(Rail, 2);
		if (FVector* SnappedLoc = VectorField(Rail, TEXT("mSnappedStartLocation")))
		{
			*SnappedLoc = Points[0];
		}
		if (FVector* SnappedTan = VectorField(Rail, TEXT("mSnappedStartTangent")))
		{
			*SnappedTan = StartDir;
		}
		if (FVector* HitLoc = VectorField(Rail, TEXT("mHitLocation")))
		{
			*HitLoc = Points.Last();
		}
		// The rail tool's end tangent points out of the track end, against the travel direction.
		if (FVector* HitTan = VectorField(Rail, TEXT("mHitTangent")))
		{
			*HitTan = -EndDir;
		}
		SetBoolField(Rail, TEXT("mUseCustomEndRotation"), true);
		SetRailSnappedConnection(Rail, 0, StartConn);
		SetRailSnappedConnection(Rail, 1, EndConn);
		FRMRailAccess::SetSplineWorld(Rail, Points, Tangents);
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
		const int32 Lanes = FMath::Clamp<int32>(Road->GetMarkings().Lanes, 1, 4);
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
		Scope.Cancel(); // the rail tool's own placement is not run (see SetupRailDirect)
		SetupRailDirect(Rail, Points, Tangents, StartConn, EndConn);
		return true;
	}

	// --- Intersection connections (vehicle path / rail track + snap key, aiming at an intersection) ---

	/** Main and child holograms currently showing intersection connections. */
	TSet<TWeakObjectPtr<const AFGHologram>> GJunctionHolograms;

	/** A path end at an intersection end: arriving (a path ends there) or leaving (a path starts there). */
	struct FJunctionNode
	{
		AFGVehiclePathNode* Node = nullptr;
		FVector Location = FVector::ZeroVector;
		FVector Dir = FVector::ForwardVector; // travel direction, horizontal
		int32 Mouth = -1;
	};

	struct FJunctionLink
	{
		FJunctionNode In;
		FJunctionNode Out;
	};

	/** Child holograms used for the connections beyond the first, per parent hologram. */
	TMap<TWeakObjectPtr<const AFGHologram>, TArray<TWeakObjectPtr<AFGHologram>>> GJunctionChildren;

	/** Which connection the main hologram shows (moved to a valid one when it is not). */
	TMap<TWeakObjectPtr<const AFGHologram>, int32> GJunctionParentLink;

	/** Every sensible connection through an intersection: each arriving path to each leaving path on another end. */
	void FindJunctionLinks(UWorld* World, const ARMBuildableIntersection* X, TArray<FJunctionLink>& OutLinks)
	{
		OutLinks.Reset();
		TArray<FRMRoadEnd> Mouths;
		X->GetWorldEnds(Mouths);
		TArray<FJunctionNode> Ins, Outs;
		TSet<TPair<const AFGVehiclePathNode*, const AFGVehiclePathNode*>> Existing;

		auto MouthOf = [&Mouths](const FVector& P) -> int32
		{
			for (int32 M = 0; M < Mouths.Num(); ++M)
			{
				const FVector In = FVector(Mouths[M].Inward.X, Mouths[M].Inward.Y, 0.f).GetSafeNormal();
				const FVector Across = FVector::CrossProduct(FVector::UpVector, In);
				const FVector D = P - Mouths[M].Center;
				if (FMath::Abs(FVector::DotProduct(D, In)) < 300.f && FMath::Abs(FVector::DotProduct(D, Across)) < Mouths[M].Width * 0.5f + 100.f
					&& FMath::Abs(D.Z) < 400.f)
				{
					return M;
				}
			}
			return -1;
		};

		for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It)
		{
			USplineComponent* Spline = It->GetSplineComponent();
			AFGVehiclePathNode* StartNode = It->GetStartNode();
			AFGVehiclePathNode* EndNode = It->GetEndNode();
			if (!Spline || Spline->GetNumberOfSplinePoints() < 2 || !StartNode || !EndNode)
			{
				continue;
			}
			Existing.Add(TPair<const AFGVehiclePathNode*, const AFGVehiclePathNode*>(StartNode, EndNode));
			const int32 Last = Spline->GetNumberOfSplinePoints() - 1;
			const FVector EndDir = Spline->GetDirectionAtSplinePoint(Last, ESplineCoordinateSpace::World).GetSafeNormal2D();
			const FVector StartDir = Spline->GetDirectionAtSplinePoint(0, ESplineCoordinateSpace::World).GetSafeNormal2D();
			const int32 EndMouth = MouthOf(EndNode->GetActorLocation());
			if (EndMouth >= 0 && FVector::DotProduct(EndDir, Mouths[EndMouth].Inward.GetSafeNormal2D()) > 0.5f)
			{
				Ins.Add({ EndNode, EndNode->GetActorLocation(), EndDir, EndMouth });   // arrives into the intersection
			}
			const int32 StartMouth = MouthOf(StartNode->GetActorLocation());
			if (StartMouth >= 0 && FVector::DotProduct(StartDir, Mouths[StartMouth].Inward.GetSafeNormal2D()) < -0.5f)
			{
				Outs.Add({ StartNode, StartNode->GetActorLocation(), StartDir, StartMouth }); // leaves the intersection
			}
		}
		for (const FJunctionNode& In : Ins)
		{
			for (const FJunctionNode& Out : Outs)
			{
				if (In.Mouth == Out.Mouth || In.Node == Out.Node
					|| Existing.Contains(TPair<const AFGVehiclePathNode*, const AFGVehiclePathNode*>(In.Node, Out.Node)))
				{
					continue; // no U-turns, no connection that is already built
				}
				OutLinks.Add({ In, Out });
				if (OutLinks.Num() >= 24)
				{
					return;
				}
			}
		}
	}

	/**
	 * Sets up a vehicle path hologram (main or child) for one connection, without its own placement
	 * input: start step at the arriving node, join it, end step at the leaving node, join it, and a
	 * smooth curve between them as the path's points. Placement calls go through the guard so our
	 * hook lets the game's code run.
	 */
	void SetupJunctionHologram(AFGVehiclePathSegmentHologram* H, const FHitResult& BaseHit, const FJunctionLink& Link)
	{
		GJunctionHolograms.Add(H);
		FPathState& State = GPathStates.FindOrAdd(H);
		State.bLaneMode = true;
		State.bStartStep = false;
		State.StartDir = FVector::ZeroVector;

		TArray<FRMSplinePoint> Curve;
		RMGeometry::MakeFixedEnds(Link.In.Location, Link.In.Dir, Link.Out.Location, Link.Out.Dir, Curve);
		if (Curve.Num() < 2)
		{
			return;
		}
		auto PlaceGuarded = [H](const FHitResult& Hit)
		{
			TGuardValue<bool> Guard(GInPathReplace, true);
			H->SetHologramLocationAndRotation(Hit);
		};
		SetBuildStep(H, 0);
		PlaceGuarded(MoveHit(BaseHit, Link.In.Location));
		FRMPathAccess::SnapToNode(H, Link.In.Node);
		if (FVector* Exit = VectorField(H, TEXT("mStartPointExitDirection")))
		{
			*Exit = Link.In.Dir;
		}
		SetBuildStep(H, 1);
		const bool bWasFlipped = State.bFlipped;
		State.bFlipped = false;
		PlacePathEnd(PlaceGuarded, H, MoveHit(BaseHit, Link.Out.Location), Link.Out.Location - Link.Out.Dir * 100.f, Link.Out.Dir, State);
		State.bFlipped = bWasFlipped;
		FRMPathAccess::SnapToNode(H, Link.Out.Node);
		SetPathSplineWorld(H, { Curve[0].Location, Curve[1].Location }, { Curve[0].Tangent, Curve[1].Tangent });
	}

	/** Protected hologram child lists: detaching a child so the main hologram no longer checks or builds it. */
	struct FRMChildAccess : public AFGHologram
	{
		static void Detach(AFGHologram* Parent, AFGHologram* Child)
		{
			FRMChildAccess* Self = static_cast<FRMChildAccess*>(Parent);
			Self->mChildren.Remove(Child);
			for (auto It = Self->mChildrenNameLookupMap.CreateIterator(); It; ++It)
			{
				if (It.Value() == Child)
				{
					It.RemoveCurrent();
				}
			}
			static_cast<FRMChildAccess*>(Child)->mParent = nullptr;
		}
	};


	/** Detaches and deletes one connection child. */
	void RemoveJunctionChild(AFGHologram* Parent, TWeakObjectPtr<AFGHologram>& Slot)
	{
		if (AFGHologram* Child = Slot.Get())
		{
			GJunctionHolograms.Remove(Child);
			FRMChildAccess::Detach(Parent, Child);
			GPathStates.Remove(Child);
			Child->Destroy();
		}
		Slot.Reset();
	}

	/** What the junction holograms were last set up for, so unchanged frames cost nothing. */
	struct FJunctionCache
	{
		TWeakObjectPtr<const ARMBuildableIntersection> Intersection;
		TArray<TPair<const void*, const void*>> Signature;
		double LastScan = 0.0;
	};
	TMap<TWeakObjectPtr<const AFGHologram>, FJunctionCache> GJunctionCache;

	/** Hides and switches off the connection children (leaving junction mode). */
	void ReleaseJunction(AFGHologram* Parent)
	{
		if (TArray<TWeakObjectPtr<AFGHologram>>* Children = GJunctionChildren.Find(Parent))
		{
			for (TWeakObjectPtr<AFGHologram>& C : *Children)
			{
				RemoveJunctionChild(Parent, C);
			}
			Children->Reset();
		}
		GJunctionParentLink.Remove(Parent);
		GJunctionCache.Remove(Parent);
		GJunctionHolograms.Remove(Parent);
	}

	/**
	 * Vehicle path + snap key aiming at an intersection: every connection between the paths at its
	 * ends is shown at once (main hologram + child holograms) and one click builds the valid ones.
	 */

	template <typename TScope, typename THologram>
	bool PlaceJunction(TScope& Scope, THologram* Hologram, const FHitResult& Hit, ARMBuildableIntersection* X)
	{
		AFGVehiclePathSegmentHologram* Parent = Cast<AFGVehiclePathSegmentHologram>(Hologram);
		UWorld* World = Hologram->GetWorld();
		if (!Parent || !World)
		{
			return false;
		}

		// Unchanged since last set up (same intersection, scanned recently): leave everything in place.
		FJunctionCache& Cache = GJunctionCache.FindOrAdd(Parent);
		const double Now = FPlatformTime::Seconds();
		const FPathState* Existing = GPathStates.Find(Parent);
		if (Cache.Intersection.Get() == X && Existing && Existing->bLaneMode && Now - Cache.LastScan < 0.5)
		{
			Scope.Cancel();
			return true;
		}

		TArray<FJunctionLink> Links;
		FindJunctionLinks(World, X, Links);
		if (Links.Num() == 0)
		{
			Cache = FJunctionCache();
			return false;
		}
		TArray<TPair<const void*, const void*>> Signature;
		for (const FJunctionLink& L : Links)
		{
			Signature.Add(TPair<const void*, const void*>(L.In.Node, L.Out.Node));
		}
		Scope.Cancel(); // everything is placed below, through the game's code
		Cache.LastScan = Now;
		if (Cache.Intersection.Get() == X && Cache.Signature == Signature && Existing && Existing->bLaneMode)
		{
			return true; // same connections as before: already placed and checked
		}
		Cache.Intersection = X;
		Cache.Signature = Signature;

		// Place every connection (main hologram takes link 0 for now, children the rest).
		TArray<TWeakObjectPtr<AFGHologram>>& Children = GJunctionChildren.FindOrAdd(Parent);
		TArray<AFGVehiclePathSegmentHologram*> LinkHolograms; // per link
		LinkHolograms.Add(Parent);
		SetupJunctionHologram(Parent, Hit, Links[0]);
		for (int32 L = 1; L < Links.Num(); ++L)
		{
			const int32 ChildIndex = L - 1;
			if (!Children.IsValidIndex(ChildIndex) || !Children[ChildIndex].IsValid())
			{
				const FName Name(*FString::Printf(TEXT("RMJunction%d"), ChildIndex));
				AFGHologram* NewChild = AFGHologram::SpawnChildHologramFromRecipe(Parent, Name, Parent->GetRecipe(), Parent->GetOwner(), Links[L].In.Location);
				if (!NewChild)
				{
					break;
				}
				if (Children.IsValidIndex(ChildIndex))
				{
					Children[ChildIndex] = NewChild;
				}
				else
				{
					Children.Add(NewChild);
				}
			}
			AFGVehiclePathSegmentHologram* Child = Cast<AFGVehiclePathSegmentHologram>(Children[ChildIndex].Get());
			if (!Child)
			{
				break;
			}
			Child->SetDisabled(false);
			Child->SetActorHiddenInGame(false);
			SetupJunctionHologram(Child, Hit, Links[L]);
			LinkHolograms.Add(Child);
		}
		for (int32 C = LinkHolograms.Num() - 1; C < Children.Num(); ++C)
		{
			RemoveJunctionChild(Parent, Children[C]); // not needed now
		}

		// Every connection is kept with the game's normal rules: if one is invalid the click is
		// refused, so the intersection can be improved until all connections are valid.
		return true;
	}

	/** Index of the intersection end a point sits at (within 3 m of it, inside its width), or -1. */
	int32 MouthIndexOf(const TArray<FRMRoadEnd>& Mouths, const FVector& P)
	{
		for (int32 M = 0; M < Mouths.Num(); ++M)
		{
			const FVector In = FVector(Mouths[M].Inward.X, Mouths[M].Inward.Y, 0.f).GetSafeNormal();
			const FVector Across = FVector::CrossProduct(FVector::UpVector, In);
			const FVector D = P - Mouths[M].Center;
			if (FMath::Abs(FVector::DotProduct(D, In)) < 300.f && FMath::Abs(FVector::DotProduct(D, Across)) < Mouths[M].Width * 0.5f + 100.f
				&& FMath::Abs(D.Z) < 400.f)
			{
				return M;
			}
		}
		return -1;
	}

	struct FRailJunctionEnd
	{
		UFGRailroadTrackConnectionComponent* Conn = nullptr;
		FVector Location = FVector::ZeroVector;
		FVector Dir = FVector::ForwardVector; // out of its track, into the intersection
		int32 Mouth = -1;
		float Across = 0.f; // sideways position at its intersection end, -1 (one side) .. 1 (other side)
	};

	/**
	 * Free track ends at the intersection's ends, each paired with the track end in the same lane on
	 * every other end. Lanes are counted in the travel direction: a train coming in on the left lane
	 * leaves on the left lane, the middle one on the middle, and so on.
	 */
	void FindRailJunctionLinks(UWorld* World, const ARMBuildableIntersection* X, TArray<TPair<FRailJunctionEnd, FRailJunctionEnd>>& OutLinks)
	{
		OutLinks.Reset();
		TArray<FRMRoadEnd> Mouths;
		X->GetWorldEnds(Mouths);
		TArray<FRailJunctionEnd> Ends;
		for (TActorIterator<AFGBuildableRailroadTrack> It(World); It; ++It)
		{
			for (int32 C = 0; C < 2; ++C)
			{
				UFGRailroadTrackConnectionComponent* Conn = It->GetConnection(C);
				if (!Conn || Conn->IsConnected())
				{
					continue;
				}
				const FVector Loc = Conn->GetConnectorLocation();
				const int32 Mouth = MouthIndexOf(Mouths, Loc);
				const FVector Dir = Conn->GetConnectorNormal();
				if (Mouth >= 0 && FVector::DotProduct(Dir.GetSafeNormal2D(), Mouths[Mouth].Inward.GetSafeNormal2D()) > 0.5f)
				{
					const FVector In = Mouths[Mouth].Inward.GetSafeNormal2D();
					const FVector Right = FVector::CrossProduct(FVector::UpVector, In);
					const float Across = FVector::DotProduct(Loc - Mouths[Mouth].Center, Right) / FMath::Max(Mouths[Mouth].Width * 0.5f, 1.f);
					Ends.Add({ Conn, Loc, Dir.GetSafeNormal(), Mouth, Across });
				}
			}
		}
		// Track ends per intersection end, sorted left to right as seen coming in.
		TArray<TArray<int32>> PerMouth;
		PerMouth.SetNum(Mouths.Num());
		for (int32 I = 0; I < Ends.Num(); ++I)
		{
			PerMouth[Ends[I].Mouth].Add(I);
		}
		for (TArray<int32>& List : PerMouth)
		{
			List.Sort([&Ends](int32 A, int32 B) { return Ends[A].Across < Ends[B].Across; });
		}
		// Coming in at end MA, right is +Across; leaving at end MB the travel direction is reversed,
		// so the same lane (counted from the train's left) is -Across there, i.e. counted from the other side.
		for (int32 MA = 0; MA < Mouths.Num(); ++MA)
		{
			for (int32 MB = MA + 1; MB < Mouths.Num(); ++MB)
			{
				const TArray<int32>& LA = PerMouth[MA];
				const TArray<int32>& LB = PerMouth[MB];
				if (LA.Num() == 0 || LB.Num() == 0)
				{
					continue;
				}
				TArray<TPair<int32, int32>> Pairs;
				if (LA.Num() == LB.Num())
				{
					// Same number of tracks: K-th from the left coming in = K-th from the left going out.
					for (int32 K = 0; K < LA.Num(); ++K)
					{
						Pairs.Add(TPair<int32, int32>(LA[K], LB[LB.Num() - 1 - K]));
					}
				}
				else
				{
					// Different counts: each track on the end with fewer tracks goes to the closest lane position.
					const bool bAFewer = LA.Num() < LB.Num();
					const TArray<int32>& Few = bAFewer ? LA : LB;
					const TArray<int32>& Many = bAFewer ? LB : LA;
					for (int32 F : Few)
					{
						int32 Best = Many[0];
						for (int32 M : Many)
						{
							if (FMath::Abs(Ends[M].Across + Ends[F].Across) < FMath::Abs(Ends[Best].Across + Ends[F].Across))
							{
								Best = M;
							}
						}
						Pairs.Add(bAFewer ? TPair<int32, int32>(F, Best) : TPair<int32, int32>(Best, F));
					}
				}
				for (const TPair<int32, int32>& P : Pairs)
				{
					OutLinks.Add(TPair<FRailJunctionEnd, FRailJunctionEnd>(Ends[P.Key], Ends[P.Value]));
					if (OutLinks.Num() >= 24)
					{
						return;
					}
				}
			}
		}
	}

	/** One rail connection through the intersection: a smooth curve from one track end to the other. */
	void SetupRailJunctionHologram(AFGRailroadTrackHologram* Rail, const FRailJunctionEnd& A, const FRailJunctionEnd& B)
	{
		GJunctionHolograms.Add(Rail);
		FPathState& State = GPathStates.FindOrAdd(Rail);
		State.bLaneMode = true; // skips the rail tool's post-placement re-route
		State.bStartStep = false;
		TArray<FRMSplinePoint> Curve;
		RMGeometry::MakeFixedEnds(A.Location, A.Dir, B.Location, -B.Dir, Curve);
		if (Curve.Num() < 2)
		{
			return;
		}
		SetupRailDirect(Rail, { Curve[0].Location, Curve[1].Location }, { Curve[0].Tangent, Curve[1].Tangent }, A.Conn, B.Conn);
	}

	/** Rail track + snap key aiming at an intersection: all connections between its track ends at once. */
	template <typename TScope, typename THologram>
	bool PlaceRailJunction(TScope& Scope, THologram* Hologram, const FHitResult& Hit, ARMBuildableIntersection* X)
	{
		AFGRailroadTrackHologram* Parent = Cast<AFGRailroadTrackHologram>(Hologram);
		UWorld* World = Hologram->GetWorld();
		if (!Parent || !World)
		{
			return false;
		}
		FJunctionCache& Cache = GJunctionCache.FindOrAdd(Parent);
		const double Now = FPlatformTime::Seconds();
		const FPathState* Existing = GPathStates.Find(Parent);
		if (Cache.Intersection.Get() == X && Existing && Existing->bLaneMode && Now - Cache.LastScan < 0.5)
		{
			Scope.Cancel();
			return true;
		}
		TArray<TPair<FRailJunctionEnd, FRailJunctionEnd>> Links;
		FindRailJunctionLinks(World, X, Links);
		if (Links.Num() == 0)
		{
			Cache = FJunctionCache();
			return false;
		}
		TArray<TPair<const void*, const void*>> Signature;
		for (const auto& L : Links)
		{
			Signature.Add(TPair<const void*, const void*>(L.Key.Conn, L.Value.Conn));
		}
		Scope.Cancel();
		Cache.LastScan = Now;
		if (Cache.Intersection.Get() == X && Cache.Signature == Signature && Existing && Existing->bLaneMode)
		{
			return true;
		}
		Cache.Intersection = X;
		Cache.Signature = Signature;

		SetupRailJunctionHologram(Parent, Links[0].Key, Links[0].Value);
		TArray<TWeakObjectPtr<AFGHologram>>& Children = GJunctionChildren.FindOrAdd(Parent);
		int32 Used = 0;
		for (int32 L = 1; L < Links.Num(); ++L)
		{
			const int32 ChildIndex = L - 1;
			if (!Children.IsValidIndex(ChildIndex) || !Children[ChildIndex].IsValid())
			{
				const FName Name(*FString::Printf(TEXT("RMRailJunction%d"), ChildIndex));
				AFGHologram* NewChild = AFGHologram::SpawnChildHologramFromRecipe(Parent, Name, Parent->GetRecipe(), Parent->GetOwner(), Links[L].Key.Location);
				if (!NewChild)
				{
					break;
				}
				if (Children.IsValidIndex(ChildIndex))
				{
					Children[ChildIndex] = NewChild;
				}
				else
				{
					Children.Add(NewChild);
				}
			}
			AFGRailroadTrackHologram* Child = Cast<AFGRailroadTrackHologram>(Children[ChildIndex].Get());
			if (!Child)
			{
				break;
			}
			Child->SetDisabled(false);
			Child->SetActorHiddenInGame(false);
			SetupRailJunctionHologram(Child, Links[L].Key, Links[L].Value);
			Used = ChildIndex + 1;
		}
		for (int32 C = Used; C < Children.Num(); ++C)
		{
			RemoveJunctionChild(Parent, Children[C]);
		}
		return true;
	}

	/** Foundations snapped to a road end: quarter turns set with the scroll wheel (per hologram). */
	TMap<TWeakObjectPtr<const AFGHologram>, int32> GFoundationQuarter;
	TSet<TWeakObjectPtr<const AFGHologram>> GFoundationSnapped;

	/**
	 * Foundations next to a free road end or intersection end: the foundation's edge sits on the end
	 * face, its top level with the road, in 4 m steps across the end (road ends are whole foundations
	 * wide, so it lines up with the road's own grid). The scroll wheel turns it in 90 degree steps.
	 * Ramps: turned so the high side touches the road its top is level with the road (the ramp goes
	 * down away from it); turned around, the low side is (it goes up). Inverted ramps have a flat top.
	 */
	template <typename TScope, typename THologram>
	bool PlaceFoundationAtRoadEnd(TScope& Scope, THologram* Hologram, const FHitResult& Hit)
	{
		const AFGBuildableFoundation* Foundation = GetDefault<AFGBuildableFoundation>(Hologram->GetBuildClass().Get());
		UWorld* World = Hologram->GetWorld();
		GFoundationSnapped.Remove(Hologram);
		if (!Foundation || !World || Foundation->mDepth < 100.f || Foundation->mWidth < 100.f)
		{
			return false;
		}
		const int32 Quarter = ((GFoundationQuarter.FindRef(Hologram) % 4) + 4) % 4;
		const bool bSideways = (Quarter % 2) == 1;
		const float AlongSize = bSideways ? Foundation->mWidth : Foundation->mDepth;
		const float AcrossSize = bSideways ? Foundation->mDepth : Foundation->mWidth;
		const bool bInverted = Hologram->GetBuildClass()->GetName().Contains(TEXT("Inverted"));
		// Top of the side touching the road, above the origin (which is half way up).
		float TopAtRoad = Foundation->mHeight * 0.5f;
		if (Quarter == 2 && !bInverted)
		{
			TopAtRoad -= Foundation->mElevation; // the ramp's low side faces the road
		}

		TArray<FRMRoadEndRef> Ends;
		RMRoadUtils::FindFreeEnds(World, Hit.Location, AlongSize + 1200.f, 4, nullptr, Ends);
		for (const FRMRoadEndRef& End : Ends)
		{
			const FVector Out = FVector(End.Outward.X, End.Outward.Y, 0.f).GetSafeNormal();
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Out);
			const FVector D = Hit.Location - End.Location;
			const float Along = FVector::DotProduct(D, Out);
			const float Across = FVector::DotProduct(D, Right);
			// Aiming at the ground (or a foundation) just beyond the end, or at the very end of the road.
			if (Along < -400.f || Along > AlongSize + 400.f
				|| FMath::Abs(Across) > End.Width * 0.5f + AcrossSize * 0.5f || FMath::Abs(D.Z) > 800.f)
			{
				continue;
			}
			const float Step = FMath::RoundToFloat(Across / 400.f) * 400.f;
			const FVector Location = End.Location + Right * Step + Out * (AlongSize * 0.5f) - FVector(0.f, 0.f, TopAtRoad);
			Hologram->SetActorLocationAndRotation(Location, FRotator(0.f, Out.Rotation().Yaw + 90.f * Quarter, 0.f));
			GFoundationSnapped.Add(Hologram);
			Scope.Cancel();
			return true;
		}
		return false;
	}

	/** Sign holograms currently placed by us on a fence wall (no leg, one click). */
	TSet<TWeakObjectPtr<const AFGHologram>> GSignsOnWall;

	/** Scroll steps (45 degrees each) a sign on a wall is turned in its plane. */
	TMap<TWeakObjectPtr<const AFGHologram>, int32> GSignRoll;

	/** Correction between "facing" and the sign model's own front axis. */
	constexpr float SignYawOffset = -90.f;

	void SetSignPoleDisabled(AFGHologram* Sign, bool bDisabled)
	{
		for (AFGHologram* Child : Sign->GetHologramChildren())
		{
			if (Child && Child->IsA<AFGSignPoleHologram>())
			{
				Child->SetDisabled(bDisabled);
			}
		}
	}

	bool IsSignOnWall(const AFGHologram* Hologram)
	{
		return GSignsOnWall.Contains(Hologram);
	}

	// ---- Debug log of the game's sign hologram (prefix "RMSignLog"): how it places, snaps, adjusts height ----
	TAutoConsoleVariable<int32> CVarSignLog(TEXT("rm.SignLog"), 1, TEXT("Log what the game's sign hologram does (RMSignLog lines)."));

	FString DescribeSign(const AFGStandaloneSignHologram* H)
	{
		if (!H)
		{
			return TEXT("null");
		}
		uint8 Step = 255;
		if (const FProperty* P = FindFProperty<FProperty>(AFGStandaloneSignHologram::StaticClass(), TEXT("mBuildStep")))
		{
			Step = *P->ContainerPtrToValuePtr<uint8>(H);
		}
		FString Out = FString::Printf(TEXT("sign %s step=%d loc=%s rot=%s hidden=%d wall=%d"), *H->GetName(), Step,
			*H->GetActorLocation().ToCompactString(), *H->GetActorRotation().ToCompactString(), H->IsHidden() ? 1 : 0, IsSignOnWall(H) ? 1 : 0);
		for (const AFGHologram* Child : H->GetHologramChildren())
		{
			if (const AFGSignPoleHologram* Pole = Cast<AFGSignPoleHologram>(Child))
			{
				Out += FString::Printf(TEXT(" | pole loc=%s rot=%s hidden=%d disabled=%d height=%.0f dirZ=%.1f"), *Pole->GetActorLocation().ToCompactString(),
					*Pole->GetActorRotation().ToCompactString(), Pole->IsHidden() ? 1 : 0, Pole->IsDisabled() ? 1 : 0, Pole->GetPoleHeight(),
					const_cast<AFGSignPoleHologram*>(Pole)->GetDirectionZ());
			}
			else if (Child)
			{
				Out += FString::Printf(TEXT(" | child %s"), *Child->GetClass()->GetName());
			}
		}
		return Out;
	}

	FString DescribeHit(const FHitResult& Hit)
	{
		return FString::Printf(TEXT("hit actor=%s (%s) loc=%s normal=%s trace=%s->%s blocking=%d"),
			Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("none"), Hit.GetActor() ? *Hit.GetActor()->GetClass()->GetName() : TEXT("-"),
			*Hit.ImpactPoint.ToCompactString(), *Hit.ImpactNormal.ToCompactString(), *Hit.TraceStart.ToCompactString(), *Hit.TraceEnd.ToCompactString(), Hit.bBlockingHit ? 1 : 0);
	}

	void SignLog(const TCHAR* Event, const AFGStandaloneSignHologram* H, const FHitResult* Hit = nullptr, const TCHAR* Extra = nullptr)
	{
		if (CVarSignLog.GetValueOnGameThread() == 0)
		{
			return;
		}
		UE_LOG(LogRoadMaker, Display, TEXT("RMSignLog %s: %s%s%s%s%s"), Event, *DescribeSign(H), Hit ? TEXT(" || ") : TEXT(""),
			Hit ? *DescribeHit(*Hit) : TEXT(""), Extra ? TEXT(" || ") : TEXT(""), Extra ? Extra : TEXT(""));
	}

	/** True if this hologram has not logged under this key for Interval seconds (keeps the log readable). */
	bool SignLogDue(const AFGHologram* Hologram, int32 Key, double Interval)
	{
		static TMap<uint64, double> LastLogged;
		double& Last = LastLogged.FindOrAdd(static_cast<uint64>(reinterpret_cast<UPTRINT>(Hologram)) * 8 + static_cast<uint64>(Key));
		const double Now = FPlatformTime::Seconds();
		if (Now - Last <= Interval)
		{
			return false;
		}
		Last = Now;
		return true;
	}

	/** Puts a sign on a surface: no leg, turned to face along Normal (and tilted with Slope), scroll rolls it. */
	template <typename TScope, typename THologram>
	void FinishSignPlacement(TScope& Scope, THologram* Hologram, const FVector& Location, const FVector& Normal, const FVector& Fwd, float Slope)
	{
		// Facing: turned with the curve, and tilted with the wall's slope. The model's X axis runs
		// along the wall (SignYawOffset), Y towards the viewer.
		const FQuat Yawed = FRotator(0.f, Normal.Rotation().Yaw + SignYawOffset, 0.f).Quaternion();
		FVector AxisX = Yawed.GetAxisX();
		const FVector AxisY = Yawed.GetAxisY();
		AxisX = (AxisX + FVector(0.f, 0.f, Slope * (FVector::DotProduct(AxisX, Fwd) >= 0.f ? 1.f : -1.f))).GetSafeNormal();
		const FQuat Base = FRotationMatrix::MakeFromXY(AxisX, AxisY).ToQuat();
		// Scroll turns it in the plane of the wall, 45 degrees per step.
		const int32 Steps = GSignRoll.FindRef(Hologram);
		const FQuat Final = FQuat(Normal, FMath::DegreesToRadians(45.f * Steps)) * Base;

		GSignsOnWall.Add(Hologram);
		SetSignPoleDisabled(Hologram, true);
		Hologram->SetActorLocationAndRotation(Location, Final);
		Scope.Cancel();
	}

	/**
	 * Signs on fence walls (snap key held): the game's sign hologram does not know our curved walls.
	 * Aimed at a wall, the sign is placed here: flat on the face you aim at, turned and tilted with the
	 * curve, free along the wall and snapped every 1 m in height (5 levels on a 4 m wall), turned in
	 * its plane with the scroll wheel (45 degrees), without its leg, built with one click.
	 */
	template <typename TScope, typename THologram>
	bool PlaceSignOnFenceWall(TScope& Scope, THologram* Hologram, const FHitResult& Hit)
	{
		// The sign gantry's beam: flat on its front or back face, at its bottom edge, middle or top edge.
		if (ARMBuildableGantry* Gantry = Cast<ARMBuildableGantry>(Hit.GetActor()))
		{
			FVector GantryLocation, GantryNormal;
			if (FRMSnapAccess::IsSnapHeld(Hologram) && Gantry->GetSignFace(Hit.Location, Hit.ImpactNormal, GantryLocation, GantryNormal))
			{
				FinishSignPlacement(Scope, Hologram, GantryLocation, GantryNormal, FVector::ZeroVector, 0.f);
				return true;
			}
			if (GSignsOnWall.Remove(Hologram) > 0)
			{
				SetSignPoleDisabled(Hologram, false);
			}
			return false;
		}
		ARMBuildableFence* Wall = Cast<ARMBuildableFence>(Hit.GetActor());
		TArray<FRMFrame> Frames;
		// Only with the snap key held: otherwise the sign places normally (with its leg), even when
		// you aim at a wall.
		if (Wall && Wall->IsWall() && FRMSnapAccess::IsSnapHeld(Hologram))
		{
			Wall->GetWorldFrames(Frames, 25.f);
		}
		if (Frames.Num() < 2)
		{
			if (GSignsOnWall.Remove(Hologram) > 0)
			{
				SetSignPoleDisabled(Hologram, false); // back to the game's own placement, with its leg
			}
			return false;
		}
		int32 Closest = 0;
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; I < Frames.Num(); ++I)
		{
			const float D = FVector::DistSquared2D(Frames[I].Pos, Hit.Location);
			if (D < Best)
			{
				Best = D;
				Closest = I;
			}
		}
		const FRMFrame& Near = Frames[Closest];
		const FVector Right = Near.Right.GetSafeNormal2D();
		const FVector Fwd = Near.Fwd.GetSafeNormal2D();
		const FVector Fwd3 = Near.Fwd.GetSafeNormal();
		// Side: the face you hit (its normal), else where the aim point lies.
		float Side = FVector::DotProduct(Hit.ImpactNormal, Right);
		if (FMath::Abs(Side) < 0.3f)
		{
			Side = FVector::DotProduct(Hit.Location - Near.Pos, Right);
		}
		Side = Side >= 0.f ? 1.f : -1.f;
		const FVector Normal = Right * Side;

		// Along the wall: where you aim, on the wall's centre line (it climbs with the wall).
		const float Along = FVector::DotProduct(Hit.Location - Near.Pos, Fwd);
		const FVector OnLine = Near.Pos + Fwd3 * Along;
		// Five heights, 1 m apart: the bottom and top edge of the wall and three on its face.
		const float Level = FMath::Clamp(FMath::RoundToFloat((Hit.Location.Z - OnLine.Z) / 100.f), 0.f, 4.f);
		const float HalfThickness = 25.f; // walls are 0.5 m thick
		FVector Location = OnLine + Normal * (HalfThickness + 1.f);
		Location.Z = OnLine.Z + 100.f * Level;

		FinishSignPlacement(Scope, Hologram, Location, Normal, Fwd, Fwd3.Z / FMath::Max(Fwd3.Size2D(), KINDA_SMALL_NUMBER));
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

		if (Kind == ESnapKind::Sign)
		{
			return PlaceSignOnFenceWall(Scope, Hologram, Hit);
		}

		if (Kind == ESnapKind::Foundation)
		{
			return PlaceFoundationAtRoadEnd(Scope, Hologram, Hit);
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
					ReleaseJunction(Hologram);
					if (PlaceLanePath(Scope, Hologram, Hit, Cast<ARMBuildableRoad>(Aimed)))
					{
						return true;
					}
				}
				else if (bAimIntersection && FRMSnapAccess::IsSnapHeld(Hologram) && (bLaneMode || IsStartStep(Hologram)))
				{
					if (PlaceJunction(Scope, Hologram, Hit, Cast<ARMBuildableIntersection>(Aimed)))
					{
						return true;
					}
					ReleaseJunction(Hologram);
					if (bLaneMode)
					{
						ReleasePathSnap(Hologram);
					}
				}
				else if (bLaneMode)
				{
					ReleaseJunction(Hologram);
					ReleasePathSnap(Hologram); // looked away from the road: back to a normal start
				}
			}
			else if (bEnableLaneTracks && Hologram->template IsA<AFGRailroadTrackHologram>())
			{
				FPathState* Lane = GPathStates.Find(Hologram);
				const bool bLaneMode = Lane && Lane->bLaneMode;
				if (bAimRoad && FRMSnapAccess::IsSnapHeld(Hologram) && (bLaneMode || IsStartStep(Hologram)))
				{
					ReleaseJunction(Hologram);
					if (PlaceLaneTrack(Scope, Hologram, Hit, Cast<ARMBuildableRoad>(Aimed)))
					{
						return true;
					}
				}
				else if (bAimIntersection && FRMSnapAccess::IsSnapHeld(Hologram) && (bLaneMode || IsStartStep(Hologram)))
				{
					if (PlaceRailJunction(Scope, Hologram, Hit, Cast<ARMBuildableIntersection>(Aimed)))
					{
						return true;
					}
					ReleaseJunction(Hologram);
					if (bLaneMode)
					{
						ReleasePathSnap(Hologram);
					}
				}
				else if (bLaneMode)
				{
					ReleaseJunction(Hologram);
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
	RM_HOOK_SET_LOCATION(AFGFoundationHologram);
	RM_HOOK_SET_LOCATION(AFGStandaloneSignHologram);

	// Foundations snapped to a road end turn 90 degrees per scroll step.
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildableHologram::ScrollRotate, GetMutableDefault<AFGFoundationHologram>(),
		[](auto& Scope, AFGBuildableHologram* Hologram, int32 Delta, int32 Step)
		{
			if (Delta != 0 && GFoundationSnapped.Contains(Hologram))
			{
				GFoundationQuarter.FindOrAdd(Hologram) += Delta > 0 ? 1 : -1;
				Scope.Cancel();
			}
		});

#undef RM_HOOK_SET_LOCATION

	// Debug log: how the game's own sign hologram works (aim at a wall, at the ground, two-click leg height...).
	SUBSCRIBE_METHOD_VIRTUAL(AFGStandaloneSignHologram::SetHologramLocationAndRotation, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](auto& Scope, AFGStandaloneSignHologram* Hologram, const FHitResult& Hit)
		{
			if (SignLogDue(Hologram, 1, 0.25))
			{
				SignLog(TEXT("SetLocation(before)"), Hologram, &Hit);
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::SetHologramLocationAndRotation, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](AFGStandaloneSignHologram* Hologram, const FHitResult& Hit)
		{
			if (SignLogDue(Hologram, 2, 0.25))
			{
				SignLog(TEXT("SetLocation(after)"), Hologram);
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::DoMultiStepPlacement, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](bool Result, AFGStandaloneSignHologram* Hologram, bool bIsInputFromARelease)
		{
			SignLog(TEXT("DoMultiStepPlacement(after)"), Hologram, nullptr, *FString::Printf(TEXT("result=%d fromRelease=%d"), Result ? 1 : 0, bIsInputFromARelease ? 1 : 0));
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::TrySnapToActor, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](bool Result, AFGStandaloneSignHologram* Hologram, const FHitResult& Hit)
		{
			SignLog(TEXT("TrySnapToActor(after)"), Hologram, &Hit, *FString::Printf(TEXT("result=%d"), Result ? 1 : 0));
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::IsValidHitResult, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](bool Result, const AFGStandaloneSignHologram* Hologram, const FHitResult& Hit)
		{
			static double Last = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - Last > 0.5)
			{
				Last = Now;
				SignLog(TEXT("IsValidHitResult(after)"), Hologram, &Hit, *FString::Printf(TEXT("result=%d"), Result ? 1 : 0));
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::PreHologramPlacement, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](AFGStandaloneSignHologram* Hologram, const FHitResult& Hit, bool bCallForChildren)
		{
			SignLog(TEXT("PreHologramPlacement(after)"), Hologram, &Hit);
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::PostHologramPlacement, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](AFGStandaloneSignHologram* Hologram, const FHitResult& Hit, bool bCallForChildren)
		{
			static double Last = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - Last > 0.5)
			{
				Last = Now;
				SignLog(TEXT("PostHologramPlacement(after)"), Hologram, &Hit);
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::Scroll, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](AFGStandaloneSignHologram* Hologram, int32 Delta)
		{
			SignLog(TEXT("Scroll(after)"), Hologram, nullptr, *FString::Printf(TEXT("delta=%d"), Delta));
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGStandaloneSignHologram::OnInvalidHitResult, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](AFGStandaloneSignHologram* Hologram)
		{
			static double Last = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - Last > 0.5)
			{
				Last = Now;
				SignLog(TEXT("OnInvalidHitResult(after)"), Hologram);
			}
		});

	// A sign on a fence wall has no leg: one click builds it, and it needs no floor.
	SUBSCRIBE_METHOD_VIRTUAL(AFGStandaloneSignHologram::DoMultiStepPlacement, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](auto& Scope, AFGStandaloneSignHologram* Hologram, bool bIsInputFromARelease)
		{
			if (IsSignOnWall(Hologram))
			{
				Scope.Override(true);
			}
		});
	// Scrolling a sign on a wall turns it in the plane of the wall, 45 degrees per step.
	SUBSCRIBE_METHOD_VIRTUAL(AFGStandaloneSignHologram::Scroll, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](auto& Scope, AFGStandaloneSignHologram* Hologram, int32 Delta)
		{
			if (Delta != 0 && IsSignOnWall(Hologram))
			{
				GSignRoll.FindOrAdd(Hologram) += Delta > 0 ? 1 : -1;
				Scope.Cancel();
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildableHologram::ScrollRotate, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](auto& Scope, AFGBuildableHologram* Hologram, int32 Delta, int32 Step)
		{
			if (Delta != 0 && IsSignOnWall(Hologram))
			{
				GSignRoll.FindOrAdd(Hologram) += Delta > 0 ? 1 : -1;
				Scope.Cancel();
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL(AFGStandaloneSignHologram::CheckValidFloor, GetMutableDefault<AFGStandaloneSignHologram>(),
		[](auto& Scope, AFGStandaloneSignHologram* Hologram)
		{
			if (IsSignOnWall(Hologram))
			{
				Scope.Cancel();
			}
		});

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
