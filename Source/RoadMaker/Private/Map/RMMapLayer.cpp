// Road layer on the game's map (1.3.0; drawn by the GPU since 1.4.0).
//
// The map widget nests the map picture in Overlay_0 (1024 x 1024 local "map units") inside
// mMapScrollContainer (render translation = pan) inside mMapZoomContainer (render scale 1..8 = zoom).
// Overlay_0 holds: mMap (the map picture, fog of war in its material), BPW_MapAreaPainter (map areas) and
// mMapObjectPanel (icons). We add our widget to that overlay directly above mMap (its ZOrder, added after it),
// so the game pans and zooms it with the map and everything else, the game's or another mod's, draws on top:
// roads are part of the ground. A world position maps to
// UFGMapFunctionLibrary::GetNormalizedPosition(capture actor, location) * 1024 in that square.
//
// Drawing (1.4.0, after FactoryAtlas's map painter): every road piece of ~4 m is one 16-byte record in a Slate
// instance buffer, drawn as a quad by the UI material M_RM_MapRoads (centre, angle, half length / width, colour
// indices into a 256-colour palette texture, side lines). The window transform rides on the quad's 4 vertices, so
// panning and zooming upload nothing; the records are uploaded again only when a road changes. Intersections
// (outlines that are no strip) are flat polygons with a 1 px soft rim, built on the CPU each frame (few of them).
// Everything is drawn lowest first, so a bridge covers what it passes over: road records in height order, split
// into runs between the intersections that lie between them in height.
// Pieces are read while the map is open: new, changed or gone pieces are found every 0.25 s (spread over frames
// with a time budget), then the draw list is rebuilt.
#include "RoadMakerModule.h"
#if !WITH_EDITOR
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SLeafWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/ISlateRHIRendererModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "SlateMaterialBrush.h"
#include "UObject/Package.h"
#include "FGMapFunctionLibrary.h"
#include "FGMinimapCaptureActor.h"
#include "UObject/UObjectIterator.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "RMGeometry.h"
#include "Map/RMMapRoadsRow.h"

namespace RMMapLayer
{
	namespace
	{
		constexpr double MapBuildBudget = 0.003; // s of piece reading per frame while the map is open
		constexpr double MapScanInterval = 0.25; // s between looks for new / changed / removed pieces
		constexpr float MapLineWidthM = 1.5f; // side lines on the map (m), at least 1 px
		constexpr int32 MapPaletteSize = 256;
		const TCHAR* MapMaterialPath = TEXT("/RoadMaker/Materials/M_RM_MapRoads.M_RM_MapRoads");

		/** An intersection, flattened to map units (0..1024). */
		struct FRMMapIsect
		{
			float Z = 0.f;
			FVector2D Centre = FVector2D::ZeroVector;
			TArray<FVector2D> Fill;               // outline
			TArray<TArray<FVector2D>> Lines;      // side lines along the curbs
			FColor FillC, EdgeC;
		};

		/** One road (records) or intersection. */
		struct FRMMapPiece
		{
			uint32 Key = 0;
			TArray<FVector4f> Recs;
			TArray<float> RecZ;
			TOptional<FRMMapIsect> Isect;
		};

		/** A run of the draw list: road records [Start, Start + Count) or intersections [Start, Start + Count). */
		struct FRMMapBatch
		{
			bool bIsect = false;
			int32 Start = 0;
			int32 Count = 0;
		};

		TWeakObjectPtr<UWorld> GMapWorld;
		TWeakObjectPtr<AActor> GMapCapture;
		// Map units = (Origin + AxisX * world X + AxisY * world Y) * 1024.
		FVector2D GMapOrigin, GMapAxisX, GMapAxisY;
		double GMapUnitsPerMetre = 0.0;

		TMap<TWeakObjectPtr<AActor>, FRMMapPiece> GMapPieces;
		bool GMapChanged = false;

		// Draw list (rebuilt when a piece changes).
		TArray<FVector4f> GMapRecords;
		TArray<FRMMapIsect> GMapIsects;
		TArray<FRMMapBatch> GMapBatches;
		bool GMapUploadPending = false;

		// Palette: colour -> index (stable while the world lives).
		TMap<uint32, uint8> GMapPaletteIndex;
		TArray<FColor> GMapPaletteColours;
		bool GMapPaletteDirty = false;

