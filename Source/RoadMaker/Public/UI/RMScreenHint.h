// A line of text at the bottom of the screen while a tool is in use (the tunnel's steps and arc angle, 1.5.0).
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RMScreenHint.generated.h"

class UBorder;
class UTextBlock;
class UWorld;

namespace RMScreenHint
{
	/** Shows Text for the local player of World (made on first use); bWarning: red box. */
	ROADMAKER_API void Show(UWorld* World, const FText& Text, bool bWarning = false);
	ROADMAKER_API void Hide();
}

UCLASS()
class ROADMAKER_API URMScreenHint : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	void SetHint(const FText& InText, bool bWarning);

protected:
	UPROPERTY(Transient)
	UBorder* Box = nullptr;

	UPROPERTY(Transient)
	UTextBlock* Text = nullptr;
};
