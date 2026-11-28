#include "Buildables/RMBuildableBarrier.h"
#include "Holograms/RMOverlayLines.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "GameFramework/Pawn.h"
#include "FGFactoryColoringTypes.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "RMGeometry.h"
#include "UI/RMBarrierWindow.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableRoad.h"

ARMBuildableBarrier::ARMBuildableBarrier(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mLODDistance = 0.f; // small: drawn in full
	// E opens our window, like the game's building windows ("Press E to configure"): the game's own members, private,
	// set through reflection on this class's defaults.
	if (FSoftClassProperty* Widget = FindFProperty<FSoftClassProperty>(AFGBuildable::StaticClass(), TEXT("mInteractWidgetSoftClass")))
	{
		Widget->SetPropertyValue_InContainer(this, FSoftObjectPtr(URMBarrierWindow::StaticClass()));
	}
	if (FBoolProperty* Useable = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mIsUseable")))
	{
		Useable->SetPropertyValue_InContainer(this, true);
	}
	mAllowColoring = true; // the body, in the primary colour (FICSIT by default)
	mArmMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BarrierArm"));
	mArmMesh->SetupAttachment(mMesh);
	// A hologram: nothing collides with it, nothing aims at it (the build gun aims at the post), no shadow.
	mArmMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mArmMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	mArmMesh->SetCanEverAffectNavigation(false);
	mArmMesh->SetCastShadow(false);
	// Small: not drawn past 250 m (many barriers cost nothing far away).
	mMesh->LDMaxDrawDistance = MaxDrawDistance;
	mMesh->CachedMaxDrawDistance = MaxDrawDistance;
	mArmMesh->LDMaxDrawDistance = MaxDrawDistance;
	mArmMesh->CachedMaxDrawDistance = MaxDrawDistance;
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
		GetWorldTimerManager().SetTimer(mDetectTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableBarrier::DetectVehicles), IdleDetectInterval, true,
			FMath::FRandRange(0.f, IdleDetectInterval));
	}
	UpdateArm(true);
}

void ARMBuildableBarrier::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (EndPlayReason == EEndPlayReason::Destroyed && HasAuthority())
	{
		ReleaseVehicles(); // dismantled while holding vehicles
	}
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

void ARMBuildableBarrier::BuildPostMesh(FRMMesh& Steel, FRMMesh& Body, FRMMesh& Light)
{
	const FVector X = FVector::ForwardVector, Y = FVector::RightVector, Z = FVector::UpVector;
	// Steel base plate with a bolt in each corner.
	Steel.AddBox(FVector(0.f, 0.f, PlateHeight * 0.5f), X, Y, Z, FVector(PlateHalf, PlateHalf, PlateHeight * 0.5f), 100.f);
	const float B = PlateHalf - 6.f;
	for (const FVector2D& C : { FVector2D(B, B), FVector2D(-B, B), FVector2D(B, -B), FVector2D(-B, -B) })
	{
		RMGeometry::AddBeveledPost(FVector(C.X, C.Y, PlateHeight), X, Y, Z, 2.2f, 2.5f, 0.8f, Steel);
	}
	// The painted body (rounded edges in the lighting), the light band all around, the steel cap.
	const int32 BodyFirst = Body.Vertices.Num();
	RMGeometry::AddBeveledPost(FVector(0.f, 0.f, PlateHeight), X, Y, Z, BodyHalf, BodyTop - PlateHeight, 3.f, Body);
	// The faces along the road (+X and -X: the 1st and 3rd side, 12 vertices each) get u + 2: the material draws the
	// vent grille there (in the normal map), low on the face.
	for (const int32 Face : { 0, 2 })
	{
		for (int32 V = BodyFirst + Face * 12; V < BodyFirst + Face * 12 + 12; ++V)
		{
			Body.UVs[V].X += 2.f;
		}
	}
	Light.AddBox(FVector(0.f, 0.f, (BodyTop + LightTop) * 0.5f), X, Y, Z, FVector(BodyHalf - 0.5f, BodyHalf - 0.5f, (LightTop - BodyTop) * 0.5f), 100.f);
	RMGeometry::AddBeveledPost(FVector(0.f, 0.f, LightTop), X, Y, Z, BodyHalf + 2.f, PostHeight - LightTop, 1.5f, Steel);
	{
		// The cap's underside (it overhangs the band by 2.5 cm), facing down.
		const float C = BodyHalf + 2.f;
		Steel.AddQuad(FVector(-C, -C, LightTop), FVector(C, -C, LightTop), FVector(C, C, LightTop), FVector(-C, C, LightTop), -Z,
			FVector2D(0.f, 0.f), FVector2D(1.f, 0.f), FVector2D(1.f, 1.f), FVector2D(0.f, 1.f));
	}
	// The hinge block on the road side, the arm comes out of its face.
	const float HingeDepth = HingeFace - BodyHalf;
	Steel.AddBox(FVector(0.f, BodyHalf + HingeDepth * 0.5f, HingeHeight), X, Y, Z, FVector(11.f, HingeDepth * 0.5f, 13.f), 100.f);
	Steel.AddBox(FVector(0.f, HingeFace + 0.6f, HingeHeight), X, Y, Z, FVector(6.f, 0.6f, ArmHalfHeight + 2.f), 100.f); // the emitter's rim
	RMGeometry::MapToPoleChrome(Steel);
}