		// GPU side (made on the first paint).
		UMaterialInstanceDynamic* GMapMaterial = nullptr;
		UTexture2D* GMapPalette = nullptr;
		UTexture2D* GMapWhite = nullptr;
		TSharedPtr<FSlateMaterialBrush> GMapBrush;
		FSlateBrush GMapWhiteBrush;
		FSlateResourceHandle GMapHandle, GMapWhiteHandle;
		TSharedPtr<ISlateUpdatableInstanceBuffer> GMapInstances;
		int32 GMapInstanceCapacity = 0;
		bool GMapGpuFailed = false;

		class SRMMapRoadsLayer;
		TSharedPtr<SRMMapRoadsLayer> GMapWidget;
		TWeakPtr<SOverlay> GMapHost;

		FVector2D MapUnits(const FVector& W)
		{
			return (GMapOrigin + GMapAxisX * W.X + GMapAxisY * W.Y) * 1024.0;
		}

		void MapReset()
		{
			GMapPieces.Reset();
			GMapRecords.Reset();
			GMapIsects.Reset();
			GMapBatches.Reset();
			GMapChanged = false;
			GMapUploadPending = false;
			GMapCapture.Reset();
			GMapUnitsPerMetre = 0.0;
		}

		/** World -> map units, measured from the game's own function (handles any offset / flip). */
		bool MapUpdateMapping(UWorld* World)
		{
			AFGMinimapCaptureActor* Capture = UFGMapFunctionLibrary::GetMinimapCaptureActor(World);
			if (!Capture)
			{
				return false;
			}
			if (GMapCapture.Get() == Capture && GMapUnitsPerMetre > 0.0)
			{
				return true;
			}
			const double D = 100000.0;
			const FVector2D N0 = UFGMapFunctionLibrary::GetNormalizedPosition(Capture, FVector(0, 0, 0));
			const FVector2D NX = UFGMapFunctionLibrary::GetNormalizedPosition(Capture, FVector(D, 0, 0));
			const FVector2D NY = UFGMapFunctionLibrary::GetNormalizedPosition(Capture, FVector(0, D, 0));
			GMapOrigin = N0;
			GMapAxisX = (NX - N0) / D;
			GMapAxisY = (NY - N0) / D;
			GMapUnitsPerMetre = GMapAxisX.Size() * 1024.0 * 100.0;
			if (GMapUnitsPerMetre <= 0.0)
			{
				return false;
			}
			GMapCapture = Capture;
			UE_LOG(LogRoadMaker, Display, TEXT("RMMapLayer: mapping origin=%s axisX=%s axisY=%s %.5f units per metre"),
				*GMapOrigin.ToString(), *(GMapAxisX * D).ToString(), *(GMapAxisY * D).ToString(), GMapUnitsPerMetre);
			return true;
		}

		/** A paint colour as the map can show it: HDR / negative values brought into 0..1, sRGB. */
		FColor MapPaintColour(const FLinearColor& C)
		{
			FLinearColor L(FMath::Max(C.R, 0.f), FMath::Max(C.G, 0.f), FMath::Max(C.B, 0.f), 1.f);
			const float Peak = FMath::Max3(L.R, L.G, L.B);
			if (Peak > 1.f)
			{
				L.R /= Peak; L.G /= Peak; L.B /= Peak;
			}
			return L.ToFColor(true);
		}

		/** Fill by look (cyberpunk: its primary colour); side lines in the secondary colour. */
		void MapPieceColours(const ARMBuildableProcedural* Piece, FColor& OutFill, FColor& OutEdge)
		{
			FLinearColor Primary, Secondary;
			Piece->GetPaintColours(Primary, Secondary);
			switch (Piece->GetLook())
			{
			case 1: // concrete blocks
				OutFill = FColor(150, 148, 140, 255);
				break;
			case 2: // cyberpunk
				OutFill = MapPaintColour(Primary);
				break;
			default: // asphalt
				OutFill = FColor(55, 57, 62, 255);
				break;
			}
			OutEdge = MapPaintColour(Secondary);
		}

		uint32 MapColourKey(const ARMBuildableProcedural* Piece)
		{
			FColor Fill, Edge;
			MapPieceColours(Piece, Fill, Edge);
			return HashCombine(Fill.DWColor(), Edge.DWColor());
		}

		uint32 MapRoadKey(const ARMBuildableRoad* Road)
		{
			uint32 Key = HashCombine(GetTypeHash(Road->GetActorLocation()), GetTypeHash(Road->GetActorRotation().Yaw));
			Key = HashCombine(Key, Road->GetWidthInFoundations() * 131u + Road->GetLook() + (Road->GetMarkings().bEdgeLines ? 7u : 0u));
			Key = HashCombine(Key, MapColourKey(Road));
			for (const FRMSplinePoint& P : Road->GetSplinePoints())
			{
				Key = HashCombine(Key, HashCombine(GetTypeHash(P.Location), GetTypeHash(P.Tangent)));
			}
			return Key;
		}

