// Lane Paint tool: left-click a road to apply lane markings, right-click to choose them.
// It sits in the build menu like any buildable, but its hologram never constructs anything.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildable.h"
#include "Hologram/FGBuildableHologram.h"
#include "Blueprint/UserWidget.h"
#include "RMGeometry.h"
#include "RMLanePaint.generated.h"

class ARMBuildableRoad;
class UButton;
class UPanelWidget;
class UTextBlock;

namespace RMLanePaint
{
	/** The markings the tool applies; chosen in the menu, kept for the whole game session. */
	ROADMAKER_API FRMMarkings& Settings();

	/** Opens the menu if closed, closes it if open. */
	ROADMAKER_API void ToggleMenu(APlayerController* PC);
	ROADMAKER_API void CloseMenu();

	/**
	 * Keys currently bound to the build gun's mode-select action (R by default), read from the
	 * player's live Enhanced Input mappings, so rebinds and gamepads show their own key.
	 */
	ROADMAKER_API FText GetMenuKeyText(APlayerController* PC);
	ROADMAKER_API TArray<FKey> GetMenuKeys(APlayerController* PC);

	/** On-screen "[key] Lane menu" hint while the tool is held. */
	ROADMAKER_API void ShowHint(APlayerController* PC);
	ROADMAKER_API void HideHint();
}

/** Placeholder buildable so the tool can have a descriptor and recipe. Never spawned. */
UCLASS()
class ROADMAKER_API ARMBuildableLanePaint : public AFGBuildable
{
	GENERATED_BODY()
};

UCLASS()
class ROADMAKER_API ARMLanePaintHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	ARMLanePaintHologram();

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual bool IsValidHitResult(const FHitResult& hitResult) const override { return hitResult.GetActor() != nullptr; }
	virtual bool TrySnapToActor(const FHitResult& hitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& hitResult) override;
	virtual void CheckValidPlacement() override;
	virtual void BeginPlay() override;
	virtual void GetSupportedBuildModes_Implementation(TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& out_buildmodes) const override;
	virtual void ScrollRotate(int32 delta, int32 step) override {}

	/** Applies the chosen markings to the aimed road. Always returns false: nothing gets built. */
	virtual bool DoMultiStepPlacement(bool isInputFromARelease) override;

protected:
	UPROPERTY(Transient)
	TWeakObjectPtr<ARMBuildableRoad> mTargetRoad;

	/** Only build mode of the tool, named "Lane menu (R)" so the HUD shows what R does. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<class UFGHologramBuildModeDescriptor> mLaneMenuMode;
};

/** Small "[key] Lane menu" hint shown while the Lane Paint tool is held. */
UCLASS()
class ROADMAKER_API URMLanePaintHint : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	void SetKeyText(const FText& KeyText);

protected:
	UPROPERTY(Transient)
	UTextBlock* mText = nullptr;

	FText mKeyText;
};

/** The small right-click menu: three lane presets with a picture each, and a side-lines toggle. */
UCLASS()
class ROADMAKER_API URMLanePaintMenu : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	void Refresh();

protected:
	void BuildTree();
	UWidget* MakeLaneButton(int32 Lanes);
	UWidget* MakePreview(int32 Lanes, bool bEdgeLines);

	UFUNCTION()
	void OnLanes1();
	UFUNCTION()
	void OnLanes2();
	UFUNCTION()
	void OnLanes3();
	UFUNCTION()
	void OnToggleEdges();
	UFUNCTION()
	void OnClose();

	void SelectLanes(int32 Lanes);

	UPROPERTY(Transient)
	TArray<UButton*> mLaneButtons;

	UPROPERTY(Transient)
	TArray<UPanelWidget*> mPreviewSlots;

	UPROPERTY(Transient)
	UButton* mEdgeButton = nullptr;

	UPROPERTY(Transient)
	UTextBlock* mEdgeText = nullptr;
};
