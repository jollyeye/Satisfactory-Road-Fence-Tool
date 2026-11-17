#include "Buildables/RMBuildableBlueprintRoad.h"
#include "FGDismantleInterface.h"
#include "EngineUtils.h"
#include "RoadMakerModule.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "FGBlueprintProxy.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGCharacterPlayer.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Equipment/FGBuildGunDismantle.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Templates/RMRoadTemplate.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace RMBlueprintRoad
{
	TSet<TWeakObjectPtr<ARMBuildableBlueprintRoad>> GAll;
	bool GShown = false;
	bool GSolid = false;

	/**
	 * Whether the local player holds the dismantle tool or draws in Road Mode (shown); solid (can be aimed at) only with
	 * the dismantle tool: in Road Mode its collision got in the way of the vehicle paths being checked as they were
	 * placed (red), and Road Mode snaps by the aim point anyway.
	 */
	bool WantShown(bool& bOutSolid)
	{
		bOutSolid = false;
		if (!GEngine)
		{
			return false;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || !World->IsGameWorld())
			{
				continue;
			}
			APlayerController* PC = World->GetFirstPlayerController();
			AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
			AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
			if (!Gun || !Gun->IsEquipped())
			{
				continue;
			}
			if (Cast<UFGBuildGunStateDismantle>(Gun->GetCurrentState()))
			{
				bOutSolid = true;
				return true;
			}
			if (const UFGBuildGunStateBuild* Build = Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()))
			{
				if (RMRoadTemplate::IsRoadModeHologram(Build->GetHologram()))
				{
					return true;
				}
			}
		}
		return false;
	}
}

ARMBuildableBlueprintRoad::ARMBuildableBlueprintRoad(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Only build gun aims (dismantle, Road Mode) find it: players, vehicles and physics pass through.
	mMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	mMesh->SetCollisionResponseToAllChannels(ECR_Block);
	mMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	mMesh->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
	mMesh->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
	mMesh->SetCollisionResponseToChannel(ECC_Destructible, ECR_Ignore);
	mMesh->SetCanEverAffectNavigation(false);
	mMesh->SetCastShadow(false);
	mFarMesh->SetCastShadow(false);
	mAllowColoring = false;
}

void ARMBuildableBlueprintRoad::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableBlueprintRoad, mWidthCm);
}

void ARMBuildableBlueprintRoad::BeginPlay()
{
	Super::BeginPlay();
	RMBlueprintRoad::GAll.Add(this);
	SetShown(RMBlueprintRoad::GShown, RMBlueprintRoad::GSolid);
}

void ARMBuildableBlueprintRoad::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMBlueprintRoad::GAll.Remove(this);
	Super::EndPlay(EndPlayReason);
}

void ARMBuildableBlueprintRoad::SetShown(bool bShown, bool bSolid)
{
	mShown = bShown;
	SetActorHiddenInGame(!bShown);
	SetActorEnableCollision(bShown && bSolid);
}

UMaterialInterface* ARMBuildableBlueprintRoad::WireMaterial()
{
	static UMaterialInterface* Material = []
	{
		UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay"));
		if (M)
		{
			M->AddToRoot();
		}
		UE_CLOG(!M, LogRoadMaker, Warning, TEXT("Blueprint Road: wireframe material not found"));
		return M;
	}();
	return Material;
}