		/** Palette index of a colour (0 = first colour seen; 256 colours, then the nearest is reused). */
		uint8 MapPaletteOf(const FColor& C)
		{
			const uint32 Key = C.DWColor() | 0xFF000000u;
			if (const uint8* Found = GMapPaletteIndex.Find(Key))
			{
				return *Found;
			}
			if (GMapPaletteColours.Num() < MapPaletteSize)
			{
				const uint8 Index = uint8(GMapPaletteColours.Num());
				GMapPaletteColours.Add(FColor(C.R, C.G, C.B, 255));
				GMapPaletteIndex.Add(Key, Index);
				GMapPaletteDirty = true;
				return Index;
			}
			int32 Best = 0;
			int32 BestD = MAX_int32;
			for (int32 I = 0; I < GMapPaletteColours.Num(); ++I)
			{
				const FColor& P = GMapPaletteColours[I];
				const int32 D = FMath::Square(int32(P.R) - C.R) + FMath::Square(int32(P.G) - C.G) + FMath::Square(int32(P.B) - C.B);
				if (D < BestD)
				{
					BestD = D;
					Best = I;
				}
			}
			return uint8(Best);
		}

		/** One road piece's record (see M_RM_MapRoads in make_materials.py for the layout). */
		FVector4f MapRecord(const FVector2D& Centre, double AngleRad, float HalfLenM, float HalfWidthM, uint8 Fill, uint8 Edge, uint8 Flags)
		{
			double Turn = FMath::Fmod(AngleRad / (2.0 * PI), 1.0);
			if (Turn < 0.0)
			{
				Turn += 1.0;
			}
			const uint32 Ang = uint32(FMath::RoundToInt(Turn * 1024.0)) & 1023u;
			const uint32 HL = uint32(FMath::Clamp(FMath::RoundToInt(HalfLenM / 0.05f), 1, 127));
			const uint32 HW = uint32(FMath::Clamp(FMath::RoundToInt(HalfWidthM / 0.25f), 1, 127));
			const uint32 Z = Ang * 16384u + HL * 128u + HW;
			const uint32 W = uint32(Fill) * 65536u + uint32(Edge) * 256u + Flags;
			return FVector4f(float(Centre.X), float(Centre.Y), float(Z), float(W));
		}

		void MapBuildRoad(const ARMBuildableRoad* Road, FRMMapPiece& Out)
		{
			TArray<FRMFrame> Frames;
			Road->GetWorldFrames(Frames, 400.f);
			if (Frames.Num() < 2)
			{
				return;
			}
			FColor FillC, EdgeC;
			MapPieceColours(Road, FillC, EdgeC);
			const uint8 Fill = MapPaletteOf(FillC);
			const uint8 Edge = MapPaletteOf(EdgeC);
			const uint8 Flags = Road->GetMarkings().bEdgeLines ? 1 : 0;
			const float HalfW = Road->GetRoadWidth() * 0.005f; // m
			const int32 Num = Frames.Num() - 1;
			TArray<double> Angles;
			Angles.SetNum(Num);
			for (int32 I = 0; I < Num; ++I)
			{
				const FVector2D D = MapUnits(Frames[I + 1].Pos) - MapUnits(Frames[I].Pos);
				Angles[I] = FMath::Atan2(D.Y, D.X);
			}
			for (int32 I = 0; I < Num; ++I)
			{
				const FRMFrame& A = Frames[I];
				const FRMFrame& B = Frames[I + 1];
				const float Len = FVector::Dist2D(A.Pos, B.Pos) * 0.01f; // m
				// On a bend each strip reaches into its neighbour far enough to close the wedge on the outer side.
				float Reach = 0.f;
				for (int32 N : { I - 1, I + 1 })
				{
					if (N >= 0 && N < Num)
					{
						const double Turn = FMath::Abs(FMath::UnwindRadians(Angles[N] - Angles[I]));
						Reach = FMath::Max(Reach, HalfW * float(FMath::Tan(FMath::Min(Turn, 1.2) * 0.5)) + 0.1f);
					}
				}
				const FVector2D Centre = (MapUnits(A.Pos) + MapUnits(B.Pos)) * 0.5;
				Out.Recs.Add(MapRecord(Centre, Angles[I], Len * 0.5f + Reach, HalfW, Fill, Edge, Flags));
				Out.RecZ.Add((A.Pos.Z + B.Pos.Z) * 0.5f);
			}
		}

