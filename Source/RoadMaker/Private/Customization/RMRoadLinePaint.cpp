#include "Customization/RMRoadLinePaint.h"
#include "RoadMakerModule.h"
#include "Customization/RMRoadLineDescriptor.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Net/RMRemoteCallObject.h"
#include "Buildables/RMBuildableMarking.h"
#include "ProceduralMeshComponent.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "UObject/UnrealType.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunPaint.h"
#include "FGCustomizationRecipe.h"
#include "FGFactoryCustomizationShortcut.h"
#include "FGCharacterPlayer.h"
#include "Patching/NativeHookManager.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Components/Image.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	TSubclassOf<URMRoadLineDescriptor> GActiveSides;
	TSubclassOf<URMRoadLineDescriptor> GActiveLanes;
	/** A lane marking option (arrow / digit) picked in the customizer, and the variant the mouse wheel chose. */
	TSubclassOf<URMRoadLineDescriptor> GActiveMarking;
	int32 GMarkingVariant = 0;

	const URMRoadLineDescriptor* MarkingOption()
	{
		return GActiveMarking ? GActiveMarking->GetDefaultObject<URMRoadLineDescriptor>() : nullptr;
	}

	int32 MarkingVariantCount()
	{
		const URMRoadLineDescriptor* D = MarkingOption();
		return D ? ARMBuildableMarking::VariantCount(D->mMarkingKind) : 1;
	}

	FString MarkingVariantName()
	{
		static const TCHAR* ArrowNames[] = { TEXT("straight"), TEXT("left"), TEXT("right"), TEXT("straight or left"), TEXT("straight or right"), TEXT("left or right") };
		const URMRoadLineDescriptor* D = MarkingOption();
		const int32 V = GMarkingVariant % FMath::Max(MarkingVariantCount(), 1);
		if (D && D->mMarkingKind == ERMMarkingKind::Digit)
		{
			return FString::FromInt(V);
		}
		return ArrowNames[FMath::Clamp(V, 0, 5)];
	}
}

bool RMRoadLinePaint::IsAnyActive()
{
	return GActiveSides || GActiveLanes || GActiveMarking;
}

FText RMRoadLinePaint::GetActiveText()
{
	if (GActiveMarking)
	{
		return FText::FromString(FString::Printf(TEXT("%s: %s (mouse wheel: next, click: paint)"),
			*UFGItemDescriptor::GetItemName(GActiveMarking).ToString(), *MarkingVariantName()));
	}
	TArray<FString> Parts;
	for (TSubclassOf<URMRoadLineDescriptor> D : { GActiveSides, GActiveLanes })
	{
		if (D)
		{
			Parts.Add(UFGItemDescriptor::GetItemName(D).ToString());
		}
	}
	return FText::FromString(TEXT("Road Lines: ") + FString::Join(Parts, TEXT(" + ")));
}

/** Road under the player's crosshair, if any. */
ARMBuildableRoad* RMRoadLinePaint::TraceRoad(APlayerController* PC)
{
	return Cast<ARMBuildableRoad>(TraceTarget(PC));
}

bool RMRoadLinePaint::CanApplyTo(const AActor* Target)
{
	if (GActiveMarking)
	{
		return Cast<ARMBuildableRoad>(Target) != nullptr; // markings go on roads only
	}
	if (Cast<ARMBuildableRoad>(Target))
	{
		return IsAnyActive();
	}
	if (Cast<ARMBuildableIntersection>(Target))
	{
		return GActiveSides != nullptr;
	}
	return false;
}

void RMRoadLinePaint::ApplyToTarget(AActor* Target, TSubclassOf<URMRoadLineDescriptor> Sides, TSubclassOf<URMRoadLineDescriptor> Lanes)
{
	if (!Target || !Target->HasAuthority())
	{
		return;
	}
	if (ARMBuildableIntersection* Intersection = Cast<ARMBuildableIntersection>(Target))
	{
		if (Sides)
		{
			Intersection->SetSideLines(Sides->GetDefaultObject<URMRoadLineDescriptor>()->mSideLines);
		}
	}
	else if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Target))
	{
		FRMMarkings Markings = Road->GetMarkings();
		URMRoadLineDescriptor::ApplyTo(Sides, Markings);
		URMRoadLineDescriptor::ApplyTo(Lanes, Markings);
		Road->SetMarkings(Markings);
	}
}

