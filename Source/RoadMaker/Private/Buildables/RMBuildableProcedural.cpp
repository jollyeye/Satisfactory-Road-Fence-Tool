#include "Buildables/RMBuildableProcedural.h"
#include "ProceduralMeshComponent.h"
#include "FGInventoryComponent.h"
#include "Net/UnrealNetwork.h"

ARMBuildableProcedural::ARMBuildableProcedural(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProceduralMesh"));
	if (RootComponent)
	{
		mMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mMesh);
	}

	// Triangle collision straight from the render mesh: vehicles and pioneers can drive and walk on it,
	// and the build gun can aim at it for dismantling and fence placement.
	mMesh->bUseComplexAsSimpleCollision = true;
	mMesh->SetCollisionProfileName(TEXT("BlockAll"));
	mMesh->SetCastShadow(true);
}

void ARMBuildableProcedural::BeginPlay()
{
	Super::BeginPlay();
	RebuildMesh();
}

void ARMBuildableProcedural::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableProcedural, mCostMultiplier);
}

void ARMBuildableProcedural::PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
	Super::PostLoadGame_Implementation(saveVersion, gameVersion);
	// Saved inputs are restored by now; the mesh itself is never saved.
	RebuildMesh();
}

void ARMBuildableProcedural::GetDismantleRefund_Implementation(TArray<FInventoryStack>& out_refund, bool noBuildCostEnabled) const
{
	Super::GetDismantleRefund_Implementation(out_refund, noBuildCostEnabled);
	for (FInventoryStack& Stack : out_refund)
	{
		Stack.NumItems *= mCostMultiplier;
	}
}

void ARMBuildableProcedural::ApplyMaterial(int32 Section, UMaterialInterface* Material)
{
	if (mMesh && Material)
	{
		mMesh->SetMaterial(Section, Material);
	}
}
