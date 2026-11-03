#include "Buildables/RMBuildableProcedural.h"
#include "ProceduralMeshComponent.h"
#include "FGInventoryComponent.h"
#include "Materials/Material.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"

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

	mFarMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProceduralMeshFar"));
	mFarMesh->SetupAttachment(mMesh);
	mFarMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mFarMesh->SetCanEverAffectNavigation(false);
	mFarMesh->SetCastShadow(true);
}

void ARMBuildableProcedural::BeginPlay()
{
	Super::BeginPlay();
	UpdateLODDistances();
	RebuildMesh();
}

void ARMBuildableProcedural::UpdateLODDistances()
{
	if (!mMesh || !mFarMesh)
	{
		return;
	}
	// Start detailed; UpdateLODsForCamera switches as the camera moves.
	SetShowFar(false);
}

void ARMBuildableProcedural::SetShowFar(bool bFar)
{
	if (!mMesh || !mFarMesh)
	{
		return;
	}
	mShowingFar = bFar;
	// Checked every time: the game's own build/dismantle effects may change visibility too.
	if (mMesh->IsVisible() == bFar)
	{
		mMesh->SetVisibility(!bFar);   // collision is unaffected by visibility
	}
	if (mFarMesh->IsVisible() != bFar)
	{
		mFarMesh->SetVisibility(bFar);
	}
}

void ARMBuildableProcedural::UpdateLODsForCamera(UWorld* World, const FVector& Camera)
{
	if (!World)
	{
		return;
	}
	for (TActorIterator<ARMBuildableProcedural> It(World); It; ++It)
	{
		ARMBuildableProcedural* Piece = *It;
		if (!Piece || !Piece->mMesh || Piece->mLODDistance <= 0.f || Piece->mFarMesh->GetNumSections() == 0)
		{
			if (Piece)
			{
				Piece->SetShowFar(false);
			}
			continue;
		}
		const float DistSq = Piece->mMesh->Bounds.GetBox().ComputeSquaredDistanceToPoint(Camera);
		// A little hysteresis so standing at the switch distance does not flicker.
		const float Switch = Piece->mShowingFar ? Piece->mLODDistance * 0.9f : Piece->mLODDistance;
		Piece->SetShowFar(DistSq > Switch * Switch);
	}
}

void ARMBuildableProcedural::ClearFarMesh()
{
	if (mFarMesh)
	{
		mFarMesh->ClearAllMeshSections();
	}
}

void ARMBuildableProcedural::ApplyFarSection(const FRMMesh& Data, int32 Section, UMaterialInterface* Material)
{
	if (!mFarMesh || mLODDistance <= 0.f || IsRunningDedicatedServer())
	{
		return;
	}
	Data.ApplyToSection(mFarMesh, Section, false);
	mFarMesh->SetMaterial(Section, Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface));
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
	// Every section needs a material: the game's build/dismantle effect reads them all and
	// crashes on an empty slot. Fall back to the engine default until a real one is assigned.
	if (mMesh)
	{
		mMesh->SetMaterial(Section, Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface));
	}
}
