// Shared base for every Road Maker buildable: a procedural mesh that is rebuilt from small saved
// inputs on spawn, on load and on replication, plus a cost multiplier that scales build cost and
// dismantle refund with the size of the piece.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildable.h"
#include "RMGeometry.h"
#include "TimerManager.h"
#include "RMBuildableProcedural.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UFGFactoryCustomizationDescriptor_Swatch;

UCLASS(Abstract)
class ROADMAKER_API ARMBuildableProcedural : public AFGBuildable
{
	GENERATED_BODY()

public:
	ARMBuildableProcedural(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// IFGSaveInterface (implemented by AFGBuildable)
	virtual void PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;

	// Scale the refund by how big this piece is (the recipe holds the cost of one unit).
	virtual void GetDismantleRefund_Implementation(TArray<FInventoryStack>& out_refund, bool noBuildCostEnabled) const override;

	/** Roads and intersections take the vehicle paths and railway tracks lying on them along when dismantled. */
	virtual void GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const override;
	virtual bool DismantlesRiders() const { return false; }

	/** Regenerates the mesh from the saved inputs. */
	virtual void RebuildMesh() PURE_VIRTUAL(ARMBuildableProcedural::RebuildMesh, );

	void SetCostMultiplier(int32 InMultiplier) { mCostMultiplier = FMath::Max(1, InMultiplier); }
	int32 GetCostMultiplier() const { return mCostMultiplier; }
	UProceduralMeshComponent* GetProceduralMesh() const { return mMesh; }

	/** Road surface looks (roads and intersections): 0 asphalt, 1 concrete blocks, 2 cyberpunk. */
	static constexpr uint8 NoPreviewLook = 255;
	uint8 GetLook() const { return mLook; }
	uint8 GetShownLook() const { return mPreviewLook != NoPreviewLook ? mPreviewLook : mLook; }
	/** Switches the look (server, or locally for a preview of a replicated change). Free. */
	void SetLook(uint8 Look);
	/** Customizer hover: shows a look without changing it; NoPreviewLook ends it. */
	void SetPreviewLook(uint8 Look);
	/** The painted colours as resolved from swatch / slot / custom colour (primary, secondary). */
	void GetPaintColours(FLinearColor& OutPrimary, FLinearColor& OutSecondary) const;

	/** The road swatch the game gives unpainted roads (set by the module); cyberpunk roads that still have it get their own colours. */
	static TSubclassOf<UFGFactoryCustomizationDescriptor_Swatch> sDefaultRoadSwatch;

protected:
	/**
	 * Build animation. The game's build effect cannot show a procedural mesh (it hides the building and
	 * animates copies of static meshes), so it is switched off for Road Maker pieces and a short rise
	 * plays instead: the piece grows up from the ground in a fraction of a second (only for pieces that
	 * were just built: not restored from a save, not part of the world that loads or replicates in at the start).
	 */
	virtual void StartRiseAnimation();
	/** How long the build animation of this piece lasts (s): the build sound follows it. */
	virtual float GetBuildAnimDuration() const { return RiseDuration; }
	void StepRiseAnimation();
	bool mLoadedFromSave = false;
	FTimerHandle mRiseTimer;
	double mRiseStart = 0.0;
	static constexpr float RiseDuration = 0.6f;

	/** Top / body material for the shown look (index look - 1 of the arrays, else the default). */
	UMaterialInterface* LookMaterial(const TArray<UMaterialInterface*>& Looks, UMaterialInterface* Default) const;

	/** Cyberpunk, unpainted: its own purple and cyan instead of the road swatch. */
	void AdjustColorsForLook(const struct FFactoryCustomizationData& Data, FLinearColor& Primary, FLinearColor& Secondary) const;

	UFUNCTION()
	void OnRep_Look() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Look)
	uint8 mLook = 0;
	uint8 mPreviewLook = NoPreviewLook;

	/** Materials of looks 1, 2... (index look - 1) for the top and the body of the slab. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TArray<UMaterialInterface*> mLookTopMaterials;
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TArray<UMaterialInterface*> mLookBodyMaterials;

	/** Applies a material to a section, falling back to the first slot material. */
	void ApplyMaterial(int32 Section, UMaterialInterface* Material);

	/**
	 * Distance version: subclasses build a simpler copy of their mesh (coarser curve sampling, fewer
	 * fillet points) into mFarMesh with this. Beyond mLODDistance the engine draws only the far mesh,
	 * closer only the detailed one (plain draw distances, no ticking). Collision stays on mMesh.
	 */
	void ApplyFarSection(const FRMMesh& Data, int32 Section, UMaterialInterface* Material);
	void ClearFarMesh();

	/** Second distance level (beyond mLOD2Distance): an even simpler shape, no shadows. */
	void ApplyFar2Section(const FRMMesh& Data, int32 Section, UMaterialInterface* Material);
	void UpdateLODDistances();

public:
	/**
	 * Shows the detailed or the far mesh of every Road Maker piece in the world for this camera.
	 * Called a few times per second by the module (local player only); uses the distance to the
	 * nearest point of each piece's bounds, so long roads switch where you actually are.
	 */
	static void UpdateLODsForCamera(UWorld* World, const FVector& Camera);

protected:
	/** 0 = detailed, 1 = far mesh, 2 = second far mesh. */
	void SetShownLOD(int32 Level);
	int32 mShownLOD = 0;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mMesh;

	/** Low-poly copy drawn beyond mLODDistance (no collision). */
	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mFarMesh;

	/** Lowest-detail copy drawn beyond mLOD2Distance (no collision, no shadows). */
	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mFar2Mesh;

	/** Switch distance in cm between the detailed and the far mesh. 0 = no far mesh. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|LOD")
	float mLODDistance = 8000.f;

	/** Switch distance in cm between the far mesh and the second far mesh. 0 = no second level. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|LOD")
	float mLOD2Distance = 0.f;

	/** How many recipe units this piece cost. Saved so the refund matches what was paid. */
	UPROPERTY(SaveGame, Replicated)
	int32 mCostMultiplier = 1;

	/** Texture tiling distance in cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mUVScale = 400.f;

private:
	/** Paths and tracks found on this piece, kept for a moment (the dismantle highlight asks every frame). */
	mutable TArray<TWeakObjectPtr<AActor>> mRiderCache;
	mutable double mRiderCacheTime = -1.0;
};
