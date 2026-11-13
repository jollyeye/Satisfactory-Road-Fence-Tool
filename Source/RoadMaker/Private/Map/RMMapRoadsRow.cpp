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
	To->SetLayout(Layout);
	To->SetAutoSize(From->GetAutoSize());
	To->SetZOrder(From->GetZOrder() + 1);
	Row->CopyLook(StampsButton, StampsText, Tick);
	GMapRoadsRow = Row;
	UE_LOG(LogRoadMaker, Display, TEXT("Map: \"Show Roads on Map\" row added above %s in %s (height %.0f, tick %s)"),
		*StampsRow->GetName(), *Canvas->GetName(), Height, Tick ? *Tick->GetClass()->GetName() : TEXT("none"));
}

TSharedRef<SWidget> URMMapRoadsRow::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		mButton = WidgetTree->ConstructWidget<UButton>();
		WidgetTree->RootWidget = mButton;
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
		mText->SetFont(Text->GetFont());
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
		if (UImage* GameTick = Cast<UImage>(Tick))
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
