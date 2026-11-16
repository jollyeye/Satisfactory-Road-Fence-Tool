#include "Templates/RMRoadTemplate.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/RMBuildableMarking.h"
#include "Buildables/RMBuildableBlueprintRoad.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "Patching/NativeHookManager.h"
#include "RMGeometry.h"
#include "ProceduralMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "FGFactoryBlueprintTypes.h"
#include "FGRecipe.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGGameState.h"
#include "FGCentralStorageSubsystem.h"
#include "Algo/Reverse.h"
#include "Components/SplineComponent.h"
#include "FGSplineBuildableInterface.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "FGCircuitConnectionComponent.h"
#include "Buildables/FGBuildableWire.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "FGBlueprintProxy.h"
#include "FGBlueprintSubsystem.h"
#include "FGDismantleInterface.h"
#include "Components/BoxComponent.h"
#include "Kismet/GameplayStatics.h"

namespace RMRoadTemplate
{
	namespace
	{
		// Road Maker pieces each blueprint hologram shows (they live in the blueprint's own world).
		TMap<TWeakObjectPtr<AFGBlueprintHologram>, TArray<TWeakObjectPtr<ARMBuildableProcedural>>> GPieces;

		UClass* RoadModeClass()
		{
			static TWeakObjectPtr<UClass> Cached;
			if (!Cached.IsValid())
			{
				Cached = LoadClass<UFGHologramBuildModeDescriptor>(nullptr, TEXT("/RoadMaker/Holograms/BM_RoadMode.BM_RoadMode_C"));
				// Kept loaded for good: the build gun remembers the last mode per hologram in a map the garbage
				// collector does not see, and an unloaded mode there crashes the next time the build gun comes out.
				if (UClass* Mode = Cached.Get())
				{
					Mode->AddToRoot();
				}
			}
			return Cached.Get();
		}

		/** Signed distance of P from the road's centre line (+ = the road's right) and height above it. */
		bool Measure(const TArray<FRMFrame>& Frames, const FVector& P, float& OutOffset, float& OutLift)
		{
			int32 Best = INDEX_NONE;
			float BestD = TNumericLimits<float>::Max();
			for (int32 I = 0; I < Frames.Num(); ++I)
			{
				const float D = FVector::DistSquared2D(Frames[I].Pos, P);
				if (D < BestD)
				{
					BestD = D;
					Best = I;
				}
			}
			if (Best == INDEX_NONE)
			{
				return false;
			}
			const FRMFrame& F = Frames[Best];
			// Beyond the road's ends: not along it.
			const float Along = FVector::DotProduct(P - F.Pos, F.Fwd);
			if ((Best == 0 && Along < -50.f) || (Best == Frames.Num() - 1 && Along > 50.f))
			{
				return false;
			}
			OutOffset = FVector::DotProduct(P - F.Pos, F.Right);
			OutLift = P.Z - F.Pos.Z;
			return true;
		}
	}


	// ---------------------------------------------------------------------------------------------
	// Road Mode, step 2: two clicks like a road. The first click fixes the start (the blueprint's back
	// edge, its arrow = the road's direction); then the blueprint is laid along a curve to the aim:
	// the pieces running its whole length (our road, fences, walls, paths) bent along the curve and
	// stretched to fit, everything else repeated tile after tile, each object at its own distance from
	// the centre line (so the outer side of a curve spreads them more than the inner side).
	// ---------------------------------------------------------------------------------------------
	namespace RMRoadMode
	{
		struct FBent
		{
			TWeakObjectPtr<UProceduralMeshComponent> Src;
			FTransform Rel; // source mesh, relative to the hologram at the first click
			TArray<TWeakObjectPtr<UProceduralMeshComponent>> Tiles;
			int32 Stretch = INDEX_NONE; // its piece in FState::Stretches
		};

		struct FRigid
		{
			TWeakObjectPtr<USceneComponent> Src;
			FTransform Rel; // source root, relative to the hologram at the first click
			TArray<TWeakObjectPtr<USceneComponent>> Tiles;
			TWeakObjectPtr<AFGBuildable> Buildable; // in the blueprint's own world (for its cost)
			bool bBentPreview = false; // a marking along the arrow: previewed bent (FBent), built rigid with its drape bent
			int32 MarkStretch = INDEX_NONE; // its preview entry in FState::Stretches (bent markings)
			bool bHasParent = false;  // a marking on a road that is repeated as it is: it goes where that road goes
			FTransform ParentRel;     // that road, in road space
			bool bLevel = true;       // kept level (buildings, machines, poles); splines and our road pieces tilt with the curve
			bool bOnWall = false;     // a sign / wall attachment on a fence wall along the arrow (bLevel set by the wall rules)
			bool bSplineBend = false; // a belt / pipe / rail / hypertube running straight along the arrow: bent with the curve
			float SplineLen = 0.f;    // its length in the blueprint
			TArray<FSplinePointData> SplinePoints; // its points (its actor's space)
			bool bSplineFull = false; // ...and it runs the blueprint's whole length with both ends free: rebuilt over the whole curve
			bool bChainPower = false; // a power pole / single wall outlet with no wire in the blueprint: wired to its copy on the next tile
			/** Preview: per tile, the copies of its spline meshes and their sources. */
			TArray<TArray<TPair<TWeakObjectPtr<USplineMeshComponent>, TWeakObjectPtr<USplineMeshComponent>>>> SplineCopies;
		};

		/** One of our pieces running straight along the arrow: built along the drawn curve at its own offset (whole length or per tile). */
		struct FStretch
		{
			TWeakObjectPtr<ARMBuildableSpline> Piece;
			float Y = 0.f;      // across, from the blueprint's centre line (road space)
			float Z = 0.f;      // up, from the blueprint's floor
			float Length = 0.f; // its length in the blueprint
			bool bReversed = false; // runs against the arrow
			bool bFull = false;     // runs the blueprint's whole length: stretched; otherwise repeated per tile
			float XMin = 0.f, XMax = 0.f; // where it runs along the blueprint, from its back edge
			FVector RootLoc = FVector::ZeroVector; // its actor in road space (to find it among a placed blueprint's pieces)
			bool bRigidCount = false; // a marking (no piece of its own): as many as the repeated objects' whole tiles
		};

		struct FState
		{
			int32 Step = 0;
			FTransform StartT; // the hologram at the first click
			FTransform RoadT;  // the same, turned so X = the blueprint's arrow (the road space everything is measured in)
			float X0 = 0.f, L = 0.f, Yc = 0.f, Zb = 0.f; // the blueprint box, hologram-local
			TArray<TPair<TWeakObjectPtr<USceneComponent>, bool>> Hidden;
			TArray<FBent> Bents;
			TArray<FRigid> Rigids;
			TArray<FStretch> Stretches;
			float RigidMaxX = -1.f; // furthest repeated object from the back edge (-1: none)
			/** Where the game's loader puts a piece vs where the hologram shows it (road space): the hologram centres
			 *  its preview on the blueprint's bounds, the loader does not. Added to what the loader spawns. */
			FVector LoaderToPreview = FVector::ZeroVector;
			/** Second click done: the game's construct goes ahead (cost 0 for the game), Road Mode builds in Construct. */
			bool bFinal = false;
			/** The end's direction, turned with the mouse wheel from the start-to-end line (degrees), and the wheel then. */
			float EndYaw = 0.f;
			int32 ScrollAtStart = 0;
			float LastYaw = TNumericLimits<float>::Max();
			/** The end snapped on a Blueprint Road end: the direction it arrives in (zero = not snapped). */
			FVector EndSnapDir = FVector::ZeroVector;
			FVector LastSnapDir = FVector(1e9f);
			float HalfW = 1600.f, Height = 400.f; // the blueprint's half width and height
			TArray<FRMFrame> Frames; // the drawn curve (world), full length
			/** The Blueprint Road being drawn: a wireframe along the curve, as wide as the blueprint. */
			TWeakObjectPtr<UProceduralMeshComponent> GhostPreview;
			FVector LastEnd = FVector(TNumericLimits<float>::Max());
		};

		TMap<TWeakObjectPtr<AFGBlueprintHologram>, FState> GStates;

		/** While Road Mode places a tile: where each spawned piece goes. The game's loader finishes spawning every piece at
		 *  its blueprint place (which also draws it there); the hook on AActor::FinishSpawning swaps in these. */
		TMap<AActor*, FTransform> GPendingMoves;

		/** On the server: Road Mode placements received in a construct message (start transform, end point), per hologram. */
		/** A placement received from a client: its start, its end and how far the end direction was turned. */
		struct FRemote
		{
			FTransform StartT;
			FVector End = FVector::ZeroVector;
			float EndYaw = 0.f;
			FVector EndSnapDir = FVector::ZeroVector;
		};
		TMap<TWeakObjectPtr<AFGBlueprintHologram>, FRemote> GRemote;

		/** Spline pieces to join to the piece before them just before they begin play: belts get their connection
		 *  components from their Blueprint, which only exist once construction ran (inside FinishSpawning). */
		TMap<AActor*, TWeakObjectPtr<AActor>> GPendingJoins;

		/** Working out a received placement on the server: no preview components. */
		bool GNoPreview = false;

		/** Moves a placed building. Its components are static (buildings never move), which makes a plain move do
		 *  nothing once they are registered: made movable for the move, static again after. */
		void MoveBuilding(AActor* Actor, const FTransform& T)
		{
			TArray<USceneComponent*> Statics;
			TInlineComponentArray<USceneComponent*> Comps(Actor);
			for (USceneComponent* C : Comps)
			{
				if (C->Mobility == EComponentMobility::Static)
				{
					C->SetMobility(EComponentMobility::Movable);
					Statics.Add(C);
				}
			}
			Actor->SetActorTransform(T);
			for (USceneComponent* C : Statics)
			{
				C->SetMobility(EComponentMobility::Static);
			}
		}

		constexpr int32 MaxTiles = 48;

		FRMFrame FrameAt(const TArray<FRMFrame>& Frames, float D)
		{
			const FRMFrame& First = Frames[0];
			const FRMFrame& Last = Frames.Last();
			if (D <= 0.f)
			{
				FRMFrame F = First;
				F.Pos += F.Fwd * D;
				return F;
			}
			if (D >= Last.Dist)
			{
				FRMFrame F = Last;
				F.Pos += F.Fwd * (D - Last.Dist);
				return F;
			}
			int32 Lo = 0, Hi = Frames.Num() - 1;
			while (Hi - Lo > 1)
			{
				const int32 Mid = (Lo + Hi) / 2;
				(Frames[Mid].Dist <= D ? Lo : Hi) = Mid;
			}
			const FRMFrame& A = Frames[Lo];
			const FRMFrame& B = Frames[Hi];
			const float T = B.Dist > A.Dist ? (D - A.Dist) / (B.Dist - A.Dist) : 0.f;
			FRMFrame F;
			F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
			F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
			F.Right = FMath::Lerp(A.Right, B.Right, T).GetSafeNormal();
			F.Up = FMath::Lerp(A.Up, B.Up, T).GetSafeNormal();
			F.Dist = D;
			return F;
		}

		/** Objects that tilt with the curve: splines (belts, pipes, rails...) and our road pieces and markings. Everything
		 *  else (foundations, machines, poles...) stays level. */
		bool TiltsWithCurve(const AActor* Actor)
		{
			return Actor && (Actor->IsA<ARMBuildableSpline>() || Actor->IsA<ARMBuildableMarking>()
				|| Actor->FindComponentByClass<USplineComponent>() != nullptr);
		}

		/**
		 * The spline points of a game spline (belt, pipe, rail, hypertube through the game's spline interface; vehicle
		 * paths through their own saved points), in its actor's space. Writable before it begins play. Null otherwise.
		 */
		TArray<FSplinePointData>* SplinePointsOf(AActor* Actor)
		{
			if (IFGSplineBuildableInterface* Spline = Cast<IFGSplineBuildableInterface>(Actor))
			{
				if (TArray<FSplinePointData>* Pts = Spline->GetMutableSplinePointData())
				{
					return Pts;
				}
			}
			if (!Actor)
			{
				return nullptr;
			}
			for (const TCHAR* Name : { TEXT("mSplinePoints"), TEXT("mSplineData") })
			{
				FArrayProperty* Prop = FindFProperty<FArrayProperty>(Actor->GetClass(), Name);
				FStructProperty* Inner = Prop ? CastField<FStructProperty>(Prop->Inner) : nullptr;
				if (Inner && Inner->Struct == FSplinePointData::StaticStruct())
				{
					return Prop->ContainerPtrToValuePtr<TArray<FSplinePointData>>(Actor);
				}
			}
			return nullptr;
		}

		/** Whether a spline end is free in the blueprint (nothing connected to it). */
		bool EndFree(UFGConnectionComponent* C)
		{
			if (UFGFactoryConnectionComponent* F = Cast<UFGFactoryConnectionComponent>(C))
			{
				return !F->IsConnected();
			}
			if (UFGPipeConnectionComponentBase* P = Cast<UFGPipeConnectionComponentBase>(C))
			{
				return !P->IsConnected();
			}
			if (UFGRailroadTrackConnectionComponent* R = Cast<UFGRailroadTrackConnectionComponent>(C))
			{
				return R->GetConnection() == nullptr;
			}
			return true;
		}

		/** Both ends of a game spline free in the blueprint (vehicle paths: their nodes lead nowhere else). */
		bool SplineEndsFree(AActor* Actor)
		{
			if (const AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(Actor))
			{
				const AFGVehiclePathNode* A = Path->GetStartNode();
				const AFGVehiclePathNode* B = Path->GetEndNode();
				return (!A || A->GetArrivingConnections().Num() + A->GetLeavingConnections().Num() <= 1)
					&& (!B || B->GetArrivingConnections().Num() + B->GetLeavingConnections().Num() <= 1);
			}
			if (IFGSplineBuildableInterface* Spline = Cast<IFGSplineBuildableInterface>(Actor))
			{
				return EndFree(Spline->GetSplineConnection0()) && EndFree(Spline->GetSplineConnection1());
			}
			return false;
		}

