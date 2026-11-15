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

	// Dismantling the wall takes its sign along.
	virtual void GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const override;

	/** The hidden sign set up or changed its design: show it on the front. */
	void OnSignDesignChanged();

	ARMSignWallSign* GetSign() const { return mSign; }

	/** Material of the face until the hidden sign has its design material (plain dark). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mFrontMaterial = nullptr;

protected:
	/** The hidden sign (made by the wall on the server, saved with it). */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Sign)
	ARMSignWallSign* mSign = nullptr;

	UFUNCTION()
	void OnRep_Sign() { OnSignDesignChanged(); }

private:
	/** Server: makes the hidden sign if this wall has none yet. */
	void EnsureSign();
	FTimerHandle mEnsureSignTimer;

	/** Material of the face: the sign's design material once it has one, else mFrontMaterial. */
	UMaterialInterface* GetFrontMaterial() const;

	/** Design width in cm along the wall (height x the design's aspect). */
	float GetRepeatLength() const;
};
