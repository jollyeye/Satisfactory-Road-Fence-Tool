// Shared base for every Road Maker buildable: a procedural mesh that is rebuilt from small saved
// inputs on spawn, on load and on replication, plus a cost multiplier that scales build cost and
// dismantle refund with the size of the piece.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildable.h"
#include "RMGeometry.h"
#include "RMBuildableProcedural.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

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
	UProceduralMeshComponent* GetProceduralMesh() const { return mMesh; }

protected:
	/** Applies a material to a section, falling back to the first slot material. */
	void ApplyMaterial(int32 Section, UMaterialInterface* Material);

	/**
	 * Distance version: subclasses build a simpler copy of their mesh (coarser curve sampling, fewer
	 * fillet points) into mFarMesh with this. Beyond mLODDistance the engine draws only the far mesh,
	 * closer only the detailed one (plain draw distances, no ticking). Collision stays on mMesh.
	 */
	void ApplyFarSection(const FRMMesh& Data, int32 Section, UMaterialInterface* Material);
	void ClearFarMesh();
	void UpdateLODDistances();

public:
	/**
	 * Shows the detailed or the far mesh of every Road Maker piece in the world for this camera.
	 * Called a few times per second by the module (local player only); uses the distance to the
	 * nearest point of each piece's bounds, so long roads switch where you actually are.
	 */
	static void UpdateLODsForCamera(UWorld* World, const FVector& Camera);

protected:
	void SetShowFar(bool bFar);
	bool mShowingFar = false;

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mMesh;

	/** Low-poly copy drawn beyond mLODDistance (no collision). */
	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mFarMesh;

	/** Switch distance in cm between the detailed and the far mesh. 0 = no far mesh. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|LOD")
	float mLODDistance = 8000.f;

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
