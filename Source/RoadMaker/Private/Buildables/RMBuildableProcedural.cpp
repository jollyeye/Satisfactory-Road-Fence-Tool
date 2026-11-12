#include "Buildables/RMBuildableProcedural.h"
#include "RoadMakerModule.h"
#include "FGFactoryColoringTypes.h"
#include "ProceduralMeshComponent.h"
#include "FGInventoryComponent.h"
#include "Materials/Material.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"
#include "Components/SplineComponent.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Buildables/RMBuildableMarking.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "FGDismantleInterface.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"

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

	mFar2Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProceduralMeshFar2"));
	mFar2Mesh->SetupAttachment(mMesh);
	mFar2Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mFar2Mesh->SetCanEverAffectNavigation(false);
	mFar2Mesh->SetCastShadow(false);

	// No game build effect (it would hide the piece for the whole animation and show nothing); see StartRiseAnimation.
	mSkipBuildEffect = true;
}

void ARMBuildableProcedural::BeginPlay()
{
	// Just built: not restored from a save, and not one of the actors that arrive with the world when it
	// loads or when a client joins (those all start within the first seconds).
	const UWorld* World = GetWorld();
	const bool bJustBuilt = !mLoadedFromSave && World && World->GetTimeSeconds() > 20.0;
	Super::BeginPlay();
	UpdateLODDistances();
	RebuildMesh();
	if (bJustBuilt)
	{
		StartRiseAnimation();
		// The game plays the construct sound as part of its build effect, which Road Maker pieces skip
		// (see the constructor): play it ourselves (cosmetic, does nothing on a dedicated server).
		PlayConstructSound();
		UE_LOG(LogRoadMaker, Display, TEXT("RMSound: construct sound requested for %s"), *GetName());
	}
}

void ARMBuildableProcedural::StartRiseAnimation()
{
	if (UWorld* World = GetWorld())
	{
		mRiseStart = World->GetTimeSeconds();
		SetActorScale3D(FVector(1.f, 1.f, 0.02f));
		World->GetTimerManager().SetTimer(mRiseTimer, this, &ARMBuildableProcedural::StepRiseAnimation, 0.02f, true);
	}
}

void ARMBuildableProcedural::StepRiseAnimation()
{
	const UWorld* World = GetWorld();
	const float T = World ? FMath::Clamp(static_cast<float>(World->GetTimeSeconds() - mRiseStart) / RiseDuration, 0.f, 1.f) : 1.f;
	const float Eased = T * T * (3.f - 2.f * T);
	SetActorScale3D(FVector(1.f, 1.f, FMath::Max(0.02f, Eased)));
	if (T >= 1.f)
	{
		SetActorScale3D(FVector::OneVector);
		if (World)
		{
			GetWorldTimerManager().ClearTimer(mRiseTimer);
		}
	}
}

void ARMBuildableProcedural::UpdateLODDistances()
{
	// Start detailed; UpdateLODsForCamera switches as the camera moves.
	SetShownLOD(0);
}

void ARMBuildableProcedural::SetShownLOD(int32 Level)
{
	if (!mMesh || !mFarMesh || !mFar2Mesh)
	{
		return;
	}
	mShownLOD = Level;
	// Checked every time: the game's own build/dismantle effects may change visibility too.
	auto Show = [](UProceduralMeshComponent* M, bool bVisible)
	{
		if (M->IsVisible() != bVisible)
		{
			M->SetVisibility(bVisible); // collision (on mMesh) is unaffected by visibility
		}
	};
	Show(mMesh, Level == 0);
	Show(mFarMesh, Level == 1);
	Show(mFar2Mesh, Level == 2);
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
				Piece->SetShownLOD(0);
			}
			continue;
		}
		const float Dist = FMath::Sqrt(Piece->mMesh->Bounds.GetBox().ComputeSquaredDistanceToPoint(Camera));
		// A little hysteresis so standing at a switch distance does not flicker.
		auto Beyond = [Dist](float Switch, bool bBeyondNow) { return Dist > (bBeyondNow ? Switch * 0.9f : Switch); };
		int32 Level = Beyond(Piece->mLODDistance, Piece->mShownLOD >= 1) ? 1 : 0;
		if (Level == 1 && Piece->mLOD2Distance > Piece->mLODDistance && Piece->mFar2Mesh->GetNumSections() > 0
			&& Beyond(Piece->mLOD2Distance, Piece->mShownLOD >= 2))
		{
			Level = 2;
		}
		Piece->SetShownLOD(Level);
	}
}

void ARMBuildableProcedural::ClearFarMesh()
{
	if (mFarMesh)
	{
		mFarMesh->ClearAllMeshSections();
	}
	if (mFar2Mesh)
	{
		mFar2Mesh->ClearAllMeshSections();
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

void ARMBuildableProcedural::ApplyFar2Section(const FRMMesh& Data, int32 Section, UMaterialInterface* Material)
{
	if (!mFar2Mesh || mLOD2Distance <= 0.f || IsRunningDedicatedServer())
	{
		return;
	}
	Data.ApplyToSection(mFar2Mesh, Section, false);
	mFar2Mesh->SetMaterial(Section, Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface));
}

void ARMBuildableProcedural::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableProcedural, mCostMultiplier);
	DOREPLIFETIME(ARMBuildableProcedural, mLook);
}

