// Lane Text (1.5.0): text painted along a lane, written vertically (see docs/ROADMAP-1.5.0.md "Lane Text").
// The shared glyph texture, drawn at run time with the game's font; the text the tool paints and its text box; a test
// command (RM.LaneText).
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Types/SlateEnums.h"
#include "RMLaneText.generated.h"

class APlayerController;
class UEditableTextBox;
class UTextureRenderTarget2D;
class UWorld;

namespace RMLaneText
{
	/** Longest text one marking takes (characters). */
	constexpr int32 MaxChars = 24;

	/** The shared glyph texture: white characters on black (one channel), 256 px cells (16 x 16). Made on first use. */
	ROADMAKER_API UTextureRenderTarget2D* GetGlyphTexture(UWorld* World);

	/** Draws the characters of Text not drawn yet. False if the texture or the font is missing. */
	ROADMAKER_API bool EnsureGlyphs(UWorld* World, const FString& Text);

	/** A character's cell in the glyph texture as UVs (min = top left, max = bottom right). */
	ROADMAKER_API FBox2D CellUV(TCHAR Char);

	/** Text as a marking keeps it: control characters and halves of surrogate pairs (emoji) out, trimmed, at most MaxChars. */
	ROADMAKER_API FString Sanitize(const FString& Text);

	/** The text the Lane Text option paints (local player; kept for the session). */
	ROADMAKER_API const FString& GetToolText();
	ROADMAKER_API void SetToolText(const FString& Text);

	/** The text box (our own window): opens with the current text; OK sets it, Cancel / Esc keeps the old one. */
	ROADMAKER_API void OpenTextBox(APlayerController* PC);
	ROADMAKER_API void CloseTextBox(bool bAccept);
	ROADMAKER_API bool IsTextBoxOpen();
}

/** The Lane Text box, built in code like the old lane menu. */
UCLASS()
class ROADMAKER_API URMLaneTextBox : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** The text in the box now (sanitized). */
	FString GetEnteredText() const;
	/** Keyboard focus into the text field. */
	void FocusText();

protected:
	UFUNCTION()
	void OnTextChanged(const FText& Text);
	UFUNCTION()
	void OnTextCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void OnOk();
	UFUNCTION()
	void OnCancel();

	UPROPERTY(Transient)
	UEditableTextBox* mEdit = nullptr;
};
