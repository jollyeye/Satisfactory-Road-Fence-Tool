// "Road Tools" tab in the build menu, after Blueprints. The build menu's own tabs map to its modes
// (Build / Customizer / Blueprint) and pages, so ours is an extra tab button outside the game's tab
// list: clicking it shows the Build page in Build mode with only the Road Tools category (Edit
// road > Cut Road). The Build tab itself no longer lists Road Tools.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RMRoadToolsTab.generated.h"

class UUserWidget;

UCLASS()
class ROADMAKER_API URMRoadToolsTab : public UObject
{
	GENERATED_BODY()

public:
	/** Starts watching for the build menu (call once at module start-up). */
	static void Install();

	UFUNCTION()
	void OnOurTabClicked();

	UFUNCTION()
	void OnGameTabClicked(int32 ButtonIndex);

private:
	bool Tick(float DeltaTime);
	void FindMenu();
	void OnMenuShown(UUserWidget* Menu);
	void EnsureOurButton(UUserWidget* Menu);
	void EnterRoadMode();
	void LeaveRoadMode(bool bRefreshBuildPage);
	void SetButtonsActive(bool bOurs);
	bool AnyGameTabActive() const;

	/** The build menu's category list: only Road Tools, or everything else. */
	void ApplyCategoryFilter(UUserWidget* Menu, bool bRoadToolsOnly);

	TWeakObjectPtr<UUserWidget> mMenu;
	TWeakObjectPtr<UUserWidget> mOurButton;
	TWeakObjectPtr<UObject> mBoundWindow;
	TArray<TWeakObjectPtr<UClass>> mFullCategories;
	bool mRoadMode = false;
	bool mWasShown = false;
	float mFindTimer = 0.f;
};