void ARMBuildableBarrier::BuildArmMesh(float Length, FRMMesh& Out)
{
	// Arm space: the hinge block's face at the origin, the arm along +Y to PostHalf + Length from the post's centre.
	// UV0: x = stripes (a red and a white one per 1.0), y = 0..1 across each long face (the material's edge glow).
	const float L = FMath::Max(PostHalf + Length - HingeFace, 10.f);
	const float U = L / (2.f * StripeLength);
	const float T = ArmHalfThickness, H = ArmHalfHeight;
	auto Long = [&Out, L, U](const FVector& A, const FVector& Bv, const FVector& N)
	{
		// A and B: the face's two long edges at the hinge end.
		Out.AddQuad(A, A + FVector(0.f, L, 0.f), Bv + FVector(0.f, L, 0.f), Bv, N,
			FVector2D(0.f, 0.f), FVector2D(U, 0.f), FVector2D(U, 1.f), FVector2D(0.f, 1.f));
	};
	Long(FVector(T, 0.f, -H), FVector(T, 0.f, H), FVector::ForwardVector);
	Long(FVector(-T, 0.f, -H), FVector(-T, 0.f, H), -FVector::ForwardVector);
	Long(FVector(-T, 0.f, H), FVector(T, 0.f, H), FVector::UpVector);
	Long(FVector(-T, 0.f, -H), FVector(T, 0.f, -H), -FVector::UpVector);
	Out.AddQuad(FVector(-T, L, -H), FVector(T, L, -H), FVector(T, L, H), FVector(-T, L, H), FVector::RightVector,
		FVector2D(U, 0.f), FVector2D(U, 0.f), FVector2D(U, 1.f), FVector2D(U, 1.f));
}

FLinearColor ARMBuildableBarrier::ModeColor(ERMBarrierMode Mode)
{
	switch (Mode)
	{
	case ERMBarrierMode::Open:
		return FLinearColor(0.05f, 1.f, 0.12f);
	case ERMBarrierMode::Closed:
		return FLinearColor(0.45f, 0.f, 0.f); // pure red, dimmer: bright red turns orange in the tone mapping
	default:
		return FLinearColor(1.f, 0.32f, 0.f);
	}
}

void ARMBuildableBarrier::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Steel, Body, Light;
	BuildPostMesh(Steel, Body, Light);
	Steel.ApplyToSection(mMesh, 0, true);
	ApplyMaterial(0, mPostMaterial);
	Body.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(1, mBodyMaterial);
	Light.ApplyToSection(mMesh, 2, true);
	ApplyMaterial(2, mLightMaterial);

	mArmMesh->ClearAllMeshSections();
	mArmMesh->SetRelativeLocation(FVector(0.f, HingeFace, HingeHeight));
	FRMMesh Arm;
	BuildArmMesh(mArmLength, Arm);
	Arm.ApplyToSection(mArmMesh, 0, false);
	if (mHoloMaterial)
	{
		mArmMesh->SetMaterial(0, mHoloMaterial);
	}
	ApplyPaint(mCustomizationData);
	UpdateArm(true);
}