		void MapBuildIntersection(const ARMBuildableIntersection* Isect, FRMMapPiece& Out)
		{
			TArray<FVector> Outline;
			TArray<bool> IsMouth;
			if (!Isect->GetWorldOutline(Outline, IsMouth) || Outline.Num() < 3)
			{
				return;
			}
			FRMMapIsect& X = Out.Isect.Emplace();
			FVector Centre = FVector::ZeroVector;
			for (const FVector& P : Outline)
			{
				Centre += P;
			}
			Centre /= Outline.Num();
			X.Centre = MapUnits(Centre);
			X.Z = Centre.Z;
			const float Inset = MapLineWidthM * 50.f; // line centre half a line width inside the curb (cm)
			TArray<FVector2D> Inner;
			for (const FVector& P : Outline)
			{
				X.Fill.Add(MapUnits(P));
				Inner.Add(MapUnits(P + (Centre - P).GetSafeNormal2D() * Inset));
			}
			// Side lines along the curbs (not the mouths, where roads continue), if switched on.
			TArray<FVector2D> Run;
			for (int32 I = 0; Isect->HasSideLines() && I < Inner.Num(); ++I)
			{
				if (IsMouth.IsValidIndex(I) && IsMouth[I])
				{
					if (Run.Num() >= 2) { X.Lines.Add(Run); }
					Run.Reset();
					continue;
				}
				if (Run.Num() == 0) { Run.Add(Inner[I]); }
				Run.Add(Inner[(I + 1) % Inner.Num()]);
			}
			if (Run.Num() >= 2) { X.Lines.Add(Run); }
			MapPieceColours(Isect, X.FillC, X.EdgeC);
		}

		/**
		 * Finds new, changed and removed pieces. Building a piece's shape stops when the time budget is used:
		 * returns false then (the rest is picked up next frame).
		 */
		bool MapScanPieces(UWorld* World)
		{
			TSet<AActor*> Seen;
			const double Deadline = FPlatformTime::Seconds() + MapBuildBudget;
			bool bComplete = true;
			int32 Built = 0;
			auto Visit = [&](AActor* Actor, uint32 Key, TFunctionRef<void(FRMMapPiece&)> Build)
			{
				Seen.Add(Actor);
				FRMMapPiece* Old = GMapPieces.Find(Actor);
				if (Old && Old->Key == Key)
				{
					return;
				}
				if (Built > 0 && FPlatformTime::Seconds() > Deadline)
				{
					bComplete = false; // keeps the old shape (if any) until its turn comes
					return;
				}
				++Built;
				FRMMapPiece Piece;
				Piece.Key = Key;
				Build(Piece);
				GMapPieces.Add(Actor, MoveTemp(Piece));
				GMapChanged = true;
			};
			for (TActorIterator<ARMBuildableRoad> It(World); It; ++It)
			{
				ARMBuildableRoad* Road = *It;
				if (IsValid(Road) && !Road->IsActorBeingDestroyed())
				{
					Visit(Road, MapRoadKey(Road), [Road](FRMMapPiece& P) { MapBuildRoad(Road, P); });
				}
			}
			for (TActorIterator<ARMBuildableIntersection> It(World); It; ++It)
			{
				ARMBuildableIntersection* Isect = *It;
				if (IsValid(Isect) && !Isect->IsActorBeingDestroyed())
				{
					Visit(Isect, HashCombine(Isect->GetShapeKey(), HashCombine(MapColourKey(Isect), Isect->HasSideLines() ? 1u : 0u)), [Isect](FRMMapPiece& P) { MapBuildIntersection(Isect, P); });
				}
			}
			for (auto It = GMapPieces.CreateIterator(); It; ++It)
			{
				AActor* Actor = It.Key().Get();
				if (!Actor || !Seen.Contains(Actor))
				{
					It.RemoveCurrent();
					GMapChanged = true;
				}
			}
			return bComplete;
		}

