// TEMPORARY research logger for the map road layer (RMMapLog). Remove before release.
// While the game's map is open, logs (only when it changes): the map widget tree around the map
// picture (class, name, visibility, slot position / size / anchors, render transform, brush resource
// and its material's texture parameters, on-screen geometry), the world bounds and the player's
// normalized map position. Run: open the map, pan, zoom in and out, close.
#include "RoadMakerModule.h"
#if !WITH_EDITOR
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Texture.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Materials/MaterialInstance.h"
#include "FGMapFunctionLibrary.h"
#include "UObject/UObjectIterator.h"

namespace RMMapDiag
{
	namespace
	{
		TMap<FString, FString> GLast;
		int32 GLines = 0;

		void Log(const FString& Key, const FString& Text)
		{
			if (GLines >= 1500)
			{
				return;
			}
			FString& Last = GLast.FindOrAdd(Key);
			if (Last != Text)
			{
				Last = Text;
				++GLines;
				UE_LOG(LogRoadMaker, Display, TEXT("RMMapLog %s %s"), *Key, *Text);
			}
		}

		FString Describe(UWidget* W)
		{
			FString S = FString::Printf(TEXT("%s<%s> vis=%d"), *W->GetName(), *W->GetClass()->GetName(), static_cast<int32>(W->GetVisibility()));
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(W->Slot))
			{
				const FAnchors A = Slot->GetAnchors();
				S += FString::Printf(TEXT(" canvas pos=%s size=%s anchors=(%.2f,%.2f)-(%.2f,%.2f) align=%s auto=%d"),
					*Slot->GetPosition().ToString(), *Slot->GetSize().ToString(), A.Minimum.X, A.Minimum.Y, A.Maximum.X, A.Maximum.Y,
					*Slot->GetAlignment().ToString(), Slot->GetAutoSize() ? 1 : 0);
			}
			else if (W->Slot)
			{
				S += FString::Printf(TEXT(" slot=%s"), *W->Slot->GetClass()->GetName());
			}
			const FWidgetTransform& RT = W->GetRenderTransform();
			if (!RT.Translation.IsZero() || RT.Scale != FVector2D(1, 1) || RT.Angle != 0.f)
			{
				S += FString::Printf(TEXT(" render T=%s S=%s A=%.1f pivot=%s"), *RT.Translation.ToString(), *RT.Scale.ToString(), RT.Angle, *W->GetRenderTransformPivot().ToString());
			}
			const FGeometry& G = W->GetCachedGeometry();
			S += FString::Printf(TEXT(" geo abs=%s absSize=%s local=%s"), *FVector2D(G.GetAbsolutePosition()).ToString(), *FVector2D(G.GetAbsoluteSize()).ToString(), *FVector2D(G.GetLocalSize()).ToString());
			if (UImage* Image = Cast<UImage>(W))
			{
				UObject* Res = Image->GetBrush().GetResourceObject();
				S += FString::Printf(TEXT(" brush=%s imgSize=%s"), Res ? *Res->GetName() : TEXT("none"), *Image->GetBrush().GetImageSize().ToString());
				if (UMaterialInstance* MI = Cast<UMaterialInstance>(Res))
				{
					S += FString::Printf(TEXT(" parent=%s"), MI->Parent ? *MI->Parent->GetName() : TEXT("none"));
					TArray<FMaterialParameterInfo> Infos;
					TArray<FGuid> Ids;
					MI->GetAllTextureParameterInfo(Infos, Ids);
					for (const FMaterialParameterInfo& Info : Infos)
					{
						UTexture* T = nullptr;
						MI->GetTextureParameterValue(Info, T);
						S += FString::Printf(TEXT(" %s=%s"), *Info.Name.ToString(), T ? *T->GetName() : TEXT("none"));
					}
					Infos.Reset();
					Ids.Reset();
					MI->GetAllScalarParameterInfo(Infos, Ids);
					for (const FMaterialParameterInfo& Info : Infos)
					{
						float V = 0.f;
						MI->GetScalarParameterValue(Info, V);
						S += FString::Printf(TEXT(" %s=%.3f"), *Info.Name.ToString(), V);
					}
					Infos.Reset();
					Ids.Reset();
					MI->GetAllVectorParameterInfo(Infos, Ids);
					for (const FMaterialParameterInfo& Info : Infos)
					{
						FLinearColor V;
						MI->GetVectorParameterValue(Info, V);
						S += FString::Printf(TEXT(" %s=%s"), *Info.Name.ToString(), *V.ToString());
					}
				}
			}
			return S;
		}