namespace
{
	/** The road under the crosshair (through a marking lying on it), with the aimed point and the view direction. */
	bool TraceAim(APlayerController* PC, ARMBuildableRoad*& OutRoad, AActor*& OutActor, FVector& OutAim, FVector& OutLook)
	{
		OutRoad = nullptr;
		OutActor = nullptr;
		if (!PC || !PC->GetWorld())
		{
			return false;
		}
		FVector Eye;
		FRotator View;
		PC->GetPlayerViewPoint(Eye, View);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMRoadLineTrace), true, PC->GetPawn());
		FHitResult Hit;
		if (!PC->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Params))
		{
			return false;
		}
		AActor* Actor = Hit.GetActor();
		if (Actor && Actor->IsA<ARMBuildableMarking>())
		{
			Actor = ARMBuildableMarking::FindRoadAt(PC->GetWorld(), Hit.Location); // a marking: the road under it
		}
		if (!Cast<ARMBuildableRoad>(Actor) && !Cast<ARMBuildableIntersection>(Actor))
		{
			return false;
		}
		OutActor = Actor;
		OutRoad = Cast<ARMBuildableRoad>(Actor);
		OutAim = Hit.Location;
		OutLook = View.Vector();
		return true;
	}
}


namespace
{
	/** Local-only picture of the marking about to be painted (never replicated or saved). */
	struct FGhost
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		int32 Variant = -1;
		float Width = -1.f;
	} GGhost;

	void DestroyGhost()
	{
		if (AActor* A = GGhost.Actor.Get())
		{
			A->Destroy();
		}
		GGhost = FGhost();
	}

	void UpdateGhost(APlayerController* PC, ARMBuildableRoad* Road)
	{
		const URMRoadLineDescriptor* D = MarkingOption();
		const ARMBuildableMarking* Default = D && D->mMarkingClass ? D->mMarkingClass->GetDefaultObject<ARMBuildableMarking>() : nullptr;
		ARMBuildableRoad* AimRoad;
		AActor* AimActor;
		FVector Aim, Look;
		FTransform T;
		float Lane = 200.f;
		if (!Default || !Road || !PC || !TraceAim(PC, AimRoad, AimActor, Aim, Look) || AimRoad != Road
			|| !ARMBuildableMarking::FindPlacement(Road, Aim, Look, T, Lane))
		{
			DestroyGhost();
			return;
		}
		if (!GGhost.Actor.IsValid())
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags = RF_Transient;
			AActor* A = PC->GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), T, Params);
			if (!A)
			{
				return;
			}
			UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A, TEXT("Ghost"), RF_Transient);
			M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			M->SetCastShadow(false);
			A->SetRootComponent(M);
			M->RegisterComponent();
			GGhost.Actor = A;
			GGhost.Mesh = M;
			GGhost.Variant = -1;
		}
		UProceduralMeshComponent* M = GGhost.Mesh.Get();
		if (!M)
		{
			DestroyGhost();
			return;
		}
		GGhost.Actor->SetActorTransform(T);
		const int32 Variant = GMarkingVariant % FMath::Max(MarkingVariantCount(), 1);
		const float Width = Lane * D->mMarkingSize;
		const uint8 LookIndex = Road->GetLook();
		M->SetMaterial(0, Default->GetPaintMaterial(LookIndex));
		FLinearColor Primary, Secondary;
		Road->GetLineColours(Primary, Secondary);
		const FLinearColor C = LookIndex == 2 ? Secondary : Primary;
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(C.R, C.G, C.B, 1.0));
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(C.R, C.G, C.B, 1.0));
		if (Variant != GGhost.Variant || !FMath::IsNearlyEqual(Width, GGhost.Width, 0.5f))
		{
			GGhost.Variant = Variant;
			GGhost.Width = Width;
			FRMMesh Paint;
			ARMBuildableMarking::BuildMarkingMesh(D->mMarkingKind, static_cast<uint8>(Variant), Width, Paint);
			Paint.ApplyToSection(M, 0, false);
			M->SetMaterial(0, Default->GetPaintMaterial(LookIndex));
		}
	}
}

AActor* RMRoadLinePaint::TraceTarget(APlayerController* PC)
{
	ARMBuildableRoad* Road;
	AActor* Actor;
	FVector Aim, Look;
	return TraceAim(PC, Road, Actor, Aim, Look) ? Actor : nullptr;
}

bool RMRoadLinePaint::IsPaintStateActive(APlayerController* PC)
{
	AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
	return Gun && Gun->IsEquipped() && Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()) != nullptr;
}

