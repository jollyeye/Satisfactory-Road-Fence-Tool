#include "Map/RMMapRoadsRow.h"
#include "RoadMakerModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UObjectIterator.h"

namespace
{
	bool GMapRoadsLoaded = false;
	bool GMapRoadsShown = true;
	TWeakObjectPtr<URMMapRoadsRow> GMapRoadsRow;
	double GNextMapRowSearch = 0.0;

	/**
	 * Sets the game's options check box (a Blueprint widget) on or off: calls its one-bool "Set...Check..."
	 * function if it has one, else sets its bool property with "Check" in the name. Logs its functions and
	 * bool properties once, to know what it offers.
	 */
	void SetGameCheck(UUserWidget* Box, bool bOn)
	{
		if (!Box)
		{
			return;
		}
		static bool bLogged = false;
		UFunction* Setter = nullptr;
		for (TFieldIterator<UFunction> It(Box->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			UFunction* F = *It;
			if (!bLogged && F->GetOuter() == Box->GetClass())
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Map: check box function %s (%d params)"), *F->GetName(), F->NumParms);
			}
			const FString Name = F->GetName();
			if (!Setter && F->NumParms == 1 && Name.StartsWith(TEXT("Set")) && Name.Contains(TEXT("Check")))
			{
				if (CastField<FBoolProperty>(F->PropertyLink))
				{
					Setter = F;
				}
			}
		}
		FBoolProperty* Flag = nullptr;
		for (TFieldIterator<FBoolProperty> It(Box->GetClass()); It; ++It)
		{
			if (!bLogged)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Map: check box bool %s"), *It->GetName());
			}
			if (!Flag && It->GetName().Contains(TEXT("Check")))
			{
				Flag = *It;
			}
		}
		bLogged = true;
		if (Setter)
		{
			TArray<uint8> Params;
			Params.SetNumZeroed(FMath::Max<int32>(Setter->ParmsSize, 16));
			CastField<FBoolProperty>(Setter->PropertyLink)->SetPropertyValue_InContainer(Params.GetData(), bOn);
			Box->ProcessEvent(Setter, Params.GetData());
		}
		else if (Flag)
		{
			Flag->SetPropertyValue_InContainer(Box, bOn);
			Box->InvalidateLayoutAndVolatility();
		}
	}

	/** The next widget up: the parent, or at the top of a user widget's tree the user widget itself. */
	UWidget* MapUp(UWidget* W)
	{
		if (!W)
		{
			return nullptr;
		}
		if (UWidget* Parent = W->GetParent())
		{
			return Parent;
		}
		UUserWidget* Owner = W->GetTypedOuter<UUserWidget>();
		return Owner != W ? Owner : nullptr;
	}
}

bool RMMapRoads::IsShown()
{
	if (!GMapRoadsLoaded)
	{
		GMapRoadsLoaded = true;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("RoadMaker"), TEXT("bMapShowRoads"), GMapRoadsShown, GGameUserSettingsIni);
		}
	}
	return GMapRoadsShown;
}

