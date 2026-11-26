#include "Buildables/RMBuildableBarrier.h"
#include "Holograms/RMOverlayLines.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "GameFramework/Pawn.h"
#include "RoadMakerModule.h"

ARMBuildableBarrier::ARMBuildableBarrier(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mLODDistance = 0.f; // small: drawn in full
	mArmMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BarrierArm"));
	mArmMesh->SetupAttachment(mMesh);
	mArmMesh->bUseComplexAsSimpleCollision = true;
	// Blocks vehicles only (players walk under / through), and only while always closed (UpdateArm).
	mArmMesh->SetCollisionObjectType(ECC_WorldDynamic);
	mArmMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	mArmMesh->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Block);
	mArmMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block); // the build gun can aim at it (dismantle, E)
	mArmMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	mArmMesh->SetCastShadow(true);
}

void ARMBuildableBarrier::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableBarrier, mArmLength);
	DOREPLIFETIME(ARMBuildableBarrier, mMode);
	DOREPLIFETIME(ARMBuildableBarrier, mVehicleNear);
}

void ARMBuildableBarrier::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(mDetectTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableBarrier::DetectVehicles), 0.25f, true,
			FMath::FRandRange(0.f, 0.25f));
	}
	UpdateArm(true);
}

void ARMBuildableBarrier::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(mDetectTimer);
	GetWorldTimerManager().ClearTimer(mArmTimer);
	Super::EndPlay(EndPlayReason);
}

void ARMBuildableBarrier::SetArmLength(float Length)
{
	mArmLength = FMath::Clamp(Length, MinArm, MaxArm);
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableBarrier::BuildPostMesh(FRMMesh& Out)
{
	Out.AddBox(FVector(0.f, 0.f, PostHeight * 0.5f), FVector::ForwardVector, FVector::RightVector, FVector::UpVector,
		FVector(PostHalf, PostHalf, PostHeight * 0.5f), 100.f);
}

void ARMBuildableBarrier::BuildArmMesh(float Length, FRMMesh& OutWhite, FRMMesh& OutRed)
{
	// Arm space: the hinge at the origin, the arm along +Y from the post's face.
	const int32 Stripes = FMath::Max(1, FMath::RoundToInt(Length / StripeLength));
	const float Step = Length / Stripes;
	for (int32 I = 0; I < Stripes; ++I)
	{
		FRMMesh& Out = (I % 2 == 0) ? OutRed : OutWhite;
		Out.AddBox(FVector(0.f, PostHalf + Step * (I + 0.5f), 0.f), FVector::ForwardVector, FVector::RightVector, FVector::UpVector,
			FVector(ArmHalf, Step * 0.5f, ArmHalf), 100.f);
	}
}

namespace RMBarrier
{
	/** A mesh into a section with one flat colour (the overlay material: unlit, temporary look). */
	void ApplyColored(const FRMMesh& Mesh, UProceduralMeshComponent* Comp, int32 Section, const FLinearColor& Color, bool bCollision)
	{
		TArray<FLinearColor> Colors;
		Colors.Init(Color, Mesh.Vertices.Num());
		Mesh.ApplyToSection(Comp, Section, bCollision, &Colors);
		if (UMaterialInterface* M = RMOverlayLines::OverlayMaterial())
		{
			Comp->SetMaterial(Section, M);
		}
	}

	bool IsKindOf(const UClass* Class, const TCHAR* Name)
	{
		for (; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == FName(Name))
			{
				return true;
			}
		}
		return false;
	}
}

void ARMBuildableBarrier::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Post;
	BuildPostMesh(Post);
	Post.ApplyToSection(mMesh, 0, true);
	ApplyMaterial(0, mPostMaterial);

	mArmMesh->ClearAllMeshSections();
	mArmMesh->SetRelativeLocation(FVector(0.f, 0.f, HingeHeight));
	FRMMesh White, Red;
	BuildArmMesh(mArmLength, White, Red);
	RMBarrier::ApplyColored(White, mArmMesh, 0, FLinearColor(0.9f, 0.9f, 0.9f), true);
	RMBarrier::ApplyColored(Red, mArmMesh, 1, FLinearColor(0.8f, 0.05f, 0.03f), true);
	UpdateArm(true);
}

void ARMBuildableBarrier::SetMode(ERMBarrierMode Mode)
{
	if (!HasAuthority() || Mode == mMode)
	{
		return;
	}
	mMode = Mode;
	UE_LOG(LogRoadMaker, Verbose, TEXT("%s: barrier mode %d"), *GetName(), int32(Mode));
	ForceNetUpdate();
	UpdateArm(false);
}