void ARMBuildableBlueprintRoad::BuildWire(const TArray<FRMFrame>& Frames, float Width, FRMMesh& Out)
{
	Out.Reset();
	if (Frames.Num() < 2)
	{
		return;
	}
	constexpr float Line = 30.f;   // line width
	constexpr float Lift = 10.f;   // over the floor
	constexpr float Across = 800.f; // a line across every 8 m
	const float Half = Width * 0.5f;
	auto At = [&](const FRMFrame& F, float Y) { return F.Pos + F.Right * Y + F.Up * Lift; };
	// Along: both edges and the centre.
	for (const float Y : { -Half + Line * 0.5f, 0.f, Half - Line * 0.5f })
	{
		for (int32 I = 0; I + 1 < Frames.Num(); ++I)
		{
			const FRMFrame& A = Frames[I];
			const FRMFrame& B = Frames[I + 1];
			Out.AddQuad(At(A, Y - Line * 0.5f), At(B, Y - Line * 0.5f), At(B, Y + Line * 0.5f), At(A, Y + Line * 0.5f),
				A.Up, FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1));
		}
	}
	// Across: every 8 m and at both ends.
	const float Length = Frames.Last().Dist;
	const int32 Count = FMath::Max(1, FMath::RoundToInt(Length / Across));
	for (int32 K = 0; K <= Count; ++K)
	{
		const float D = FMath::Clamp(Length * K / Count, Line * 0.5f, Length - Line * 0.5f);
		int32 I = 0;
		while (I + 2 < Frames.Num() && Frames[I + 1].Dist < D)
		{
			++I;
		}
		const FRMFrame& A = Frames[I];
		const FRMFrame& B = Frames[I + 1];
		const float T = B.Dist > A.Dist ? FMath::Clamp((D - A.Dist) / (B.Dist - A.Dist), 0.f, 1.f) : 0.f;
		FRMFrame F = A;
		F.Pos = FMath::Lerp(A.Pos, B.Pos, T);
		F.Fwd = FMath::Lerp(A.Fwd, B.Fwd, T).GetSafeNormal();
		F.Right = FMath::Lerp(A.Right, B.Right, T).GetSafeNormal();
		F.Up = FMath::Lerp(A.Up, B.Up, T).GetSafeNormal();
		const FVector Back = -F.Fwd * (Line * 0.5f);
		const FVector Ahead = F.Fwd * (Line * 0.5f);
		Out.AddQuad(At(F, -Half) + Back, At(F, -Half) + Ahead, At(F, Half) + Ahead, At(F, Half) + Back,
			F.Up, FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1));
	}
}

TArray<FLinearColor> ARMBuildableBlueprintRoad::WireColors(const FRMMesh& Wire)
{
	TArray<FLinearColor> Colors;
	Colors.Init(FLinearColor(0.15f, 0.65f, 1.f, 0.55f), Wire.Vertices.Num());
	return Colors;
}

void ARMBuildableBlueprintRoad::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, 100.f);
	FRMMesh Wire;
	BuildWire(Frames, mWidthCm, Wire);
	if (Wire.IsEmpty())
	{
		return;
	}
	const TArray<FLinearColor> Colors = WireColors(Wire);
	Wire.ApplyToSection(mMesh, 0, true, &Colors);
	ApplyMaterial(0, WireMaterial());
}

FText ARMBuildableBlueprintRoad::GetDismantleDisplayName_Implementation(AFGCharacterPlayer* byCharacter) const
{
	return FText::FromString(FString::Printf(TEXT("Blueprint Road (%d wide)"), GetWidthUnits()));
}

void ARMBuildableBlueprintRoad::GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const
{
	Super::GetChildDismantleActors_Implementation(out_ChildDismantleActors);
	// The whole Road Mode placement goes with it.
	if (const AFGBlueprintProxy* Proxy = GetBlueprintProxy())
	{
		for (AFGBuildable* B : Proxy->GetBuildables())
		{
			if (IsValid(B) && B != this && !B->IsActorBeingDestroyed())
			{
				out_ChildDismantleActors.AddUnique(B);
			}
		}
		// And its lightweight buildings (e.g. a large vent: no actor of their own, only an instance the blueprint keeps
		// by index): the blueprint hands out stand-in actors for them, as for the game's blueprint dismantle.
		TArray<AActor*> FromProxy;
		IFGDismantleInterface::Execute_GetChildDismantleActors(const_cast<AFGBlueprintProxy*>(Proxy), FromProxy);
		for (AActor* A : FromProxy)
		{
			if (IsValid(A) && A != this && !A->IsActorBeingDestroyed())
			{
				out_ChildDismantleActors.AddUnique(A);
			}
		}
	}
}