void RMMapRoads::SetShown(bool bShown)
{
	GMapRoadsLoaded = true;
	GMapRoadsShown = bShown;
	if (GConfig)
	{
		GConfig->SetBool(TEXT("RoadMaker"), TEXT("bMapShowRoads"), bShown, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

void RMMapRoads::EnsureRow(UWorld* World)
{
	URMMapRoadsRow* Row = GMapRoadsRow.Get();
	if (Row && Row->GetParent() && Row->GetWorld() == World)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (!World || Now < GNextMapRowSearch)
	{
		return;
	}
	GNextMapRowSearch = Now + 1.0;
	// The game's "Show Stamps on Map" text (its English source, whatever the game's language).
	UTextBlock* StampsText = nullptr;
	for (TObjectIterator<UTextBlock> It; It; ++It)
	{
		UTextBlock* Text = *It;
		if (Text && !Text->IsTemplate() && Text->GetWorld() == World && Text->IsVisible()
			&& Text->GetText().BuildSourceString().Equals(TEXT("Show Stamps on Map"), ESearchCase::IgnoreCase))
		{
			StampsText = Text;
			break;
		}
	}
	// Its button (mShowStampsButton) and the row around it that sits in the map menu's canvas (2026-11-13:
	// TextBlock in HorizontalBox_0 in mShowStampsButton in Overlay_2 in CanvasPanel_0 of BPW_MapMenu).
	UButton* StampsButton = nullptr;
	UWidget* StampsRow = nullptr;
	UCanvasPanel* Canvas = nullptr;
	for (UWidget* W = StampsText; W; W = MapUp(W))
	{
		if (!StampsButton)
		{
			StampsButton = Cast<UButton>(W);
		}
		if (StampsButton)
		{
			if (UCanvasPanel* Panel = Cast<UCanvasPanel>(W->GetParent()))
			{
				StampsRow = W;
				Canvas = Panel;
				break;
			}
		}
	}
	if (!StampsRow || !Canvas)
	{
		static int32 Misses = 0;
		if (Misses++ < 3)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Map: \"Show Stamps on Map\" row not found (text %s, button %s)"),
				StampsText ? TEXT("found") : TEXT("missing"), StampsButton ? TEXT("found") : TEXT("missing"));
		}
		return;
	}
	UCanvasPanelSlot* From = Cast<UCanvasPanelSlot>(StampsRow->Slot);
	if (!From)
	{
		return;
	}
	// The widget beside the text in the button (the tick).
	UWidget* Tick = nullptr;
	if (UPanelWidget* TextBox = StampsText->GetParent())
	{
		for (UWidget* Child : TextBox->GetAllChildren())
		{
			if (Child != StampsText)
			{
				Tick = Child;
				break;
			}
		}
	}
	APlayerController* PC = StampsText->GetOwningPlayer();
	Row = PC ? CreateWidget<URMMapRoadsRow>(PC, URMMapRoadsRow::StaticClass()) : CreateWidget<URMMapRoadsRow>(World, URMMapRoadsRow::StaticClass());
	if (!Row)
	{
		return;
	}
	UCanvasPanelSlot* To = Canvas->AddChildToCanvas(Row);
	// Same place and size as the game's row, one row higher (the bottom of the menu list is above it).
	FAnchorData Layout = From->GetLayout();
	const bool bStretchY = !FMath::IsNearlyEqual(Layout.Anchors.Minimum.Y, Layout.Anchors.Maximum.Y);
	const float Height = From->GetAutoSize() || bStretchY ? StampsRow->GetDesiredSize().Y : Layout.Offsets.Bottom;
	if (bStretchY)
	{
		Layout.Offsets.Top -= Height;
		Layout.Offsets.Bottom += Height;
	}
	else
	{
		Layout.Offsets.Top -= Height;
	}
	// Room for it: whatever in the menu reaches down to the game's row (the list) ends one row higher.
	const FGeometry CanvasGeo = Canvas->GetCachedGeometry();
	const float RowTop = CanvasGeo.AbsoluteToLocal(StampsRow->GetCachedGeometry().GetAbsolutePosition()).Y;
	for (UWidget* Other : Canvas->GetAllChildren())
	{
		UCanvasPanelSlot* OtherSlot = Other ? Cast<UCanvasPanelSlot>(Other->Slot) : nullptr;
		if (!OtherSlot || Other == StampsRow || !Other->IsVisible())
		{
			continue;
		}
		const FGeometry& Geo = Other->GetCachedGeometry();
		const float Top = CanvasGeo.AbsoluteToLocal(Geo.GetAbsolutePosition()).Y;
		const float Bottom = CanvasGeo.AbsoluteToLocal(Geo.GetAbsolutePosition() + Geo.GetAbsoluteSize()).Y;
		if (Top < RowTop - Height && Bottom > RowTop - Height + 1.f && Bottom <= RowTop + 2.f)
		{
			FAnchorData OtherLayout = OtherSlot->GetLayout();
			const bool bOtherStretchY = !FMath::IsNearlyEqual(OtherLayout.Anchors.Minimum.Y, OtherLayout.Anchors.Maximum.Y);
			if (bOtherStretchY)
			{
				OtherLayout.Offsets.Bottom += Height; // margin from the bottom
			}
			else if (!OtherSlot->GetAutoSize())
			{
				OtherLayout.Offsets.Bottom -= Height; // its height
			}
			OtherSlot->SetLayout(OtherLayout);
			UE_LOG(LogRoadMaker, Display, TEXT("Map: %s (%s) made %.0f shorter for the roads row"), *Other->GetName(), *Other->GetClass()->GetName(), Height);
		}
	}
	To->SetLayout(Layout);
	To->SetAutoSize(From->GetAutoSize());
	To->SetZOrder(From->GetZOrder() + 1);
	Row->CopyBackground(StampsRow, StampsButton);
	Row->CopyLook(StampsButton, StampsText, Tick);
	GMapRoadsRow = Row;
	UE_LOG(LogRoadMaker, Display, TEXT("Map: \"Show Roads on Map\" row added above %s in %s (height %.0f, tick %s)"),
		*StampsRow->GetName(), *Canvas->GetName(), Height, Tick ? *Tick->GetClass()->GetName() : TEXT("none"));
}