TSharedRef<SWidget> URMRoadLinesHint::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UVerticalBox* Screen = WidgetTree->ConstructWidget<UVerticalBox>();
		WidgetTree->RootWidget = Screen;
		UVerticalBoxSlot* Fill = Screen->AddChildToVerticalBox(WidgetTree->ConstructWidget<USpacer>());
		Fill->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		Box = WidgetTree->ConstructWidget<UBorder>();
		Box->SetPadding(FMargin(12.f, 6.f));
		UVerticalBoxSlot* BoxSlot = Screen->AddChildToVerticalBox(Box);
		BoxSlot->SetHorizontalAlignment(HAlign_Center);
		BoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 180.f));
		Text = WidgetTree->ConstructWidget<UTextBlock>();
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = 16;
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Box->SetContent(Text);
	}
	return Super::RebuildWidget();
}

void URMRoadLinesHint::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!Box || !Text)
	{
		return;
	}
	APlayerController* PC = GetOwningPlayer();
	const bool bShow = RMRoadLinePaint::IsAnyActive() && RMRoadLinePaint::IsPaintStateActive(PC);
	Box->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (!bShow)
	{
		UpdatePreview(nullptr);
		DestroyGhost();
		return;
	}
	AActor* Target = RMRoadLinePaint::TraceTarget(PC);
	UpdatePreview(GActiveMarking || !RMRoadLinePaint::CanApplyTo(Target) ? nullptr : Target);
	if (GActiveMarking && RMRoadLinePaint::CanApplyTo(Target))
	{
		UpdateGhost(PC, Cast<ARMBuildableRoad>(Target));
	}
	else
	{
		DestroyGhost();
	}
	if (RMRoadLinePaint::CanApplyTo(Target))
	{
		Text->SetText(Cast<ARMBuildableIntersection>(Target)
			? FText::FromString(TEXT("Intersection: ") + UFGItemDescriptor::GetItemName(GActiveSides).ToString())
			: RMRoadLinePaint::GetActiveText());
		Box->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.75f));
	}
	else if (Target)
	{
		// An intersection with only a lane option picked: lanes do not exist there.
		Text->SetText(GActiveMarking ? NSLOCTEXT("RoadMaker", "MarkingsOnRoads", "Lane markings go on roads")
			: NSLOCTEXT("RoadMaker", "IntersectionSidesOnly", "Intersections take Sidelines only"));
		Box->SetBrushColor(FLinearColor(0.6f, 0.08f, 0.05f, 0.85f));
	}
	else
	{
		// Same warning as before: the option only applies to roads.
		Text->SetText(NSLOCTEXT("RoadMaker", "AimAtRoadLines", "Aim at a road"));
		Box->SetBrushColor(FLinearColor(0.6f, 0.08f, 0.05f, 0.85f));
	}
}

void URMRoadLinesHint::NativeDestruct()
{
	UpdatePreview(nullptr);
	DestroyGhost();
	Super::NativeDestruct();
}

void URMRoadLinesHint::UpdatePreview(AActor* Target)
{
	AActor* Old = mPreviewTarget.Get();
	if (Old && Old != Target)
	{
		if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Old))
		{
			Road->SetPreviewMarkings(TOptional<FRMMarkings>());
		}
		else if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(Old))
		{
			X->SetPreviewSideLines(TOptional<bool>());
		}
	}
	mPreviewTarget = Target;
	if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(Target))
	{
		FRMMarkings M = Road->GetMarkings();
		URMRoadLineDescriptor::ApplyTo(GActiveSides, M);
		URMRoadLineDescriptor::ApplyTo(GActiveLanes, M);
		M.Lanes = FMath::Clamp<uint8>(M.Lanes, 1, 4);
		Road->SetPreviewMarkings(M);
	}
	else if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(Target))
	{
		if (GActiveSides)
		{
			X->SetPreviewSideLines(GActiveSides->GetDefaultObject<URMRoadLineDescriptor>()->mSideLines);
		}
	}
}

#if !WITH_EDITOR

namespace
{
	TWeakObjectPtr<URMRoadLinesHint> GRoadLinesHint;

	void EnsureHint(UFGBuildGunStatePaint* State)
	{
		if (GRoadLinesHint.IsValid() || !State)
		{
			return;
		}
		AFGBuildGun* Gun = State->GetBuildGun();
		APawn* Pawn = Gun ? Cast<APawn>(Gun->GetOwner()) : nullptr;
		APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		if (!PC || !PC->IsLocalController())
		{
			return;
		}
		if (URMRoadLinesHint* Hint = CreateWidget<URMRoadLinesHint>(PC, URMRoadLinesHint::StaticClass()))
		{
			Hint->SetVisibility(ESlateVisibility::HitTestInvisible);
			Hint->AddToViewport(500);
			GRoadLinesHint = Hint;
		}
	}