		/** One line per widget of a map user widget's tree, with its depth (children of panels in order). */
		void DumpTree(UWidget* W, const FString& Path, int32 Depth)
		{
			if (!W || Depth > 12)
			{
				return;
			}
			Log(Path, Describe(W));
			if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
			{
				for (int32 I = 0; I < Panel->GetChildrenCount(); ++I)
				{
					UWidget* Child = Panel->GetChildAt(I);
					DumpTree(Child, Path + TEXT("/") + FString::FromInt(I) + TEXT(":") + (Child ? Child->GetName() : TEXT("?")), Depth + 1);
				}
			}
			if (UUserWidget* User = Cast<UUserWidget>(W))
			{
				if (User->WidgetTree && User->WidgetTree->RootWidget)
				{
					DumpTree(User->WidgetTree->RootWidget, Path + TEXT(">") + User->WidgetTree->RootWidget->GetName(), Depth + 1);
				}
			}
		}

		bool Tick(float)
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
			if (!World || GLines >= 1500)
			{
				return true;
			}
			// The map area painter (the fog of war / map areas drawn over the map) sits inside the zoomable
			// map content: log its ancestors (each with its direct children) up to the top, so the map
			// picture, the icon layer and whatever applies pan / zoom show up.
			bool bAny = false;
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				UUserWidget* Painter = *It;
				if (!Painter || Painter->IsTemplate() || Painter->GetWorld() != World || !Painter->IsVisible()
					|| !Painter->GetClass()->GetName().Contains(TEXT("MapAreaPainter")))
				{
					continue;
				}
				bAny = true;
				UWidget* W = Painter;
				int32 Level = 0;
				while (W && Level < 25)
				{
					const FString Key = FString::Printf(TEXT("up%02d"), Level);
					Log(Key, Describe(W) + FString::Printf(TEXT(" outer=%s"), *GetNameSafe(W->GetTypedOuter<UUserWidget>() ? W->GetTypedOuter<UUserWidget>()->GetClass() : nullptr)));
					UPanelWidget* Parent = W->GetParent();
					if (Parent)
					{
						for (int32 I = 0; I < Parent->GetChildrenCount() && I < 12; ++I)
						{
							UWidget* Child = Parent->GetChildAt(I);
							if (Child && !Child->GetClass()->GetName().Contains(TEXT("MapObject")))
							{
								Log(FString::Printf(TEXT("up%02d.child%02d"), Level, I), Describe(Child));
							}
						}
						W = Parent;
					}
					else
					{
						// Root of a user widget's tree: continue in the user widget that owns it.
						UWidgetTree* Tree = Cast<UWidgetTree>(W->GetOuter());
						W = Tree ? Cast<UUserWidget>(Tree->GetOuter()) : nullptr;
					}
					++Level;
				}
			}
			if (bAny)
			{
				FVector2D Min, Max;
				UFGMapFunctionLibrary::GetWorldBounds(World, Min, Max);
				FString Player = TEXT("-");
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					if (APawn* Pawn = PC->GetPawn())
					{
						const FVector2D N = UFGMapFunctionLibrary::GetNormalizedPosition(UFGMapFunctionLibrary::GetMinimapCaptureActor(World), Pawn->GetActorLocation());
						Player = FString::Printf(TEXT("%s normalized=%s"), *Pawn->GetActorLocation().ToCompactString(), *N.ToString());
					}
				}
				Log(TEXT("world"), FString::Printf(TEXT("bounds min=%s max=%s capture=%s player=%s"), *Min.ToString(), *Max.ToString(),
					*GetNameSafe(UFGMapFunctionLibrary::GetMinimapCaptureActor(World)), *Player));
			}
			return true;
		}
	}

	void InstallHooks()
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.25f);
	}
}
#endif