void ARMBuildableBarrier::ApplyPaint(const FFactoryCustomizationData& Data)
{
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	ForEachMeshComponent([&P, &S](UProceduralMeshComponent* M)
	{
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
	});
	const FLinearColor L = ModeColor(mMode);
	mMesh->SetCustomPrimitiveDataVector4(LightColorIndex, FVector4(L.R, L.G, L.B, 1.0));
}

void ARMBuildableBarrier::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyPaint(customizationData);
}

void ARMBuildableBarrier::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyPaint(customizationData);
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
	if (Mode != ERMBarrierMode::Closed)
	{
		ReleaseVehicles();
	}
	else
	{
		DetectVehicles(); // stop the ones already coming
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

FText ARMBuildableBarrier::AM_Info_Vehicle_Waiting() const
{
	return NSLOCTEXT("RoadMaker", "BarrierInfoWaiting", "A vehicle is near the barrier, on either side of the arm.");
}

FText ARMBuildableBarrier::AM_Info_Open() const
{
	return NSLOCTEXT("RoadMaker", "BarrierInfoOpenNow", "The arm is drawn back (always open, or automatic with a vehicle near).");
}

FText ARMBuildableBarrier::AM_Info_Mode() const
{
	return NSLOCTEXT("RoadMaker", "BarrierInfoMode", "Sets the barrier's mode. Automatic: the arm draws back while a vehicle is near. Open: always drawn back. Closed: always out; self-driving vehicles coming towards it stop (their autopilot is turned off) and drive on when it opens.");
}

void ARMBuildableBarrier::DetectVehicles()
{
	// The game's wheeled vehicles (its list of them, with their real place even when drawn as a far proxy): near =
	// within 12 m along the road, the arm's reach across it (plus 3 m), up to 6 m above. Always closed, a self-driving
	// vehicle without a driver coming towards the arm (stopped with its front just before the arm) gets its autopilot turned off.
	AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(GetWorld());
	if (!Vehicles)
	{
		return;
	}
	const FTransform& T = GetActorTransform();
	const FVector Here = T.GetLocation();
	const bool bClosed = mMode == ERMBarrierMode::Closed;
	bool bNear = false;
	// Cheap first test: within the arm's length plus 30 m of the post (seen in 3D). The closest one sets the next
	// check: 10 a second with a vehicle within that, twice a second otherwise (a thousand barriers stay cheap).
	const float Reach = PostHalf + mArmLength + ApproachReach + 500.f;
	bool bAnyClose = false;
	for (AFGWheeledVehicleIdentifier* Id : Vehicles->GetAllVehicles())
	{
		AFGWheeledVehicle* Vehicle = Id ? Id->GetOwnerVehicle() : nullptr;
		if (!Vehicle)
		{
			continue;
		}
		FVector Where;
		FRotator Facing;
		Vehicle->GetVehicleLocationAndRotation(Where, Facing);
		if (FVector::DistSquared(Where, Here) > FMath::Square(Reach))
		{
			continue;
		}
		bAnyClose = true;
		const FVector L = T.InverseTransformPosition(Where);
		if (L.Y < -300.f || L.Y > PostHalf + mArmLength + 300.f || L.Z < -300.f || L.Z > 600.f)
		{
			continue;
		}
		bNear |= FMath::Abs(L.X) < 1200.f;
		if (!bClosed || !Id->IsAutopilotEnabled() || Vehicle->GetDriver() || FMath::Abs(L.X) > ApproachReach)
		{
			continue;
		}
		// Coming towards the arm: its heading (backwards when reversing) points at the arm's line.
		FVector Heading = T.InverseTransformVectorNoScale(Facing.Vector());
		if (Vehicle->GetForwardSpeed() < -10.f)
		{
			Heading = -Heading;
		}
		if (L.X * Heading.X >= 0.f)
		{
			continue;
		}
		// Stopped just before the arm: once the gap between its front and the arm's line is within what it covers until
		// the next check (and a little more). Its front: half its length (local bounds, along its forward axis) ahead
		// of its centre, measured along the road.
		const float HalfLength = FMath::Clamp(Vehicle->CalculateComponentsBoundingBoxInLocalSpace(true).GetExtent().X, 100.f, 800.f);
		const float Gap = FMath::Abs(L.X) - HalfLength * FMath::Max(FMath::Abs(Heading.X), 0.3f);
		const float Speed = FMath::Abs(float(Vehicle->GetSpeedInKMH())) / 0.036f; // cm/s
		if (Gap > StopGap + Speed * DetectInterval * 1.5f || Gap < -100.f)
		{
			continue; // still further away, or already through
		}
		Id->SetAutopilotEnabled(false);
		UE_LOG(LogRoadMaker, Log, TEXT("%s: autopilot off for %s (front %.1f m before the arm, %d km/h)"), *GetName(), *Vehicle->GetName(), Gap / 100.f,
			Vehicle->GetSpeedInKMH());
	}
	if (bNear != mVehicleNear)
	{
		mVehicleNear = bNear;
		ForceNetUpdate();
		UpdateArm(false);
	}
	if (bAnyClose != mDetectFast)
	{
		mDetectFast = bAnyClose;
		const float Interval = mDetectFast ? DetectInterval : IdleDetectInterval;
		GetWorldTimerManager().SetTimer(mDetectTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableBarrier::DetectVehicles), Interval, true);
	}
}