	/** A customization was picked in the customizer: track Road Lines picks, drop them otherwise. */
	void OnCustomizationPicked(UFGBuildGunStatePaint* State, TSubclassOf<UFGFactoryCustomizationDescriptor> Desc)
	{
		if (Desc && Desc->IsChildOf(URMRoadLineDescriptor::StaticClass()))
		{
			TSubclassOf<URMRoadLineDescriptor> Option(Desc.Get());
			const URMRoadLineDescriptor* Picked = Option->GetDefaultObject<URMRoadLineDescriptor>();
			if (Picked->mIsMarking)
			{
				// A marking replaces the line options (the click paints it); a line option replaces the marking.
				if (GActiveMarking != Option)
				{
					GMarkingVariant = 0;
				}
				GActiveMarking = Option;
				GActiveSides = nullptr;
				GActiveLanes = nullptr;
			}
			else if (Picked->mIsSideLineOption)
			{
				GActiveSides = Option;
				GActiveMarking = nullptr;
			}
			else
			{
				GActiveLanes = Option;
				GActiveMarking = nullptr;
			}
			EnsureHint(State);
			UE_LOG(LogRoadMaker, Display, TEXT("Road Lines: selected %s"), *Desc->GetName());
		}
		else if (Desc)
		{
			GActiveSides = nullptr;
			GActiveLanes = nullptr;
			GActiveMarking = nullptr;
			// Road Lines options are material-type descriptors. If the paint gun still holds one as
			// its active material (e.g. a colour was picked after it), drop it so it never gets
			// applied to a building as a material.
			if (State)
			{
				TSubclassOf<UFGFactoryCustomizationDescriptor_Material> Material = State->GetActiveMaterialDesc();
				if (Material && Material->IsChildOf(URMRoadLineDescriptor::StaticClass()))
				{
					State->SetActiveMaterialDesc(nullptr);
				}
			}
		}
	}
}


namespace
{
	/**
	 * The customizer's own menu tiles never show a mod option's icon (they keep whatever the reused
	 * tile showed before). While the paint gun is out, find tiles labelled with a Road Lines option
	 * name and put that option's icon into the tile's icon image (plain texture or material).
	 */
	TMap<FString, TWeakObjectPtr<UTexture2D>> GIconsByName;
	bool GLoggedTileLayout = false;

