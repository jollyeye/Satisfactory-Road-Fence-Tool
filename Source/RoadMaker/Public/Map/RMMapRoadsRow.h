// "Show Roads on Map" row in the map's side menu, under the game's "Show Stamps on Map" (copies its
// look). The choice is per player and remembered (GameUserSettings.ini, [RoadMaker] bMapShowRoads);
// on by default. While off, Road Maker's road layer is hidden and not painted.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RMMapRoadsRow.generated.h"

class UTextBlock;
class UCheckBox;

namespace RMMapRoads
{
	/** Roads shown on the map (remembered; on by default). */
	ROADMAKER_API bool IsShown();
	ROADMAKER_API void SetShown(bool bShown);

	/**
	 * While the map is open: puts the row under "Show Stamps on Map" if it is not there yet (looked
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

	/** Takes the look (font, colours, check box style) of the game's row. */
	void CopyLook(UTextBlock* Text, UCheckBox* Check);

protected:
	UFUNCTION()
	void OnCheckChanged(bool bIsChecked);

	UPROPERTY(Transient)
	UTextBlock* mText = nullptr;

	UPROPERTY(Transient)
	UCheckBox* mCheck = nullptr;
};
