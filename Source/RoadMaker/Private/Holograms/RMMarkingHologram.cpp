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

namespace
{
	/** The road a point on its surface belongs to (nearest road frame). */
	ARMBuildableRoad* FindRoadAt(UWorld* World, const FVector& Location)
	{
		TArray<ARMBuildableRoad*> Roads;
		RMRoadUtils::GatherNearby(World, Location, 150.f, Roads);
		ARMBuildableRoad* Best = nullptr;
		float BestDist = TNumericLimits<float>::Max();
		for (ARMBuildableRoad* Road : Roads)
		{
			if (!Road || Road->GetIsDismantled() || Road->IsActorBeingDestroyed())
			{
				continue;
			}
			TArray<FRMFrame> Frames;
			Road->GetWorldFrames(Frames, 50.f);
			for (const FRMFrame& F : Frames)
			{
				const FVector Rel = Location - F.Pos;
				// Within the road's width and just above / below its surface.
				if (FMath::Abs(FVector::DotProduct(Rel, F.Right)) > Road->GetRoadWidth() * 0.5f + 20.f)
				{
					continue;
				}
				const float D = Rel.SizeSquared();
				if (D < BestDist)
				{
					BestDist = D;
					Best = Road;
				}
			}
		}
		return Best;
	}
}

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
		Road = FindRoadAt(World, hitResult.Location); // aiming at a marking: the road under it
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

	// The spot along the road: the frame nearest the aim; the lane: the centre nearest across.
	const FRMFrame* Near = &Frames[0];
	float Best = TNumericLimits<float>::Max();
	for (const FRMFrame& F : Frames)
	{
		const float D = FVector::DistSquared(F.Pos, hitResult.Location);
		if (D < Best)
		{
			Best = D;
			Near = &F;
		}
	}
	TArray<float> Offsets;
	float LaneWidth = 200.f;
	Road->GetLaneCentres(Offsets, LaneWidth);
	const float U = FVector::DotProduct(hitResult.Location - Near->Pos, Near->Right);
	float Off = 0.f;
	float BestOff = TNumericLimits<float>::Max();
	for (float O : Offsets)
	{
		if (FMath::Abs(O - U) < BestOff)
		{
			BestOff = FMath::Abs(O - U);
			Off = O;
		}
	}

	// Faces the way you look along the road.
	FVector LookDir = FVector::ForwardVector;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		LookDir = PC->PlayerCameraManager->GetCameraRotation().Vector();
	}
	const FVector Dir = FVector::DotProduct(LookDir, Near->Fwd) >= 0.f ? Near->Fwd : -Near->Fwd;
	const FRotator Rot = FRotationMatrix::MakeFromXZ(Dir, Near->Up).Rotator();
	SetActorLocationAndRotation(Near->Pos + Near->Right * Off, Rot);

	mLaneWidth = LaneWidth;
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
	ARMBuildableMarking::BuildMarkingMesh(Default ? Default->mKind : ERMMarkingKind::Arrow, static_cast<uint8>(mVariant), mLaneWidth, Paint);
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
	UMaterialInterface* Material = mPreviewMaterial;
	if (!Material)
	{
		Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
			? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	}
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}