	void BuildIconTable()
	{
		if (GIconsByName.Num() > 0)
		{
			return;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(URMRoadLineDescriptor::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract)
				&& *It != URMRoadLineDescriptor::StaticClass())
			{
				URMRoadLineDescriptor* D = It->GetDefaultObject<URMRoadLineDescriptor>();
				if (UTexture2D* Icon = D ? D->GetIcon() : nullptr)
				{
					GIconsByName.Add(UFGItemDescriptor::GetItemName(*It).ToString(), Icon);
				}
			}
		}
	}

	/** All widgets of a tile, including those inside nested user widgets (the icon is). */
	void CollectWidgets(UUserWidget* Root, TArray<UWidget*>& Out, int32 Depth = 0)
	{
		if (!Root || !Root->WidgetTree || Depth > 4)
		{
			return;
		}
		TArray<UWidget*> Widgets;
		Root->WidgetTree->GetAllWidgets(Widgets);
		for (UWidget* W : Widgets)
		{
			Out.Add(W);
			if (UUserWidget* Nested = Cast<UUserWidget>(W))
			{
				CollectWidgets(Nested, Out, Depth + 1);
			}
		}
	}

	/**
	 * Tiles we changed, so the game's own look can be put back when the menu reuses a tile for a
	 * colour, material or pattern (tiles are pooled across tabs).
	 */
	struct FPatchedTile
	{
		TWeakObjectPtr<UTextBlock> Label;
		TWeakObjectPtr<UImage> Icon;
		FSlateBrush OriginalBrush;
		// The brush copy does not keep its material alive; without this the game frees the
		// original icon material while our picture is shown, and restoring it crashes.
		TStrongObjectPtr<UObject> OriginalResource;
		TArray<TPair<TWeakObjectPtr<UTextBlock>, ESlateVisibility>> HiddenTexts;
		/** The Road Lines picture we put in. */
		TWeakObjectPtr<UTexture2D> OurIcon;
		FLinearColor OriginalColor = FLinearColor::White;
		/** The tile's own icon material (the game sets each option's picture on it). */
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		TStrongObjectPtr<UTexture> OriginalTexture;
		float OriginalIsColor = 0.f;
	};
	TArray<FPatchedTile> GPatched;

	bool IsRoadLinesLabel(const UTextBlock* Label)
	{
		return Label && GIconsByName.Contains(Label->GetText().ToString().TrimStartAndEnd());
	}

	/** Puts the game's look back on tiles that no longer show a Road Lines option. */
	void RestoreReusedTiles()
	{
		for (int32 I = GPatched.Num() - 1; I >= 0; --I)
		{
			FPatchedTile& T = GPatched[I];
			if (T.Label.IsValid() && IsRoadLinesLabel(T.Label.Get()))
			{
				continue;
			}
			// The picture is a parameter of the tile's icon material, which the game sets again for
			// whatever option it puts in the tile. Only if it is still our picture (the game has not
			// refilled the tile yet) do we put the old values back.
			if (UMaterialInstanceDynamic* Mid = T.Mid.Get())
			{
				UTexture* Current = nullptr;
				Mid->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Texture")), Current);
				if (T.OurIcon.IsValid() && Current == T.OurIcon.Get())
				{
					// Only the picture: the colour flag is the game's (it sets it for colour swatches,
					// and putting back the old one turned swatches into stale pictures).
					Mid->SetTextureParameterValue(TEXT("Texture"), T.OriginalTexture.Get());
				}
			}
			for (const auto& Hidden : T.HiddenTexts)
			{
				if (UTextBlock* Text = Hidden.Key.Get())
				{
					Text->SetVisibility(Hidden.Value);
				}
			}
			GPatched.RemoveAtSwap(I);
		}
	}

	FPatchedTile* FindPatched(const UTextBlock* Label)
	{
		return GPatched.FindByPredicate([Label](const FPatchedTile& T) { return T.Label.Get() == Label; });
	}

	void PutIconInTile(UTextBlock* Label, UUserWidget* Tile, UTexture2D* Icon)
	{
		TArray<UWidget*> Widgets;
		CollectWidgets(Tile, Widgets);

		FPatchedTile* Patched = FindPatched(Label);
		for (UWidget* W : Widgets)
		{
			UImage* Image = Cast<UImage>(W);
			UUserWidget* Owner = W->GetTypedOuter<UUserWidget>();
			// The option picture is the image "mIcon" of the customizer icon widget. It is normally a
			// material that paints swatch colours; we draw the Road Lines picture in its place.
			if (!Image || Image->GetName() != TEXT("mIcon") || !Owner || !Owner->GetClass()->GetName().Contains(TEXT("CustomiserIcon")))
			{
				continue;
			}
			if (!Patched)
			{
				FPatchedTile New;
				New.Label = Label;
				New.Icon = Image;
				New.OriginalBrush = Image->GetBrush();
				New.OriginalColor = Image->GetColorAndOpacity();
				New.OriginalResource.Reset(New.OriginalBrush.GetResourceObject());
				// The "Edit" label left over from the colour tab: Road Lines options are not editable.
				for (UWidget* Other : Widgets)
				{
					UTextBlock* Text = Cast<UTextBlock>(Other);
					if (Text && Text->GetText().ToString().Equals(TEXT("Edit"), ESearchCase::IgnoreCase))
					{
						New.HiddenTexts.Emplace(Text, Text->GetVisibility());
						Text->SetVisibility(ESlateVisibility::Collapsed);
					}
				}
				Patched = &GPatched.Add_GetRef(New);
				if (!GLoggedTileLayout)
				{
					GLoggedTileLayout = true;
					UE_LOG(LogRoadMaker, Display, TEXT("Road Lines icons: drawing into %s.mIcon (was %s)"),
						*Owner->GetClass()->GetName(),
						New.OriginalBrush.GetResourceObject() ? *New.OriginalBrush.GetResourceObject()->GetName() : TEXT("none"));
					if (const UMaterialInstance* MI = Cast<UMaterialInstance>(New.OriginalBrush.GetResourceObject()))
					{
						for (const FTextureParameterValue& P : MI->TextureParameterValues)
						{
							UE_LOG(LogRoadMaker, Display, TEXT("  icon material texture %s = %s"), *P.ParameterInfo.Name.ToString(), P.ParameterValue ? *P.ParameterValue->GetPathName() : TEXT("none"));
						}
						for (const FScalarParameterValue& P : MI->ScalarParameterValues)
						{
							UE_LOG(LogRoadMaker, Display, TEXT("  icon material scalar %s = %f"), *P.ParameterInfo.Name.ToString(), P.ParameterValue);
						}
						for (const FVectorParameterValue& P : MI->VectorParameterValues)
						{
							UE_LOG(LogRoadMaker, Display, TEXT("  icon material vector %s = %s"), *P.ParameterInfo.Name.ToString(), *P.ParameterValue.ToString());
						}
					}
				}
			}
			// Draw our picture through the tile's own icon material (its "Texture" parameter), the
			// way the game draws pattern and material icons. The brush itself is left alone, so the
			// game can refill the tile for other tabs.
			UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Image->GetBrush().GetResourceObject());
			if (!Mid)
			{
				return;
			}
			UTexture* Current = nullptr;
			Mid->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Texture")), Current);
			if (Current != Icon)
			{
				if (Patched->Mid.Get() != Mid || !Patched->OurIcon.IsValid())
				{
					Patched->Mid = Mid;
					Patched->OriginalTexture.Reset(Current);
					Mid->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("IsColor")), Patched->OriginalIsColor);
				}
				Patched->OurIcon = Icon;
				Mid->SetTextureParameterValue(TEXT("Texture"), Icon);
				Mid->SetScalarParameterValue(TEXT("IsColor"), 0.f);
			}
			return;
		}
	}

	TArray<FString> GetKeysOf(const TMap<FString, TWeakObjectPtr<UTexture2D>>& Map)
	{
		TArray<FString> Keys;
		Map.GetKeys(Keys);
		return Keys;
	}

	bool FixCustomizerIcons(float)
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
		// The customizer menu runs while the build gun is out (in its menu state, not the paint state).
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
		if (!Gun || !Gun->IsEquipped())
		{
			return true;
		}
		BuildIconTable();
		RestoreReusedTiles();
		static bool bLoggedLabels = false;
		TArray<FString> SeenLabels;
		int32 Matches = 0;
		for (TObjectIterator<UTextBlock> It; It; ++It)
		{
			UTextBlock* Label = *It;
			if (!Label || Label->IsTemplate() || !Label->IsVisible())
			{
				continue;
			}
			const FString Text = Label->GetText().ToString();
			if (!bLoggedLabels && SeenLabels.Num() < 60 && !Text.IsEmpty())
			{
				SeenLabels.Add(FString::Printf(TEXT("%s<%s>"), *Text, *Label->GetClass()->GetName()));
			}
			const TWeakObjectPtr<UTexture2D>* Icon = GIconsByName.Find(Text.TrimStartAndEnd());
			if (Icon && Icon->IsValid())
			{
				++Matches;
				PutIconInTile(Label, Label->GetTypedOuter<UUserWidget>(), Icon->Get());
			}
		}
		// Once per session, when the Road Lines tab is open but no tile was matched, log what the
		// menu labels look like (helps if the game draws its labels differently).
		if (!bLoggedLabels && Matches == 0 && RMRoadLinePaint::IsAnyLabelVisible(SeenLabels))
		{
			bLoggedLabels = true;
			UE_LOG(LogRoadMaker, Display, TEXT("Road Lines icons: no tile matched. Labels: %s | Known: %s"),
				*FString::Join(SeenLabels, TEXT(", ")), *FString::Join(GetKeysOf(GIconsByName), TEXT(", ")));
		}
		return true;
	}
}

