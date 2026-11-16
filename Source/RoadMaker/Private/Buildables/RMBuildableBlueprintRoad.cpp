#include "Buildables/RMBuildableBlueprintRoad.h"
#include "RoadMakerModule.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "FGBlueprintProxy.h"
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

	/** Whether the local player holds the dismantle tool or draws in Road Mode. */
	bool WantShown()
	{
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
	SetShown(RMBlueprintRoad::GShown);
}

void ARMBuildableBlueprintRoad::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMBlueprintRoad::GAll.Remove(this);
	Super::EndPlay(EndPlayReason);
}

void ARMBuildableBlueprintRoad::SetShown(bool bShown)
{
	mShown = bShown;
	SetActorHiddenInGame(!bShown);
	SetActorEnableCollision(bShown);
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
	}
}

void ARMBuildableBlueprintRoad::InstallVisibility()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		const bool bWant = RMBlueprintRoad::WantShown();
		if (bWant != RMBlueprintRoad::GShown)
		{
			RMBlueprintRoad::GShown = bWant;
			for (auto It = RMBlueprintRoad::GAll.CreateIterator(); It; ++It)
			{
				if (ARMBuildableBlueprintRoad* Road = It->Get())
				{
					Road->SetShown(bWant);
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
