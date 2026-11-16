#include "Templates/RMRoadTemplate.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Buildables/RMBuildableMarking.h"
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

		bool HasRoad(const AFGBlueprintHologram* Hologram)
		{
			const TArray<TWeakObjectPtr<ARMBuildableProcedural>>* Pieces = GPieces.Find(const_cast<AFGBlueprintHologram*>(Hologram));
			if (!Pieces)
			{
				return false;
			}
			for (const TWeakObjectPtr<ARMBuildableProcedural>& P : *Pieces)
			{
				const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(P.Get());
				if (Road && !Road->IsWalkingPath())
				{
					return true;
				}
			}
			return false;
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
			float HalfW = 1600.f, Height = 400.f; // the blueprint's half width and height
			TArray<FRMFrame> Frames; // the drawn curve (world), full length
			FVector LastEnd = FVector(TNumericLimits<float>::Max());
		};

		TMap<TWeakObjectPtr<AFGBlueprintHologram>, FState> GStates;

		/** While Road Mode places a tile: where each spawned piece goes. The game's loader finishes spawning every piece at
		 *  its blueprint place (which also draws it there); the hook on AActor::FinishSpawning swaps in these. */
		TMap<AActor*, FTransform> GPendingMoves;

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
		void MeasureBox(AFGBlueprintHologram* Self, FState& S)
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
				UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: arrow mesh at %s (box centre %s), rotation %s"),
					*DirT.GetLocation().ToString(), *Box.GetCenter().ToString(), *DirT.Rotator().ToString());
			}
			const FTransform Turn(FRotationMatrix::MakeFromXZ(Arrow, FVector::UpVector).ToQuat());
			S.RoadT = Turn * ActorT;
			Box = Box.TransformBy(Turn.Inverse());
			UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: forward = hologram %s (from the %s)"), *Arrow.ToString(), *ArrowFrom);
			S.X0 = Box.Min.X;
			S.L = Box.GetSize().X;
			S.Yc = Box.GetCenter().Y;
			S.HalfW = Box.GetExtent().Y;
			S.Height = Box.GetSize().Z;
			S.Zb = Box.Min.Z;
			UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: blueprint box X %.0f..%.0f, Y %.0f..%.0f, floor %.0f (descriptor %dx%dx%d = %.0f cm long)"),
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
			MeasureBox(Self, S);

			int32 Stretched = 0, Repeated = 0;
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
			// Markings on a road that is repeated as it is (a curved road in the blueprint) stay on it: they go where it goes.
			for (FRigid& R : S.Rigids)
			{
				const ARMBuildableMarking* Marking = Cast<ARMBuildableMarking>(R.Buildable.Get());
				if (!Marking)
				{
					continue;
				}
				const FVector At = Marking->GetActorLocation();
				const FRigid* Best = nullptr;
				float BestD = TNumericLimits<float>::Max();
				for (const FRigid& Other : S.Rigids)
				{
					const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Other.Buildable.Get());
					if (!Road)
					{
						continue;
					}
					TArray<FRMFrame> Frames;
					Road->GetWorldFrames(Frames, 50.f);
					for (const FRMFrame& F : Frames)
					{
						const FVector Off = At - F.Pos;
						if (FMath::Abs(FVector::DotProduct(Off, F.Right)) <= Road->GetRoadWidth() * 0.5f + 20.f && FMath::Abs(Off.Z) < 150.f
							&& Off.SizeSquared() < BestD)
						{
							BestD = Off.SizeSquared();
							Best = &Other;
						}
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
		 *  across the curve, turned with it. */
		FTransform MapRigid(const FState& S, const FTransform& Rel, int32 K)
		{
			const FVector P = Rel.GetLocation();
			const FRMFrame F = FrameAt(S.Frames, (P.X - S.X0) + K * S.L);
			const FQuat FrameRot = FMatrix(F.Fwd, F.Right, F.Up, FVector::ZeroVector).ToQuat();
			return FTransform(FrameRot * Rel.GetRotation(), F.Pos + F.Right * (P.Y - S.Yc) + F.Up * (P.Z - S.Zb), Rel.GetScale3D());
		}

		/** The same for one of the blueprint's repeated objects, following its road when it lies on one. */
		FTransform MapObject(const FState& S, const FTransform& Rel, int32 K, bool bHasParent, const FTransform& ParentRel)
		{
			return bHasParent ? Rel.GetRelativeTransform(ParentRel) * MapRigid(S, ParentRel, K) : MapRigid(S, Rel, K);
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

		/** How many tiles of the repeated objects fit whole on a curve Len long. */
		int32 RigidTiles(const FState& S, float Len)
		{
			int32 N = 0;
			while (S.RigidMaxX >= 0.f && N < MaxTiles && S.RigidMaxX + N * S.L <= Len + 50.f)
			{
				++N;
			}
			return N;
		}

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
				for (const FRigid& R : S.Rigids)
				{
					if (AFGBuildable* B = R.Buildable.Get())
					{
						TArray<FInventoryStack> Refund;
						IFGDismantleInterface::Execute_GetDismantleRefund(B, Refund, false);
						for (const FInventoryStack& Stack : Refund)
						{
							Cost.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems * Tiles;
						}
					}
				}
			}
			return Tiles;
		}

		void Update(AFGBlueprintHologram* Self, FState& S, const FVector& End)
		{
			if (FVector::DistSquared(End, S.LastEnd) < 25.f)
			{
				return;
			}
			S.LastEnd = End;
			const FVector Start = S.RoadT.TransformPosition(FVector(S.X0, S.Yc, S.Zb));
			const FVector StartDir = S.RoadT.GetUnitAxis(EAxis::X);
			TArray<FRMSplinePoint> Points;
			RMGeometry::MakeArc(Start, StartDir, End, Points);
			TArray<FRMFrame> Frames;
			RMGeometry::SampleSpline(Points, 25.f, Frames);
			if (Frames.Num() < 2 || S.L < 1.f)
			{
				return;
			}
			S.Frames = Frames;
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
					Tile->SetWorldTransform(MapObject(S, R.Rel, K, R.bHasParent, R.ParentRel));
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

		/** Second click: our stretched pieces built along the drawn curve, paid from the player's inventory. */
		bool Build(AFGBlueprintHologram* Self, const FState& S)
		{
			UWorld* World = Self->GetWorld();
			if (!World || S.Frames.Num() < 2)
			{
				return false;
			}
			if (!Self->HasAuthority())
			{
				UE_LOG(LogRoadMaker, Warning, TEXT("Road Mode: building is host only for now"));
				return false;
			}
			TArray<FPlan> Plans;
			TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
			const int32 Tiles = MakePlans(Self, S, Plans, Cost);
			if (Plans.Num() == 0 && Tiles == 0)
			{
				return false;
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
						return false;
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

			// Our pieces along the arrow: built along the curve.
			int32 Built = 0;
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

			// Everything else: the blueprint placed once per tile by the game's own loader, each piece moved onto the
			// curve before it begins play (its distance along and across, turned with the curve). Our pieces along the
			// arrow come along too and go again (built above, bent).
			AFGBlueprintSubsystem* Blueprints = AFGBlueprintSubsystem::Get(World);
			if (Tiles > 0 && Blueprints && Self->mBlueprintDescriptor)
			{
				for (int32 K = 0; K < Tiles; ++K)
				{
					TArray<AFGBuildable*> Spawned;
					TArray<AFGBuildable*> Drop;
					TArray<TPair<AFGBuildable*, FTransform>> Moves;
					int32 Logged = 0;
					// The loader finishes spawning each piece at its blueprint place after this callback: the moves are
					// made again just before they begin play.
					auto ApplyMoves = [&Moves, K]()
					{
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
						[&S, &Drop, &Moves, &Logged, K](AFGBuildable* B, int32 Index)
						{
							if (!B)
							{
								return;
							}
							const FTransform Rel = B->GetActorTransform().GetRelativeTransform(S.RoadT);
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
							const bool bHasParent = Match && Match->bHasParent;
							const FTransform Target = MapObject(S, Rel, K, bHasParent, bHasParent ? Match->ParentRel : FTransform::Identity);
							if (Logged++ < 3)
							{
								UE_LOG(LogRoadMaker, Display, TEXT("Road Mode:   tile %d: %s at %s (road space) -> %s"), K, *B->GetName(), *P.ToString(), *Target.GetLocation().ToString());
							}
							// A marking along the arrow: the road surface under it follows the curve too.
							if (ARMBuildableMarking* Marking = Cast<ARMBuildableMarking>(B))
							{
								if (!bHasParent && FMath::Abs(Rel.GetUnitAxis(EAxis::X).X) > 0.98f)
								{
									BendMarking(S, K, Marking, Rel, Target);
								}
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
					Built += Spawned.Num() - Drop.Num();
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: tile %d placed, %d pieces (%d along the arrow left out)"), K, Spawned.Num() - Drop.Num(), Drop.Num());
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %d pieces built under blueprint proxy %s"), Built, Proxy ? *Proxy->GetName() : TEXT("none"));
			return true;
		}

		bool IsRoadMode(const AFGBlueprintHologram* Self)
		{
			UClass* Mode = RoadModeClass();
			return Mode && Self && Self->IsCurrentBuildMode(Mode);
		}
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
				if (Mode && HasRoad(Self))
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
				const RMRoadMode::FState* S = RMRoadMode::GStates.Find(Self);
				if (!S || S->Step == 0)
				{
					RMRoadMode::Begin(Self);
				}
				else
				{
					if (RMRoadMode::Build(Self, *S))
					{
						RMRoadMode::Stop(Self);
					}
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
				if (S && S->Step == 1 && S->Frames.Num() > 1 && S->L > 1.f)
				{
					Scope.Override(FMath::Max(1, FMath::RoundToInt(S->Frames.Last().Dist / S->L)));
				}
			});
		// While drawing, the blueprint stays at its start and the curve follows the aim.
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::SetHologramLocationAndRotation, GetMutableDefault<AFGBlueprintHologram>(),
			[](AFGBlueprintHologram* Self, const FHitResult& Hit)
			{
				RMRoadMode::FState* S = RMRoadMode::GStates.Find(Self);
				if (!S || S->Step != 1)
				{
					return;
				}
				if (!RMRoadMode::IsRoadMode(Self))
				{
					RMRoadMode::Stop(Self);
					return;
				}
				Self->SetActorTransform(S->StartT);
				RMRoadMode::Update(Self, *S, Hit.Location);
			});
#endif
	}
}
