#include "Map/RMMapRoadsRow.h"
#include "RoadMakerModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
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

	/** The smallest widget around Text that also holds a check box (the game's row), and that box. */
	UWidget* FindRow(UTextBlock* Text, UCheckBox*& OutCheck)
	{
		OutCheck = nullptr;
		for (UWidget* W = Text; W; )
		{
			if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
			{
				TArray<UWidget*> Stack(Panel->GetAllChildren());
				while (Stack.Num() > 0 && !OutCheck)
				{
					UWidget* Child = Stack.Pop();
					OutCheck = Cast<UCheckBox>(Child);
					if (UPanelWidget* Sub = Cast<UPanelWidget>(Child))
					{
						Stack.Append(Sub->GetAllChildren());
					}
					else if (UUserWidget* SubUser = Cast<UUserWidget>(Child); SubUser && SubUser->WidgetTree && SubUser->WidgetTree->RootWidget)
					{
						Stack.Add(SubUser->WidgetTree->RootWidget);
					}
				}
				if (OutCheck)
				{
					return W;
				}
			}
			else if (UUserWidget* User = Cast<UUserWidget>(W); User && User->WidgetTree && W != Text)
			{
				User->WidgetTree->ForEachWidget([&OutCheck](UWidget* Child)
				{
					if (!OutCheck)
					{
						OutCheck = Cast<UCheckBox>(Child);
					}
				});
				if (OutCheck)
				{
					return W;
				}
			}
			UWidget* Parent = W->GetParent();
			if (!Parent)
			{
				// The top of a user widget's own tree: carry on from the user widget itself.
				Parent = W->GetTypedOuter<UUserWidget>();
				if (Parent == W)
				{
					break;
				}
			}
			W = Parent;
		}
		return nullptr;
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
	UCheckBox* StampsCheck = nullptr;
	UWidget* StampsRow = StampsText ? FindRow(StampsText, StampsCheck) : nullptr;
	if (StampsText && !StampsRow)
	{
		// No UCheckBox around the text (the game's box is its own widget): the row is the text's
		// nearest ancestor that sits in a vertical list (a vertical box or scroll box).
		static bool bChainLogged = false;
		for (UWidget* W = StampsText; W; )
		{
			UWidget* Parent = W->GetParent();
			UWidget* Up = Parent ? Parent : W->GetTypedOuter<UUserWidget>();
			if (!bChainLogged)
			{
				UPanelWidget* P = Cast<UPanelWidget>(Parent);
				UE_LOG(LogRoadMaker, Display, TEXT("Map: row search: %s (%s) in %s (%s), %d children"), *W->GetName(), *W->GetClass()->GetName(),
					Up ? *Up->GetName() : TEXT("-"), Up ? *Up->GetClass()->GetName() : TEXT("-"), P ? P->GetChildrenCount() : -1);
			}
			if (Parent && (Parent->GetClass()->GetName().Contains(TEXT("VerticalBox")) || Parent->GetClass()->GetName().Contains(TEXT("ScrollBox"))))
			{
				StampsRow = W;
				break;
			}
			if (Up == W)
			{
				break;
			}
			W = Up;
		}
		bChainLogged = true;
	}
	UPanelWidget* List = StampsRow ? StampsRow->GetParent() : nullptr;
	if (!List)
	{
		static int32 Misses = 0;
		if (Misses++ < 3)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Map: \"Show Stamps on Map\" row not found (text %s, row %s)"),
				StampsText ? TEXT("found") : TEXT("missing"), StampsRow ? *StampsRow->GetName() : TEXT("-"));
		}
		return;
	}
	APlayerController* PC = StampsText->GetOwningPlayer();
	Row = PC ? CreateWidget<URMMapRoadsRow>(PC, URMMapRoadsRow::StaticClass()) : CreateWidget<URMMapRoadsRow>(World, URMMapRoadsRow::StaticClass());
	if (!Row)
	{
		return;
	}
	const int32 Index = List->GetChildIndex(StampsRow);
	UPanelSlot* NewSlot = List->InsertChildAt(Index + 1, Row);
	// Same spacing as the game's row.
	if (UVerticalBoxSlot* To = Cast<UVerticalBoxSlot>(NewSlot))
	{
		if (UVerticalBoxSlot* From = Cast<UVerticalBoxSlot>(StampsRow->Slot))
		{
			To->SetPadding(From->GetPadding());
			To->SetHorizontalAlignment(From->GetHorizontalAlignment());
			To->SetVerticalAlignment(From->GetVerticalAlignment());
			To->SetSize(From->GetSize());
		}
	}
	Row->CopyLook(StampsText, StampsCheck);
	GMapRoadsRow = Row;
	UE_LOG(LogRoadMaker, Display, TEXT("Map: \"Show Roads on Map\" row added to %s (%s) at %d"), *List->GetName(), *List->GetClass()->GetName(), Index + 1);
}

