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

	/** Regenerates the mesh from the saved inputs. */
	virtual void RebuildMesh() PURE_VIRTUAL(ARMBuildableProcedural::RebuildMesh, );

	void SetCostMultiplier(int32 InMultiplier) { mCostMultiplier = FMath::Max(1, InMultiplier); }
	UProceduralMeshComponent* GetProceduralMesh() const { return mMesh; }

protected:
	/** Applies a material to a section, falling back to the first slot material. */
	void ApplyMaterial(int32 Section, UMaterialInterface* Material);

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mMesh;

	/** How many recipe units this piece cost. Saved so the refund matches what was paid. */
	UPROPERTY(SaveGame, Replicated)
	int32 mCostMultiplier = 1;

	/** Texture tiling distance in cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mUVScale = 400.f;
};