		/** Every road record and intersection, lowest first, cut into runs for drawing. */
		void MapRebuild()
		{
			GMapChanged = false;
			struct FItem { float Z; int32 Index; bool bIsect; };
			TArray<FItem> Items;
			TArray<const FVector4f*> RecPtrs;
			TArray<const FRMMapIsect*> IsectPtrs;
			for (const TPair<TWeakObjectPtr<AActor>, FRMMapPiece>& P : GMapPieces)
			{
				for (int32 I = 0; I < P.Value.Recs.Num(); ++I)
				{
					Items.Add({ P.Value.RecZ[I], RecPtrs.Num(), false });
					RecPtrs.Add(&P.Value.Recs[I]);
				}
				if (P.Value.Isect.IsSet())
				{
					Items.Add({ P.Value.Isect->Z, IsectPtrs.Num(), true });
					IsectPtrs.Add(&P.Value.Isect.GetValue());
				}
			}
			Items.StableSort([](const FItem& A, const FItem& B) { return A.Z < B.Z; });
			GMapRecords.Reset(RecPtrs.Num());
			GMapIsects.Reset(IsectPtrs.Num());
			GMapBatches.Reset();
			for (const FItem& It : Items)
			{
				if (GMapBatches.Num() == 0 || GMapBatches.Last().bIsect != It.bIsect)
				{
					FRMMapBatch& B = GMapBatches.AddDefaulted_GetRef();
					B.bIsect = It.bIsect;
					B.Start = It.bIsect ? GMapIsects.Num() : GMapRecords.Num();
				}
				if (It.bIsect)
				{
					GMapIsects.Add(*IsectPtrs[It.Index]);
				}
				else
				{
					GMapRecords.Add(*RecPtrs[It.Index]);
				}
				++GMapBatches.Last().Count;
			}
			GMapUploadPending = true; // the widget paints every frame and uploads on its next paint
			UE_LOG(LogRoadMaker, Verbose, TEXT("RMMapLayer: %d road records, %d intersections, %d draw runs"), GMapRecords.Num(), GMapIsects.Num(), GMapBatches.Num());
		}

		// ---- GPU ------------------------------------------------------------------------------------

		ISlateRHIRendererModule& MapRenderer()
		{
			return FModuleManager::Get().LoadModuleChecked<ISlateRHIRendererModule>(TEXT("SlateRHIRenderer"));
		}