namespace
{
	/** The hotbar shortcut a hotbar entry widget shows (found once per widget class by reflection). */
	UFGHotbarShortcut* GetEntryShortcut(UUserWidget* Entry)
	{
		static TMap<TWeakObjectPtr<UClass>, FObjectPropertyBase*> PropByClass;
		UClass* Class = Entry->GetClass();
		FObjectPropertyBase** Found = PropByClass.Find(Class);
		if (!Found)
		{
			FObjectPropertyBase* Prop = nullptr;
			for (TFieldIterator<FObjectPropertyBase> It(Class); It; ++It)
			{
				if (It->PropertyClass && It->PropertyClass->IsChildOf(UFGHotbarShortcut::StaticClass()))
				{
					Prop = *It;
					break;
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("Road Lines hotbar: %s shortcut property = %s"), *Class->GetName(), Prop ? *Prop->GetName() : TEXT("none"));
			Found = &PropByClass.Add(Class, Prop);
		}
		return *Found ? Cast<UFGHotbarShortcut>((*Found)->GetObjectPropertyValue_InContainer(Entry)) : nullptr;
	}

	/**
	 * The hotbar draws customizer shortcuts with the customizer icon material, but its Blueprint only
	 * fills it in for the game's own swatches, patterns and materials: a Road Lines shortcut is left
	 * with the bare material (a white / magenta swatch). Fill it in the way the game does for
	 * materials: a dynamic instance with Texture = the option's icon and IsColor = 0.
	 */
	bool FixRoadLinesHotbarIcons(float)
	{
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* Entry = *It;
			if (!Entry || Entry->IsTemplate() || !Entry->IsVisible() || !Entry->GetClass()->GetName().Contains(TEXT("HotbarEntry")))
			{
				continue;
			}
			UFGFactoryCustomizationShortcut* Shortcut = Cast<UFGFactoryCustomizationShortcut>(GetEntryShortcut(Entry));
			TSubclassOf<UFGCustomizationRecipe> Recipe = Shortcut ? Shortcut->GetCustomizationRecipe() : nullptr;
			TSubclassOf<UFGFactoryCustomizationDescriptor> Desc = Recipe ? UFGCustomizationRecipe::GetCustomizationDescriptor(Recipe) : nullptr;
			if (!Desc || !Desc->IsChildOf(URMRoadLineDescriptor::StaticClass()))
			{
				continue;
			}
			UTexture2D* Icon = Shortcut->GetDisplayImage();
			if (!Icon)
			{
				continue;
			}
			TArray<UWidget*> Widgets;
			CollectWidgets(Entry, Widgets);
			for (UWidget* W : Widgets)
			{
				UImage* Image = Cast<UImage>(W);
				UUserWidget* Owner = W->GetTypedOuter<UUserWidget>();
				if (!Image || Image->GetName() != TEXT("mIcon") || !Owner || !Owner->GetClass()->GetName().Contains(TEXT("CustomiserIcon")))
				{
					continue;
				}
				UObject* Res = Image->GetBrush().GetResourceObject();
				UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Res);
				if (!Mid)
				{
					UMaterialInterface* Base = Cast<UMaterialInterface>(Res);
					if (!Base)
					{
						continue;
					}
					Mid = UMaterialInstanceDynamic::Create(Base, Image);
					Image->SetBrushFromMaterial(Mid);
				}
				UTexture* Current = nullptr;
				Mid->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Texture")), Current);
				float IsColor = 1.f;
				Mid->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("IsColor")), IsColor);
				if (Current != Icon || IsColor != 0.f)
				{
					Mid->SetTextureParameterValue(TEXT("Texture"), Icon);
					Mid->SetScalarParameterValue(TEXT("IsColor"), 0.f);
				}
			}
		}
		return true;
	}
}

