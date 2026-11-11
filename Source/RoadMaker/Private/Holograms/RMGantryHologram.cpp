#include "Holograms/RMGantryHologram.h"
#include "RMRoadUtils.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableGantry.h"
#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

ARMGantryHologram::ARMGantryHologram()
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

void ARMGantryHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMGantryHologram, mStep);
	DOREPLIFETIME(ARMGantryHologram, mHalfSpan);
	DOREPLIFETIME(ARMGantryHologram, mBeamBottom);
}

int32 ARMGantryHologram::GetBaseCostMultiplier() const
{
	return ARMBuildableGantry::CostUnits(mHalfSpan, mBeamBottom);
}

bool ARMGantryHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	return mStep == 1 || Super::IsValidHitResult(hitResult);
}

void ARMGantryHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	if (mStep == 0)
	{
		ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(hitResult.GetActor());
		TArray<FRMFrame> Frames;
		if (Road && !Road->GetIsDismantled() && !Road->IsActorBeingDestroyed())
		{
			Road->GetWorldFrames(Frames, 25.f);
		}
		mOnRoad = Frames.Num() >= 2;
		if (mOnRoad)
		{
			float Best = TNumericLimits<float>::Max();
			const FRMFrame* Near = &Frames[0];
			for (const FRMFrame& F : Frames)
			{
				const float D = FVector::DistSquared2D(F.Pos, hitResult.Location);
				if (D < Best)
				{
					Best = D;
					Near = &F;
				}
			}
			// Across the road, level: X along the road (horizontal), Y across.
			const FVector Fwd = Near->Fwd.GetSafeNormal2D();
			SetActorLocationAndRotation(Near->Pos, FRotator(0.f, Fwd.Rotation().Yaw, 0.f));
			mHalfSpan = FMath::Max(100.f, Road->GetRoadWidth() * 0.5f - ARMBuildableGantry::ColumnInset);
		}
		else
		{
			SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
		}
	}
	else
	{
		// The beam is as high as the aim passes the centre of the gantry (closest point between the
		// view line and the vertical line through the gantry), in 2 m steps.
		const FVector Origin = GetActorLocation();
		FVector Start = hitResult.TraceStart;
		FVector Dir = (hitResult.TraceEnd - hitResult.TraceStart).GetSafeNormal();
		// The aim at the sky has no usable trace: use the player's camera.
		APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager)
		{
			Start = PC->PlayerCameraManager->GetCameraLocation();
			Dir = PC->PlayerCameraManager->GetCameraRotation().Vector();
		}
		const FVector W0 = Start - Origin;
		const float B = Dir.Z;
		const float D = FVector::DotProduct(Dir, W0);
		const float E = W0.Z;
		const float Denom = 1.f - B * B;
		float Height = -1.f;
		if (Denom > 0.01f)
		{
			Height = (E - B * D) / Denom;
			const float Steps = FMath::RoundToFloat(Height / ARMBuildableGantry::HeightStep);
			mBeamBottom = FMath::Clamp(Steps * ARMBuildableGantry::HeightStep, ARMBuildableGantry::MinClearance, ARMBuildableGantry::MaxClearance);
		}
		if (!FMath::IsNearlyEqual(mShownBottom, mBeamBottom, 0.5f))
		{
			UE_LOG(LogRoadMaker, Display, TEXT("Gantry height step: camera %s dir %s -> aim height %.0f, beam bottom %.0f"),
				*Start.ToCompactString(), *Dir.ToCompactString(), Height, mBeamBottom);
		}
	}
	RebuildPreview();
}

bool ARMGantryHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mStep == 0)
	{
		if (!mOnRoad)
		{
			return false;
		}
		mStep = 1;
		mBeamBottom = ARMBuildableGantry::MinClearance;
		UE_LOG(LogRoadMaker, Display, TEXT("Gantry: first click, now setting the height"));
		return false;
	}
	return true;
}

void ARMGantryHologram::CheckValidPlacement()
{
	// No Super call on purpose (see the intersection hologram): the vanilla floor check is for boxes.
	if (mStep == 0 && !mOnRoad)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

void ARMGantryHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableGantry* G = Cast<ARMBuildableGantry>(inBuildable))
	{
		G->SetShape(mHalfSpan, mBeamBottom);
		G->SetCostMultiplier(GetBaseCostMultiplier());
	}
}

AActor* ARMGantryHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	if (Built)
	{
		mStep = 0; // next one starts over
		mBeamBottom = ARMBuildableGantry::MinClearance;
	}
	return Built;
}

void ARMGantryHologram::RebuildPreview()
{
	if (FMath::IsNearlyEqual(mShownHalfSpan, mHalfSpan, 0.5f) && FMath::IsNearlyEqual(mShownBottom, mBeamBottom, 0.5f))
	{
		return;
	}
	mShownHalfSpan = mHalfSpan;
	mShownBottom = mBeamBottom;
	mPreviewMesh->ClearAllMeshSections();
	FRMMesh Steel;
	ARMBuildableGantry::BuildGantryMesh(mHalfSpan, mBeamBottom, Steel);
	Steel.ApplyToSection(mPreviewMesh, 0, false);
	UpdateHologramLook();
}

void ARMGantryHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateHologramLook();
}

void ARMGantryHologram::UpdateHologramLook()
{
	UMaterialInterface* Material = mPreviewMaterial;
	if (!Material)
	{
		Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
			? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	}
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}
