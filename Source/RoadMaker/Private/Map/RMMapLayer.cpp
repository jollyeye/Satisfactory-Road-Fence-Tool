// Road layer on the game's map (1.3.0).
//
// The map widget nests the map picture in Overlay_0 (1024 x 1024 local "map units") inside
// mMapScrollContainer (render translation = pan) inside mMapZoomContainer (render scale 1..8 = zoom).
// Overlay_0 holds: mMap (the map picture), BPW_MapAreaPainter (fog / map areas) and mMapObjectPanel
// (icons). We add a canvas to that overlay right after the area painter, so the game pans and zooms it
// with the map and the icons stay on top. A world position maps to
// UFGMapFunctionLibrary::GetNormalizedPosition(capture actor, location) * 1024 in that square.
//
// The canvas holds one picture per 512 m tile that has roads (512 x 512 px, 1 px = 1 m, with mips).
// Roads and intersections are painted into the tiles lowest first, so a bridge covers what is under it.
// Only while the map is open: pieces that are new, changed or gone mark the tiles they touch (old and
// new place) dirty, and only those tiles are painted again; the first opening paints everything.
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
#include "Widgets/SCanvas.h"
#include "Widgets/Images/SImage.h"
#include "FGMapFunctionLibrary.h"
#include "FGMinimapCaptureActor.h"
#include "UObject/UObjectIterator.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "RMGeometry.h"

namespace RMMapLayer
{
	namespace
	{
		constexpr int32 MapTilePixels = 512;
		constexpr int32 MapSuperSample = 2; // painted at 2x, averaged down (anti-aliasing)
		constexpr int32 MapTileMips = 5; // 512 .. 32
		constexpr double MapTileWorld = 51200.0; // cm
		constexpr int32 MapTilesPerTick = 6;

		/** One road or intersection, flattened to map units (0..1024). */
		struct FRMMapPiece
		{
			uint32 Key = 0;
			float Z = 0.f;
			FBox2D Bounds = FBox2D(ForceInit);
			TArray<TArray<FVector2D>> Fills; // polygons
			TArray<TArray<FVector2D>> Edges; // open polylines
			FColor Fill;
			FColor Edge;
		};

		struct FRMMapTile
		{
			UTexture2D* Tex = nullptr;
			FSlateBrush Brush;
			TSharedPtr<SImage> Image;
		};

		TWeakObjectPtr<UWorld> GMapWorld;
		TWeakObjectPtr<AActor> GMapCapture;
		// Map units = (Origin + AxisX * world X + AxisY * world Y) * 1024.
		FVector2D GMapOrigin, GMapAxisX, GMapAxisY;
		double GMapTileUnits = 0.0; // tile size in map units

		TMap<TWeakObjectPtr<AActor>, FRMMapPiece> GMapPieces;
		TMap<FIntPoint, TUniquePtr<FRMMapTile>> GMapTiles;
		TSet<FIntPoint> GMapDirty;
		TSharedPtr<SCanvas> GMapCanvas;
		TWeakPtr<SOverlay> GMapHost;

		FVector2D MapUnits(const FVector& W)
		{
			return (GMapOrigin + GMapAxisX * W.X + GMapAxisY * W.Y) * 1024.0;
		}

		void MapReset()
		{
			if (GMapCanvas.IsValid())
			{
				GMapCanvas->ClearChildren();
			}
			for (TPair<FIntPoint, TUniquePtr<FRMMapTile>>& T : GMapTiles)
			{
				if (T.Value->Tex)
				{
					T.Value->Tex->RemoveFromRoot();
				}
			}
			GMapTiles.Reset();
			GMapPieces.Reset();
			GMapDirty.Reset();
			GMapCapture.Reset();
			GMapTileUnits = 0.0;
		}