TSharedRef<SWidget> URMMapRoadsRow::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>();
		WidgetTree->RootWidget = Box;
		mText = WidgetTree->ConstructWidget<UTextBlock>();
		mText->SetText(NSLOCTEXT("RoadMaker", "MapShowRoads", "Show Roads on Map"));
		UHorizontalBoxSlot* TextSlot = Box->AddChildToHorizontalBox(mText);
		TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextSlot->SetVerticalAlignment(VAlign_Center);
		mCheck = WidgetTree->ConstructWidget<UCheckBox>();
		mCheck->SetIsChecked(RMMapRoads::IsShown());
		mCheck->OnCheckStateChanged.AddDynamic(this, &URMMapRoadsRow::OnCheckChanged);
		UHorizontalBoxSlot* CheckSlot = Box->AddChildToHorizontalBox(mCheck);
		CheckSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Super::RebuildWidget();
}

void URMMapRoadsRow::CopyLook(UTextBlock* Text, UCheckBox* Check)
{
	TakeWidget(); // builds mText / mCheck
	if (mText && Text)
	{
		mText->SetFont(Text->GetFont());
		mText->SetColorAndOpacity(Text->GetColorAndOpacity());
		mText->SetShadowOffset(Text->GetShadowOffset());
		mText->SetShadowColorAndOpacity(Text->GetShadowColorAndOpacity());
		if (UHorizontalBoxSlot* TextSlot = Cast<UHorizontalBoxSlot>(mText->Slot))
		{
			// The game's text sits in its row with some padding: the same here (when it is in a slot that has it).
			if (UHorizontalBoxSlot* From = Cast<UHorizontalBoxSlot>(Text->Slot))
			{
				TextSlot->SetPadding(From->GetPadding());
			}
		}
	}
	if (mCheck && !Check)
	{
		// No game check box to copy: a plain box drawn as a white square with a tick.
		FCheckBoxStyle Style = mCheck->GetWidgetStyle();
		mCheck->SetWidgetStyle(Style);
	}
	if (mCheck && Check)
	{
		mCheck->SetWidgetStyle(Check->GetWidgetStyle());
		if (UHorizontalBoxSlot* CheckSlot = Cast<UHorizontalBoxSlot>(mCheck->Slot))
		{
			if (UHorizontalBoxSlot* From = Cast<UHorizontalBoxSlot>(Check->Slot))
			{
				CheckSlot->SetPadding(From->GetPadding());
			}
		}
	}
	if (mCheck)
	{
		mCheck->SetIsChecked(RMMapRoads::IsShown());
	}
}

void URMMapRoadsRow::OnCheckChanged(bool bIsChecked)
{
	RMMapRoads::SetShown(bIsChecked);
	UE_LOG(LogRoadMaker, Display, TEXT("Map: roads %s"), bIsChecked ? TEXT("shown") : TEXT("hidden"));
}
