// Road layer on the game's map (1.3.0, work in progress).
//
// The map widget nests the map picture in Overlay_0 (1024 x 1024 local units) inside
// mMapScrollContainer (render translation = pan) inside mMapZoomContainer (render scale 1..8 = zoom).
// Overlay_0 holds: mMap (the map picture), BPW_MapAreaPainter (fog / map areas) and mMapObjectPanel
// (icons). We add our own picture to that overlay right after the area painter, so the game pans and
// zooms it with the map and the icons stay on top. A world position maps to
// UFGMapFunctionLibrary::GetNormalizedPosition(capture actor, location) * 1024 in that square.
//
// STEP 2 (test): the picture is a frame around the map square and a red cross at the player, to check
// the alignment while panning and zooming. The road tiles replace it in the next step.
#include "RoadMakerModule.h"
#if !WITH_EDITOR
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "FGMapFunctionLibrary.h"
#include "FGMinimapCaptureActor.h"
#include "UObject/UObjectIterator.h"

namespace RMMapLayer
{
	namespace
	{
		constexpr int32 MapLayerTexSize = 1024;

		UTexture2D* GMapLayerTex = nullptr;
		FSlateBrush GMapLayerBrush;
		TWeakPtr<SOverlay> GMapLayerHost;
		FIntPoint GMapLayerDrawnAt(-1000000, -1000000);

		void MapLayerPutPixel(TArray<FColor>& Pixels, int32 X, int32 Y, const FColor& C)
		{
			if (X >= 0 && Y >= 0 && X < MapLayerTexSize && Y < MapLayerTexSize)
			{
				Pixels[Y * MapLayerTexSize + X] = C;
			}
		}

		void MapLayerFillRect(TArray<FColor>& Pixels, int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& C)
		{
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					MapLayerPutPixel(Pixels, X, Y, C);
				}
			}
		}

		// Redraws the test picture: a yellow frame on the map square's edge and a red cross (dark outline)
		// centred on pixel P.
		void MapLayerDrawTest(const FIntPoint& P)
		{
			if (!GMapLayerTex)
			{
				GMapLayerTex = UTexture2D::CreateTransient(MapLayerTexSize, MapLayerTexSize, PF_B8G8R8A8, TEXT("RM_MapLayerTest"));
				if (!GMapLayerTex)
				{
					return;
				}
				GMapLayerTex->AddToRoot();
				GMapLayerTex->SRGB = true;
				GMapLayerTex->Filter = TF_Bilinear;
				GMapLayerTex->NeverStream = true;
				GMapLayerBrush.SetResourceObject(GMapLayerTex);
				GMapLayerBrush.ImageSize = FVector2D(MapLayerTexSize, MapLayerTexSize);
				GMapLayerBrush.DrawAs = ESlateBrushDrawType::Image;
			}
			TArray<FColor> Pixels;
			Pixels.Init(FColor(0, 0, 0, 0), MapLayerTexSize * MapLayerTexSize);
			const int32 E = MapLayerTexSize - 1;
			const FColor Yellow(255, 220, 0, 255);
			MapLayerFillRect(Pixels, 0, 0, E, 2, Yellow);
			MapLayerFillRect(Pixels, 0, E - 2, E, E, Yellow);
			MapLayerFillRect(Pixels, 0, 0, 2, E, Yellow);
			MapLayerFillRect(Pixels, E - 2, 0, E, E, Yellow);
			const FColor Dark(20, 0, 0, 255);
			const FColor Red(255, 30, 30, 255);
			MapLayerFillRect(Pixels, P.X - 25, P.Y - 3, P.X + 25, P.Y + 3, Dark);
			MapLayerFillRect(Pixels, P.X - 3, P.Y - 25, P.X + 3, P.Y + 25, Dark);
			MapLayerFillRect(Pixels, P.X - 24, P.Y - 2, P.X + 24, P.Y + 2, Red);
			MapLayerFillRect(Pixels, P.X - 2, P.Y - 24, P.X + 2, P.Y + 24, Red);

			FTexture2DMipMap& Mip = GMapLayerTex->GetPlatformData()->Mips[0];
			void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			Mip.BulkData.Unlock();
			GMapLayerTex->UpdateResource();
			GMapLayerDrawnAt = P;
		}

		// Puts our picture into the live slate overlay that holds the area painter, right after it.
		void MapLayerAttach(UUserWidget* Painter)
		{
			UPanelWidget* Parent = Painter->GetParent();
			TSharedPtr<SWidget> Live = Parent ? Parent->GetCachedWidget() : nullptr;
			TSharedPtr<SWidget> PainterLive = Painter->GetCachedWidget();
			if (!Live.IsValid() || !PainterLive.IsValid() || Live->GetType() != FName(TEXT("SOverlay")))
			{
				return;
			}
			TSharedPtr<SOverlay> Host = StaticCastSharedPtr<SOverlay>(Live);
			if (GMapLayerHost.Pin() == Host)
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
			Host->AddSlot(ZOrder)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					SNew(SImage)
					.Image(&GMapLayerBrush)
					.Visibility(EVisibility::HitTestInvisible)
				];
			GMapLayerHost = Host;
			UE_LOG(LogRoadMaker, Display, TEXT("RMMapLayer: attached to %s after ZOrder %d (%d slots)"), *Parent->GetName(), ZOrder, Children ? Children->Num() : -1);
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
			if (!Painter)
			{
				return true;
			}
			FIntPoint P(-1000000, -1000000);
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (APawn* Pawn = PC->GetPawn())
				{
					const FVector2D N = UFGMapFunctionLibrary::GetNormalizedPosition(UFGMapFunctionLibrary::GetMinimapCaptureActor(World), Pawn->GetActorLocation());
					P = FIntPoint(FMath::RoundToInt(N.X * MapLayerTexSize), FMath::RoundToInt(N.Y * MapLayerTexSize));
				}
			}
			if (!GMapLayerTex || P != GMapLayerDrawnAt)
			{
				MapLayerDrawTest(P);
			}
			if (GMapLayerTex)
			{
				MapLayerAttach(Painter);
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