TSharedRef<SWidget> URMMapRoadsRow::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		mRoot = WidgetTree->ConstructWidget<UOverlay>();
		WidgetTree->RootWidget = mRoot;
		mButton = WidgetTree->ConstructWidget<UButton>();
		if (UOverlaySlot* ButtonSlot = mRoot->AddChildToOverlay(mButton))
		{
			ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
			ButtonSlot->SetVerticalAlignment(VAlign_Fill);
		}
		mButton->OnClicked.AddDynamic(this, &URMMapRoadsRow::OnClicked);
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>();
		if (UButtonSlot* BoxSlot = Cast<UButtonSlot>(mButton->AddChild(Box)))
		{
			BoxSlot->SetHorizontalAlignment(HAlign_Fill);
			BoxSlot->SetVerticalAlignment(VAlign_Fill);
		}
		mText = WidgetTree->ConstructWidget<UTextBlock>();
		mText->SetText(NSLOCTEXT("RoadMaker", "MapShowRoads", "Show Roads on Map"));
		UHorizontalBoxSlot* TextSlot = Box->AddChildToHorizontalBox(mText);
		TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Super::RebuildWidget();
}

void URMMapRoadsRow::CopyLook(UButton* Button, UTextBlock* Text, UWidget* Tick)
{
	TakeWidget(); // builds the button and text
	UHorizontalBox* Box = mText ? Cast<UHorizontalBox>(mText->GetParent()) : nullptr;
	if (mButton && Button)
	{
		mButton->SetStyle(Button->GetStyle());
		mButton->SetColorAndOpacity(Button->GetColorAndOpacity());
		mButton->SetBackgroundColor(Button->GetBackgroundColor());
		if (UButtonSlot* ToSlot = Box ? Cast<UButtonSlot>(Box->Slot) : nullptr)
		{
			if (UButtonSlot* FromSlot = Text && Text->GetParent() ? Cast<UButtonSlot>(Text->GetParent()->Slot) : nullptr)
			{
				ToSlot->SetPadding(FromSlot->GetPadding());
				ToSlot->SetHorizontalAlignment(FromSlot->GetHorizontalAlignment());
				ToSlot->SetVerticalAlignment(FromSlot->GetVerticalAlignment());
			}
		}
	}
	if (mText && Text)
	{
		// A little bigger than the game's row (asked 2026-11-13).
		FSlateFontInfo Font = Text->GetFont();
		Font.Size += 2;
		mText->SetFont(Font);
		mText->SetColorAndOpacity(Text->GetColorAndOpacity());
		mText->SetShadowOffset(Text->GetShadowOffset());
		mText->SetShadowColorAndOpacity(Text->GetShadowColorAndOpacity());
		if (UHorizontalBoxSlot* ToSlot = Cast<UHorizontalBoxSlot>(mText->Slot))
		{
			if (UHorizontalBoxSlot* FromSlot = Cast<UHorizontalBoxSlot>(Text->Slot))
			{
				ToSlot->SetPadding(FromSlot->GetPadding());
				ToSlot->SetSize(FromSlot->GetSize());
				ToSlot->SetHorizontalAlignment(FromSlot->GetHorizontalAlignment());
				ToSlot->SetVerticalAlignment(FromSlot->GetVerticalAlignment());
			}
		}
	}
	if (Box && WidgetTree)
	{
		UWidget* Added = nullptr;
		UUserWidget* GameBox = Cast<UUserWidget>(Tick);
		if (GameBox && GetOwningPlayer())
		{
			// The game's own check box widget (BPW_OptionsCheckbox): the same look; we only set its state.
			mGameCheck = CreateWidget<UUserWidget>(GetOwningPlayer(), GameBox->GetClass());
			if (mGameCheck)
			{
				mGameCheck->SetVisibility(ESlateVisibility::HitTestInvisible); // clicks go to our button
			}
			Added = mGameCheck;
		}
		UImage* GameTick = Added ? nullptr : Cast<UImage>(Tick);
		if (Added)
		{
			// (the game's check box)
		}
		else if (GameTick)
		{
			// The game's tick picture: full when on, faint when off.
			mTick = WidgetTree->ConstructWidget<UImage>();
			mTick->SetBrush(GameTick->GetBrush());
			mTick->SetColorAndOpacity(GameTick->GetColorAndOpacity());
			Added = mTick;
		}
		else
		{
			mCheck = WidgetTree->ConstructWidget<UCheckBox>();
			mCheck->OnCheckStateChanged.AddDynamic(this, &URMMapRoadsRow::OnCheckChanged);
			Added = mCheck;
		}
		if (!Added)
		{
			Refresh();
			return;
		}
		UHorizontalBoxSlot* TickSlot = Box->AddChildToHorizontalBox(Added);
		TickSlot->SetVerticalAlignment(VAlign_Center);
		if (UHorizontalBoxSlot* FromSlot = Tick ? Cast<UHorizontalBoxSlot>(Tick->Slot) : nullptr)
		{
			TickSlot->SetPadding(FromSlot->GetPadding());
			TickSlot->SetSize(FromSlot->GetSize());
			TickSlot->SetHorizontalAlignment(FromSlot->GetHorizontalAlignment());
			TickSlot->SetVerticalAlignment(FromSlot->GetVerticalAlignment());
		}
	}
	Refresh();
}