		/** World -> map units, measured from the game's own function (handles any offset / flip). */
		bool MapUpdateMapping(UWorld* World)
		{
			AFGMinimapCaptureActor* Capture = UFGMapFunctionLibrary::GetMinimapCaptureActor(World);
			if (!Capture)
			{
				return false;
			}
			if (GMapCapture.Get() == Capture && GMapTileUnits > 0.0)
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
			GMapTileUnits = MapTileWorld * GMapAxisX.Size() * 1024.0;
			if (GMapTileUnits <= 0.0)
			{
				return false;
			}
			GMapCapture = Capture;
			UE_LOG(LogRoadMaker, Display, TEXT("RMMapLayer: mapping origin=%s axisX=%s axisY=%s tile=%.2f units"),
				*GMapOrigin.ToString(), *(GMapAxisX * D).ToString(), *(GMapAxisY * D).ToString(), GMapTileUnits);
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

		/** Side lines sit just inside the edge: half a map pixel (1 px = 1 m). */
		constexpr float MapEdgeInset = 50.f;

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

		void MapBuildRoad(const ARMBuildableRoad* Road, FRMMapPiece& Out)
		{
			TArray<FRMFrame> Frames;
			Road->GetWorldFrames(Frames, 400.f);
			if (Frames.Num() < 2)
			{
				return;
			}
			const float Half = Road->GetRoadWidth() * 0.5f;
			const float LineHalf = Half - MapEdgeInset;
			TArray<FVector2D> Left, Right, LeftLine, RightLine;
			double ZSum = 0.0;
			for (const FRMFrame& F : Frames)
			{
				Left.Add(MapUnits(F.Pos - F.Right * Half));
				Right.Add(MapUnits(F.Pos + F.Right * Half));
				LeftLine.Add(MapUnits(F.Pos - F.Right * LineHalf));
				RightLine.Add(MapUnits(F.Pos + F.Right * LineHalf));
				ZSum += F.Pos.Z;
			}
			for (int32 I = 0; I + 1 < Frames.Num(); ++I)
			{
				Out.Fills.Add({ Left[I], Right[I], Right[I + 1], Left[I + 1] });
			}
			for (const FVector2D& P : Left) { Out.Bounds += P; }
			for (const FVector2D& P : Right) { Out.Bounds += P; }
			if (Road->GetMarkings().bEdgeLines)
			{
				Out.Edges.Add(MoveTemp(LeftLine));
				Out.Edges.Add(MoveTemp(RightLine));
			}
			Out.Z = ZSum / Frames.Num();
			MapPieceColours(Road, Out.Fill, Out.Edge);
		}

		void MapBuildIntersection(const ARMBuildableIntersection* Isect, FRMMapPiece& Out)
		{
			TArray<FVector> Outline;
			TArray<bool> IsMouth;
			if (!Isect->GetWorldOutline(Outline, IsMouth))
			{
				return;
			}
			TArray<FVector2D> Poly, Inset;
			double ZSum = 0.0;
			FVector Centre = FVector::ZeroVector;
			for (const FVector& P : Outline)
			{
				Centre += P;
			}
			Centre /= Outline.Num();
			for (const FVector& P : Outline)
			{
				Poly.Add(MapUnits(P));
				Inset.Add(MapUnits(P + (Centre - P).GetSafeNormal2D() * MapEdgeInset));
				Out.Bounds += Poly.Last();
				ZSum += P.Z;
			}
			// Side lines along the curbs (not the mouths, where roads continue), if switched on.
			TArray<FVector2D> Run;
			for (int32 I = 0; Isect->HasSideLines() && I < Inset.Num(); ++I)
			{
				const bool bMouth = IsMouth.IsValidIndex(I) && IsMouth[I];
				if (bMouth)
				{
					if (Run.Num() >= 2) { Out.Edges.Add(Run); }
					Run.Reset();
					continue;
				}
				if (Run.Num() == 0) { Run.Add(Inset[I]); }
				Run.Add(Inset[(I + 1) % Inset.Num()]);
			}
			if (Run.Num() >= 2) { Out.Edges.Add(Run); }
			Out.Fills.Add(MoveTemp(Poly));
			Out.Z = ZSum / Outline.Num();
			MapPieceColours(Isect, Out.Fill, Out.Edge);
		}

		void MapMarkDirty(const FBox2D& Box)
		{
			if (!Box.bIsValid || GMapTileUnits <= 0.0)
			{
				return;
			}
			const int32 X0 = FMath::FloorToInt(Box.Min.X / GMapTileUnits), X1 = FMath::FloorToInt(Box.Max.X / GMapTileUnits);
			const int32 Y0 = FMath::FloorToInt(Box.Min.Y / GMapTileUnits), Y1 = FMath::FloorToInt(Box.Max.Y / GMapTileUnits);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					GMapDirty.Add(FIntPoint(X, Y));
				}
			}
		}