		bool MapEnsureGpu()
		{
			if (GMapHandle.IsValid() && GMapWhiteHandle.IsValid())
			{
				return true;
			}
			if (GMapGpuFailed)
			{
				return false;
			}
			UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, MapMaterialPath);
			if (!Parent)
			{
				GMapGpuFailed = true;
				UE_LOG(LogRoadMaker, Warning, TEXT("RMMapLayer: %s not found: no roads on the map"), MapMaterialPath);
				return false;
			}
			if (!GMapPalette)
			{
				GMapPalette = UTexture2D::CreateTransient(MapPaletteSize, 1, PF_B8G8R8A8);
				GMapPalette->Filter = TF_Nearest;
				GMapPalette->SRGB = true;
				GMapPalette->AddToRoot();
				GMapPalette->UpdateResource();
				GMapPaletteDirty = true;
			}
			if (!GMapWhite)
			{
				GMapWhite = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
				FTexture2DMipMap& Mip = GMapWhite->GetPlatformData()->Mips[0];
				void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
				FMemory::Memset(Data, 0xFF, 4 * 4 * sizeof(FColor));
				Mip.BulkData.Unlock();
				GMapWhite->AddToRoot();
				GMapWhite->UpdateResource();
				GMapWhiteBrush.SetResourceObject(GMapWhite);
				GMapWhiteBrush.ImageSize = FVector2D(4, 4);
				GMapWhiteBrush.DrawAs = ESlateBrushDrawType::Image;
			}
			GMapMaterial = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
			GMapMaterial->AddToRoot();
			GMapMaterial->SetTextureParameterValue(TEXT("Palette"), GMapPalette);
			GMapBrush = MakeShared<FSlateMaterialBrush>(*GMapMaterial, FVector2D(64.0, 64.0));
			GMapHandle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*GMapBrush);
			GMapWhiteHandle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(GMapWhiteBrush);
			UE_LOG(LogRoadMaker, Display, TEXT("RMMapLayer: GPU map roads ready (material %s, white %s)"),
				GMapHandle.IsValid() ? TEXT("ok") : TEXT("FAILED"), GMapWhiteHandle.IsValid() ? TEXT("ok") : TEXT("FAILED"));
			return GMapHandle.IsValid() && GMapWhiteHandle.IsValid();
		}

		void MapUploadPalette()
		{
			if (!GMapPaletteDirty || !GMapPalette)
			{
				return;
			}
			GMapPaletteDirty = false;
			const int32 Bytes = MapPaletteSize * sizeof(FColor);
			uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Bytes));
			FMemory::Memzero(Copy, Bytes);
			FMemory::Memcpy(Copy, GMapPaletteColours.GetData(), GMapPaletteColours.Num() * sizeof(FColor));
			FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, MapPaletteSize, 1);
			GMapPalette->UpdateTextureRegions(0, 1, Region, MapPaletteSize * sizeof(FColor), sizeof(FColor), Copy,
				[](uint8* Data, const FUpdateTextureRegion2D* R) { FMemory::Free(Data); delete R; });
		}

		void MapUploadRecords()
		{
			if (!GMapUploadPending)
			{
				return;
			}
			GMapUploadPending = false;
			if (GMapRecords.Num() == 0)
			{
				return;
			}
			if (!GMapInstances.IsValid() || GMapRecords.Num() > GMapInstanceCapacity)
			{
				GMapInstanceCapacity = FMath::Max<int32>(1024, int32(FMath::RoundUpToPowerOfTwo(uint32(GMapRecords.Num()))));
				GMapInstances = MapRenderer().CreateInstanceBuffer(GMapInstanceCapacity);
			}
			FSlateInstanceBufferData Data = GMapRecords;
			GMapInstances->Update(Data);
		}

		/** Intersections of one run as flat triangles in window space, with a 1 px soft rim; curb lines on top. */
		void MapIsectVerts(int32 Start, int32 Count, const FVector2f& Origin, const FVector2f& Scale, float PxPerMetre,
			TArray<FSlateVertex>& V, TArray<SlateIndex>& I)
		{
			auto ToPx = [&](const FVector2D& M) { return Origin + Scale * FVector2f(M / 1024.0); };
			auto Vert = [&](const FVector2f& P, const FColor& C)
			{
				FSlateVertex& Out = V.AddZeroed_GetRef();
				Out.TexCoords[0] = 0.5f; Out.TexCoords[1] = 0.5f; Out.TexCoords[2] = 1.f; Out.TexCoords[3] = 1.f;
				Out.MaterialTexCoords = FVector2f(0.5f, 0.5f);
				Out.Position = P;
				Out.Color = C;
				Out.SecondaryColor = FColor::White;
				return SlateIndex(V.Num() - 1);
			};
			const float LineHalf = FMath::Max(MapLineWidthM * PxPerMetre * 0.5f, 0.5f);
			for (int32 K = Start; K < Start + Count; ++K)
			{
				const FRMMapIsect& X = GMapIsects[K];
				const int32 N = X.Fill.Num();
				const FVector2f C = ToPx(X.Centre);
				const SlateIndex Mid = Vert(C, X.FillC);
				FColor Clear = X.FillC;
				Clear.A = 0;
				const SlateIndex First = SlateIndex(V.Num());
				for (int32 J = 0; J < N; ++J)
				{
					const FVector2f P = ToPx(X.Fill[J]);
					Vert(P, X.FillC);
					Vert(P + (P - C).GetSafeNormal() * 1.f, Clear);
				}
				for (int32 J = 0; J < N; ++J)
				{
					const SlateIndex A = First + 2 * J, B = First + 2 * ((J + 1) % N);
					I.Append({ Mid, A, B });                // fill
					I.Append({ A, SlateIndex(A + 1), SlateIndex(B + 1), A, SlateIndex(B + 1), B }); // rim
				}
				if (LineHalf * 2.f < 1.f || X.Lines.Num() == 0)
				{
					continue;
				}
				for (const TArray<FVector2D>& Line : X.Lines)
				{
					for (int32 J = 0; J + 1 < Line.Num(); ++J)
					{
						const FVector2f A = ToPx(Line[J]), B = ToPx(Line[J + 1]);
						const FVector2f D = (B - A).GetSafeNormal();
						const FVector2f Nrm(-D.Y, D.X);
						const SlateIndex Q = Vert(A - Nrm * LineHalf, X.EdgeC);
						Vert(A + Nrm * LineHalf, X.EdgeC);
						Vert(B + Nrm * LineHalf, X.EdgeC);
						Vert(B - Nrm * LineHalf, X.EdgeC);
						I.Append({ Q, SlateIndex(Q + 1), SlateIndex(Q + 2), Q, SlateIndex(Q + 2), SlateIndex(Q + 3) });
					}
				}
			}
		}

		/** Our layer: covers Overlay_0 (the 1024 x 1024 map square), draws the roads in it. */
		class SRMMapRoadsLayer : public SLeafWidget
		{
		public:
			SLATE_BEGIN_ARGS(SRMMapRoadsLayer) {}
			SLATE_END_ARGS()

			void Construct(const FArguments&)
			{
				// Painted every frame (pan / zoom are render transforms of the containers above us).
				ForceVolatile(true);
			}

			virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

			virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
				FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
			{
				if (GMapBatches.Num() == 0 || GMapUnitsPerMetre <= 0.0 || !MapEnsureGpu())
				{
					return LayerId;
				}
				MapUploadPalette();
				MapUploadRecords();
				static float AppliedUnitsPerMetre = -1.f;
				if (AppliedUnitsPerMetre != float(GMapUnitsPerMetre))
				{
					AppliedUnitsPerMetre = float(GMapUnitsPerMetre);
					GMapMaterial->SetScalarParameterValue(TEXT("UnitsPerMetre"), AppliedUnitsPerMetre);
				}

				// Map square (0..1) -> window pixels: origin and size from the transform of the map's containers.
				const FVector2f Size(AllottedGeometry.GetLocalSize());
				const FSlateRenderTransform& T = AllottedGeometry.GetAccumulatedRenderTransform();
				const FVector2f Origin = T.TransformPoint(FVector2f::ZeroVector);
				const FVector2f Scale(T.TransformPoint(FVector2f(Size.X, 0.f)).X - Origin.X, T.TransformPoint(FVector2f(0.f, Size.Y)).Y - Origin.Y);
				const float PxPerMetre = FMath::Abs(Scale.X) * float(GMapUnitsPerMetre) / 1024.f;

				static const TArray<SlateIndex> QuadIndices = { 0, 1, 2, 0, 2, 3 };
				TArray<FSlateVertex> Quad;
				Quad.SetNumZeroed(4);
				const FVector2f Corners[4] = { { -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } };
				for (int32 K = 0; K < 4; ++K)
				{
					FSlateVertex& Vx = Quad[K];
					Vx.TexCoords[0] = Scale.X;
					Vx.TexCoords[1] = Scale.Y;
					Vx.TexCoords[2] = Origin.X;
					Vx.TexCoords[3] = Origin.Y;
					Vx.MaterialTexCoords = Corners[K];
					Vx.Position = Corners[K];
					Vx.Color = FColor::White;
					Vx.SecondaryColor = FColor::White;
				}
				int32 Layer = LayerId;
				TArray<FSlateVertex> IV;
				TArray<SlateIndex> II;
				for (const FRMMapBatch& B : GMapBatches)
				{
					++Layer;
					if (!B.bIsect)
					{
						if (GMapInstances.IsValid())
						{
							FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, GMapHandle, Quad, QuadIndices, GMapInstances.Get(), uint32(B.Start), uint32(B.Count));
						}
						continue;
					}
					IV.Reset();
					II.Reset();
					MapIsectVerts(B.Start, B.Count, Origin, Scale, PxPerMetre, IV, II);
					if (II.Num() > 0)
					{
						FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, GMapWhiteHandle, IV, II, nullptr, 0, 0);
					}
				}
				return Layer;
			}
		};

		// ---- Widget ---------------------------------------------------------------------------------

		/**
		 * Puts our layer widget into the live slate overlay that holds the area painter (Overlay_0), directly above
		 * the map picture (mMap): roads are part of the ground, so everything else on the map, the game's and
		 * any other mod's, draws on top of them. Our slot takes mMap's own ZOrder; an overlay draws equal
		 * ZOrders in the order they were added, so we come right after mMap and before every other child,
		 * and whatever is added later (by a widget hook, with no ZOrder, or with that same ZOrder) is on top.
		 */
		void MapAttach(UUserWidget* Painter)
		{
			UPanelWidget* Parent = Painter->GetParent();
			TSharedPtr<SWidget> Live = Parent ? Parent->GetCachedWidget() : nullptr;
			if (!Live.IsValid() || Live->GetType() != FName(TEXT("SOverlay")))
			{
				return;
			}
			TSharedPtr<SOverlay> Host = StaticCastSharedPtr<SOverlay>(Live);
			if (GMapHost.Pin() == Host)
			{
				return;
			}
			// The map picture: the child named mMap, else the overlay's first child.
			TSharedPtr<SWidget> MapLive;
			for (int32 I = 0; I < Parent->GetChildrenCount(); ++I)
			{
				UWidget* Child = Parent->GetChildAt(I);
				if (Child && Child->GetFName() == FName(TEXT("mMap")))
				{
					MapLive = Child->GetCachedWidget();
					break;
				}
			}
			FChildren* Children = Host->GetChildren();
			if (!Children || Children->Num() == 0)
			{
				return;
			}
			int32 ZOrder = INDEX_NONE;
			for (int32 I = 0; I < Children->Num(); ++I)
			{
				if (MapLive.IsValid() ? Children->GetChildAt(I) == MapLive : I == 0)
				{
					ZOrder = static_cast<const SOverlay::FOverlaySlot&>(Children->GetSlotAt(I)).GetZOrder();
					break;
				}
			}
			if (ZOrder == INDEX_NONE)
			{
				UE_LOG(LogRoadMaker, Warning, TEXT("RMMapLayer: map picture not found in its overlay"));
				return;
			}
			if (!GMapWidget.IsValid())
			{
				GMapWidget = SNew(SRMMapRoadsLayer);
				GMapWidget->SetVisibility(EVisibility::HitTestInvisible);
			}
			if (TSharedPtr<SOverlay> OldHost = GMapHost.Pin())
			{
				OldHost->RemoveSlot(GMapWidget.ToSharedRef());
			}
			Host->AddSlot(ZOrder)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					GMapWidget.ToSharedRef()
				];
			GMapHost = Host;
			// The stack, bottom to top, to check the order in the log.
			TArray<FString> Stack;
			for (int32 I = 0; I < Children->Num(); ++I)
			{
				const TSharedRef<SWidget> W = Children->GetChildAt(I);
				FString Name = W == GMapWidget.ToSharedRef() ? TEXT("RoadMaker") : W->GetTypeAsString();
				if (W == MapLive)
				{
					Name = TEXT("mMap");
				}
				else if (W == Painter->GetCachedWidget())
				{
					Name = TEXT("AreaPainter");
				}
				else if (W->GetType() == FName(TEXT("SObjectWidget")))
				{
					for (int32 C = 0; C < Parent->GetChildrenCount(); ++C)
					{
						UWidget* Child = Parent->GetChildAt(C);
						if (Child && Child->GetCachedWidget() == W)
						{
							Name = Child->GetName();
						}
					}
				}
				Stack.Add(FString::Printf(TEXT("%s(%d)"), *Name, static_cast<const SOverlay::FOverlaySlot&>(Children->GetSlotAt(I)).GetZOrder()));
			}
			UE_LOG(LogRoadMaker, Display, TEXT("RMMapLayer: attached to %s above the map picture, bottom to top: %s"), *Parent->GetName(),
				*FString::Join(Stack, TEXT(", ")));
		}

		bool MapLayerTick(float)
		{
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Game && Context.World())
				{
					World = Context.World();
					break;
				}
			}
			if (World != GMapWorld.Get())
			{
				MapReset();
				GMapWorld = World;
			}
			if (!World)
			{
				return true;
			}
			// Every frame while there is work, otherwise every MapScanInterval.
			static double NextScan = 0.0;
			static bool bPending = false;
			static TWeakObjectPtr<UUserWidget> CachedPainter;
			const double Now = FPlatformTime::Seconds();
			if (!bPending && Now < NextScan)
			{
				return true;
			}
			NextScan = Now + MapScanInterval;
			UUserWidget* Painter = CachedPainter.Get();
			// A known painter that is hidden = the map is closed: nothing to look for. Every widget is only
			// searched when there is no painter yet (or it is gone), and every 5 s while hidden in case the game
			// made a new map widget (issue #3: this used to sweep every widget 4 times a second, by class name).
			static double NextHiddenRescan = 0.0;
			const bool bKnownHidden = Painter && Painter->GetWorld() == World && !Painter->IsVisible();
			if (!Painter || Painter->GetWorld() != World || (bKnownHidden && Now >= NextHiddenRescan))
			{
				NextHiddenRescan = Now + 5.0;
				static TWeakObjectPtr<UClass> PainterClass;
				UUserWidget* Found = nullptr;
				for (TObjectIterator<UUserWidget> It; It; ++It)
				{
					UUserWidget* W = *It;
					if (!W || W->IsTemplate() || W->GetWorld() != World)
					{
						continue;
					}
					UClass* C = W->GetClass();
					if (PainterClass.IsValid() ? C != PainterClass.Get() : !C->GetName().Contains(TEXT("MapAreaPainter")))
					{
						continue;
					}
					PainterClass = C;
					Found = W;
					if (W->IsVisible())
					{
						break;
					}
				}
				Painter = Found;
				CachedPainter = Painter;
			}
			if (!Painter || !Painter->IsVisible() || !MapUpdateMapping(World))
			{
				bPending = false;
				return true;
			}
			MapAttach(Painter);
			// "Show Roads on Map" (map side menu): off = the layer hidden and nothing painted.
			RMMapRoads::EnsureRow(World);
			if (GMapWidget.IsValid())
			{
				GMapWidget->SetVisibility(RMMapRoads::IsShown() ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
			}
			if (!RMMapRoads::IsShown())
			{
				bPending = false;
				return true;
			}
			const bool bScanned = MapScanPieces(World);
			// The draw list waits until every piece is known (no half-drawn network while a big save is first read).
			if (bScanned && GMapChanged)
			{
				MapRebuild();
			}
			bPending = !bScanned;
			return true;
		}
	}

	void InstallHooks()
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&MapLayerTick), 0.f);
	}
}
#endif