namespace
{
	/**
	 * Road Lines options are material-type options that no building maps to, so the paint gun's own
	 * check flags every target as "Invalid customization target!". Our hint shows the real state, so
	 * the game's message is dropped while a Road Lines option is selected.
	 */
	void ClearPaintDisqualifiers(UFGBuildGunStatePaint* State)
	{
		static FArrayProperty* Prop = FindFProperty<FArrayProperty>(UFGBuildGunStatePaint::StaticClass(), TEXT("mConstructDisqualifiers"));
		if (!State || !Prop)
		{
			return;
		}
		FScriptArrayHelper Array(Prop, Prop->ContainerPtrToValuePtr<void>(State));
		if (Array.Num() > 0)
		{
			Array.EmptyValues();
		}
	}
}

namespace
{
	/** While a marking is selected in the paint gun the mouse wheel picks the arrow / digit instead of anything else. */
	class FMarkingWheel : public IInputProcessor
	{
	public:
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& WheelEvent, const FPointerEvent*) override
		{
			if (!GActiveMarking || WheelEvent.GetWheelDelta() == 0.f || !GEngine)
			{
				return false;
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
			if (!RMRoadLinePaint::IsPaintStateActive(PC))
			{
				return false; // in the customizer menu or another state: the game keeps the wheel
			}
			const int32 Count = FMath::Max(MarkingVariantCount(), 1);
			GMarkingVariant = ((GMarkingVariant + (WheelEvent.GetWheelDelta() > 0.f ? 1 : -1)) % Count + Count) % Count;
			return true;
		}
	};
}