void ARMBuildableBlueprintRoad::Dismantle_Implementation()
{
	AFGBlueprintProxy* Proxy = GetBlueprintProxy();
	UWorld* World = GetWorld();
	if (Proxy && World && HasAuthority())
	{
		// A moment later (once the dismantle is done): whatever of the placement was left behind, still pointing at
		// its blueprint, goes too.
		const void* ProxyKey = Proxy;
		FBox Area(ForceInit);
		for (const FRMSplinePoint& P : mSplinePoints)
		{
			Area += GetActorTransform().TransformPosition(P.Location);
		}
		Area = Area.ExpandBy(mWidthCm + 3000.f);
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TWeakObjectPtr<AFGBlueprintProxy> WeakProxy(Proxy);
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld, WeakProxy, ProxyKey, Area]()
		{
			UWorld* W = WeakWorld.Get();
			if (!W)
			{
				return;
			}
			int32 Left = 0;
			// Lightweight buildings the blueprint still holds.
			if (AFGBlueprintProxy* P = WeakProxy.Get())
			{
				if (AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(W))
				{
					const TArray<FBuildableClassLightweightIndices> Held = P->GetLightweightClassAndIndices();
					for (const FBuildableClassLightweightIndices& Entry : Held)
					{
						for (const int32 Index : Entry.Indices)
						{
							UE_LOG(LogRoadMaker, Verbose, TEXT("Blueprint Road:   lightweight %s #%d was left behind: removed"), *GetNameSafe(Entry.BuildableClass.Get()), Index);
							P->UnregisterLightweightInstance(Entry.BuildableClass, Index);
							Lightweights->RemoveByInstanceIndex(Entry.BuildableClass, Index);
							++Left;
						}
					}
				}
			}
			for (TActorIterator<AFGBuildable> It(W); It; ++It)
			{
				AFGBuildable* B = *It;
				if (static_cast<const void*>(B->GetBlueprintProxy()) != ProxyKey || B->GetIsDismantled() || B->IsActorBeingDestroyed()
					|| !Area.IsInside(B->GetActorLocation()))
				{
					continue;
				}
				UE_LOG(LogRoadMaker, Verbose, TEXT("Blueprint Road:   %s was left behind: dismantled"), *B->GetName());
				IFGDismantleInterface::Execute_Dismantle(B);
				++Left;
			}
			UE_LOG(LogRoadMaker, Verbose, TEXT("Blueprint Road:   %d pieces left behind by the dismantle"), Left);
		}), 0.5f, false);
	}
	Super::Dismantle_Implementation();
}

bool ARMBuildableBlueprintRoad::FindEnd(const FVector& Aim, FVector& OutPos, FVector& OutOutward, float& OutWidth, bool* OutIsStart)
{
	float Best = TNumericLimits<float>::Max();
	for (const TWeakObjectPtr<ARMBuildableBlueprintRoad>& Weak : RMBlueprintRoad::GAll)
	{
		const ARMBuildableBlueprintRoad* Road = Weak.Get();
		if (!Road || Road->GetIsDismantled() || Road->IsActorBeingDestroyed())
		{
			continue;
		}
		TArray<FRMFrame> Frames;
		Road->GetWorldFrames(Frames, 200.f);
		if (Frames.Num() < 2)
		{
			continue;
		}
		const float Reach = Road->mWidthCm * 0.5f + 400.f;
		for (int32 End = 0; End < 2; ++End)
		{
			const FRMFrame& F = End == 0 ? Frames[0] : Frames.Last();
			const float D = FVector::Dist2D(F.Pos, Aim);
			if (D > Reach || FMath::Abs(F.Pos.Z - Aim.Z) > 400.f || D >= Best)
			{
				continue;
			}
			Best = D;
			OutPos = F.Pos;
			const FVector Along = FVector(F.Fwd.X, F.Fwd.Y, 0.f).GetSafeNormal();
			OutOutward = End == 0 ? -Along : Along;
			OutWidth = Road->mWidthCm;
			if (OutIsStart)
			{
				*OutIsStart = End == 0;
			}
		}
	}
	return Best < TNumericLimits<float>::Max();
}

void ARMBuildableBlueprintRoad::InstallVisibility()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		bool bSolid = false;
		const bool bWant = RMBlueprintRoad::WantShown(bSolid);
		if (bWant != RMBlueprintRoad::GShown || bSolid != RMBlueprintRoad::GSolid)
		{
			RMBlueprintRoad::GShown = bWant;
			RMBlueprintRoad::GSolid = bSolid;
			for (auto It = RMBlueprintRoad::GAll.CreateIterator(); It; ++It)
			{
				if (ARMBuildableBlueprintRoad* Road = It->Get())
				{
					Road->SetShown(bWant, bSolid);
				}
				else
				{
					It.RemoveCurrent();
				}
			}
		}
		return true;
	}), 0.2f);
}