void ARMBuildableBarrier::OnUse_Implementation(AFGCharacterPlayer* byCharacter, const FUseState& state)
{
	// No window yet: E cycles the mode (on the server; the mode is sent to everyone).
	if (HasAuthority())
	{
		SetMode(ERMBarrierMode((uint8(mMode) + 1) % 3));
	}
}

FText ARMBuildableBarrier::ModeName(ERMBarrierMode Mode)
{
	switch (Mode)
	{
	case ERMBarrierMode::Open:
		return NSLOCTEXT("RoadMaker", "BarrierModeOpen", "Always open");
	case ERMBarrierMode::Closed:
		return NSLOCTEXT("RoadMaker", "BarrierModeClosed", "Always closed");
	default:
		return NSLOCTEXT("RoadMaker", "BarrierModeAuto", "Automatic");
	}
}

FText ARMBuildableBarrier::GetLookAtDecription_Implementation(AFGCharacterPlayer* byCharacter, const FUseState& state) const
{
	return FText::Format(NSLOCTEXT("RoadMaker", "BarrierLookAt", "Boom Barrier: {0} (change)"), ModeName(mMode));
}

FText ARMBuildableBarrier::AM_Info_Vehicle_Waiting() const
{
	return NSLOCTEXT("RoadMaker", "BarrierInfoWaiting", "A vehicle is near the barrier, on either side of the arm.");
}

FText ARMBuildableBarrier::AM_Info_Open() const
{
	return NSLOCTEXT("RoadMaker", "BarrierInfoOpen", "On: always open. Off: always closed (vehicles are stopped).");
}

FText ARMBuildableBarrier::AM_Info_Automatic() const
{
	return NSLOCTEXT("RoadMaker", "BarrierInfoAutomatic", "The arm lifts by itself while a vehicle is near.");
}

void ARMBuildableBarrier::DetectVehicles()
{
	// Vehicles (pawns of the game's wheeled vehicle class) within 12 m along the road, the arm's reach across it (plus
	// 3 m), up to 6 m above: few actors, cheap.
	const FTransform& T = GetActorTransform();
	bool bNear = false;
	for (TActorIterator<APawn> It(GetWorld()); It && !bNear; ++It)
	{
		if (!RMBarrier::IsKindOf(It->GetClass(), TEXT("FGWheeledVehicle")))
		{
			continue;
		}
		const FVector L = T.InverseTransformPosition(It->GetActorLocation());
		bNear = FMath::Abs(L.X) < 1200.f && L.Y > -300.f && L.Y < mArmLength + 300.f && L.Z > -300.f && L.Z < 600.f;
	}
	if (bNear != mVehicleNear)
	{
		mVehicleNear = bNear;
		ForceNetUpdate();
		UpdateArm(false);
	}
}

void ARMBuildableBarrier::UpdateArm(bool bSnap)
{
	// Vehicles are stopped only while always closed (automatic never blocks; open is up anyway).
	mArmMesh->SetCollisionResponseToChannel(ECC_Vehicle, mMode == ERMBarrierMode::Closed ? ECR_Block : ECR_Ignore);
	mArmMesh->SetCollisionEnabled(mMode == ERMBarrierMode::Closed ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
	const float Target = ShouldBeOpen() ? OpenAngle : 0.f;
	if (bSnap || !HasActorBegunPlay())
	{
		mArmAngle = Target;
		mArmMesh->SetRelativeRotation(FQuat(FVector::ForwardVector, FMath::DegreesToRadians(mArmAngle)));
		return;
	}
	if (!GetWorldTimerManager().IsTimerActive(mArmTimer))
	{
		GetWorldTimerManager().SetTimer(mArmTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableBarrier::StepArm), 1.f / 30.f, true);
	}
}

void ARMBuildableBarrier::StepArm()
{
	// About a second and a half from closed to open.
	const float Target = ShouldBeOpen() ? OpenAngle : 0.f;
	mArmAngle = FMath::FInterpConstantTo(mArmAngle, Target, 1.f / 30.f, 60.f);
	mArmMesh->SetRelativeRotation(FQuat(FVector::ForwardVector, FMath::DegreesToRadians(mArmAngle)));
	if (FMath::IsNearlyEqual(mArmAngle, Target, 0.01f))
	{
		GetWorldTimerManager().ClearTimer(mArmTimer);
	}
}
