// Hidden sign inside a sign wall (1.4.0): a real game sign (design, sign editor, saving, multiplayer, copy / paste) with no
// mesh and no collision. It takes the settings of the game's 8 x 4 m billboard at run time; the game draws its design into a
// render target, which the wall shows along its road side.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildableWidgetSign.h"
#include "RMSignWallSign.generated.h"

class ARMBuildableSignWall;
class UTextureRenderTarget2D;
class UMaterialInstanceDynamic;

UCLASS()
class ROADMAKER_API ARMSignWallSign : public AFGBuildableWidgetSign
{
	GENERATED_BODY()

public:
	ARMSignWallSign();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

	/** Seen from anywhere along a long wall: the wall's half length on top of the sign's own range. */
	virtual float GetSignificanceRange_Implementation() const override;

	/** The game sign class whose settings this one takes (the 8 x 4 m billboard). */
	static UClass* GetGameSignClass();

	/** The wall this sign belongs to (set by the wall when it makes the sign). */
	UPROPERTY(SaveGame, Replicated)
	ARMBuildableSignWall* mWall = nullptr;

	/** Extra significance range in cm (half the wall's length), set by the wall. */
	UPROPERTY(SaveGame, Replicated)
	float mExtraRange = 0.f;

	/** What the game set up last for the design: its material instance and render target (may be null). */
	UMaterialInstanceDynamic* GetSignMaterial() const { return mSignMaterial; }
	UTextureRenderTarget2D* GetSignTexture() const { return mSignTexture; }

	/** Size of the design in cm (the billboard's world dimensions), for the wall's repeat length. */
	FVector2D GetDesignSize() const;

protected:
	virtual void SetupMaterialInstanceForProxyPlane(UMaterialInstanceDynamic* Instance, UTextureRenderTarget2D* RenderTarget) override;

private:
	/** Copies the billboard's settings (layouts, materials, size, interact widget, plane mesh) onto this sign. */
	void CopyGameSignDefaults();
	bool mCopiedDefaults = false;

	UPROPERTY(Transient)
	UMaterialInstanceDynamic* mSignMaterial = nullptr;

	UPROPERTY(Transient)
	UTextureRenderTarget2D* mSignTexture = nullptr;
};
