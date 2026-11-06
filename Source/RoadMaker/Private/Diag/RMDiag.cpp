// Development logger (temporary): writes "[Diag]" lines to FactoryGame.log when something it watches
// changes, to see what the game does with the customizer tiles and the paint gun outline.
// Remove before release (also its InstallHooks call in RoadMakerModule.cpp).
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableFence.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunPaint.h"
#include "FGCharacterPlayer.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectIterator.h"
#include "FGFactoryCustomizationShortcut.h"
#include "FGCustomizationRecipe.h"

namespace RMDiag
{
	void InstallHooks();
}

#if !WITH_EDITOR
namespace
{
	TMap<FString, FString> GDiagLast;

	void DiagSet(const FString& Key, const FString& Value)
	{
		FString* Old = GDiagLast.Find(Key);
		if (!Old || *Old != Value)
		{
			UE_LOG(LogRoadMaker, Display, TEXT("[Diag] %s = %s"), *Key, *Value);
			GDiagLast.Add(Key, Value);
		}
	}

	FString ObjName(const UObject* O)
	{
		return O ? FString::Printf(TEXT("%s(%s)"), *O->GetName(), *O->GetClass()->GetName()) : TEXT("none");
	}

	FString MaterialInfo(UMaterialInterface* M)
	{
		FString S = ObjName(M);
		if (UMaterialInstance* MI = Cast<UMaterialInstance>(M))
		{
			for (const FVectorParameterValue& P : MI->VectorParameterValues)
			{
				S += FString::Printf(TEXT(" %s=%s"), *P.ParameterInfo.Name.ToString(), *P.ParameterValue.ToString());
			}
			for (const FScalarParameterValue& P : MI->ScalarParameterValues)
			{
				S += FString::Printf(TEXT(" %s=%.3f"), *P.ParameterInfo.Name.ToString(), P.ParameterValue);
			}
			for (const FTextureParameterValue& P : MI->TextureParameterValues)
			{
				S += FString::Printf(TEXT(" %s=%s"), *P.ParameterInfo.Name.ToString(), *GetNameSafe(P.ParameterValue));
			}
		}
		return S;
	}

	FString ComponentInfo(UPrimitiveComponent* C)
	{
		FString S = FString::Printf(TEXT("%s vis=%d customDepth=%d stencil=%d"), *ObjName(C), C->IsVisible() ? 1 : 0,
			C->bRenderCustomDepth ? 1 : 0, C->CustomDepthStencilValue);
		for (int32 I = 0; I < FMath::Min(C->GetNumMaterials(), 3); ++I)
		{
			S += FString::Printf(TEXT(" | mat%d %s"), I, *MaterialInfo(C->GetMaterial(I)));
		}
		return S;
	}

	/** Customizer tiles: picture and draw settings of every visible option tile. */
	void DiagTiles()
	{
		int32 N = 0;
		for (TObjectIterator<UImage> It; It; ++It)
		{
			UImage* Image = *It;
			if (!Image || Image->IsTemplate() || Image->GetName() != TEXT("mIcon") || !Image->IsVisible())
			{
				continue;
			}
			UUserWidget* Owner = Image->GetTypedOuter<UUserWidget>();
			if (!Owner || !Owner->GetClass()->GetName().Contains(TEXT("CustomiserIcon")) || !Owner->IsVisible())
			{
				continue;
			}
			FString Label;
			if (Owner->WidgetTree)
			{
				Owner->WidgetTree->ForEachWidget([&Label](UWidget* W)
				{
					if (const UTextBlock* T = Cast<UTextBlock>(W))
					{
						const FString Text = T->GetText().ToString();
						if (Label.IsEmpty() && !Text.IsEmpty() && !Text.Equals(TEXT("Edit"), ESearchCase::IgnoreCase))
						{
							Label = Text;
						}
					}
				});
			}
			// The label is often on the parent tile widget, not the icon widget.
			if (Label.IsEmpty())
			{
				if (UUserWidget* Parent = Owner->GetTypedOuter<UUserWidget>())
				{
					if (Parent->WidgetTree)
					{
						Parent->WidgetTree->ForEachWidget([&Label](UWidget* W)
						{
							if (const UTextBlock* T = Cast<UTextBlock>(W))
							{
								const FString Text = T->GetText().ToString();
								if (Label.IsEmpty() && !Text.IsEmpty() && !Text.Equals(TEXT("Edit"), ESearchCase::IgnoreCase))
								{
									Label = Text;
								}
							}
						});
					}
				}
			}
			const FSlateBrush& B = Image->GetBrush();
			UObject* Res = B.GetResourceObject();
			FString ResInfo = Cast<UMaterialInterface>(Res) ? MaterialInfo(Cast<UMaterialInterface>(Res)) : ObjName(Res);
			DiagSet(FString::Printf(TEXT("tile %p"), Image), FString::Printf(TEXT("'%s' res=%s drawAs=%d tint=%s color=%s size=%s"),
				*Label, *ResInfo, (int32)B.DrawAs.GetValue(), *B.TintColor.GetSpecifiedColor().ToString(),
				*Image->GetColorAndOpacity().ToString(), *B.ImageSize.ToString()));
			++N;
		}
		DiagSet(TEXT("visible tiles"), FString::FromInt(N));
	}

