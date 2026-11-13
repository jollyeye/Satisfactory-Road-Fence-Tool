#include "Holograms/RMMarkingHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableMarking.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Math/RotationMatrix.h"

ARMMarkingHologram::ARMMarkingHologram()
{
	mPreviewMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMesh"));
	if (RootComponent)
	{
		mPreviewMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mPreviewMesh);
	}
	mPreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMMarkingHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMMarkingHologram, mVariant);
	DOREPLIFETIME(ARMMarkingHologram, mLaneWidth);
	DOREPLIFETIME(ARMMarkingHologram, mRoadLook);
}

void ARMMarkingHologram::ScrollRotate(int32 delta, int32 step)
{
	const ARMBuildableMarking* Default = GetDefaultBuildable<ARMBuildableMarking>();
	const int32 Count = Default ? ARMBuildableMarking::VariantCount(Default->mKind) : 1;
	if (delta != 0 && Count > 0)
	{
		mVariant = ((mVariant + (delta > 0 ? 1 : -1)) % Count + Count) % Count;
		RebuildPreview();
	}
}

void ARMMarkingHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	UWorld* World = GetWorld();
	ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(hitResult.GetActor());
	if (!Road && hitResult.GetActor() && hitResult.GetActor()->IsA<ARMBuildableMarking>())
	{
		Road = ARMBuildableMarking::FindRoadAt(World, hitResult.Location); // aiming at a marking: the road under it
	}
	TArray<FRMFrame> Frames;
	if (Road && !Road->GetIsDismantled() && !Road->IsActorBeingDestroyed())
	{
		Road->GetWorldFrames(Frames, 25.f);
	}
	mOnRoad = Frames.Num() >= 2;
	if (!mOnRoad)
	{
		SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
		RebuildPreview();
		return;
	}

	// Same placement as the customizer's Road Lines markings: lane centre, facing the way you look, capped
	// size, lying on the road (draped).
	FVector LookDir = FVector::ForwardVector;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		LookDir = PC->PlayerCameraManager->GetCameraRotation().Vector();
	}
	const ARMBuildableMarking* Default = GetDefaultBuildable<ARMBuildableMarking>();
	FTransform T;
	float Width = 200.f;
	if (!Default || !ARMBuildableMarking::PlanOn(Road, hitResult.Location, LookDir, Default->mKind, Default->mSizeMultiplier, T, Width, mDrape))
	{
		mOnRoad = false;
		SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
		return;
	}
	SetActorTransform(T);
	mLaneWidth = Width;
	mShownVariant = -1; // the drape changes with every spot
	mRoadLook = Road->GetLook();
	if (mPaintedFrom.Get() != Road)
	{
		// The marking is painted like this road's lines; it can be repainted on its own afterwards.
		mPaintedFrom = Road;
		SetCustomizationData(Road->GetCustomizationData_Native());
	}
	RebuildPreview();
}

void ARMMarkingHologram::CheckValidPlacement()
{
	// No Super call on purpose (see the gantry hologram): the vanilla floor check is for boxes.
	if (!mOnRoad)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

void ARMMarkingHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableMarking* M = Cast<ARMBuildableMarking>(inBuildable))
	{
		M->SetShape(static_cast<uint8>(mVariant), mLaneWidth);
		M->SetLook(mRoadLook);
		// The road surface under it, worked out again here (on the server in multiplayer).
		const FVector Location = GetActorLocation();
		if (ARMBuildableRoad* Road = ARMBuildableMarking::FindRoadAt(GetWorld(), Location))
		{
			FTransform T;
			float Width = mLaneWidth;
			FRMMarkingDrape Drape;
			if (ARMBuildableMarking::PlanOn(Road, Location, GetActorForwardVector(), M->mKind, 1.f, T, Width, Drape))
			{
				M->SetDrape(Drape);
			}
		}
	}
}

void ARMMarkingHologram::RebuildPreview()
{
	if (mShownVariant == mVariant && FMath::IsNearlyEqual(mShownLaneWidth, mLaneWidth, 0.5f))
	{
		return;
	}
	mShownVariant = mVariant;
	mShownLaneWidth = mLaneWidth;
	mPreviewMesh->ClearAllMeshSections();
	const ARMBuildableMarking* Default = GetDefaultBuildable<ARMBuildableMarking>();
	FRMMesh Paint;
	ARMBuildableMarking::BuildMarkingMesh(Default ? Default->mKind : ERMMarkingKind::Arrow, static_cast<uint8>(mVariant), mLaneWidth, Paint, &mDrape);
	Paint.ApplyToSection(mPreviewMesh, 0, false);
	UpdateHologramLook();
}

void ARMMarkingHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateHologramLook();
}

void ARMMarkingHologram::UpdateHologramLook()
{
	// The marking's own paint material for the road's look, in the road's colours (the marking starts
	// with the road's paint), with the game's valid / invalid tint and outline on top.
	const ARMBuildableMarking* Default = GetDefaultBuildable<ARMBuildableMarking>();
	if (Default && mPreviewMesh->GetNumSections() > 0)
	{
		if (UMaterialInterface* Paint = Default->GetPaintMaterial(mRoadLook))
		{
			mPreviewMesh->SetMaterial(0, Paint);
			Default->ApplyMarkingColorsTo(mPreviewMesh, mCustomizationData);
			RMHologramLook::Apply(mPreviewMesh, nullptr, GetStencilForHologramMaterialState(mPlacementMaterialState));
			return;
		}
	}
	UMaterialInterface* Material = mPreviewMaterial;
	if (!Material)
	{
		Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
			? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	}
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}