		void CopyLook(const UPrimitiveComponent* From, UPrimitiveComponent* To)
		{
			for (int32 I = 0; I < From->GetNumMaterials(); ++I)
			{
				To->SetMaterial(I, From->GetMaterial(I));
			}
			const TArray<float>& Data = From->GetCustomPrimitiveData().Data;
			for (int32 I = 0; I < Data.Num(); ++I)
			{
				To->SetCustomPrimitiveDataFloat(I, Data[I]);
			}
			To->SetRenderCustomDepth(From->bRenderCustomDepth);
			To->SetCustomDepthStencilValue(From->CustomDepthStencilValue);
			To->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			To->SetCastShadow(false);
		}

		/** A copy of one visible mesh component (static, instanced, spline or procedural), or null. */
		UPrimitiveComponent* CopyMesh(AActor* Owner, UPrimitiveComponent* Src)
		{
			UPrimitiveComponent* Out = nullptr;
			if (UProceduralMeshComponent* Proc = Cast<UProceduralMeshComponent>(Src))
			{
				UProceduralMeshComponent* Copy = NewObject<UProceduralMeshComponent>(Owner);
				for (int32 I = 0; I < Proc->GetNumSections(); ++I)
				{
					if (FProcMeshSection* Section = Proc->GetProcMeshSection(I))
					{
						Copy->SetProcMeshSection(I, *Section);
					}
				}
				Out = Copy;
			}
			else if (UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Src))
			{
				UInstancedStaticMeshComponent* Copy = NewObject<UInstancedStaticMeshComponent>(Owner);
				Copy->SetStaticMesh(Ism->GetStaticMesh());
				for (int32 I = 0; I < Ism->GetInstanceCount(); ++I)
				{
					FTransform T;
					if (Ism->GetInstanceTransform(I, T, false))
					{
						Copy->AddInstance(T, false);
					}
				}
				Out = Copy;
			}
			else if (USplineMeshComponent* SplineMesh = Cast<USplineMeshComponent>(Src))
			{
				USplineMeshComponent* Copy = NewObject<USplineMeshComponent>(Owner);
				Copy->SetStaticMesh(SplineMesh->GetStaticMesh());
				Copy->SetForwardAxis(SplineMesh->ForwardAxis, false);
				Copy->SplineParams = SplineMesh->SplineParams;
				Copy->SplineUpDir = SplineMesh->SplineUpDir;
				Out = Copy;
			}
			else if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Src))
			{
				UStaticMeshComponent* Copy = NewObject<UStaticMeshComponent>(Owner);
				Copy->SetStaticMesh(Static->GetStaticMesh());
				Out = Copy;
			}
			if (Out)
			{
				CopyLook(Src, Out);
			}
			return Out;
		}

		bool IsRoadMode(const AFGBlueprintHologram* Self);

		void Stop(AFGBlueprintHologram* Self)
		{
			FState* S = GStates.Find(Self);
			if (!S)
			{
				return;
			}
			for (FBent& B : S->Bents)
			{
				for (TWeakObjectPtr<UProceduralMeshComponent>& C : B.Tiles)
				{
					if (C.IsValid()) { C->DestroyComponent(); }
				}
			}
			for (FRigid& R : S->Rigids)
			{
				for (TWeakObjectPtr<USceneComponent>& C : R.Tiles)
				{
					if (USceneComponent* TileRoot = C.Get())
					{
						TArray<USceneComponent*> Kids;
						TileRoot->GetChildrenComponents(true, Kids);
						for (USceneComponent* Kid : Kids)
						{
							Kid->DestroyComponent();
						}
						TileRoot->DestroyComponent();
					}
				}
			}
			if (UProceduralMeshComponent* Ghost = S->GhostPreview.Get())
		{
			Ghost->DestroyComponent();
		}
		for (TPair<TWeakObjectPtr<USceneComponent>, bool>& H : S->Hidden)
			{
				if (H.Key.IsValid()) { H.Key->SetVisibility(H.Value, false); }
			}
			if (S->Step == 1 && Self)
			{
				Self->SetActorTransform(S->StartT);
			}
			GStates.Remove(Self);
		}

		/** The blueprint's box in hologram space: back edge X0, length L along the arrow, centre Yc, floor Zb. */
		void MeasureBox(AFGBlueprintHologram* Self, FState& S, bool bLog = true)
		{
			const FTransform ActorT = Self->GetActorTransform();
			FBox Box(ForceInit);
			static FObjectProperty* BoundsProp = FindFProperty<FObjectProperty>(AFGBlueprintHologram::StaticClass(), TEXT("mBlueprintBoundsMesh"));
			if (BoundsProp)
			{
				if (UStaticMeshComponent* BoundsMesh = Cast<UStaticMeshComponent>(BoundsProp->GetObjectPropertyValue_InContainer(Self)))
				{
					Box = BoundsMesh->CalcBounds(BoundsMesh->GetComponentTransform().GetRelativeTransform(ActorT)).GetBox();
				}
			}
			const FIntVector Dim = Self->mBlueprintDescriptor ? Self->mBlueprintDescriptor->mDimensions : FIntVector::ZeroValue;
			const float DescL = Dim.X * RM::FoundationSize;
			if (Dim.X > 0 && Dim.Y > 0)
			{
				// The designer's size from the descriptor, centred on the hologram, floor at the hologram (the bounds
				// mesh is a 1 m cube scaled by its material, so its bounds say nothing).
				const float HalfX = Dim.X * RM::FoundationSize * 0.5f;
				const float HalfY = Dim.Y * RM::FoundationSize * 0.5f;
				Box = FBox(FVector(-HalfX, -HalfY, 0.f), FVector(HalfX, HalfY, FMath::Max(Dim.Z, 1) * 400.f));
			}
			else if (!Box.IsValid || FMath::Min(Box.GetSize().X, Box.GetSize().Y) < 400.f)
			{
				Box = FBox(FVector(-1600.f, -1600.f, 0.f), FVector(1600.f, 1600.f, 400.f));
			}
			// The blueprint's arrow: the direction mesh sits at the box's front edge. Snapped to the box's sides.
			FVector Arrow = FVector::YAxisVector;
			static FObjectProperty* DirProp = FindFProperty<FObjectProperty>(AFGBlueprintHologram::StaticClass(), TEXT("mBlueprintDirectionMesh"));
			USceneComponent* DirMesh = DirProp ? Cast<USceneComponent>(DirProp->GetObjectPropertyValue_InContainer(Self)) : nullptr;
			FString ArrowFrom = TEXT("default");
			if (DirMesh)
			{
				const FTransform DirT = DirMesh->GetComponentTransform().GetRelativeTransform(ActorT);
				const FVector Off = DirT.GetLocation() - Box.GetCenter();
				FVector Pick = FVector(Off.X, Off.Y, 0.f);
				ArrowFrom = TEXT("arrow position");
				if (Pick.Size() < 100.f)
				{
					Pick = DirT.GetUnitAxis(EAxis::X);
					Pick.Z = 0.f;
					ArrowFrom = TEXT("arrow rotation");
				}
				if (Pick.Size() > 0.01f)
				{
					Arrow = FMath::Abs(Pick.X) >= FMath::Abs(Pick.Y) ? FVector(FMath::Sign(Pick.X), 0.f, 0.f) : FVector(0.f, FMath::Sign(Pick.Y), 0.f);
				}
				UE_CLOG(bLog, LogRoadMaker, Display, TEXT("Road Mode: arrow mesh at %s (box centre %s), rotation %s"),
					*DirT.GetLocation().ToString(), *Box.GetCenter().ToString(), *DirT.Rotator().ToString());
			}
			const FTransform Turn(FRotationMatrix::MakeFromXZ(Arrow, FVector::UpVector).ToQuat());
			S.RoadT = Turn * ActorT;
			Box = Box.TransformBy(Turn.Inverse());
			UE_CLOG(bLog, LogRoadMaker, Display, TEXT("Road Mode: forward = hologram %s (from the %s)"), *Arrow.ToString(), *ArrowFrom);
			S.X0 = Box.Min.X;
			S.L = Box.GetSize().X;
			S.Yc = Box.GetCenter().Y;
			S.HalfW = Box.GetExtent().Y;
			S.Height = Box.GetSize().Z;
			S.Zb = Box.Min.Z;
			UE_CLOG(bLog, LogRoadMaker, Display, TEXT("Road Mode: blueprint box X %.0f..%.0f, Y %.0f..%.0f, floor %.0f (descriptor %dx%dx%d = %.0f cm long)"),
				Box.Min.X, Box.Max.X, Box.Min.Y, Box.Max.Y, Box.Min.Z, Dim.X, Dim.Y, Dim.Z, DescL);
		}

		void Begin(AFGBlueprintHologram* Self)
		{
			// Forget holograms that are gone.
			for (auto It = GStates.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid()) { It.RemoveCurrent(); }
			}
			Stop(Self);
			FState& S = GStates.Add(Self);
			S.Step = 1;
			S.StartT = Self->GetActorTransform();
			S.ScrollAtStart = Self->GetScrollRotateValue();
			MeasureBox(Self, S);

			int32 Stretched = 0, Repeated = 0;
			{
				// The loader places a blueprint-world piece at (its transform there) x (the origin); the hologram shows it at its root.
				FVector Sum = FVector::ZeroVector;
				FVector Min(TNumericLimits<float>::Max()), Max(-TNumericLimits<float>::Max());
				int32 Count = 0;
				for (const TPair<TObjectPtr<AFGBuildable>, TObjectPtr<USceneComponent>>& Pair : Self->mBuildableToNewRoot)
				{
					if (Pair.Key && Pair.Value)
					{
						const FVector Shown = S.RoadT.InverseTransformPosition(Pair.Value->GetComponentLocation());
						const FVector Loaded = S.RoadT.InverseTransformPosition(S.StartT.TransformPosition(Pair.Key->GetActorLocation()));
						const FVector D = Shown - Loaded;
						Sum += D;
						Min = Min.ComponentMin(D);
						Max = Max.ComponentMax(D);
						++Count;
					}
				}
				if (Count > 0)
				{
					S.LoaderToPreview = Sum / Count;
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: loader to preview offset %s (spread %s)"), *S.LoaderToPreview.ToString(), *(Max - Min).ToString());
				}
			}
			for (const TPair<TObjectPtr<AFGBuildable>, TObjectPtr<USceneComponent>>& Pair : Self->mBuildableToNewRoot)
			{
				USceneComponent* Root = Pair.Value;
				if (!Root)
				{
					continue;
				}
				TArray<USceneComponent*> Children;
				Root->GetChildrenComponents(true, Children);

				// Our roads, paths and fences running straight along the arrow bend with the curve: stretched over it
				// when they run the blueprint's whole length, repeated tile after tile (and bent) when shorter.
				int32 StretchIndex = INDEX_NONE;
				ARMBuildableSpline* Piece = Cast<ARMBuildableSpline>(Pair.Key.Get());
				if (Piece && (Piece->IsA<ARMBuildableRoad>() || Piece->IsA<ARMBuildableFence>()))
				{
					// Its course in road space: the root stands for the piece's actor.
					const FTransform RootRel = Root->GetComponentTransform().GetRelativeTransform(S.RoadT);
					const FTransform PieceT = Piece->GetActorTransform();
					TArray<FRMFrame> Own;
					Piece->GetWorldFrames(Own, 100.f);
					FStretch St;
					St.Piece = Piece;
					St.RootLoc = RootRel.GetLocation();
					St.Length = Own.Num() > 1 ? Own.Last().Dist : 0.f;
					float FwdX = 0.f, MinAlign = 1.f, MinY = TNumericLimits<float>::Max(), MaxY = -TNumericLimits<float>::Max();
					St.XMin = TNumericLimits<float>::Max();
					St.XMax = -TNumericLimits<float>::Max();
					for (const FRMFrame& F : Own)
					{
						const FVector P = RootRel.TransformPosition(PieceT.InverseTransformPosition(F.Pos));
						const float Fx = RootRel.TransformVectorNoScale(PieceT.InverseTransformVectorNoScale(F.Fwd)).X;
						St.Y += P.Y - S.Yc;
						St.Z += P.Z - S.Zb;
						FwdX += Fx;
						MinAlign = FMath::Min(MinAlign, FMath::Abs(Fx));
						MinY = FMath::Min(MinY, P.Y);
						MaxY = FMath::Max(MaxY, P.Y);
						St.XMin = FMath::Min(St.XMin, P.X - S.X0);
						St.XMax = FMath::Max(St.XMax, P.X - S.X0);
					}
					if (Own.Num() > 1)
					{
						St.Y /= Own.Num();
						St.Z /= Own.Num();
						St.bReversed = FwdX < 0.f;
						const bool bAlong = MinAlign > 0.98f && MaxY - MinY <= 50.f;
						St.bFull = bAlong && St.XMin <= 100.f && St.XMax >= S.L - 100.f;
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s %.0f cm across, %.0f cm up, X %.0f..%.0f of %.0f%s: %s"), *Piece->GetName(),
							St.Y, St.Z, St.XMin, St.XMax, S.L, St.bReversed ? TEXT(", runs against the arrow") : TEXT(""),
							!bAlong ? TEXT("not along the arrow, repeated as it is") : St.bFull ? TEXT("stretched") : TEXT("repeated, bent with the curve"));
						if (bAlong)
						{
							StretchIndex = S.Stretches.Add(St);
						}
					}
				}
				if (StretchIndex != INDEX_NONE)
				{
					for (USceneComponent* C : Children)
					{
						UProceduralMeshComponent* Proc = Cast<UProceduralMeshComponent>(C);
						if (Proc && Proc->IsVisible() && Proc->GetNumSections() > 0)
						{
							FBent& B = S.Bents.AddDefaulted_GetRef();
							B.Src = Proc;
							B.Rel = Proc->GetComponentTransform().GetRelativeTransform(S.RoadT);
							B.Stretch = StretchIndex;
						}
					}
					++Stretched;
				}
				else
				{
					FRigid& R = S.Rigids.AddDefaulted_GetRef();
					R.Src = Root;
					R.Rel = Root->GetComponentTransform().GetRelativeTransform(S.RoadT);
					R.Buildable = Pair.Key.Get();
					R.bLevel = !TiltsWithCurve(Pair.Key.Get());
					// A power pole or single wall outlet without any wire in the blueprint: wired to its copy on the next tile.
					if (const AFGBuildablePowerPole* Pole = Cast<AFGBuildablePowerPole>(Pair.Key.Get()))
					{
						if (!Pole->GetClass()->GetName().Contains(TEXT("Double")))
						{
							int32 Wires = 0;
							TInlineComponentArray<UFGCircuitConnectionComponent*> Circuits(Pole);
							for (const UFGCircuitConnectionComponent* C : Circuits)
							{
								Wires += C->GetNumConnections();
							}
							R.bChainPower = Wires == 0 && Circuits.Num() > 0;
							UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: power %s, %d connections in the blueprint: %s"), *Pole->GetName(), Wires,
								R.bChainPower ? TEXT("wired to the next tile") : TEXT("left as it is"));
						}
					}
					// A game spline (belt, pipe, rail, hypertube) running straight along the arrow: bent with the curve.
					if (const TArray<FSplinePointData>* PtsPtr = SplinePointsOf(Pair.Key.Get()))
					{
						const TArray<FSplinePointData>& Pts = *PtsPtr;
						if (Pts.Num() >= 2)
						{
							// Straight along the arrow: its ends' line runs along X (it may climb or shift a little, like a
							// hypertube onto its entrance), every point on that line, tangents along it.
							const FVector A = R.Rel.TransformPosition(Pts[0].Location);
							const FVector B = R.Rel.TransformPosition(Pts.Last().Location);
							const FVector Dir = (B - A).GetSafeNormal();
							bool bStraight = FMath::Abs(B.X - A.X) > 50.f && FMath::Abs(Dir.X) > 0.98f;
							for (const FSplinePointData& Pt : Pts)
							{
								const FVector P = R.Rel.TransformPosition(Pt.Location);
								const FVector T1 = R.Rel.TransformVector(Pt.ArriveTangent).GetSafeNormal();
								const FVector T2 = R.Rel.TransformVector(Pt.LeaveTangent).GetSafeNormal();
								const FVector OffLine = (P - A) - Dir * FVector::DotProduct(P - A, Dir);
								if (OffLine.Size() > 30.f
									|| (!T1.IsNearlyZero() && FMath::Abs(T1.X) < 0.95f) || (!T2.IsNearlyZero() && FMath::Abs(T2.X) < 0.95f))
								{
									bStraight = false;
								}
							}
							if (bStraight)
							{
								R.bSplineBend = true;
								R.SplineLen = FVector::Dist(A, B);
								R.SplinePoints = Pts;
								// The whole blueprint length, both ends free: rebuilt over the whole curve, not tiled.
								R.bSplineFull = FMath::Min(A.X, B.X) <= S.X0 + 100.f && FMath::Max(A.X, B.X) >= S.X0 + S.L - 100.f
									&& SplineEndsFree(Pair.Key.Get());
							}
							UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: spline %s, %d points, %.0f cm: %s"), *Pair.Key->GetName(), Pts.Num(),
								FVector::Dist(A, B), !bStraight ? TEXT("repeated as it is") : R.bSplineFull ? TEXT("rebuilt over the whole curve")
								: TEXT("straight along the arrow, bent with the curve per tile"));
						}
					}
					S.RigidMaxX = FMath::Max(S.RigidMaxX, R.Rel.GetLocation().X - S.X0);
					// A marking lying along the arrow bends with the curve like the roads (shown bent; built in place
					// with the road surface under it bent).
					if (Cast<ARMBuildableMarking>(Pair.Key.Get()) && FMath::Abs(R.Rel.GetUnitAxis(EAxis::X).X) > 0.98f)
					{
						R.bBentPreview = true;
						FStretch Mark;
						Mark.bRigidCount = true;
						const int32 MarkIndex = S.Stretches.Add(Mark);
						R.MarkStretch = MarkIndex;
						for (USceneComponent* C : Children)
						{
							UProceduralMeshComponent* Proc = Cast<UProceduralMeshComponent>(C);
							if (Proc && Proc->IsVisible() && Proc->GetNumSections() > 0)
							{
								FBent& B = S.Bents.AddDefaulted_GetRef();
								B.Src = Proc;
								B.Rel = Proc->GetComponentTransform().GetRelativeTransform(S.RoadT);
								B.Stretch = MarkIndex;
							}
						}
					}
					++Repeated;
				}
				// The blueprint's own rigid preview goes away while drawing.
				S.Hidden.Emplace(Root, Root->IsVisible());
				Root->SetVisibility(false, false);
				for (USceneComponent* C : Children)
				{
					S.Hidden.Emplace(C, C->IsVisible());
					C->SetVisibility(false, false);
				}
			}
			// Signs and wall attachments on a fence wall running along the arrow: they follow the bent wall like the wall
			// snapping places them (facing out of the wall, tilted with its slope; belt wall mounts and ladders level).
			for (FRigid& R : S.Rigids)
			{
				const AFGBuildable* B = R.Buildable.Get();
				if (!B || TiltsWithCurve(B))
				{
					continue;
				}
				const FVector P = R.Rel.GetLocation();
				for (const FStretch& St : S.Stretches)
				{
					const ARMBuildableFence* Wall = Cast<ARMBuildableFence>(St.Piece.Get());
					if (Wall && Wall->IsWall() && FMath::Abs(P.Y - S.Yc - St.Y) <= 60.f
						&& P.X - S.X0 >= St.XMin - 10.f && P.X - S.X0 <= St.XMax + 10.f
						&& P.Z - S.Zb >= St.Z - 50.f && P.Z - S.Zb <= St.Z + ARMBuildableFence::GetWallHeight() + 50.f)
					{
						const FString Name = B->GetClass()->GetName();
						R.bLevel = (Name.Contains(TEXT("Conveyor")) && (Name.Contains(TEXT("Wall")) || Name.Contains(TEXT("Hole")))) || Name.Contains(TEXT("Ladder"));
						R.bOnWall = true;
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s is on the wall %s: follows it%s"), *B->GetName(), *Wall->GetName(), R.bLevel ? TEXT(" (level)") : TEXT(""));
						break;
					}
				}
			}
			// Markings on a road that is repeated as it is (a curved road in the blueprint) stay on it: they go where it goes.
			for (FRigid& R : S.Rigids)
			{
				const ARMBuildableMarking* Marking = Cast<ARMBuildableMarking>(R.Buildable.Get());
				if (!Marking)
				{
					continue;
				}
				const FVector At = Marking->GetActorLocation();
				// The road it lies on: the nearest road surface under it (within the road's width, beside a frame).
				auto Under = [&At](const ARMBuildableRoad* Road, float& InOutBestD) -> bool
				{
					if (!Road)
					{
						return false;
					}
					bool bCloser = false;
					TArray<FRMFrame> Frames;
					Road->GetWorldFrames(Frames, 50.f);
					for (const FRMFrame& F : Frames)
					{
						const FVector Off = At - F.Pos;
						if (FMath::Abs(FVector::DotProduct(Off, F.Fwd)) <= 40.f
							&& FMath::Abs(FVector::DotProduct(Off, F.Right)) <= Road->GetRoadWidth() * 0.5f + 20.f
							&& FMath::Abs(Off.Z) < 150.f && Off.SizeSquared() < InOutBestD)
						{
							InOutBestD = Off.SizeSquared();
							bCloser = true;
						}
					}
					return bCloser;
				};
				const FRigid* Best = nullptr;
				float BestD = TNumericLimits<float>::Max();
				for (const FRigid& Other : S.Rigids)
				{
					if (Under(Cast<ARMBuildableRoad>(Other.Buildable.Get()), BestD))
					{
						Best = &Other;
					}
				}
				// On a road along the arrow (bent with the curve): it bends with it, no parent.
				for (const FStretch& St : S.Stretches)
				{
					if (Under(Cast<ARMBuildableRoad>(St.Piece.Get()), BestD))
					{
						Best = nullptr;
					}
				}
				if (Best)
				{
					R.bHasParent = true;
					R.ParentRel = Best->Rel;
					if (R.bBentPreview)
					{
						// Not bent after all: shown as it is, with its road.
						R.bBentPreview = false;
						for (FBent& B : S.Bents)
						{
							if (B.Stretch == R.MarkStretch)
							{
								B.Stretch = INDEX_NONE;
							}
						}
					}
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s lies on %s (repeated as it is): it goes with it"), *Marking->GetName(), *Best->Buildable->GetName());
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: start fixed, %d pieces stretched along the road, %d repeated"), Stretched, Repeated);
		}

		UProceduralMeshComponent* MakeBentTile(AFGBlueprintHologram* Self, UProceduralMeshComponent* Src)
		{
			UProceduralMeshComponent* Copy = NewObject<UProceduralMeshComponent>(Self);
			Copy->SetupAttachment(Self->GetRootComponent());
			Copy->SetRelativeTransform(FTransform::Identity);
			CopyLook(Src, Copy);
			Copy->RegisterComponent();
			return Copy;
		}

		USceneComponent* MakeRigidTile(AFGBlueprintHologram* Self, USceneComponent* Src)
		{
			USceneComponent* TileRoot = NewObject<USceneComponent>(Self);
			TileRoot->SetupAttachment(Self->GetRootComponent());
			TileRoot->RegisterComponent();
			const FTransform SrcT = Src->GetComponentTransform();
			TArray<USceneComponent*> Children;
			Src->GetChildrenComponents(true, Children);
			Children.Insert(Src, 0);
			for (USceneComponent* C : Children)
			{
				UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(C);
				if (!Prim)
				{
					continue;
				}
				// Only what the blueprint preview showed (it is hidden now: its saved visibility tells).
				const FState* S = GStates.Find(Self);
				bool bWasVisible = true;
				if (S)
				{
					for (const TPair<TWeakObjectPtr<USceneComponent>, bool>& H : S->Hidden)
					{
						if (H.Key.Get() == C) { bWasVisible = H.Value; break; }
					}
				}
				if (!bWasVisible)
				{
					continue;
				}
				if (UPrimitiveComponent* Copy = CopyMesh(Self, Prim))
				{
					Copy->SetupAttachment(TileRoot);
					Copy->SetRelativeTransform(C->GetComponentTransform().GetRelativeTransform(SrcT));
					Copy->RegisterComponent();
				}
			}
			return TileRoot;
		}

		/** Where an object of tile K goes (Rel: where it is in the blueprint, road space): at its own distance along and
		 *  across the curve, turned with it; bLevel: only turned (yaw), standing upright, offset level across. */
		FTransform MapRigid(const FState& S, const FTransform& Rel, int32 K, bool bLevel)
		{
			const FVector P = Rel.GetLocation();
			FRMFrame F = FrameAt(S.Frames, (P.X - S.X0) + K * S.L);
			if (bLevel)
			{
				FVector Fwd(F.Fwd.X, F.Fwd.Y, 0.f);
				if (Fwd.Normalize())
				{
					F.Fwd = Fwd;
					F.Right = FVector::CrossProduct(FVector::UpVector, Fwd);
					F.Up = FVector::UpVector;
				}
			}
			const FQuat FrameRot = FMatrix(F.Fwd, F.Right, F.Up, FVector::ZeroVector).ToQuat();
			return FTransform(FrameRot * Rel.GetRotation(), F.Pos + F.Right * (P.Y - S.Yc) + F.Up * (P.Z - S.Zb), Rel.GetScale3D());
		}

		/** The same for one of the blueprint's repeated objects, following its road when it lies on one. */
		FTransform MapObject(const FState& S, const FTransform& Rel, int32 K, bool bHasParent, const FTransform& ParentRel, bool bLevel)
		{
			return bHasParent ? Rel.GetRelativeTransform(ParentRel) * MapRigid(S, ParentRel, K, false) : MapRigid(S, Rel, K, bLevel);
		}

		/**
		 * Points of a spline laid at an offset from the curve (in the way it runs), without the loops an offset makes on
		 * the inside of a curve tighter than the offset: a point that does not move on along the curve from the last
		 * one kept is dropped (the inside then turns in a corner instead of looping back).
		 */
		void DropFolds(TArray<FVector>& Pos, TArray<FVector>& Fwd)
		{
			if (Pos.Num() < 3)
			{
				return;
			}
			TArray<FVector> KeptPos = { Pos[0] };
			TArray<FVector> KeptFwd = { Fwd[0] };
			for (int32 I = 1; I < Pos.Num(); ++I)
			{
				const bool bLast = I == Pos.Num() - 1;
				const float Ahead = FVector::DotProduct(Pos[I] - KeptPos.Last(), Fwd[I]);
				if (Ahead > 50.f)
				{
					KeptPos.Add(Pos[I]);
					KeptFwd.Add(Fwd[I]);
				}
				else if (bLast)
				{
					// The end stays where it is (it meets the next piece / the curve's end).
					if (KeptPos.Num() > 1)
					{
						KeptPos.Last() = Pos[I];
						KeptFwd.Last() = Fwd[I];
					}
					else
					{
						KeptPos.Add(Pos[I]);
						KeptFwd.Add(Fwd[I]);
					}
				}
			}
			Pos = MoveTemp(KeptPos);
			Fwd = MoveTemp(KeptFwd);
		}

		/** A road-space point of tile K on the curve, offset level across and straight up (like the level objects). */
		FVector MapPointLevel(const FState& S, const FVector& P, int32 K, FRMFrame& OutF, float Scale = 1.f)
		{
			OutF = FrameAt(S.Frames, ((P.X - S.X0) + K * S.L) * Scale);
			const FVector Right = FVector(OutF.Right.X, OutF.Right.Y, 0.f).GetSafeNormal();
			return OutF.Pos + Right * (P.Y - S.Yc) + FVector::UpVector * (P.Z - S.Zb);
		}

		/** A road-space direction at frame F (along = the curve's direction, across level, up straight up). */
		FVector MapDirLevel(const FRMFrame& F, const FVector& V)
		{
			const FVector Right = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal();
			return F.Fwd * V.X + Right * V.Y + FVector::UpVector * V.Z;
		}

		/**
		 * A straight game spline (points in its actor's space, Rel: its actor in road space) laid on tile K of the curve:
		 * resampled every 4 m, tangents along the curve. World space. Returns its new length.
		 */
		float BendSpline(const FState& S, const FTransform& Rel, const TArray<FSplinePointData>& In, int32 K, TArray<FVector>& OutPos, TArray<FVector>& OutTan)
		{
			OutPos.Reset();
			OutTan.Reset();
			if (In.Num() < 2)
			{
				return 0.f;
			}
			const FVector A = Rel.TransformPosition(In[0].Location);
			const FVector B = Rel.TransformPosition(In.Last().Location);
			const int32 N = FMath::Clamp(FMath::CeilToInt(FVector::Dist(A, B) / 400.f), 1, 64);
			const float Sign = B.X >= A.X ? 1.f : -1.f;
			TArray<FVector> Fwd;
			for (int32 I = 0; I <= N; ++I)
			{
				FRMFrame F;
				OutPos.Add(MapPointLevel(S, FMath::Lerp(A, B, static_cast<float>(I) / N), K, F));
				Fwd.Add(F.Fwd * Sign);
			}
			DropFolds(OutPos, Fwd);
			const int32 M = OutPos.Num() - 1;
			float Len = 0.f;
			for (int32 I = 0; I < M; ++I)
			{
				Len += FVector::Dist(OutPos[I], OutPos[I + 1]);
			}
			for (int32 I = 0; I <= M; ++I)
			{
				const float Before = I > 0 ? FVector::Dist(OutPos[I - 1], OutPos[I]) : 0.f;
				const float After = I < M ? FVector::Dist(OutPos[I], OutPos[I + 1]) : 0.f;
				const float Seg = I == 0 ? After : I == M ? Before : 0.5f * (Before + After);
				OutTan.Add(Fwd[I] * Seg);
			}
			return Len;
		}

		/**
		 * Longest piece of a game spline the game lets you build in one go, read from its hologram (belts and pipes
		 * 56 m, vehicle paths 100 m, rails their own); longer ones are split and joined. 50 m if not found.
		 */
		float MaxSplinePiece(UClass* BuildableClass)
		{
			static TMap<TWeakObjectPtr<UClass>, float> Cache;
			if (const float* Found = Cache.Find(BuildableClass))
			{
				return *Found;
			}
			float Max = 5000.f;
			const AFGBuildable* Cdo = BuildableClass ? GetDefault<AFGBuildable>(BuildableClass) : nullptr;
			UClass* Holo = Cdo ? Cdo->mHologramClass.Get() : nullptr;
			if (Holo)
			{
				for (const TCHAR* Name : { TEXT("mMaxSplineLength"), TEXT("mMaxLength"), TEXT("mMaxPathLength") })
				{
					if (const FFloatProperty* Prop = FindFProperty<FFloatProperty>(Holo, Name))
					{
						const float Value = Prop->GetPropertyValue_InContainer(Holo->GetDefaultObject());
						if (Value > 100.f)
						{
							Max = Value - 10.f; // a hair under the limit
							break;
						}
					}
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s pieces up to %.0f m (hologram %s)"), *GetNameSafe(BuildableClass), Max / 100.f, *GetNameSafe(Holo));
			Cache.Add(BuildableClass, Max);
			return Max;
		}

		/**
		 * A full-length game spline over the whole curve: points every ~4 m in the way it runs (world), split into
		 * pieces of at most MaxSplinePiece. Returns its total length.
		 */
		float PlanFullSpline(const FState& S, const FRigid& R, TArray<TArray<FVector>>& OutPos, TArray<TArray<FVector>>& OutTan)
		{
			OutPos.Reset();
			OutTan.Reset();
			if (S.Frames.Num() < 2 || R.SplinePoints.Num() < 2)
			{
				return 0.f;
			}
			const FVector A = R.Rel.TransformPosition(R.SplinePoints[0].Location);
			const FVector B = R.Rel.TransformPosition(R.SplinePoints.Last().Location);
			const bool bForward = B.X >= A.X;
			const float Len = S.Frames.Last().Dist;
			const int32 N = FMath::Clamp(FMath::CeilToInt(Len / 400.f), 1, 4096);
			TArray<FVector> Pos, Fwd;
			for (int32 I = 0; I <= N; ++I)
			{
				const float Along = static_cast<float>(I) / N;               // in the way it runs
				const float D = (bForward ? Along : 1.f - Along) * Len;       // along the curve
				const FVector Off = FMath::Lerp(A, B, Along);                 // its offset across / up there
				const FRMFrame F = FrameAt(S.Frames, D);
				const FVector Right = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal();
				Pos.Add(F.Pos + Right * (Off.Y - S.Yc) + FVector::UpVector * (Off.Z - S.Zb));
				Fwd.Add(bForward ? F.Fwd : -F.Fwd);
			}
			DropFolds(Pos, Fwd);
			const int32 NKept = Pos.Num() - 1;
			if (NKept < 1)
			{
				return 0.f;
			}
			float Total = 0.f;
			for (int32 I = 0; I < NKept; ++I)
			{
				Total += FVector::Dist(Pos[I], Pos[I + 1]);
			}
			const int32 Pieces = FMath::Max(1, FMath::CeilToInt(Total / MaxSplinePiece(R.Buildable.IsValid() ? R.Buildable->GetClass() : nullptr)));
			for (int32 P = 0; P < Pieces; ++P)
			{
				const int32 First = FMath::RoundToInt(static_cast<float>(NKept) * P / Pieces);
				const int32 Last = FMath::Max(First + 1, FMath::RoundToInt(static_cast<float>(NKept) * (P + 1) / Pieces));
				TArray<FVector>& PP = OutPos.AddDefaulted_GetRef();
				TArray<FVector>& TT = OutTan.AddDefaulted_GetRef();
				for (int32 I = First; I <= Last && I <= NKept; ++I)
				{
					PP.Add(Pos[I]);
				}
				for (int32 I = 0; I < PP.Num(); ++I)
				{
					const float Before = I > 0 ? FVector::Dist(PP[I - 1], PP[I]) : 0.f;
					const float After = I + 1 < PP.Num() ? FVector::Dist(PP[I], PP[I + 1]) : 0.f;
					const float Seg = I == 0 ? After : I + 1 == PP.Num() ? Before : 0.5f * (Before + After);
					TT.Add(Fwd[First + I] * Seg);
				}
			}
			return Total;
		}

		/** How many copies of a shorter piece fit on a curve Len long (each tile L further on). */
		int32 TilesFitting(const FState& S, const FStretch& St, float Len)
		{
			int32 N = 0;
			while (N < MaxTiles && St.XMax + N * S.L <= Len + 50.f)
			{
				++N;
			}
			return N;
		}

		/** How many whole tiles of the repeated objects fit on a curve Len long (the last one ends at or before the end). */
		int32 RigidTiles(const FState& S, float Len)
		{
			int32 N = 0;
			// A tile counts once the curve reaches its far edge (the aim is the end edge of the last tile), not just its
			// furthest object.
			while (S.RigidMaxX >= 0.f && N < MaxTiles && (N + 1) * S.L <= Len + 50.f)
			{
				++N;
			}
			return N;
		}

		/** The wire the game builds between power poles, and its recipe (loaded once, kept). */
		UClass* WireClass()
		{
			static TWeakObjectPtr<UClass> Cached;
			if (!Cached.IsValid())
			{
				Cached = LoadClass<AFGBuildableWire>(nullptr, TEXT("/Game/FactoryGame/Buildable/Factory/PowerLine/Build_PowerLine.Build_PowerLine_C"));
				UE_CLOG(!Cached.IsValid(), LogRoadMaker, Warning, TEXT("Road Mode: power line class not found"));
			}
			return Cached.Get();
		}

		TSubclassOf<UFGRecipe> WireRecipe()
		{
			static TWeakObjectPtr<UClass> Cached;
			if (!Cached.IsValid())
			{
				Cached = LoadClass<UFGRecipe>(nullptr, TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_PowerLine.Recipe_PowerLine_C"));
				UE_CLOG(!Cached.IsValid(), LogRoadMaker, Warning, TEXT("Road Mode: power line recipe not found"));
			}
			return Cached.Get();
		}

		/** How many wire recipe units a wire this long costs. */
		int32 WireUnits(float Length)
		{
			const AFGBuildableWire* Cdo = WireClass() ? GetDefault<AFGBuildableWire>(WireClass()) : nullptr;
			const float Per = Cdo && Cdo->mLengthPerCost > 0.f ? Cdo->mLengthPerCost : 0.f;
			return Per > 0.f ? FMath::Max(1, FMath::CeilToInt(Length / Per)) : 1;
		}

		/** The connection a wire goes on: the one taking the most wires. */
		UFGCircuitConnectionComponent* WireConnectionOf(AActor* Actor)
		{
			UFGCircuitConnectionComponent* Best = nullptr;
			TInlineComponentArray<UFGCircuitConnectionComponent*> Circuits(Actor);
			for (UFGCircuitConnectionComponent* C : Circuits)
			{
				if (!Best || C->GetMaxNumConnections() > Best->GetMaxNumConnections())
				{
					Best = C;
				}
			}
			return Best;
		}

		/** Joins two spline ends of the same kind (belt, pipe, hypertube, rail) if both are free. */
		bool JoinEnds(UFGConnectionComponent* A, UFGConnectionComponent* B)
		{
			if (!A || !B || A == B || A->GetClass() != B->GetClass())
			{
				return false;
			}
			if (UFGFactoryConnectionComponent* FA = Cast<UFGFactoryConnectionComponent>(A))
			{
				UFGFactoryConnectionComponent* FB = Cast<UFGFactoryConnectionComponent>(B);
				if (FA->IsConnected() || FB->IsConnected() || FA->GetDirection() == FB->GetDirection())
				{
					return false;
				}
				FA->SetConnection(FB);
				return true;
			}
			if (UFGPipeConnectionComponentBase* PA = Cast<UFGPipeConnectionComponentBase>(A))
			{
				UFGPipeConnectionComponentBase* PB = Cast<UFGPipeConnectionComponentBase>(B);
				if (PA->IsConnected() || PB->IsConnected())
				{
					return false;
				}
				PA->SetConnection(PB);
				return true;
			}
			if (UFGRailroadTrackConnectionComponent* RA = Cast<UFGRailroadTrackConnectionComponent>(A))
			{
				UFGRailroadTrackConnectionComponent* RB = Cast<UFGRailroadTrackConnectionComponent>(B);
				if (RA->GetConnection() || RB->GetConnection())
				{
					return false;
				}
				RA->AddConnection(RB);
				return true;
			}
			return false;
		}

		/** A vehicle path end of a placed tile: where it is (world), its segment, start or end. */
		struct FPathEnd
		{
			TWeakObjectPtr<AFGVehiclePathSegment> Seg;
			FVector Pos = FVector::ZeroVector;
			bool bStart = true;
		};

		/** A free spline end of a placed tile: where it is (world) and its connection. */
		struct FSplineEnd
		{
			TWeakObjectPtr<UFGConnectionComponent> Conn;
			FVector Pos = FVector::ZeroVector;
		};

		struct FPlan
		{
			ARMBuildableSpline* Piece = nullptr;
			TArray<FRMSplinePoint> Points;
			int32 Mult = 1;
		};

		/** What the drawn curve builds: our pieces along it (Plans) and the whole tiles of the rest (returned), with the cost. */
		int32 MakePlans(AFGBlueprintHologram* Self, const FState& S, TArray<FPlan>& Plans, TMap<TSubclassOf<UFGItemDescriptor>, int32>& Cost)
		{
			Plans.Reset();
			Cost.Reset();
			if (S.Frames.Num() < 2)
			{
				return 0;
			}
			const float Len = S.Frames.Last().Dist;
			// A stretch of the curve from D0 to D1 (cm along it), its frames re-counted from 0.
			auto SubFrames = [&S](float D0, float D1, TArray<FRMFrame>& Out)
			{
				Out.Reset();
				Out.Add(FrameAt(S.Frames, D0));
				for (const FRMFrame& F : S.Frames)
				{
					if (F.Dist > D0 + 1.f && F.Dist < D1 - 1.f)
					{
						Out.Add(F);
					}
				}
				Out.Add(FrameAt(S.Frames, D1));
				for (FRMFrame& F : Out)
				{
					F.Dist -= D0;
				}
			};
			for (const FStretch& St : S.Stretches)
			{
				ARMBuildableSpline* Piece = St.Piece.Get();
				if (!Piece)
				{
					continue;
				}
				TArray<TPair<float, float>> Spans;
				if (St.bFull)
				{
					Spans.Emplace(0.f, Len);
				}
				else
				{
					const int32 N = TilesFitting(S, St, Len);
					for (int32 K = 0; K < N; ++K)
					{
						Spans.Emplace(St.XMin + K * S.L, FMath::Min(St.XMax + K * S.L, Len));
					}
				}
				for (const TPair<float, float>& Span : Spans)
				{
					TArray<FRMFrame> Sub;
					SubFrames(Span.Key, Span.Value, Sub);
					FPlan& P = Plans.AddDefaulted_GetRef();
					P.Piece = Piece;
					RMGeometry::OffsetCurve(Sub, St.Y, 400.f, P.Points);
					for (FRMSplinePoint& Pt : P.Points)
					{
						Pt.Location.Z += St.Z;
					}
					if (St.bReversed)
					{
						Algo::Reverse(P.Points);
						for (FRMSplinePoint& Pt : P.Points)
						{
							Pt.Tangent = -Pt.Tangent;
						}
					}
					// Paid like the piece in the blueprint, by length.
					const float SpanLen = Span.Value - Span.Key;
					P.Mult = FMath::Max(1, FMath::CeilToInt(Piece->GetCostMultiplier() * SpanLen / FMath::Max(St.Length, 1.f) - 0.01f));
					if (TSubclassOf<UFGRecipe> Recipe = Piece->GetBuiltWithRecipe())
					{
						for (const FItemAmount& A : UFGRecipe::GetIngredients(Self, Recipe))
						{
							Cost.FindOrAdd(A.ItemClass) += A.Amount * P.Mult;
						}
					}
				}
			}
			// The repeated objects: what each costs (its refund), once per tile.
			const int32 Tiles = RigidTiles(S, Len);
			if (Tiles > 0)
			{
				// Wires between the tiles' copies of the chained power poles.
				if (TSubclassOf<UFGRecipe> Recipe = WireRecipe())
				{
					for (const FRigid& R : S.Rigids)
					{
						for (int32 K = 1; R.bChainPower && K < Tiles; ++K)
						{
							const float WireLen = FVector::Dist(MapObject(S, R.Rel, K - 1, false, FTransform::Identity, R.bLevel).GetLocation(),
								MapObject(S, R.Rel, K, false, FTransform::Identity, R.bLevel).GetLocation());
							for (const FItemAmount& A : UFGRecipe::GetIngredients(Self, Recipe))
							{
								Cost.FindOrAdd(A.ItemClass) += A.Amount * WireUnits(WireLen);
							}
						}
					}
				}
				for (const FRigid& R : S.Rigids)
				{
					if (AFGBuildable* B = R.Buildable.Get())
					{
						TArray<FInventoryStack> Refund;
						IFGDismantleInterface::Execute_GetDismantleRefund(B, Refund, false);
						for (const FInventoryStack& Stack : Refund)
						{
							if (R.bSplineFull)
							{
								continue; // paid below, by its length over the whole curve
							}
							if (R.bSplineBend && R.SplineLen > 1.f)
							{
								// Bent splines are paid by their bent length, tile by tile.
								for (int32 K = 0; K < Tiles; ++K)
								{
									TArray<FVector> Pos, Tan;
									const float BentLen = BendSpline(S, R.Rel, R.SplinePoints, K, Pos, Tan);
									Cost.FindOrAdd(Stack.Item.GetItemClass()) += FMath::CeilToInt(Stack.NumItems * BentLen / R.SplineLen - 0.01f);
								}
							}
							else
							{
								Cost.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems * Tiles;
							}
						}
					}
				}
			}
			// Full-length game splines, rebuilt over the whole curve: paid by their length there.
			for (const FRigid& R : S.Rigids)
			{
				AFGBuildable* B = R.Buildable.Get();
				if (!B || !R.bSplineFull || R.SplineLen <= 1.f)
				{
					continue;
				}
				TArray<TArray<FVector>> Pos, Tan;
				const float Total = PlanFullSpline(S, R, Pos, Tan);
				TArray<FInventoryStack> Refund;
				IFGDismantleInterface::Execute_GetDismantleRefund(B, Refund, false);
				for (const FInventoryStack& Stack : Refund)
				{
					Cost.FindOrAdd(Stack.Item.GetItemClass()) += FMath::CeilToInt(Stack.NumItems * Total / R.SplineLen - 0.01f);
				}
			}
			return Tiles;
		}

		bool HasFullSplines(const FState& S)
		{
			for (const FRigid& R : S.Rigids)
			{
				if (R.bSplineFull && R.Buildable.IsValid())
				{
					return true;
				}
			}
			return false;
		}

		/** Preview of a bent spline: per tile, its spline meshes copied and laid on the curve (other parts are not shown). */
		void UpdateSplineTiles(AFGBlueprintHologram* Self, const FState& S, FRigid& R, USceneComponent* Src, int32 Count, float Scale = 1.f)
		{
			for (int32 K = 0; K < FMath::Max(Count, R.Tiles.Num()); ++K)
			{
				const bool bShow = K < Count;
				if (K >= R.Tiles.Num())
				{
					if (!bShow)
					{
						break;
					}
					USceneComponent* TileRoot = NewObject<USceneComponent>(Self);
					TileRoot->SetupAttachment(Self->GetRootComponent());
					TileRoot->RegisterComponent();
					TileRoot->SetWorldTransform(FTransform::Identity);
					auto& Pairs = R.SplineCopies.AddDefaulted_GetRef();
					TArray<USceneComponent*> Children;
					Src->GetChildrenComponents(true, Children);
					for (USceneComponent* C : Children)
					{
						USplineMeshComponent* SrcMesh = Cast<USplineMeshComponent>(C);
						if (!SrcMesh)
						{
							continue;
						}
						USplineMeshComponent* Copy = Cast<USplineMeshComponent>(CopyMesh(Self, SrcMesh));
						if (Copy)
						{
							Copy->SetupAttachment(TileRoot);
							Copy->SetRelativeTransform(FTransform::Identity);
							Copy->RegisterComponent();
							Pairs.Emplace(SrcMesh, Copy);
						}
					}
					R.Tiles.Add(TileRoot);
				}
				USceneComponent* Tile = R.Tiles[K].Get();
				if (!Tile)
				{
					continue;
				}
				Tile->SetVisibility(bShow, true);
				if (!bShow || !R.SplineCopies.IsValidIndex(K))
				{
					continue;
				}
				for (const TPair<TWeakObjectPtr<USplineMeshComponent>, TWeakObjectPtr<USplineMeshComponent>>& Pair : R.SplineCopies[K])
				{
					USplineMeshComponent* SrcMesh = Pair.Key.Get();
					USplineMeshComponent* Copy = Pair.Value.Get();
					if (!SrcMesh || !Copy)
					{
						continue;
					}
					const FTransform SrcRoad = SrcMesh->GetComponentTransform().GetRelativeTransform(S.RoadT);
					FRMFrame F0, F1;
					const FVector P0 = MapPointLevel(S, SrcRoad.TransformPosition(SrcMesh->GetStartPosition()), K, F0, Scale);
					const FVector P1 = MapPointLevel(S, SrcRoad.TransformPosition(SrcMesh->GetEndPosition()), K, F1, Scale);
					const FVector T0 = MapDirLevel(F0, SrcRoad.TransformVector(SrcMesh->GetStartTangent()));
					const FVector T1 = MapDirLevel(F1, SrcRoad.TransformVector(SrcMesh->GetEndTangent()));
					const FTransform CopyT = Copy->GetComponentTransform();
					Copy->SetStartAndEnd(CopyT.InverseTransformPosition(P0), CopyT.InverseTransformVector(T0),
						CopyT.InverseTransformPosition(P1), CopyT.InverseTransformVector(T1), true);
				}
			}
		}

		void Update(AFGBlueprintHologram* Self, FState& S, const FVector& End)
		{
			// The end's direction: along the start-to-end line, turned by the mouse wheel (like our roads). A placement
			// received on the server keeps the yaw it came with.
			if (!GNoPreview)
			{
				S.EndYaw = static_cast<float>(Self->GetScrollRotateValue() - S.ScrollAtStart);
			}
			if (FVector::DistSquared(End, S.LastEnd) < 25.f && FMath::IsNearlyEqual(S.EndYaw, S.LastYaw) && S.EndSnapDir.Equals(S.LastSnapDir))
			{
				return;
			}
			S.LastSnapDir = S.EndSnapDir;
			S.LastEnd = End;
			S.LastYaw = S.EndYaw;
			const FVector Start = S.RoadT.TransformPosition(FVector(S.X0, S.Yc, S.Zb));
			const FVector StartDir = S.RoadT.GetUnitAxis(EAxis::X);
			FVector EndDir = FVector(End.X - Start.X, End.Y - Start.Y, 0.f).GetSafeNormal();
			if (EndDir.IsNearlyZero())
			{
				EndDir = StartDir;
			}
			EndDir = EndDir.RotateAngleAxis(S.EndYaw, FVector::UpVector);
			if (!S.EndSnapDir.IsNearlyZero())
			{
				EndDir = S.EndSnapDir; // arriving onto a Blueprint Road end, in its direction
			}
			TArray<FRMSplinePoint> Points;
			RMGeometry::MakeQuadratic(Start, StartDir, End, EndDir, Points);
			TArray<FRMFrame> Frames;
			RMGeometry::SampleSpline(Points, 25.f, Frames);
			if (Frames.Num() < 2 || S.L < 1.f)
			{
				return;
			}
			S.Frames = Frames;
			if (GNoPreview)
			{
				return;
			}
			// The Blueprint Road being drawn (world space, on a component kept at the world origin).
			{
				UProceduralMeshComponent* Ghost = S.GhostPreview.Get();
				if (!Ghost)
				{
					Ghost = NewObject<UProceduralMeshComponent>(Self);
					Ghost->SetupAttachment(Self->GetRootComponent());
					Ghost->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Ghost->SetCastShadow(false);
					Ghost->RegisterComponent();
					S.GhostPreview = Ghost;
				}
				Ghost->SetWorldTransform(FTransform::Identity);
				FRMMesh Wire;
				ARMBuildableBlueprintRoad::BuildWire(Frames, S.HalfW * 2.f, Wire);
				const TArray<FLinearColor> Colors = ARMBuildableBlueprintRoad::WireColors(Wire);
				Wire.ApplyToSection(Ghost, 0, false, &Colors);
				Ghost->SetMaterial(0, ARMBuildableBlueprintRoad::WireMaterial());
			}
			const float Len = FMath::Min(Frames.Last().Dist, S.L * MaxTiles);

			// Where a hologram-space point of tile K goes (D = distance along the curve).
			auto Place = [&](const FVector& P, float D, FRMFrame& OutF) -> FVector
			{
				OutF = FrameAt(Frames, D);
				return OutF.Pos + OutF.Right * (P.Y - S.Yc) + OutF.Up * (P.Z - S.Zb);
			};

			// Stretched: whole tiles bent along the curve, scaled a little so they end exactly at the aim.
			// Shorter ones: one per tile, bent, not scaled, as many as fit.
			const int32 NFull = FMath::Clamp(FMath::RoundToInt(Len / S.L), 1, MaxTiles);
			const float FullScale = Len / (NFull * S.L);
			for (FBent& B : S.Bents)
			{
				UProceduralMeshComponent* Src = B.Src.Get();
				if (!Src || !S.Stretches.IsValidIndex(B.Stretch))
				{
					continue;
				}
				const FStretch& St = S.Stretches[B.Stretch];
				const int32 N = St.bFull ? NFull : St.bRigidCount ? RigidTiles(S, Len) : TilesFitting(S, St, Len);
				const float Scale = St.bFull ? FullScale : 1.f;
				for (int32 K = 0; K < FMath::Max(N, B.Tiles.Num()); ++K)
				{
					if (K >= B.Tiles.Num())
					{
						B.Tiles.Add(MakeBentTile(Self, Src));
					}
					UProceduralMeshComponent* Tile = B.Tiles[K].Get();
					if (!Tile)
					{
						continue;
					}
					Tile->SetVisibility(K < N);
					if (K >= N)
					{
						continue;
					}
					for (int32 I = 0; I < Src->GetNumSections(); ++I)
					{
						const FProcMeshSection* SrcSection = Src->GetProcMeshSection(I);
						if (!SrcSection)
						{
							continue;
						}
						FProcMeshSection Section = *SrcSection;
						FBox Bounds(ForceInit);
						for (FProcMeshVertex& V : Section.ProcVertexBuffer)
						{
							const FVector P = B.Rel.TransformPosition(V.Position);
							const float D = ((P.X - S.X0) + K * S.L) * Scale;
							FRMFrame F;
							const FVector World = Place(P, D, F);
							auto Bend = [&](const FVector& Dir)
							{
								const FVector Lc = B.Rel.TransformVectorNoScale(Dir);
								return S.StartT.InverseTransformVectorNoScale(F.Fwd * Lc.X + F.Right * Lc.Y + F.Up * Lc.Z);
							};
							V.Position = S.StartT.InverseTransformPosition(World);
							V.Normal = Bend(V.Normal);
							V.Tangent.TangentX = Bend(V.Tangent.TangentX);
							Bounds += V.Position;
						}
						Section.SectionLocalBox = Bounds;
						Tile->SetProcMeshSection(I, Section);
					}
				}
			}

			// Repeated: each object stays rigid, placed at its own distance along and across the curve, tile after
			// tile, whole tiles only (what gets built).
			const int32 Count = RigidTiles(S, Len);
			for (FRigid& R : S.Rigids)
			{
				USceneComponent* Src = R.Src.Get();
				if (!Src || R.bBentPreview)
				{
					continue;
				}
				if (R.bSplineBend)
				{
					// Full-length ones run over the whole curve (like our stretched roads), the others tile by tile.
					UpdateSplineTiles(Self, S, R, Src, R.bSplineFull ? NFull : Count, R.bSplineFull ? FullScale : 1.f);
					continue;
				}
				for (int32 K = 0; K < FMath::Max(Count, R.Tiles.Num()); ++K)
				{
					const bool bShow = K < Count;
					if (K >= R.Tiles.Num())
					{
						if (!bShow)
						{
							break;
						}
						R.Tiles.Add(MakeRigidTile(Self, Src));
					}
					USceneComponent* Tile = R.Tiles[K].Get();
					if (!Tile)
					{
						continue;
					}
					Tile->SetVisibility(bShow, true);
					if (!bShow)
					{
						continue;
					}
					Tile->SetWorldTransform(MapObject(S, R.Rel, K, R.bHasParent, R.ParentRel, R.bLevel));
				}
			}
		}

		int32 Available(const UObject* Context, UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item)
		{
			int32 Count = Inventory ? Inventory->GetNumItems(Item) : 0;
			if (AFGCentralStorageSubsystem* Central = AFGCentralStorageSubsystem::Get(const_cast<UObject*>(Context)))
			{
				Count += Central->GetNumItemsFromCentralStorage(Item);
			}
			return Count;
		}

		void Spend(const UObject* Context, UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item, int32 Num)
		{
			const int32 FromInventory = Inventory ? FMath::Min(Num, Inventory->GetNumItems(Item)) : 0;
			if (FromInventory > 0)
			{
				Inventory->Remove(Item, FromInventory);
			}
			if (Num - FromInventory > 0)
			{
				if (AFGCentralStorageSubsystem* Central = AFGCentralStorageSubsystem::Get(const_cast<UObject*>(Context)))
				{
					Central->TryRemoveItemsFromCentralStorage(Item, Num - FromInventory);
				}
			}
		}

		/** A marking moved onto the curve (Rel: where it was, in road space; Target: where it goes): its drape bent along the curve. */
		void BendMarking(const FState& S, int32 K, ARMBuildableMarking* Marking, const FTransform& Rel, const FTransform& Target)
		{
			FRMMarkingDrape Drape = Marking->GetDrape();
			if (!Drape.IsValid())
			{
				// A flat marking: a straight drape over its length.
				const float Half = ARMBuildableMarking::HalfLength(Marking->mKind, Marking->GetLaneWidth()) + 25.f;
				const int32 Count = FMath::Max(2, FMath::CeilToInt(2.f * Half / 50.f) + 1);
				Drape = FRMMarkingDrape();
				Drape.Start = -Half;
				Drape.Step = 2.f * Half / (Count - 1);
				for (int32 I = 0; I < Count; ++I)
				{
					Drape.Pos.Add(FVector(Drape.Start + I * Drape.Step, 0.f, 0.f));
					Drape.Right.Add(FVector::RightVector);
					Drape.Up.Add(FVector::UpVector);
				}
			}
			for (int32 I = 0; I < Drape.Pos.Num(); ++I)
			{
				const FVector P = Rel.TransformPosition(Drape.Pos[I]);
				const FRMFrame F = FrameAt(S.Frames, (P.X - S.X0) + K * S.L);
				auto Bend = [&](const FVector& Dir)
				{
					const FVector V = Rel.TransformVectorNoScale(Dir);
					return Target.InverseTransformVectorNoScale(F.Fwd * V.X + F.Right * V.Y + F.Up * V.Z);
				};
				Drape.Right[I] = Bend(Drape.Right[I]);
				Drape.Up[I] = Bend(Drape.Up[I]);
				Drape.Pos[I] = Target.InverseTransformPosition(F.Pos + F.Right * (P.Y - S.Yc) + F.Up * (P.Z - S.Zb));
			}
			Marking->SetDrape(Drape);
		}

		/** Whether the player can pay for the drawn placement (checked on the click, before the game sends it to the server). */
		bool CanAffordPlan(AFGBlueprintHologram* Self, const FState& S)
		{
			UWorld* World = Self->GetWorld();
			TArray<FPlan> Plans;
			TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
			const int32 Tiles = MakePlans(Self, S, Plans, Cost);
			if (!World || (Plans.Num() == 0 && Tiles == 0 && !HasFullSplines(S)))
			{
				return false;
			}
			const AFGGameState* GameState = World->GetGameState<AFGGameState>();
			AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(Self->GetConstructionInstigator());
			if (!Player)
			{
				Player = Cast<AFGCharacterPlayer>(Self->GetInstigator());
			}
			UFGInventoryComponent* Inventory = Player && !(GameState && GameState->GetCheatNoCost()) ? Player->GetInventory() : nullptr;
			for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& C : Cost)
			{
				if (Inventory && Available(Self, Inventory, C.Key) < C.Value)
				{
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: not enough %s (%d needed)"), *GetNameSafe(C.Key.Get()), C.Value);
					return false;
				}
			}
			return true;
		}

		/** Second click: our stretched pieces built along the drawn curve, paid from the player's inventory. */
		AActor* Build(AFGBlueprintHologram* Self, const FState& S)
		{
			UWorld* World = Self->GetWorld();
			if (!World || S.Frames.Num() < 2)
			{
				return nullptr;
			}
			if (!Self->HasAuthority())
			{
				UE_LOG(LogRoadMaker, Warning, TEXT("Road Mode: building runs on the server"));
				return nullptr;
			}
			TArray<FPlan> Plans;
			TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
			const int32 Tiles = MakePlans(Self, S, Plans, Cost);
			if (Plans.Num() == 0 && Tiles == 0 && !HasFullSplines(S))
			{
				return nullptr;
			}
			const AFGGameState* GameState = World->GetGameState<AFGGameState>();
			const bool bFree = GameState && GameState->GetCheatNoCost();
			AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(Self->GetConstructionInstigator());
			if (!Player)
			{
				Player = Cast<AFGCharacterPlayer>(Self->GetInstigator());
			}
			UFGInventoryComponent* Inventory = Player && !bFree ? Player->GetInventory() : nullptr;
			if (Inventory)
			{
				for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& C : Cost)
				{
					if (Available(Self, Inventory, C.Key) < C.Value)
					{
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: not enough %s (%d needed)"), *GetNameSafe(C.Key.Get()), C.Value);
						return nullptr;
					}
				}
				for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& C : Cost)
				{
					Spend(Self, Inventory, C.Key, C.Value);
				}
			}
			// One blueprint proxy for everything built, like a placed blueprint: the blueprint dismantle mode takes it all.
			const FVector StartPos = S.Frames[0].Pos;
			const FTransform ProxyT(FRotator(0.f, S.Frames[0].Fwd.Rotation().Yaw, 0.f), StartPos);
			FBox LocalBounds(ForceInit);
			for (const FRMFrame& F : S.Frames)
			{
				LocalBounds += ProxyT.InverseTransformPosition(F.Pos + F.Right * S.HalfW);
				LocalBounds += ProxyT.InverseTransformPosition(F.Pos - F.Right * S.HalfW);
			}
			LocalBounds.Min.Z -= 100.f;
			LocalBounds.Max.Z += FMath::Max(S.Height, 400.f);
			AFGBlueprintProxy* Proxy = World->SpawnActorDeferred<AFGBlueprintProxy>(AFGBlueprintProxy::StaticClass(), ProxyT, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Proxy)
			{
				UClass* ProxyClass = AFGBlueprintProxy::StaticClass();
				if (FProperty* NameProp = ProxyClass->FindPropertyByName(TEXT("mBlueprintName")))
				{
					*NameProp->ContainerPtrToValuePtr<FText>(Proxy) = Self->mBlueprintDescriptor
						? FText::FromString(Self->mBlueprintDescriptor->GetBlueprintNameAsString()) : FText::FromString(TEXT("Road"));
				}
				if (FObjectProperty* DescProp = FindFProperty<FObjectProperty>(ProxyClass, TEXT("mBlueprintDescriptor")))
				{
					DescProp->SetObjectPropertyValue_InContainer(Proxy, Self->mBlueprintDescriptor);
				}
				if (FProperty* BoundsProp = ProxyClass->FindPropertyByName(TEXT("mLocalBounds")))
				{
					*BoundsProp->ContainerPtrToValuePtr<FBox>(Proxy) = LocalBounds;
				}
				UGameplayStatics::FinishSpawningActor(Proxy, ProxyT);
				if (UBoxComponent* BoxComp = Proxy->GetBoundingBox())
				{
					BoxComp->SetRelativeLocation(LocalBounds.GetCenter());
					BoxComp->SetBoxExtent(LocalBounds.GetExtent());
				}
			}

			// The Blueprint Road: the placement's centre line, as wide as the blueprint, hidden (Road Mode / dismantle only).
			int32 Built = 0;
			{
				TArray<FRMSplinePoint> Centre;
				RMGeometry::OffsetCurve(S.Frames, 0.f, 400.f, Centre);
				if (Centre.Num() >= 2)
				{
					const FTransform GhostT(FRotator(0.f, S.Frames[0].Fwd.Rotation().Yaw, 0.f), Centre[0].Location);
					TArray<FRMSplinePoint> Local;
					for (const FRMSplinePoint& P : Centre)
					{
						Local.Add(FRMSplinePoint(GhostT.InverseTransformPosition(P.Location), GhostT.InverseTransformVectorNoScale(P.Tangent)));
					}
					ARMBuildableBlueprintRoad* Ghost = World->SpawnActorDeferred<ARMBuildableBlueprintRoad>(ARMBuildableBlueprintRoad::StaticClass(),
						GhostT, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
					if (Ghost)
					{
						Ghost->SetSplinePoints(Local);
						Ghost->SetWidth(S.HalfW * 2.f);
						UGameplayStatics::FinishSpawningActor(Ghost, GhostT);
						if (Proxy)
						{
							Ghost->SetBlueprintProxy(Proxy);
							Proxy->RegisterBuildable(Ghost);
						}
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: Blueprint Road (%d wide) laid, %.0f m"), Ghost->GetWidthUnits(), S.Frames.Last().Dist / 100.f);
					}
				}
			}

			// Our pieces along the arrow: built along the curve.
			for (FPlan& P : Plans)
			{
				ARMBuildableSpline* New = P.Piece->SpawnLike(P.Piece->GetClass(), P.Piece->GetBuiltWithRecipe(), P.Points, P.Mult,
					ERMRevealFrom::Start, FVector::ZeroVector, World);
				UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: built %s (%d points, cost x%d)"), New ? *New->GetName() : TEXT("nothing"), P.Points.Num(), P.Mult);
				if (New && Proxy)
				{
					New->SetBlueprintProxy(Proxy);
					Proxy->RegisterBuildable(New);
				}
				Built += New ? 1 : 0;
			}

			// Full-length belts, pipes, rails, hypertubes and vehicle paths: built over the whole curve, in pieces no
			// longer than the game allows for each (MaxSplinePiece), each joined to the one before it (before it begins play).
			for (const FRigid& R : S.Rigids)
			{
				AFGBuildable* Source = R.Buildable.Get();
				if (!Source || !R.bSplineFull)
				{
					continue;
				}
				TArray<TArray<FVector>> PiecePos, PieceTan;
				PlanFullSpline(S, R, PiecePos, PieceTan);
				AFGBuildable* Prev = nullptr;
				for (int32 P = 0; P < PiecePos.Num(); ++P)
				{
					const TArray<FVector>& Pos = PiecePos[P];
					const TArray<FVector>& Tan = PieceTan[P];
					if (Pos.Num() < 2)
					{
						continue;
					}
					const FTransform T(FRotator(0.f, Tan[0].Rotation().Yaw, 0.f), Pos[0]);
					AFGBuildable* New = World->SpawnActorDeferred<AFGBuildable>(Source->GetClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
					if (!New)
					{
						continue;
					}
					New->SetBuiltWithRecipe(Source->GetBuiltWithRecipe());
					New->SetCustomizationData_Native(Source->GetCustomizationData_Native());
					bool bJoinAfter = false;
					TArray<FSplinePointData> Pts;
					for (int32 I = 0; I < Pos.Num(); ++I)
					{
						FSplinePointData Pt;
						Pt.Location = T.InverseTransformPosition(Pos[I]);
						Pt.ArriveTangent = T.InverseTransformVector(Tan[I]);
						Pt.LeaveTangent = Pt.ArriveTangent;
						Pts.Add(Pt);
					}
					if (AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(New))
					{
						Path->SetSplinePointsAndValidatePath(Pts);
						const AFGVehiclePathSegment* PrevPath = Cast<AFGVehiclePathSegment>(Prev);
						Path->SetNodeConnections(PrevPath ? PrevPath->GetEndNode() : nullptr, nullptr);
					}
					else if (TArray<FSplinePointData>* Data = SplinePointsOf(New))
					{
						*Data = Pts;
						IFGSplineBuildableInterface* Cur = Cast<IFGSplineBuildableInterface>(New);
						IFGSplineBuildableInterface* Before = Cast<IFGSplineBuildableInterface>(Prev);
						if (Cur && Before)
						{
							// Belts are joined once they are built (below), like the game's belt placement does it: joined
							// before, the belt's chain never took the join. The others right away.
							UFGConnectionComponent* Conn0 = Cur->GetSplineConnection0();
							if (Conn0 && !Cast<UFGFactoryConnectionComponent>(Conn0))
							{
								JoinEnds(Conn0, Before->GetSplineConnection1());
							}
							else
							{
								bJoinAfter = true;
							}
						}
					}
					UGameplayStatics::FinishSpawningActor(New, T);
					if (bJoinAfter)
					{
						IFGSplineBuildableInterface* Cur = Cast<IFGSplineBuildableInterface>(New);
						IFGSplineBuildableInterface* Before = Cast<IFGSplineBuildableInterface>(Prev);
						const bool bJoined = Cur && Before && JoinEnds(Cur->GetSplineConnection0(), Before->GetSplineConnection1());
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s joined to %s after it was built: %d"), *New->GetName(), *GetNameSafe(Prev), bJoined ? 1 : 0);
					}
					if (Proxy)
					{
						New->SetBlueprintProxy(Proxy);
						Proxy->RegisterBuildable(New);
					}
					Prev = New;
					++Built;
				}
				UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s rebuilt over the whole curve in %d pieces"), *Source->GetName(), PiecePos.Num());
			}

			// Everything else: the blueprint placed once per tile by the game's own loader, each piece moved onto the
			// curve before it begins play (its distance along and across, turned with the curve). Our pieces along the
			// arrow come along too and go again (built above, bent).
			AFGBlueprintSubsystem* Blueprints = AFGBlueprintSubsystem::Get(World);
			if (Tiles > 0 && Blueprints && Self->mBlueprintDescriptor)
			{
				TArray<FSplineEnd> PrevEnds;
				TArray<FPathEnd> PrevPaths;
				TMap<const FRigid*, AFGBuildable*> PrevPower;
				int32 Joined = 0, Wired = 0;
				for (int32 K = 0; K < Tiles; ++K)
				{
					TArray<FSplineEnd> CurEnds;
					TArray<FPathEnd> CurPaths;
					TArray<AFGVehiclePathNode*> Orphans;
					TMap<const FRigid*, AFGBuildable*> CurPower;
					TArray<AFGBuildable*> Spawned;
					TArray<AFGBuildable*> Drop;
					TArray<TPair<AFGBuildable*, FTransform>> Moves;
					int32 Logged = 0;
					// The loader finishes spawning each piece at its blueprint place after this callback: the moves are
					// made again just before they begin play.
					auto ApplyMoves = [&Moves, &CurEnds, &PrevEnds, &CurPaths, &PrevPaths, &Orphans, &Joined, K]()
					{
						// Vehicle paths: a segment starting where the previous tile's one ends (or ending where it starts)
						// takes over that node; its own node there is left without segments and goes after.
						for (const FPathEnd& Cur : CurPaths)
						{
							AFGVehiclePathSegment* Seg = Cur.Seg.Get();
							if (!Seg)
							{
								continue;
							}
							for (const FPathEnd& Prev : PrevPaths)
							{
								AFGVehiclePathSegment* PrevSeg = Prev.Seg.Get();
								if (!PrevSeg || Prev.bStart == Cur.bStart || FVector::DistSquared(Cur.Pos, Prev.Pos) >= 400.f)
								{
									continue;
								}
								AFGVehiclePathNode* Shared = Prev.bStart ? PrevSeg->GetStartNode() : PrevSeg->GetEndNode();
								AFGVehiclePathNode* Own = Cur.bStart ? Seg->GetStartNode() : Seg->GetEndNode();
								if (!Shared || Shared == Own)
								{
									continue;
								}
								if (Cur.bStart)
								{
									Seg->SetNodeConnections(Shared, Seg->GetEndNode());
									if (Own)
									{
										Own->RemoveLeavingConnection(Seg, false);
									}
									Shared->AddLeavingConnection(Seg);
								}
								else
								{
									Seg->SetNodeConnections(Seg->GetStartNode(), Shared);
									if (Own)
									{
										Own->RemoveArrivingConnection(Seg, false);
									}
									Shared->AddArrivingConnection(Seg);
								}
								if (Own)
								{
									Orphans.AddUnique(Own);
								}
								++Joined;
								break;
							}
						}
						// Spline ends meeting the previous tile's free ends: joined before they begin play (like building a
						// belt onto an existing one).
						for (const FSplineEnd& Cur : CurEnds)
						{
							for (const FSplineEnd& Prev : PrevEnds)
							{
								if (FVector::DistSquared(Cur.Pos, Prev.Pos) < 400.f && JoinEnds(Cur.Conn.Get(), Prev.Conn.Get()))
								{
									++Joined;
									break;
								}
							}
						}
						int32 Checked = 0;
						for (const TPair<AFGBuildable*, FTransform>& M : Moves)
						{
							if (!IsValid(M.Key))
							{
								continue;
							}
							if (Checked++ < 1)
							{
								UE_LOG(LogRoadMaker, Display, TEXT("Road Mode:   tile %d before begin play: %s at %s, moving to %s"), K, *M.Key->GetName(),
									*M.Key->GetActorLocation().ToString(), *M.Value.GetLocation().ToString());
							}
							MoveBuilding(M.Key, M.Value);
						}
					};
					Blueprints->LoadStoredBlueprint(Self->mBlueprintDescriptor, S.StartT, Spawned, false, nullptr, Player, Proxy,
						[&S, &Drop, &Moves, &Logged, &CurEnds, &CurPaths, &CurPower, K](AFGBuildable* B, int32 Index)
						{
							if (!B)
							{
								return;
							}
							FTransform Rel = B->GetActorTransform().GetRelativeTransform(S.RoadT);
							Rel.AddToTranslation(S.LoaderToPreview); // where the hologram showed it
							const FVector P = Rel.GetLocation();
							if (B->IsA<ARMBuildableRoad>() || B->IsA<ARMBuildableFence>())
							{
								for (const FStretch& St : S.Stretches)
								{
									if (St.Piece.IsValid() && St.Piece->GetClass() == B->GetClass() && FVector::DistSquared(St.RootLoc, P) < 2500.f)
									{
										Drop.Add(B);
										return;
									}
								}
							}
							// Which of the blueprint's objects this is (a marking may follow its road).
							const FRigid* Match = nullptr;
							for (const FRigid& R : S.Rigids)
							{
								if (R.Buildable.IsValid() && R.Buildable->GetClass() == B->GetClass() && FVector::DistSquared(R.Rel.GetLocation(), P) < 2500.f)
								{
									Match = &R;
									break;
								}
							}
							if (Match && Match->bSplineFull)
							{
								Drop.Add(B); // rebuilt over the whole curve instead
								return;
							}
							const bool bHasParent = Match && Match->bHasParent;
							const FTransform Target = MapObject(S, Rel, K, bHasParent, bHasParent ? Match->ParentRel : FTransform::Identity,
								Match ? Match->bLevel : !TiltsWithCurve(B));
							if (Logged++ < 3)
							{
								UE_LOG(LogRoadMaker, Display, TEXT("Road Mode:   tile %d: %s at %s (road space) -> %s"), K, *B->GetName(), *P.ToString(), *Target.GetLocation().ToString());
							}
							// A straight game spline: its points laid on the curve (before it begins play, which builds its mesh).
							if (Match && Match->bSplineBend)
							{
								{
									if (TArray<FSplinePointData>* Pts = SplinePointsOf(B))
									{
										TArray<FVector> Pos, Tan;
										BendSpline(S, Rel, *Pts, K, Pos, Tan);
										if (AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(B))
										{
											if (Pos.Num() >= 2)
											{
												CurPaths.Add({ Path, Pos[0], true });
												CurPaths.Add({ Path, Pos.Last(), false });
											}
										}
										if (IFGSplineBuildableInterface* Ends = Cast<IFGSplineBuildableInterface>(B))
										{
											if (Pos.Num() >= 2)
											{
												CurEnds.Add({ Ends->GetSplineConnection0(), Pos[0] });
												CurEnds.Add({ Ends->GetSplineConnection1(), Pos.Last() });
											}
										}
										Pts->Reset();
										for (int32 I = 0; I < Pos.Num(); ++I)
										{
											FSplinePointData Pt;
											Pt.Location = Target.InverseTransformPosition(Pos[I]);
											Pt.ArriveTangent = Target.InverseTransformVector(Tan[I]);
											Pt.LeaveTangent = Pt.ArriveTangent;
											Pts->Add(Pt);
										}
										// A vehicle path keeps checks made along its course (which vehicles fit): made again on the new one.
										if (AFGVehiclePathSegment* Path = Cast<AFGVehiclePathSegment>(B))
										{
											const TArray<FSplinePointData> Bent = *Pts;
											Path->SetSplinePointsAndValidatePath(Bent);
										}
									}
								}
							}
							// A marking along the arrow: the road surface under it follows the curve too.
							if (ARMBuildableMarking* Marking = Cast<ARMBuildableMarking>(B))
							{
								if (!bHasParent && FMath::Abs(Rel.GetUnitAxis(EAxis::X).X) > 0.98f)
								{
									BendMarking(S, K, Marking, Rel, Target);
								}
							}
							if (Match && Match->bChainPower)
							{
								CurPower.Add(Match, B);
							}
							Moves.Emplace(B, Target);
							GPendingMoves.Add(B, Target);
							MoveBuilding(B, Target);
						},
						ApplyMoves);
					GPendingMoves.Reset();
					if (Moves.Num() > 0 && IsValid(Moves[0].Key))
					{
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode:   tile %d after placing: %s at %s"), K, *Moves[0].Key->GetName(), *Moves[0].Key->GetActorLocation().ToString());
					}
					for (AFGBuildable* B : Drop)
					{
						if (Proxy)
						{
							Proxy->UnregisterBuildable(B);
						}
						B->SetBlueprintProxy(nullptr);
						B->Destroy();
					}
					// Power: each chained pole wired to its copy on the previous tile.
					for (const TPair<const FRigid*, AFGBuildable*>& Cur : CurPower)
					{
						AFGBuildable* const* Prev = PrevPower.Find(Cur.Key);
						UFGCircuitConnectionComponent* A = Prev && IsValid(*Prev) ? WireConnectionOf(*Prev) : nullptr;
						UFGCircuitConnectionComponent* B2 = IsValid(Cur.Value) ? WireConnectionOf(Cur.Value) : nullptr;
						UClass* Wire = WireClass();
						if (!A || !B2 || !Wire)
						{
							continue;
						}
						const FTransform WireT(A->GetComponentLocation());
						AFGBuildableWire* NewWire = World->SpawnActorDeferred<AFGBuildableWire>(Wire, WireT, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
						if (NewWire)
						{
							NewWire->SetBuiltWithRecipe(WireRecipe());
							UGameplayStatics::FinishSpawningActor(NewWire, WireT);
							if (NewWire->Connect(A, B2))
							{
								++Wired;
								if (Proxy)
								{
									NewWire->SetBlueprintProxy(Proxy);
									Proxy->RegisterBuildable(NewWire);
								}
							}
							else
							{
								NewWire->Destroy();
							}
						}
					}
					// Vehicle path nodes left without segments (their segment took over the previous tile's node) go.
					for (AFGVehiclePathNode* Node : Orphans)
					{
						if (IsValid(Node) && Node->GetArrivingConnections().Num() == 0 && Node->GetLeavingConnections().Num() == 0)
						{
							if (Proxy)
							{
								Proxy->UnregisterBuildable(Node);
							}
							Node->SetBlueprintProxy(nullptr);
							Node->Destroy();
						}
					}
					PrevPaths = MoveTemp(CurPaths);
					PrevPower = MoveTemp(CurPower);
					PrevEnds = MoveTemp(CurEnds);
					Built += Spawned.Num() - Drop.Num();
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: tile %d placed, %d pieces (%d along the arrow left out), %d ends joined and %d wires so far"),
						K, Spawned.Num() - Drop.Num(), Drop.Num(), Joined, Wired);
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %d pieces built under blueprint proxy %s"), Built, Proxy ? *Proxy->GetName() : TEXT("none"));
			return Proxy;
		}

		bool IsRoadMode(const AFGBlueprintHologram* Self)
		{
			UClass* Mode = RoadModeClass();
			return Mode && Self && Self->IsCurrentBuildMode(Mode);
		}

		/** While drawing: the blueprint stays at its start and the curve follows where the game put it for this aim. */
		void Follow(AFGBlueprintHologram* Self, const FHitResult& Hit)
		{
			FState* S = GStates.Find(Self);
			if (!S || S->Step != 1)
			{
				// Before the first click: the point aimed at is where the road starts, the middle of the blueprint's back
				// edge, so the blueprint is shifted forward from where the game put its middle.
				if (!S && IsRoadMode(Self))
				{
					FState Probe;
					Probe.StartT = Self->GetActorTransform();
					MeasureBox(Self, Probe, false);
					FVector SnapPos, SnapOut;
					float SnapWidth = 0.f;
					if (ARMBuildableBlueprintRoad::FindEnd(Hit.Location, SnapPos, SnapOut, SnapWidth))
					{
						// On a Blueprint Road end (before foundations): the road starts there and carries on out of it.
						const FTransform Turn = Probe.RoadT.GetRelativeTransform(Probe.StartT);
						const FQuat RoadRot = FRotationMatrix::MakeFromXZ(SnapOut, FVector::UpVector).ToQuat();
						const FVector RoadLoc = SnapPos - RoadRot.RotateVector(FVector(Probe.X0, Probe.Yc, Probe.Zb));
						Self->SetActorTransform(Turn.Inverse() * FTransform(RoadRot, RoadLoc));
					}
					else
					{
						const FVector BackEdge = Probe.RoadT.TransformPosition(FVector(Probe.X0, Probe.Yc, Probe.Zb));
						const FVector Middle = Probe.RoadT.TransformPosition(FVector(Probe.X0 + Probe.L * 0.5f, Probe.Yc, Probe.Zb));
						Self->SetActorLocation(Self->GetActorLocation() + (Middle - BackEdge));
					}
				}
				return;
			}
			if (!IsRoadMode(Self))
			{
				Stop(Self);
				return;
			}
			// The end goes where the game just put the blueprint for this aim: on foundations that is on top, on their
			// grid, like the start. Anywhere it did not move it (or put it far from the aim), the aim point.
			const FVector Placed = Self->GetActorLocation();
			const bool bPlaced = !Placed.Equals(S->StartT.GetLocation(), 1.f) && FVector::Dist2D(Placed, Hit.Location) < 2000.f;
			// Diagnostics, once a second while drawing: what the aim hits and where the game placed the blueprint.
			static double LastLog = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastLog > 1.0)
			{
				LastLog = Now;
				UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: aim hits %s (%s) at %s, blocking %d; game placed it at %s (start %s), used: %s"),
					*GetNameSafe(Hit.GetActor()), Hit.GetActor() ? *Hit.GetActor()->GetClass()->GetName() : TEXT("-"), *Hit.Location.ToString(),
					Hit.bBlockingHit ? 1 : 0, *Placed.ToString(), *S->StartT.GetLocation().ToString(), bPlaced ? TEXT("placed") : TEXT("aim"));
			}
			Self->SetActorTransform(S->StartT);
			// On a Blueprint Road end (before foundations): the curve ends there, arriving in its direction.
			FVector SnapPos, SnapOut;
			float SnapWidth = 0.f;
			const FVector Start = S->RoadT.TransformPosition(FVector(S->X0, S->Yc, S->Zb));
			if (ARMBuildableBlueprintRoad::FindEnd(Hit.Location, SnapPos, SnapOut, SnapWidth) && FVector::Dist2D(SnapPos, Start) > 100.f)
			{
				S->EndSnapDir = -SnapOut;
				Update(Self, *S, SnapPos);
				return;
			}
			S->EndSnapDir = FVector::ZeroVector;
			Update(Self, *S, bPlaced ? Placed : Hit.Location);
		}

	}
	

	bool IsRoadModeHologram(const AFGHologram* Hologram)
	{
		return RMRoadMode::IsRoadMode(Cast<AFGBlueprintHologram>(Hologram));
	}

	void NotePiece(AFGBlueprintHologram* Hologram, ARMBuildableProcedural* Piece)
	{
		if (Hologram && Piece)
		{
			GPieces.FindOrAdd(Hologram).AddUnique(Piece);
		}
	}

	FRMRoadTemplate Read(AFGBlueprintHologram* Hologram)
	{
		FRMRoadTemplate T;
		TArray<TWeakObjectPtr<ARMBuildableProcedural>>* Pieces = GPieces.Find(Hologram);
		if (!Pieces)
		{
			return T;
		}
		// The main road: the longest road (not a walking path).
		ARMBuildableRoad* Main = nullptr;
		float MainLen = 0.f;
		for (const TWeakObjectPtr<ARMBuildableProcedural>& P : *Pieces)
		{
			ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(P.Get());
			if (Road && !Road->IsWalkingPath() && Road->GetLength() > MainLen)
			{
				MainLen = Road->GetLength();
				Main = Road;
			}
		}
		if (!Main)
		{
			return T;
		}
		T.RoadClass = Main->GetClass();
		T.RoadWidth = Main->GetRoadWidth();
		T.Look = Main->GetLook();
		T.Sources.Add(Main);
		TArray<FRMFrame> Frames;
		Main->GetWorldFrames(Frames, 50.f);
		// Every fence / walking path running along it: the same distance from its centre all the way (within 50 cm).
		for (const TWeakObjectPtr<ARMBuildableProcedural>& P : *Pieces)
		{
			ARMBuildableSpline* Piece = Cast<ARMBuildableSpline>(P.Get());
			if (!Piece || Piece == Main)
			{
				continue;
			}
			ARMBuildableRoad* Path = Cast<ARMBuildableRoad>(Piece);
			ARMBuildableFence* Fence = Cast<ARMBuildableFence>(Piece);
			if (!(Fence || (Path && Path->IsWalkingPath())))
			{
				continue;
			}
			TArray<FRMFrame> Own;
			Piece->GetWorldFrames(Own, 100.f);
			float Sum = 0.f, Min = TNumericLimits<float>::Max(), Max = -TNumericLimits<float>::Max(), LiftSum = 0.f;
			int32 Count = 0;
			for (const FRMFrame& F : Own)
			{
				float Off, Lift;
				if (Measure(Frames, F.Pos, Off, Lift))
				{
					Sum += Off;
					LiftSum += Lift;
					Min = FMath::Min(Min, Off);
					Max = FMath::Max(Max, Off);
					++Count;
				}
			}
			if (Count < 2 || Max - Min > 50.f)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Road template: %s is not along the road (spread %.0f cm), left out"), *Piece->GetName(), Count < 2 ? 0.f : Max - Min);
				continue;
			}
			FRMTemplatePiece TP;
			TP.Class = Piece->GetClass();
			TP.Offset = Sum / Count;
			TP.Lift = LiftSum / Count;
			TP.bWalkingPath = Path != nullptr;
			if (Fence)
			{
				// Facing relative to the road's direction (the fence may run the other way).
				TP.InwardSign = Fence->GetInwardSign() * (FVector::DotProduct(Own[0].Fwd, Frames[0].Fwd) >= 0.f ? 1.f : -1.f);
				TP.Look = Fence->GetWallLook();
			}
			else
			{
				TP.Look = Path->GetLook();
			}
			T.Pieces.Add(TP);
			T.Sources.Add(Piece);
		}
		T.Pieces.Sort([](const FRMTemplatePiece& A, const FRMTemplatePiece& B) { return A.Offset < B.Offset; });
		return T;
	}

	void InstallHooks()
	{
#if !WITH_EDITOR
		// Road Mode next to the blueprint's own modes, when it holds a Road Maker road.
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::GetSupportedBuildModes_Implementation, GetMutableDefault<AFGBlueprintHologram>(),
			[](const AFGBlueprintHologram* Self, TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& OutModes)
			{
				UClass* Mode = RoadModeClass();
				if (Mode)
				{
					OutModes.AddUnique(Mode);
				}
			});
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::OnBuildModeChanged, GetMutableDefault<AFGBlueprintHologram>(),
			[](AFGBlueprintHologram* Self, TSubclassOf<UFGHologramBuildModeDescriptor> BuildMode)
			{
				if (!BuildMode || BuildMode.Get() != RoadModeClass())
				{
					RMRoadMode::Stop(Self); // left Road Mode: the blueprint's own preview back
					return;
				}
				const FRMRoadTemplate T = Read(Self);
				UE_LOG(LogRoadMaker, Display, TEXT("Road template: road %s, %.0f cm wide, look %d, %d pieces along it"),
					T.RoadClass ? *T.RoadClass->GetName() : TEXT("none"), T.RoadWidth, T.Look, T.Pieces.Num());
				for (const FRMTemplatePiece& P : T.Pieces)
				{
					UE_LOG(LogRoadMaker, Display, TEXT("Road template:   %s at %.0f cm (%s), %.0f cm up, facing %.0f, look %d"),
						P.Class ? *P.Class->GetName() : TEXT("?"), P.Offset, P.Offset >= 0.f ? TEXT("right") : TEXT("left"), P.Lift, P.InwardSign, P.Look);
				}
			});
		// Road Mode: two clicks like a road. First click fixes the start, the second ends the drawing (building: step 3).
		SUBSCRIBE_METHOD_VIRTUAL(AFGBlueprintHologram::DoMultiStepPlacement, GetMutableDefault<AFGBlueprintHologram>(),
			[](auto& Scope, AFGBlueprintHologram* Self, bool bIsInputFromARelease)
			{
				if (!RMRoadMode::IsRoadMode(Self))
				{
					return;
				}
				Scope.Override(false);
				if (bIsInputFromARelease)
				{
					return;
				}
				RMRoadMode::FState* S = RMRoadMode::GStates.Find(Self);
				if (!S || S->Step == 0)
				{
					RMRoadMode::Begin(Self);
				}
				else if (!S->bFinal && S->Frames.Num() > 1 && RMRoadMode::CanAffordPlan(Self, *S))
				{
					// Done: the game constructs it the usual way (sent to the server with the Road Mode data added to its
					// construct message), and Road Mode builds it in Construct, on the server.
					S->bFinal = true;
					Scope.Override(true);
					// A client keeps no state once the message is sent (the host's Construct ran meanwhile and cleared it).
					TWeakObjectPtr<AFGBlueprintHologram> Weak(Self);
					FTimerHandle Handle;
					Self->GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Weak]()
					{
						if (AFGBlueprintHologram* H = Weak.Get())
						{
							RMRoadMode::Stop(H);
						}
					}), 1.f, false);
				}
			});
		// The construct message (client to server): whether it is a Road Mode placement, and its start and end.
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::SerializeConstructMessage, GetMutableDefault<AFGBlueprintHologram>(),
			[](AFGBlueprintHologram* Self, FArchive& Ar, FNetConstructionID Id)
			{
				uint8 bRoad = 0;
				FTransform StartT;
				FVector End = FVector::ZeroVector;
				float EndYaw = 0.f;
				FVector EndSnapDir = FVector::ZeroVector;
				if (Ar.IsSaving())
				{
					if (const RMRoadMode::FState* S = RMRoadMode::GStates.Find(Self))
					{
						if (S->bFinal)
						{
							bRoad = 1;
							StartT = S->StartT;
							End = S->LastEnd;
							EndYaw = S->EndYaw;
							EndSnapDir = S->EndSnapDir;
						}
					}
				}
				Ar << bRoad;
				if (bRoad)
				{
					Ar << StartT;
					Ar << End;
					Ar << EndYaw;
					Ar << EndSnapDir;
					if (Ar.IsLoading())
					{
						RMRoadMode::FRemote Remote;
						Remote.EndSnapDir = EndSnapDir;
						Remote.StartT = StartT;
						Remote.End = End;
						Remote.EndYaw = EndYaw;
						RMRoadMode::GRemote.Add(Self, Remote);
						UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: placement received (start %s, end %s)"), *StartT.GetLocation().ToString(), *End.ToString());
					}
				}
			});
		// Construct (server): a Road Mode placement builds along its curve instead of the blueprint as it is.
		SUBSCRIBE_METHOD_VIRTUAL(AFGBlueprintHologram::Construct, GetMutableDefault<AFGBlueprintHologram>(),
			[](auto& Scope, AFGBlueprintHologram* Self, TArray<AActor*>& OutChildren, FNetConstructionID Id)
			{
				RMRoadMode::FState* S = RMRoadMode::GStates.Find(Self);
				if (!S || !S->bFinal)
				{
					// Received from a client: worked out again here from its start and end.
					const RMRoadMode::FRemote* Remote = RMRoadMode::GRemote.Find(Self);
					if (!Remote)
					{
						return;
					}
					const FTransform StartT = Remote->StartT;
					const FVector End = Remote->End;
					const float EndYaw = Remote->EndYaw;
					const FVector EndSnapDir = Remote->EndSnapDir;
					RMRoadMode::GRemote.Remove(Self);
					Self->SetActorTransform(StartT);
					RMRoadMode::GNoPreview = true;
					RMRoadMode::Begin(Self);
					S = RMRoadMode::GStates.Find(Self);
					if (S)
					{
						S->EndYaw = EndYaw;
						S->EndSnapDir = EndSnapDir;
						RMRoadMode::Update(Self, *S, End);
						S->bFinal = true;
					}
					RMRoadMode::GNoPreview = false;
					if (!S)
					{
						return;
					}
				}
				AActor* Built = RMRoadMode::Build(Self, *S);
				RMRoadMode::Stop(Self);
				if (Built)
				{
					Scope.Override(Built);
				}
				else
				{
					UE_LOG(LogRoadMaker, Warning, TEXT("Road Mode: could not build the placement; the blueprint is placed as it is"));
				}
			});
		// Road Mode building a long belt in pieces: each piece joined to the one before it once its components exist,
		// just before it begins play.
		SUBSCRIBE_METHOD(AActor::DispatchBeginPlay,
			[](auto& Scope, AActor* Self, bool bFromLevelStreaming)
			{
				if (RMRoadMode::GPendingJoins.Num() == 0)
				{
					return;
				}
				TWeakObjectPtr<AActor> Prev;
				if (RMRoadMode::GPendingJoins.RemoveAndCopyValue(Self, Prev))
				{
					IFGSplineBuildableInterface* Cur = Cast<IFGSplineBuildableInterface>(Self);
					IFGSplineBuildableInterface* Before = Cast<IFGSplineBuildableInterface>(Prev.Get());
					UFGConnectionComponent* A = Cur ? Cur->GetSplineConnection0() : nullptr;
					UFGConnectionComponent* B = Before ? Before->GetSplineConnection1() : nullptr;
					const UFGFactoryConnectionComponent* FA = Cast<UFGFactoryConnectionComponent>(A);
					const UFGFactoryConnectionComponent* FB = Cast<UFGFactoryConnectionComponent>(B);
					const bool bJoined = RMRoadMode::JoinEnds(A, B);
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s joined to %s before begin play: %d (%s %s dir %d connected %d / %s %s dir %d connected %d)"),
						*Self->GetName(), *GetNameSafe(Prev.Get()), bJoined ? 1 : 0,
						*GetNameSafe(A), A ? *A->GetClass()->GetName() : TEXT("-"), FA ? static_cast<int32>(FA->GetDirection()) : -1, FA && FA->IsConnected() ? 1 : 0,
						*GetNameSafe(B), B ? *B->GetClass()->GetName() : TEXT("-"), FB ? static_cast<int32>(FB->GetDirection()) : -1, FB && FB->IsConnected() ? 1 : 0);
				}
			});
		// Road Mode placing a tile: each piece finishes spawning (and is drawn) where it goes on the curve.
		SUBSCRIBE_METHOD(AActor::FinishSpawning,
			[](auto& Scope, AActor* Self, const FTransform& Transform, bool bIsDefaultTransform,
				const FComponentInstanceDataCache* InstanceDataCache, ESpawnActorScaleMethod ScaleMethod)
			{
				if (RMRoadMode::GPendingMoves.Num() == 0)
				{
					return;
				}
				if (const FTransform* Target = RMRoadMode::GPendingMoves.Find(Self))
				{
					const FTransform Moved = *Target;
					RMRoadMode::GPendingMoves.Remove(Self);
					Scope(Self, Moved, false, InstanceDataCache, ScaleMethod);
				}
			});
		// While drawing, the hologram's cost grows with the drawn curve: the blueprint's cost once per tile of length
		// (rounded). Hooking GetBaseCost itself (an array returned by value) crashes, so the multiplier it is.
		// What is paid on the second click is the exact plan (our bent pieces by length, the rest per whole tile).
		SUBSCRIBE_METHOD_VIRTUAL(AFGHologram::GetBaseCostMultiplier, GetMutableDefault<AFGBlueprintHologram>(),
			[](auto& Scope, const AFGHologram* Self)
			{
				const AFGBlueprintHologram* Blueprint = Cast<AFGBlueprintHologram>(Self);
				const RMRoadMode::FState* S = Blueprint ? RMRoadMode::GStates.Find(const_cast<AFGBlueprintHologram*>(Blueprint)) : nullptr;
				// Sent / received for building: Road Mode pays the exact plan itself, the game charges nothing.
				if ((S && S->bFinal) || (Blueprint && RMRoadMode::GRemote.Contains(const_cast<AFGBlueprintHologram*>(Blueprint))))
				{
					Scope.Override(0);
					return;
				}
				if (S && S->Step == 1 && S->Frames.Num() > 1 && S->L > 1.f)
				{
					Scope.Override(FMath::Max(1, FMath::RoundToInt(S->Frames.Last().Dist / S->L)));
				}
			});
		// While drawing, the blueprint stays at its start and the curve follows the aim. On foundations the game snaps the
		// blueprint in TrySnapToActor and skips SetHologramLocationAndRotation: both are followed.
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::SetHologramLocationAndRotation, GetMutableDefault<AFGBlueprintHologram>(),
			[](AFGBlueprintHologram* Self, const FHitResult& Hit)
			{
				RMRoadMode::Follow(Self, Hit);
			});
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::TrySnapToActor, GetMutableDefault<AFGBlueprintHologram>(),
			[](bool bSnapped, AFGBlueprintHologram* Self, const FHitResult& Hit)
			{
				if (bSnapped)
				{
					RMRoadMode::Follow(Self, Hit);
				}
			});
#endif
	}
}