void URMMapRoadsRow::Refresh()
{
	const bool bShown = RMMapRoads::IsShown();
	if (mTick)
	{
		mTick->SetRenderOpacity(bShown ? 1.f : 0.2f);
	}
	if (mCheck)
	{
		mCheck->SetIsChecked(bShown);
	}
	SetGameCheck(mGameCheck, bShown);
}

void URMMapRoadsRow::CopyBackground(UWidget* GameRow, UButton* GameButton)
{
	// The thin grey line the game draws on top of its row (2 px, sRGB 102 102 103): on top of ours too.
	if (mRoot && WidgetTree)
	{
		UImage* Line = WidgetTree->ConstructWidget<UImage>();
		Line->SetColorAndOpacity(FLinearColor::FromSRGBColor(FColor(102, 102, 103)));
		Line->SetDesiredSizeOverride(FVector2D(1.f, 2.f));
		Line->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UOverlaySlot* LineSlot = mRoot->AddChildToOverlay(Line))
		{
			LineSlot->SetHorizontalAlignment(HAlign_Fill);
			LineSlot->SetVerticalAlignment(VAlign_Top);
		}
	}
	UPanelWidget* GamePanel = Cast<UPanelWidget>(GameRow);
	if (!GamePanel || !mRoot || !WidgetTree)
	{
		return;
	}
	int32 Index = 0;
	for (UWidget* Child : GamePanel->GetAllChildren())
	{
		if (!Child || Child == GameButton)
		{
			continue;
		}
		UWidget* Copy = nullptr;
		if (UImage* Image = Cast<UImage>(Child))
		{
			UImage* NewImage = WidgetTree->ConstructWidget<UImage>();
			NewImage->SetBrush(Image->GetBrush());
			NewImage->SetColorAndOpacity(Image->GetColorAndOpacity());
			Copy = NewImage;
		}
		UE_LOG(LogRoadMaker, Display, TEXT("Map: stamps row part %s (%s)%s"), *Child->GetName(), *Child->GetClass()->GetName(), Copy ? TEXT(" copied") : TEXT(""));
		if (Copy)
		{
			Copy->SetRenderOpacity(Child->GetRenderOpacity());
			Copy->SetVisibility(ESlateVisibility::HitTestInvisible);
			mRoot->InsertChildAt(Index++, Copy);
			if (UOverlaySlot* ToSlot = Cast<UOverlaySlot>(Copy->Slot))
			{
				ToSlot->SetHorizontalAlignment(HAlign_Fill);
				ToSlot->SetVerticalAlignment(VAlign_Fill);
				if (UOverlaySlot* FromSlot = Cast<UOverlaySlot>(Child->Slot))
				{
					ToSlot->SetPadding(FromSlot->GetPadding());
					ToSlot->SetHorizontalAlignment(FromSlot->GetHorizontalAlignment());
					ToSlot->SetVerticalAlignment(FromSlot->GetVerticalAlignment());
				}
			}
		}
	}
}

void URMMapRoadsRow::OnClicked()
{
	RMMapRoads::SetShown(!RMMapRoads::IsShown());
	UE_LOG(LogRoadMaker, Display, TEXT("Map: roads %s"), RMMapRoads::IsShown() ? TEXT("shown") : TEXT("hidden"));
	Refresh();
}

void URMMapRoadsRow::OnCheckChanged(bool bIsChecked)
{
	RMMapRoads::SetShown(bIsChecked);
	UE_LOG(LogRoadMaker, Display, TEXT("Map: roads %s"), bIsChecked ? TEXT("shown") : TEXT("hidden"));
}
