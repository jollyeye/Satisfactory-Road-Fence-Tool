// Sign wall (1.4.0): a fence wall (same shape and snapping as the concrete fence wall) that shows a sign design along its road
// side, repeated every design width. The design belongs to a hidden game sign inside the wall (ARMSignWallSign): E on the
// wall opens the game's sign editor for it. Ends and top in the grid fence's metal, the back with the circuit grooves.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableFence.h"
#include "RMBuildableSignWall.generated.h"

class ARMSignWallSign;
class UMaterialInstanceDynamic;

UCLASS()
class ROADMAKER_API ARMBuildableSignWall : public ARMBuildableFence
{
	GENERATED_BODY()

public:
	ARMBuildableSignWall(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void RebuildMesh() override;

	// E on the wall: the hidden sign's editor.
	virtual bool IsUseable_Implementation() const override;
	virtual void OnUse_Implementation(class AFGCharacterPlayer* byCharacter, const FUseState& state) override;
	virtual FText GetLookAtDecription_Implementation(class AFGCharacterPlayer* byCharacter, const FUseState& state) const override;

	// Copy / paste settings: the sign's design (works with the game's signs).
	virtual bool CanUseFactoryClipboard_Implementation() override;
	virtual TSubclassOf<UObject> GetClipboardMappingClass_Implementation() override;
	virtual UFGFactoryClipboardSettings* CopySettings_Implementation() override;
	virtual bool PasteSettings_Implementation(UFGFactoryClipboardSettings* settings, class AFGPlayerController* player) override;

	// Dismantling the wall takes its sign along.
	virtual void GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const override;

	/** The hidden sign set up or changed its design: show it on the front. */
	void OnSignDesignChanged();

	ARMSignWallSign* GetSign() const { return mSign; }

	/** The hidden sign it makes: the billboard one, or the Label Sign (4m) one for the 1 m sign wall (1.5.0). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	TSubclassOf<ARMSignWallSign> mSignClass;

	/** Material of the face until the hidden sign has its design material (plain dark). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mFrontMaterial = nullptr;

protected:
	/** The hidden sign (made by the wall on the server, saved with it). */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Sign)
	ARMSignWallSign* mSign = nullptr;

	UFUNCTION()
	void OnRep_Sign() { OnSignDesignChanged(); }

	/** Cut / Join: the new piece gets this wall's design (its sign is made a moment later, see EnsureSign). */
	virtual void CopyCutSettingsTo(ARMBuildableSpline* New) const override;

public:
	// Blueprints: the design rides on the wall (see mBlueprintHasDesign).
	virtual void PreSerializedToBlueprint() override;
	virtual void PostSerializedToBlueprint() override;
	virtual void PostSerializedFromBlueprint(bool isBlueprintWorld = false) override;
protected:

private:
	void ClearBlueprintDesign();
	/** Server: makes the hidden sign if this wall has none yet. */
	void EnsureSign();
	FTimerHandle mEnsureSignTimer;

	/** Server, a piece of a cut or join: the design its sign starts with (the old wall's copied settings). */
	UPROPERTY(Transient)
	class UFGFactoryClipboardSettings* mPendingDesign = nullptr;


	// The design, copied onto the wall while it is saved into a blueprint (the hidden sign is no building of the
	// blueprint): the wall placed from the blueprint gives it to its new sign. Empty otherwise.
	UPROPERTY(SaveGame)
	bool mBlueprintHasDesign = false;
	UPROPERTY(SaveGame)
	FString mBlueprintLayout;
	UPROPERTY(SaveGame)
	TMap<FString, FString> mBlueprintTexts;
	UPROPERTY(SaveGame)
	TMap<FString, int32> mBlueprintIcons;
	UPROPERTY(SaveGame)
	FLinearColor mBlueprintForeground = FLinearColor::Black;
	UPROPERTY(SaveGame)
	FLinearColor mBlueprintBackground = FLinearColor::Black;
	UPROPERTY(SaveGame)
	FLinearColor mBlueprintAuxiliary = FLinearColor::Black;
	UPROPERTY(SaveGame)
	float mBlueprintEmissive = 0.f;
	UPROPERTY(SaveGame)
	float mBlueprintGlossiness = 0.f;
	UPROPERTY(SaveGame)
	FString mBlueprintSignType;

	/** Material of the face: the sign's design material once it has one, else mFrontMaterial. */
	UMaterialInterface* GetFrontMaterial() const;

	/** Twice a second (not on a dedicated server): the face follows the sign's design material if the game swapped it. */
	void CheckDesignMaterial();
	FTimerHandle mDesignTimer;
	TWeakObjectPtr<UMaterialInterface> mShownFront;

	/** Design width in cm along the wall (height x the design's aspect). */
	float GetRepeatLength() const;
};
