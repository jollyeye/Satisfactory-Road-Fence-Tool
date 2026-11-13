// "Show Roads on Map" row in the map's side menu, right above the game's "Show Stamps on Map" (a copy
// of its look: same button style, font and tick box). The choice is per player and remembered
// (GameUserSettings.ini, [RoadMaker] bMapShowRoads); on by default. While off, Road Maker's road layer
// is hidden and not painted.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RMMapRoadsRow.generated.h"

class UTextBlock;
class UButton;
class UImage;
class UCheckBox;

namespace RMMapRoads
{
	/** Roads shown on the map (remembered; on by default). */
	ROADMAKER_API bool IsShown();
	ROADMAKER_API void SetShown(bool bShown);

	/**
	 * While the map is open: puts the row above "Show Stamps on Map" if it is not there yet (looked
	 * for at most once a second). World: the game world the map belongs to.
	 */
	ROADMAKER_API void EnsureRow(UWorld* World);
}

UCLASS()
class ROADMAKER_API URMMapRoadsRow : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	/**
	 * Takes the look of the game's row: its button style, its text's font and padding, and its tick
	 * (Tick: the widget beside the text; a picture is copied, else a plain check box is used).
	 */
	void CopyLook(UButton* Button, UTextBlock* Text, UWidget* Tick);

protected:
	UFUNCTION()
	void OnClicked();

	UFUNCTION()
	void OnCheckChanged(bool bIsChecked);

	/** Shows the current state (tick picture full / faint, or the check box). */
	void Refresh();

	UPROPERTY(Transient)
	UButton* mButton = nullptr;

	UPROPERTY(Transient)
	UTextBlock* mText = nullptr;

	UPROPERTY(Transient)
	UImage* mTick = nullptr;

	UPROPERTY(Transient)
	UCheckBox* mCheck = nullptr;
};