		/** Finds new, changed and removed roads / intersections; marks their tiles dirty. */
		void MapScanPieces(UWorld* World)
		{
			TSet<AActor*> Seen;
			auto Visit = [&](AActor* Actor, uint32 Key, TFunctionRef<void(FRMMapPiece&)> Build)
			{
				Seen.Add(Actor);
				FRMMapPiece* Old = GMapPieces.Find(Actor);
				if (Old && Old->Key == Key)
				{
					return;
				}
				if (Old)
				{
					MapMarkDirty(Old->Bounds);
				}
				FRMMapPiece Piece;
				Piece.Key = Key;
				Build(Piece);
				MapMarkDirty(Piece.Bounds);
				GMapPieces.Add(Actor, MoveTemp(Piece));
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
					MapMarkDirty(It.Value().Bounds);
					It.RemoveCurrent();
				}
			}
		}

		// ---- Painting -------------------------------------------------------------------------------

		void MapFillPolygon(TArray<FColor>& Px, int32 Size, const TArray<FVector2D>& Poly, const FColor& C)
		{
			if (Poly.Num() < 3)
			{
				return;
			}
			double MinY = Poly[0].Y, MaxY = Poly[0].Y;
			for (const FVector2D& P : Poly)
			{
				MinY = FMath::Min(MinY, P.Y);
				MaxY = FMath::Max(MaxY, P.Y);
			}
			const int32 Y0 = FMath::Max(0, FMath::FloorToInt(MinY));
			const int32 Y1 = FMath::Min(Size - 1, FMath::CeilToInt(MaxY));
			TArray<double, TInlineAllocator<16>> Xs;
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				const double SY = Y + 0.5;
				Xs.Reset();
				for (int32 I = 0, J = Poly.Num() - 1; I < Poly.Num(); J = I++)
				{
					const FVector2D& A = Poly[J];
					const FVector2D& B = Poly[I];
					if ((A.Y <= SY) != (B.Y <= SY))
					{
						Xs.Add(A.X + (SY - A.Y) / (B.Y - A.Y) * (B.X - A.X));
					}
				}
				Xs.Sort();
				for (int32 K = 0; K + 1 < Xs.Num(); K += 2)
				{
					const int32 XA = FMath::Max(0, FMath::CeilToInt(Xs[K] - 0.5));
					const int32 XB = FMath::Min(Size - 1, FMath::CeilToInt(Xs[K + 1] - 0.5) - 1);
					for (int32 X = XA; X <= XB; ++X)
					{
						Px[Y * Size + X] = C;
					}
				}
			}
		}

		/** A line Thick pixels wide (centred on A-B). */
		void MapDrawLine(TArray<FColor>& Px, int32 Size, const FVector2D& A, const FVector2D& B, int32 Thick, const FColor& C)
		{
			const int32 Steps = FMath::Max(1, FMath::CeilToInt((B - A).GetAbsMax() * 2.0));
			const double Back = Thick * 0.5;
			for (int32 S = 0; S <= Steps; ++S)
			{
				const FVector2D P = FMath::Lerp(A, B, double(S) / Steps);
				const int32 X0 = FMath::RoundToInt(P.X - Back), Y0 = FMath::RoundToInt(P.Y - Back);
				for (int32 Y = FMath::Max(0, Y0); Y < FMath::Min(Size, Y0 + Thick); ++Y)
				{
					for (int32 X = FMath::Max(0, X0); X < FMath::Min(Size, X0 + Thick); ++X)
					{
						Px[Y * Size + X] = C;
					}
				}
			}
		}

		/** Half the size: each pixel the average of 2 x 2, colours weighted by alpha (no dark fringes). */
		void MapDownsample(const TArray<FColor>& Src, int32 Size, TArray<FColor>& Out)
		{
			const int32 Half = Size / 2;
			Out.SetNumUninitialized(Half * Half);
			for (int32 Y = 0; Y < Half; ++Y)
			{
				for (int32 X = 0; X < Half; ++X)
				{
					const FColor* Q[4] = { &Src[(2 * Y) * Size + 2 * X], &Src[(2 * Y) * Size + 2 * X + 1],
						&Src[(2 * Y + 1) * Size + 2 * X], &Src[(2 * Y + 1) * Size + 2 * X + 1] };
					uint32 R = 0, G = 0, B = 0, A = 0;
					for (const FColor* C : Q)
					{
						R += C->R * C->A; G += C->G * C->A; B += C->B * C->A; A += C->A;
					}
					Out[Y * Half + X] = A > 0
						? FColor(uint8(R / A), uint8(G / A), uint8(B / A), uint8((A + 2) / 4))
						: FColor(0, 0, 0, 0);
				}
			}
		}

		/**
		 * Transparent pixels next to painted ones take their colour (alpha stays 0). The picture is
		 * filtered without premultiplied alpha, so a transparent black neighbour would darken every edge
		 * into a dark outline; with the colour bled out the edge only fades.
		 */
		void MapBleedEdges(TArray<FColor>& Px, int32 Size)
		{
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				const TArray<FColor> Src = Px;
				for (int32 Y = 0; Y < Size; ++Y)
				{
					for (int32 X = 0; X < Size; ++X)
					{
						FColor& Out = Px[Y * Size + X];
						if (Out.A != 0 || (Pass > 0 && (Out.R | Out.G | Out.B) != 0))
						{
							continue;
						}
						uint32 R = 0, G = 0, B = 0, N = 0;
						const int32 DX[4] = { -1, 1, 0, 0 };
						const int32 DY[4] = { 0, 0, -1, 1 };
						for (int32 K = 0; K < 4; ++K)
						{
							const int32 NX = X + DX[K], NY = Y + DY[K];
							if (NX < 0 || NY < 0 || NX >= Size || NY >= Size)
							{
								continue;
							}
							const FColor& C = Src[NY * Size + NX];
							if (C.A != 0 || (C.R | C.G | C.B) != 0)
							{
								R += C.R; G += C.G; B += C.B; ++N;
							}
						}
						if (N > 0)
						{
							Out = FColor(uint8(R / N), uint8(G / N), uint8(B / N), 0);
						}
					}
				}
			}
		}

		/** Writes the pixels (and box-filtered mips) into the tile's texture, creating it if needed. */
		void MapUpload(FRMMapTile& Tile, const TArray<FColor>& Pixels)
		{
			if (!Tile.Tex)
			{
				Tile.Tex = UTexture2D::CreateTransient(MapTilePixels, MapTilePixels, PF_B8G8R8A8, NAME_None);
				if (!Tile.Tex)
				{
					return;
				}
				Tile.Tex->AddToRoot();
				Tile.Tex->SRGB = true;
				Tile.Tex->Filter = TF_Trilinear;
				Tile.Tex->AddressX = TA_Clamp;
				Tile.Tex->AddressY = TA_Clamp;
				Tile.Tex->NeverStream = true;
				FTexturePlatformData* PD = Tile.Tex->GetPlatformData();
				for (int32 Level = PD->Mips.Num(); Level < MapTileMips; ++Level)
				{
					FTexture2DMipMap* Mip = new FTexture2DMipMap();
					Mip->SizeX = MapTilePixels >> Level;
					Mip->SizeY = MapTilePixels >> Level;
					Mip->BulkData.Lock(LOCK_READ_WRITE);
					Mip->BulkData.Realloc(int64(Mip->SizeX) * Mip->SizeY * sizeof(FColor));
					Mip->BulkData.Unlock();
					PD->Mips.Add(Mip);
				}
				Tile.Brush.SetResourceObject(Tile.Tex);
				Tile.Brush.ImageSize = FVector2D(MapTilePixels, MapTilePixels);
				Tile.Brush.DrawAs = ESlateBrushDrawType::Image;
			}
			FTexturePlatformData* PD = Tile.Tex->GetPlatformData();
			TArray<FColor> Level = Pixels;
			int32 Size = MapTilePixels;
			for (int32 L = 0; L < PD->Mips.Num(); ++L)
			{
				FTexture2DMipMap& Mip = PD->Mips[L];
				MapBleedEdges(Level, Size);
				void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
				FMemory::Memcpy(Data, Level.GetData(), FMath::Min<int64>(Mip.BulkData.GetBulkDataSize(), int64(Level.Num()) * sizeof(FColor)));
				Mip.BulkData.Unlock();
				if (L + 1 < PD->Mips.Num())
				{
					TArray<FColor> Next;
					MapDownsample(Level, Size, Next);
					Level = MoveTemp(Next);
					Size /= 2;
				}
			}
			Tile.Tex->UpdateResource();
		}

		void MapPaintTile(const FIntPoint& Key)
		{
			const FVector2D TileMin(Key.X * GMapTileUnits, Key.Y * GMapTileUnits);
			FBox2D TileBox(TileMin, TileMin + FVector2D(GMapTileUnits, GMapTileUnits));
			TArray<const FRMMapPiece*> Pieces;
			for (const TPair<TWeakObjectPtr<AActor>, FRMMapPiece>& P : GMapPieces)
			{
				if (P.Value.Bounds.bIsValid && P.Value.Bounds.Intersect(TileBox))
				{
					Pieces.Add(&P.Value);
				}
			}
			TUniquePtr<FRMMapTile>* Existing = GMapTiles.Find(Key);
			if (Pieces.Num() == 0)
			{
				if (Existing && (*Existing)->Image.IsValid())
				{
					(*Existing)->Image->SetVisibility(EVisibility::Collapsed);
				}
				return;
			}
			Pieces.Sort([](const FRMMapPiece& A, const FRMMapPiece& B) { return A.Z < B.Z; });

			// Painted at MapSuperSample x the tile resolution, then averaged down: smooth edges and lines.
			const int32 Big = MapTilePixels * MapSuperSample;
			const double Scale = Big / GMapTileUnits;
			auto ToPx = [&](const FVector2D& M) { return (M - TileMin) * Scale; };
			TArray<FColor> Pixels;
			Pixels.Init(FColor(0, 0, 0, 0), Big * Big);
			TArray<FVector2D> Poly;
			for (const FRMMapPiece* Piece : Pieces)
			{
				for (const TArray<FVector2D>& Fill : Piece->Fills)
				{
					Poly.Reset();
					for (const FVector2D& M : Fill) { Poly.Add(ToPx(M)); }
					MapFillPolygon(Pixels, Big, Poly, Piece->Fill);
				}
				for (const TArray<FVector2D>& Edge : Piece->Edges)
				{
					for (int32 I = 0; I + 1 < Edge.Num(); ++I)
					{
						MapDrawLine(Pixels, Big, ToPx(Edge[I]), ToPx(Edge[I + 1]), MapSuperSample, Piece->Edge);
					}
				}
			}
			for (int32 Size = Big; Size > MapTilePixels; Size /= 2)
			{
				TArray<FColor> Smaller;
				MapDownsample(Pixels, Size, Smaller);
				Pixels = MoveTemp(Smaller);
			}

			if (!Existing)
			{
				Existing = &GMapTiles.Add(Key, MakeUnique<FRMMapTile>());
			}
			FRMMapTile& Tile = **Existing;
			MapUpload(Tile, Pixels);
			if (!Tile.Tex)
			{
				return;
			}
			if (!Tile.Image.IsValid() && GMapCanvas.IsValid())
			{
				GMapCanvas->AddSlot()
					.Position(TileMin)
					.Size(FVector2D(GMapTileUnits, GMapTileUnits))
					[
						SAssignNew(Tile.Image, SImage)
						.Image(&Tile.Brush)
						.Visibility(EVisibility::HitTestInvisible)
					];
			}
			if (Tile.Image.IsValid())
			{
				Tile.Image->SetVisibility(EVisibility::HitTestInvisible);
				Tile.Image->Invalidate(EInvalidateWidgetReason::Paint);
			}
		}

		// ---- Widget ---------------------------------------------------------------------------------

		/** Puts our canvas into the live slate overlay that holds the area painter, right after it. */
		void MapAttach(UUserWidget* Painter)
		{
			UPanelWidget* Parent = Painter->GetParent();
			TSharedPtr<SWidget> Live = Parent ? Parent->GetCachedWidget() : nullptr;
			TSharedPtr<SWidget> PainterLive = Painter->GetCachedWidget();
			if (!Live.IsValid() || !PainterLive.IsValid() || Live->GetType() != FName(TEXT("SOverlay")))
			{
				return;
			}
			TSharedPtr<SOverlay> Host = StaticCastSharedPtr<SOverlay>(Live);
			if (GMapHost.Pin() == Host)
			{
				return;
			}
			int32 ZOrder = INDEX_NONE;
			FChildren* Children = Host->GetChildren();
			for (int32 I = 0; Children && I < Children->Num(); ++I)
			{
				if (Children->GetChildAt(I) == PainterLive)
				{
					ZOrder = static_cast<const SOverlay::FOverlaySlot&>(Children->GetSlotAt(I)).GetZOrder();
					break;
				}
			}
			if (ZOrder == INDEX_NONE)
			{
				UE_LOG(LogRoadMaker, Warning, TEXT("RMMapLayer: area painter not found in its overlay"));
				return;
			}
			if (!GMapCanvas.IsValid())
			{
				GMapCanvas = SNew(SCanvas).Visibility(EVisibility::HitTestInvisible);
			}
			if (TSharedPtr<SOverlay> OldHost = GMapHost.Pin())
			{
				OldHost->RemoveSlot(GMapCanvas.ToSharedRef());
			}
			Host->AddSlot(ZOrder)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					GMapCanvas.ToSharedRef()
				];
			GMapHost = Host;
			UE_LOG(LogRoadMaker, Display, TEXT("RMMapLayer: attached to %s after ZOrder %d"), *Parent->GetName(), ZOrder);
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
			UUserWidget* Painter = nullptr;
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				UUserWidget* W = *It;
				if (W && !W->IsTemplate() && W->GetWorld() == World && W->IsVisible()
					&& W->GetClass()->GetName().Contains(TEXT("MapAreaPainter")))
				{
					Painter = W;
					break;
				}
			}
			if (!Painter || !MapUpdateMapping(World))
			{
				return true;
			}
			MapAttach(Painter);
			MapScanPieces(World);
			int32 Budget = MapTilesPerTick;
			for (auto It = GMapDirty.CreateIterator(); It && Budget > 0; ++It, --Budget)
			{
				MapPaintTile(*It);
				It.RemoveCurrent();
			}
			return true;
		}
	}

	void InstallHooks()
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&MapLayerTick), 0.25f);
	}
}
#endif
