#include "Templates/RMRoadTemplate.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "Patching/NativeHookManager.h"
#include "RMGeometry.h"
#include "ProceduralMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "FGFactoryBlueprintTypes.h"

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
		};

		struct FRigid
		{
			TWeakObjectPtr<USceneComponent> Src;
			FTransform Rel; // source root, relative to the hologram at the first click
			TArray<TWeakObjectPtr<USceneComponent>> Tiles;
		};

		struct FState
		{
			int32 Step = 0;
			FTransform StartT;
			float X0 = 0.f, L = 0.f, Yc = 0.f, Zb = 0.f; // the blueprint box, hologram-local
			TArray<TPair<TWeakObjectPtr<USceneComponent>, bool>> Hidden;
			TArray<FBent> Bents;
			TArray<FRigid> Rigids;
			FVector LastEnd = FVector(TNumericLimits<float>::Max());
		};

		TMap<TWeakObjectPtr<AFGBlueprintHologram>, FState> GStates;

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
			if (!Box.IsValid || Box.GetSize().X < 100.f)
			{
				// No bounds mesh: the designer's size from the descriptor, centred on the hologram, floor at the lowest piece.
				float MinZ = TNumericLimits<float>::Max();
				for (const TPair<TObjectPtr<AFGBuildable>, TObjectPtr<USceneComponent>>& Pair : Self->mBuildableToNewRoot)
				{
					if (Pair.Value)
					{
						MinZ = FMath::Min(MinZ, Pair.Value->GetComponentTransform().GetRelativeTransform(ActorT).GetLocation().Z);
					}
				}
				const float HalfL = FMath::Max(DescL, RM::FoundationSize) * 0.5f;
				Box = FBox(FVector(-HalfL, -HalfL, MinZ < TNumericLimits<float>::Max() ? MinZ : 0.f), FVector(HalfL, HalfL, 0.f));
			}
			S.X0 = Box.Min.X;
			S.L = Box.GetSize().X;
			S.Yc = Box.GetCenter().Y;
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

			const FRMRoadTemplate T = RMRoadTemplate::Read(Self);
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

				// Ours, running the blueprint's whole length: bent along the curve.
				bool bStretch = false;
				if (T.Sources.Contains(Pair.Key.Get()))
				{
					FBox Span(ForceInit);
					for (USceneComponent* C : Children)
					{
						if (UProceduralMeshComponent* Proc = Cast<UProceduralMeshComponent>(C))
						{
							Span += Proc->CalcBounds(Proc->GetComponentTransform().GetRelativeTransform(S.StartT)).GetBox();
						}
					}
					bStretch = Span.IsValid && Span.Min.X <= S.X0 + 100.f && Span.Max.X >= S.X0 + S.L - 100.f;
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: %s spans X %.0f..%.0f: %s"), *Pair.Key->GetName(),
						Span.IsValid ? Span.Min.X : 0.f, Span.IsValid ? Span.Max.X : 0.f, bStretch ? TEXT("stretched") : TEXT("repeated"));
				}
				if (bStretch)
				{
					for (USceneComponent* C : Children)
					{
						UProceduralMeshComponent* Proc = Cast<UProceduralMeshComponent>(C);
						if (Proc && Proc->IsVisible() && Proc->GetNumSections() > 0)
						{
							FBent& B = S.Bents.AddDefaulted_GetRef();
							B.Src = Proc;
							B.Rel = Proc->GetComponentTransform().GetRelativeTransform(S.StartT);
						}
					}
					++Stretched;
				}
				else
				{
					FRigid& R = S.Rigids.AddDefaulted_GetRef();
					R.Src = Root;
					R.Rel = Root->GetComponentTransform().GetRelativeTransform(S.StartT);
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

		void Update(AFGBlueprintHologram* Self, FState& S, const FVector& End)
		{
			if (FVector::DistSquared(End, S.LastEnd) < 25.f)
			{
				return;
			}
			S.LastEnd = End;
			const FVector Start = S.StartT.TransformPosition(FVector(S.X0, S.Yc, S.Zb));
			const FVector StartDir = S.StartT.GetUnitAxis(EAxis::X);
			TArray<FRMSplinePoint> Points;
			RMGeometry::MakeArc(Start, StartDir, End, Points);
			TArray<FRMFrame> Frames;
			RMGeometry::SampleSpline(Points, 25.f, Frames);
			if (Frames.Num() < 2 || S.L < 1.f)
			{
				return;
			}
			const float Len = FMath::Min(Frames.Last().Dist, S.L * MaxTiles);

			// Where a hologram-space point of tile K goes (D = distance along the curve).
			auto Place = [&](const FVector& P, float D, FRMFrame& OutF) -> FVector
			{
				OutF = FrameAt(Frames, D);
				return OutF.Pos + OutF.Right * (P.Y - S.Yc) + OutF.Up * (P.Z - S.Zb);
			};

			// Stretched: whole tiles bent along the curve, scaled a little so they end exactly at the aim.
			const int32 N = FMath::Clamp(FMath::RoundToInt(Len / S.L), 1, MaxTiles);
			const float Scale = Len / (N * S.L);
			for (FBent& B : S.Bents)
			{
				UProceduralMeshComponent* Src = B.Src.Get();
				if (!Src)
				{
					continue;
				}
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

			// Repeated: each object stays rigid, placed at its own distance along and across the curve, tile after tile.
			const int32 Count = FMath::Clamp(FMath::CeilToInt(Len / S.L), 1, MaxTiles);
			for (FRigid& R : S.Rigids)
			{
				USceneComponent* Src = R.Src.Get();
				if (!Src)
				{
					continue;
				}
				const FVector P = R.Rel.GetLocation();
				for (int32 K = 0; K < FMath::Max(Count, R.Tiles.Num()); ++K)
				{
					const float D = (P.X - S.X0) + K * S.L;
					const bool bShow = K < Count && D <= Len + 1.f;
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
					FRMFrame F;
					const FVector World = Place(P, D, F);
					const FQuat FrameRot = FMatrix(F.Fwd, F.Right, F.Up, FVector::ZeroVector).ToQuat();
					Tile->SetWorldTransform(FTransform(FrameRot * R.Rel.GetRotation(), World, R.Rel.GetScale3D()));
				}
			}
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
					UE_LOG(LogRoadMaker, Display, TEXT("Road Mode: end clicked at %s (building it comes in the next step)"), *S->LastEnd.ToString());
					RMRoadMode::Stop(Self);
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