void RMRoadLinePaint::InstallHooks()
{
	// The wheel: registered once Slate is up.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return !IsRunningDedicatedServer();
		}
		FSlateApplication::Get().RegisterInputPreProcessor(MakeShared<FMarkingWheel>());
		return false;
	}), 1.0f);
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(UFGBuildGunStatePaint::TickState_Implementation, GetMutableDefault<UFGBuildGunStatePaint>(),
		[](UFGBuildGunStatePaint* State, float DeltaTime)
		{
			if (IsAnyActive())
			{
				ClearPaintDisqualifiers(State);
			}
		});

	// Road Lines tiles in the customizer menu: put our icons in, ten times a second while it is open.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FixCustomizerIcons), 0.1f);

	// Road Lines shortcuts on the hotbar: their picture (the game leaves it as a blank swatch).
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FixRoadLinesHotbarIcons), 0.2f);
	// Right after a shortcut is bound, check every frame for a moment, so the blank swatch is not seen.
	static int32 GHotbarFastFrames = 0;
	SUBSCRIBE_METHOD_AFTER(UFGFactoryCustomizationShortcut::SetCustomizationRecipe,
		[](UFGFactoryCustomizationShortcut*, TSubclassOf<UFGCustomizationRecipe>, bool)
		{
			GHotbarFastFrames = 30;
		});
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float DeltaTime)
		{
			if (GHotbarFastFrames > 0)
			{
				--GHotbarFastFrames;
				FixRoadLinesHotbarIcons(DeltaTime);
			}
			return true;
		}));

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::SetActiveCustomization,
		[](UFGBuildGunStatePaint* State, TSubclassOf<UFGFactoryCustomizationDescriptor> Desc)
		{
			OnCustomizationPicked(State, Desc);
		});

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::SetActiveRecipe,
		[](UFGBuildGunStatePaint* State, TSubclassOf<UFGCustomizationRecipe> Recipe)
		{
			if (Recipe)
			{
				OnCustomizationPicked(State, UFGCustomizationRecipe::GetCustomizationDescriptor(Recipe));
			}
		});

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::ClearAllCustomizations, [](UFGBuildGunStatePaint* State)
	{
		GActiveSides = nullptr;
		GActiveLanes = nullptr;
		GActiveMarking = nullptr;
	});

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::RemoveCustomization,
		[](UFGBuildGunStatePaint* State, TSubclassOf<UFGFactoryCustomizationDescriptor> Desc)
		{
			if (Desc == GActiveSides.Get())
			{
				GActiveSides = nullptr;
			}
			if (Desc == GActiveLanes.Get())
			{
				GActiveLanes = nullptr;
			}
			if (Desc == GActiveMarking.Get())
			{
				GActiveMarking = nullptr;
			}
		});

	// Fire with a Road Lines option selected: apply it to the aimed road instead of painting.
	SUBSCRIBE_METHOD(AFGBuildGun::OnPrimaryFirePressed, [](auto& Scope, AFGBuildGun* Gun)
	{
		if (!IsAnyActive() || !Gun || !Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()))
		{
			return;
		}
		Scope.Cancel();
		APawn* Pawn = Cast<APawn>(Gun->GetOwner());
		APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		if (GActiveMarking)
		{
			ARMBuildableRoad* Road;
			AActor* Actor;
			FVector Aim, Look;
			if (!TraceAim(PC, Road, Actor, Aim, Look) || !Road)
			{
				return; // the hint says "Aim at a road"
			}
			const URMRoadLineDescriptor* D = MarkingOption();
			if (Road->HasAuthority())
			{
				ARMBuildableMarking::SpawnOn(D->mMarkingClass, D->mMarkingRecipe, Road, static_cast<uint8>(GMarkingVariant), D->mMarkingSize, Aim, Look);
			}
			else if (URMRemoteCallObject* RCO = URMRemoteCallObject::Get(PC))
			{
				RCO->ServerPaintMarking(GActiveMarking, Road, GMarkingVariant, Aim, Look);
			}
			return;
		}
		AActor* Target = TraceTarget(PC);
		if (!CanApplyTo(Target))
		{
			return; // the hint already says what is wrong
		}
		if (Target->HasAuthority())
		{
			ApplyToTarget(Target, GActiveSides, GActiveLanes);
		}
		else if (URMRemoteCallObject* RCO = URMRemoteCallObject::Get(PC))
		{
			// Multiplayer client: only the server may change the road.
			RCO->ServerApplyRoadLines(Target, GActiveSides, GActiveLanes);
		}
		else
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Road Lines: no remote call object, cannot apply as a client"));
		}
	});
}

#else

void RMRoadLinePaint::InstallHooks() {}

#endif