void ARMBuildableBarrier::ReleaseVehicles()
{
	// No list kept (a vehicle can be dismantled meanwhile): every vehicle in the barrier's zone (the arm's reach across
	// plus 3 m, up to 30 m before or after the arm) with its autopilot off and nobody driving gets it back on.
	AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(GetWorld());
	if (!Vehicles)
	{
		return;
	}
	const FTransform& T = GetActorTransform();
	for (AFGWheeledVehicleIdentifier* Id : Vehicles->GetAllVehicles())
	{
		AFGWheeledVehicle* Vehicle = Id ? Id->GetOwnerVehicle() : nullptr;
		if (!Vehicle || Vehicle->GetDriver() || Id->IsAutopilotEnabled() || !Id->CanEnableAutopilot())
		{
			continue;
		}
		const FVector L = T.InverseTransformPosition(Vehicle->GetVehicleLocation());
		if (L.Y < -300.f || L.Y > PostHalf + mArmLength + 300.f || L.Z < -300.f || L.Z > 600.f || FMath::Abs(L.X) > ApproachReach)
		{
			continue;
		}
		Id->SetAutopilotEnabled(true);
		UE_LOG(LogRoadMaker, Log, TEXT("%s: autopilot back on for %s"), *GetName(), *Vehicle->GetName());
	}
}

void ARMBuildableBarrier::UpdateArm(bool bSnap)
{
	const FLinearColor L = ModeColor(mMode);
	mMesh->SetCustomPrimitiveDataVector4(LightColorIndex, FVector4(L.R, L.G, L.B, 1.0));
	const float Target = ShouldBeOpen() ? 0.f : 1.f;
	if (bSnap || !HasActorBegunPlay())
	{
		mArmOut = Target;
		ApplyArm();
		return;
	}
	if (!GetWorldTimerManager().IsTimerActive(mArmTimer))
	{
		GetWorldTimerManager().SetTimer(mArmTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableBarrier::StepArm), 1.f / 30.f, true);
	}
}

void ARMBuildableBarrier::StepArm()
{
	// About a second all the way out or back.
	const float Target = ShouldBeOpen() ? 0.f : 1.f;
	mArmOut = FMath::FInterpConstantTo(mArmOut, Target, 1.f / 30.f, 1.f);
	ApplyArm();
	if (FMath::IsNearlyEqual(mArmOut, Target, 0.001f))
	{
		GetWorldTimerManager().ClearTimer(mArmTimer);
	}
}

void ARMBuildableBarrier::ApplyArm()
{
	// Drawn back into the hinge block: squeezed along its length towards the block's face (eased), hidden once in.
	const float Ease = mArmOut * mArmOut * (3.f - 2.f * mArmOut);
	mArmMesh->SetRelativeScale3D(FVector(1.f, FMath::Max(Ease, 0.01f), 1.f));
	mArmMesh->SetVisibility(Ease > 0.01f);
}