void ARMBuildableProcedural::PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
	Super::PostLoadGame_Implementation(saveVersion, gameVersion);
	mLoadedFromSave = true;
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

void ARMBuildableProcedural::GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const
{
	Super::GetChildDismantleActors_Implementation(out_ChildDismantleActors);
	UWorld* World = GetWorld();
	if (!DismantlesRiders() || !mMesh || !World)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (mRiderCacheTime < 0.0 || Now - mRiderCacheTime > 1.0)
	{
		mRiderCacheTime = Now;
		mRiderCache.Reset();
		const FBox Box = mMesh->Bounds.GetBox().ExpandBy(FVector(200.f, 200.f, 500.f));

		// On this piece = right above its surface (a short trace down hits this piece's mesh).
		auto IsOnThis = [this](const FVector& P)
		{
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(RMRiders), true);
			return mMesh->LineTraceComponent(Hit, P + FVector(0.f, 0.f, 150.f), P - FVector(0.f, 0.f, 250.f), Params);
		};
		// Most of the spline (at least 4 of 7 samples) must lie on this piece, so a path or track that
		// only touches it at one end stays with the road or intersection it really runs on.
		auto MostlyOnThis = [&IsOnThis](const USplineComponent* Spline)
		{
			const float Length = Spline->GetSplineLength();
			int32 On = 0;
			for (int32 K = 0; K <= 6; ++K)
			{
				if (IsOnThis(Spline->GetLocationAtDistanceAlongSpline(Length * K / 6.f, ESplineCoordinateSpace::World)))
				{
					++On;
				}
			}
			return On >= 4;
		};
		auto Consider = [&](AActor* Actor, const USplineComponent* Spline)
		{
			if (Actor && Spline && IsValid(Actor) && Box.Intersect(Spline->Bounds.GetBox()) && MostlyOnThis(Spline))
			{
				mRiderCache.Add(Actor);
			}
		};
		for (TActorIterator<AFGBuildableRailroadTrack> It(World); It; ++It)
		{
			Consider(*It, It->GetSplineComponent());
		}
		for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It)
		{
			Consider(*It, It->GetSplineComponent());
		}
		// Lane markings painted on this piece go with it.
		for (TActorIterator<ARMBuildableMarking> It(World); It; ++It)
		{
			ARMBuildableMarking* Marking = *It;
			if (Marking && IsValid(Marking) && Marking != this && Box.IsInside(Marking->GetActorLocation()) && IsOnThis(Marking->GetActorLocation()))
			{
				mRiderCache.Add(Marking);
			}
		}
	}

	for (const TWeakObjectPtr<AActor>& Weak : mRiderCache)
	{
		AActor* Actor = Weak.Get();
		if (!Actor || out_ChildDismantleActors.Contains(Actor))
		{
			continue;
		}
		out_ChildDismantleActors.Add(Actor);
		// Their own attached pieces too (e.g. a track's switch control).
		if (Actor->Implements<UFGDismantleInterface>())
		{
			TArray<AActor*> Sub;
			IFGDismantleInterface::Execute_GetChildDismantleActors(Actor, Sub);
			for (AActor* S : Sub)
			{
				out_ChildDismantleActors.AddUnique(S);
			}
		}
	}
}

TSubclassOf<UFGFactoryCustomizationDescriptor_Swatch> ARMBuildableProcedural::sDefaultRoadSwatch;

void ARMBuildableProcedural::SetLook(uint8 Look)
{
	if (Look == mLook)
	{
		return;
	}
	mLook = Look;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableProcedural::SetPreviewLook(uint8 Look)
{
	if (Look == mPreviewLook)
	{
		return;
	}
	mPreviewLook = Look;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

UMaterialInterface* ARMBuildableProcedural::LookMaterial(const TArray<UMaterialInterface*>& Looks, UMaterialInterface* Default) const
{
	const uint8 Look = GetShownLook();
	return Look > 0 && Looks.IsValidIndex(Look - 1) && Looks[Look - 1] ? Looks[Look - 1] : Default;
}

void ARMBuildableProcedural::GetPaintColours(FLinearColor& OutPrimary, FLinearColor& OutSecondary) const
{
	const FResolvedFactoryCustomizationData Resolved(mCustomizationData, mColorSlot);
	OutPrimary = Resolved.PrimaryColor;
	OutSecondary = Resolved.SecondaryColor;
	AdjustColorsForLook(mCustomizationData, OutPrimary, OutSecondary);
}

void ARMBuildableProcedural::AdjustColorsForLook(const FFactoryCustomizationData& Data, FLinearColor& Primary, FLinearColor& Secondary) const
{
	// Every look uses the colours as painted; unpainted roads show the game's default swatch (FICSIT orange).
}
