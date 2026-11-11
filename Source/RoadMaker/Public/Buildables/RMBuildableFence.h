// Fence along a spline. One C++ class; make three Blueprint children, one per mFenceType.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableSpline.h"
#include "RMBuildableFence.generated.h"

UENUM(BlueprintType)
enum class ERMFenceType : uint8
{
	GuardRail     UMETA(DisplayName = "Metal Guard Rail"),
	JerseyBarrier UMETA(DisplayName = "Cement Blocks"),
	GridFence     UMETA(DisplayName = "Metal Grid Fence"),
	GridFenceTall UMETA(DisplayName = "Steel Mesh Fence Wall"),
	ConcreteWall  UMETA(DisplayName = "Concrete Fence Wall"),
	SteelWall     UMETA(DisplayName = "Steel Fence Wall"),
	FicsitWall    UMETA(DisplayName = "FICSIT Fence Wall"),
};

UCLASS()
class ROADMAKER_API ARMBuildableFence : public ARMBuildableSpline
{
	GENERATED_BODY()

public:
	ARMBuildableFence(const FObjectInitializer& ObjectInitializer);

	virtual void RebuildMesh() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * Builds any fence type facing the road. Builders are written for a road on +Right; for
	 * InwardSign < 0 the frames are mirrored first, so every fence type (also future ones) mirrors.
	 */
	static void BuildFenceMesh(ERMFenceType Type, TArray<FRMFrame> Frames, float UVScale, float InwardSign, FRMMesh& Main, FRMMesh& Panel,
		const FRMWallTexLayout& WallLayout = FRMWallTexLayout());

	/**
	 * Lowest-detail version (second distance level): one simple shape per fence type, few rings.
	 * Guard rail: the beam only; cement blocks: a plain wedge; grid fences: the mesh sheet only;
	 * fence walls: a plain slab with the wall's texture.
	 */
	static void BuildFenceFar2Mesh(ERMFenceType Type, TArray<FRMFrame> Frames, float InwardSign, FRMMesh& Main, FRMMesh& Panel,
		const FRMWallTexLayout& WallLayout = FRMWallTexLayout());

	/** Fence walls: where a wall panel sits in the texture of mMainMaterial (a game wall material). */
	FRMWallTexLayout GetWallLayout() const;

	/**
	 * Customizer material swap (concrete / FICSIT / steel): the game builds the other fence wall
	 * variant in place of this one. Its shape (spline, side) is handed over here, or picked up by the
	 * new wall from PreUpgrade if the game does not call Upgrade.
	 */
	virtual void PreUpgrade_Implementation() override;
	virtual void Upgrade_Implementation(AActor* newActor) override;
	virtual void BeginPlay() override;
	void CopyShapeTo(ARMBuildableFence* Other) const;

	/** +1: the road is on the fence's right (+Right of its spline), -1: on its left. Set by the hologram. */
	void SetInwardSign(float InSign) { mInwardSign = InSign >= 0.f ? 1.f : -1.f; }
	float GetInwardSign() const { return mInwardSign; }

	/**
	 * Fence wall look chosen in the customizer's Material tab (concrete / FICSIT / steel), switched
	 * in place: 0 = as built, 1 = concrete, 2 = FICSIT, 3 = steel. The preview look is local only
	 * (shown while the customizer aims at the wall).
	 */
	/**
	 * Switches the look on the server. With a player, the new look's cost is taken from their
	 * inventory and the old look's cost given back (like the game's material swap); false if they
	 * cannot pay.
	 */
	bool SetWallLook(uint8 Look, class AFGCharacterPlayer* Payer = nullptr);
	uint8 GetShownLook() const { return mWallLook ? mWallLook : LookOfType(mFenceType); }

	/** True if the player can pay for switching to this look (or nothing has to be paid). */
	bool CanAffordLook(uint8 Look, class AFGCharacterPlayer* Payer) const;
	virtual void GetDismantleRefund_Implementation(TArray<FInventoryStack>& out_refund, bool noBuildCostEnabled) const override;
	void SetPreviewWallLook(uint8 Look);
	uint8 GetWallLook() const { return mWallLook; }
	ERMFenceType GetEffectiveType() const;
	UMaterialInterface* GetEffectiveMainMaterial() const;
	static uint8 LookOfType(ERMFenceType Type);

	/** Fence walls stack: a new wall aimed at one goes on top of it. */
	/** Fence walls (4 m, stack, replace each other): the concrete / FICSIT / steel walls and the steel mesh wall. */
	bool IsWall() const { return IsWallType(mFenceType); }
	static bool IsWallType(ERMFenceType Type)
	{
		return HasLooksType(Type) || Type == ERMFenceType::GridFenceTall;
	}

	/** Fence walls whose look the customizer's Material tab switches (concrete / FICSIT / steel). */
	bool HasLooks() const { return HasLooksType(mFenceType); }
	static bool HasLooksType(ERMFenceType Type)
	{
		return Type == ERMFenceType::ConcreteWall || Type == ERMFenceType::SteelWall || Type == ERMFenceType::FicsitWall;
	}
	static float GetWallHeight() { return 400.f; }

	// Cut Fence: both pieces keep the side and look; outline of the fence's cross-section.
	virtual void GetCutSection(float& OutHalfWidth, float& OutBottom, float& OutTop) const override;
	virtual void CopyCutSettingsTo(ARMBuildableSpline* New) const override;

	/** Spline points in world space (for stacking). */
	void GetWorldSplinePoints(TArray<FRMSplinePoint>& Out) const;

	/** Customizer colours: the fence wall's concrete takes the primary colour (custom primitive data). */
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;
	void ApplyPaintColors(const FFactoryCustomizationData& Data);

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Fence")
	ERMFenceType mFenceType = ERMFenceType::GuardRail;

	/** How far the fence sits in from the road edge, in cm (half the fence footprint is enough). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Fence")
	float mEdgeInset = 35.f;

	/** Section 0: steel for guard rail and grid posts, concrete for cement blocks. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mMainMaterial = nullptr;

	/** Fence walls: texture layout of the wall panel in mMainMaterial (see FRMWallTexLayout). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mWallUCentre = 0.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mWallUPerMetre = 0.214f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mWallVTop = 0.021f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mWallVPerMetre = 0.21675f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	bool mWallMirror = true;

	/** Fence walls: the fence wall class of each look (concrete, FICSIT, steel); switching swaps to it. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	TArray<TSubclassOf<class ARMBuildableFence>> mWallLookClasses;

	/** Fence walls: the recipe of each look (concrete, FICSIT, steel), for its cost. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	TArray<TSubclassOf<class UFGRecipe>> mWallLookRecipes;

	/** Fence walls: the game wall material for each look (concrete, FICSIT, steel). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	TArray<UMaterialInterface*> mWallLookMaterials;

	/** Section 1: masked grid material for the grid fence panel. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mPanelMaterial = nullptr;

protected:
	UFUNCTION()
	void OnRep_InwardSign() { RebuildMesh(); }

	UFUNCTION()
	void OnRep_WallLook() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_WallLook)
	uint8 mWallLook = 0;

	uint8 mPreviewWallLook = 0;

	/** Which side of the fence the road is on; the guard rail beam faces that way. */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_InwardSign)
	float mInwardSign = 1.f;
};
