// Boom barrier window (1.6.0), opened with E like the game's building windows: the game's dark window frame
// (Widget_Window_DarkMode, its title tab and close button) around our page: the three modes as buttons (the chosen one
// lit), the arm's state and whether a vehicle is near, and an "Automation" button that opens Automated Machines'
// Automation window for the barrier when that mod is installed (found by name: no dependency).
// Built in C++ (no widget asset): the building's interact widget class is set to this class.
#pragma once

#include "CoreMinimal.h"
#include "UI/FGInteractWidget.h"
#include "RMBarrierWindow.generated.h"

class UButton;
class UTextBlock;
class UBorder;
class ARMBuildableBarrier;

UCLASS()
class ROADMAKER_API URMBarrierWindow : public UFGInteractWidget
{
	GENERATED_BODY()

public:
	URMBarrierWindow(const FObjectInitializer& ObjectInitializer);

	/** Opens Automated Machines' Automation window for Building, if that mod is installed (by name). */
	static bool OpenAutomationWindow(AActor* Building);
	/** Whether Automated Machines is installed (its library class exists). */
	static bool HasAutomation();

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	ARMBuildableBarrier* GetBarrier() const;
	void Refresh();
	void SetMode(uint8 Mode);

	UFUNCTION() void OnAutomaticClicked();
	UFUNCTION() void OnOpenClicked();
	UFUNCTION() void OnClosedClicked();
	UFUNCTION() void OnAutomationClicked();
	UFUNCTION() void OnCloseClicked();

	/** The game's window frame (Widget_Window_DarkMode); our page goes in its WindowBody slot. */
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> mFrame;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UButton>> mModeButtons;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> mModeLabels;
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> mStatus;
	UPROPERTY(Transient)
	TObjectPtr<UButton> mAutomationButton;

	FTimerHandle mRefreshTimer;
	bool mFrameSetUp = false;
};