	/** Paint gun: selected material, what the game thinks, the aimed actor's outline settings. */
	void DiagPaint(APlayerController* PC, UFGBuildGunStatePaint* State)
	{
		DiagSet(TEXT("paint material"), GetNameSafe(State->GetActiveMaterialDesc().Get()));
		DiagSet(TEXT("paint valid swatch/pattern/material"), FString::Printf(TEXT("%d/%d/%d"),
			State->IsSwatchHitValid() ? 1 : 0, State->IsPatternHitValid() ? 1 : 0, State->IsMaterialHitValid() ? 1 : 0));
		static bool bLoggedFn = false;
		if (!bLoggedFn)
		{
			bLoggedFn = true;
			if (UFunction* Fn = UFGBuildGunStatePaint::StaticClass()->FindFunctionByName(TEXT("OnHitStatusChanged")))
			{
				FString Params;
				for (TFieldIterator<FProperty> P(Fn); P; ++P)
				{
					Params += FString::Printf(TEXT(" %s@%d(%d)"), *P->GetName(), P->GetOffset_ForUFunction(), P->GetSize());
				}
				UE_LOG(LogRoadMaker, Display, TEXT("[Diag] OnHitStatusChanged ParmsSize=%d owner=%s params:%s"), Fn->ParmsSize,
					*GetNameSafe(Fn->GetOuter()), *Params);
			}
			else
			{
				UE_LOG(LogRoadMaker, Display, TEXT("[Diag] OnHitStatusChanged not found"));
			}
			UE_LOG(LogRoadMaker, Display, TEXT("[Diag] paint state class %s"), *State->GetClass()->GetPathName());
		}
		FVector Eye;
		FRotator View;
		PC->GetPlayerViewPoint(Eye, View);
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(RMDiag), true, PC->GetPawn());
		AActor* Aimed = PC->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Q) ? Hit.GetActor() : nullptr;
		DiagSet(TEXT("paint aimed"), ObjName(Aimed));
		// Every part drawn with an outline (custom depth) right now, with its stencil value: shows
		// which value the game uses for "can apply" (white) and "cannot apply" (red) outlines.
		{
			TSet<FString> Seen;
			for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
			{
				UPrimitiveComponent* C = *It;
				if (!C || C->IsTemplate() || !C->bRenderCustomDepth || C->CustomDepthStencilValue <= 1 || C->GetWorld() != PC->GetWorld())
				{
					continue;
				}
				AActor* Owner = C->GetOwner();
				if (Owner == PC->GetPawn() || (Owner && Owner->IsA<AFGBuildGun>()))
				{
					continue;
				}
				Seen.Add(FString::Printf(TEXT("%s/%s stencil=%d"), Owner ? *Owner->GetClass()->GetName() : TEXT("none"), *C->GetClass()->GetName(), C->CustomDepthStencilValue));
			}
			TArray<FString> List = Seen.Array();
			List.Sort();
			DiagSet(TEXT("outlined now"), FString::Printf(TEXT("valid material %d: %s"), State->IsMaterialHitValid() ? 1 : 0, *FString::Join(List, TEXT(", "))));
		}
		if (Aimed)
		{
			TInlineComponentArray<UPrimitiveComponent*> Comps(Aimed);
			int32 I = 0;
			for (UPrimitiveComponent* C : Comps)
			{
				if (I < 6)
				{
					DiagSet(FString::Printf(TEXT("aimed comp %d"), I), ComponentInfo(C));
				}
				++I;
			}
		}
		// Hover proxies: components the paint state adds (on the build gun or elsewhere) to outline the hit.
		AActor* GunOwner = State->GetTypedOuter<AActor>();
		if (GunOwner)
		{
			TInlineComponentArray<UPrimitiveComponent*> Comps(GunOwner);
			int32 I = 0;
			for (UPrimitiveComponent* C : Comps)
			{
				if (C->bRenderCustomDepth || C->GetName().Contains(TEXT("Proxy")) || C->GetName().Contains(TEXT("Hover")))
				{
					DiagSet(FString::Printf(TEXT("gun comp %d"), I++), ComponentInfo(C));
				}
			}
			DiagSet(TEXT("gun proxies"), FString::FromInt(I));
		}
	}

	/** Hotbar: the customization shortcuts and the pictures of the hotbar slot widgets. */
	void DiagHotbar()
	{
		for (TObjectIterator<UFGFactoryCustomizationShortcut> It; It; ++It)
		{
			UFGFactoryCustomizationShortcut* Shortcut = *It;
			if (!Shortcut || Shortcut->IsTemplate())
			{
				continue;
			}
			TSubclassOf<UFGCustomizationRecipe> Recipe = Shortcut->GetCustomizationRecipe();
			TSubclassOf<UFGFactoryCustomizationDescriptor> Desc = Recipe ? UFGCustomizationRecipe::GetCustomizationDescriptor(Recipe) : nullptr;
			DiagSet(TEXT("shortcut ") + Shortcut->GetName(), FString::Printf(TEXT("recipe=%s desc=%s descParent=%s image=%s"),
				*GetNameSafe(Recipe.Get()), *GetNameSafe(Desc.Get()), Desc ? *GetNameSafe(Desc->GetSuperClass()) : TEXT("none"),
				*GetNameSafe(Shortcut->GetDisplayImage())));
		}
		for (TObjectIterator<UImage> It; It; ++It)
		{
			UImage* Image = *It;
			if (!Image || Image->IsTemplate() || !Image->IsVisible())
			{
				continue;
			}
			// Path of the widgets it sits in, innermost first; keep only hotbar / shortcut ones.
			FString Path;
			bool bHotbar = false;
			int32 Depth = 0;
			for (UUserWidget* W = Image->GetTypedOuter<UUserWidget>(); W && Depth < 5; W = W->GetTypedOuter<UUserWidget>(), ++Depth)
			{
				const FString Name = W->GetClass()->GetName();
				bHotbar |= Name.Contains(TEXT("Hotbar")) || Name.Contains(TEXT("Shortcut"));
				Path += Name + TEXT("/") + W->GetName() + TEXT(" < ");
				if (!W->GetOuter() || W->GetOuter() == W)
				{
					break;
				}
			}
			const FSlateBrush& Brush = Image->GetBrush();
			UObject* Res = Brush.GetResourceObject();
			// Also any other picture drawn with the customizer icon material outside the customizer tiles.
			const UMaterialInstance* ResMI = Cast<UMaterialInstance>(Res);
			const bool bCustomiserMaterial = ResMI && GetNameSafe(ResMI->Parent).Contains(TEXT("Customi")) && !Path.Contains(TEXT("CustomiserIcon"));
			if (!bHotbar && !bCustomiserMaterial)
			{
				continue;
			}
			FString ResInfo = ObjName(Res);
			if (UMaterialInterface* M = Cast<UMaterialInterface>(Res))
			{
				ResInfo = MaterialInfo(M);
				if (UMaterialInstance* MI = Cast<UMaterialInstance>(M))
				{
					ResInfo += TEXT(" parent=") + GetNameSafe(MI->Parent);
				}
			}
			DiagSet(TEXT("hotbar img ") + Path + Image->GetName(), FString::Printf(TEXT("res=%s tint=%s drawAs=%d"),
				*ResInfo, *Image->GetColorAndOpacity().ToString(), (int32)Brush.DrawAs));
		}
	}

	bool DiagTick(float)
	{
		if (!GEngine || IsRunningDedicatedServer())
		{
			return true;
		}
		APlayerController* PC = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.World())
			{
				PC = Context.World()->GetFirstPlayerController();
				break;
			}
		}
		if (PC)
		{
			DiagHotbar();
		}
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
		if (!Gun || !Gun->IsEquipped())
		{
			return true;
		}
		DiagSet(TEXT("gun state"), ObjName(Gun->GetCurrentState()));
		DiagTiles();
		if (UFGBuildGunStatePaint* State = Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()))
		{
			DiagPaint(PC, State);
		}
		return true;
	}
}

void RMDiag::InstallHooks()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&DiagTick), 0.1f);
	UE_LOG(LogRoadMaker, Display, TEXT("[Diag] development logger on"));
}
#else
void RMDiag::InstallHooks() {}
#endif
