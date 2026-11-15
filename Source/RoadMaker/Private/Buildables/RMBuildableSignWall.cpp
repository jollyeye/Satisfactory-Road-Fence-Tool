#include "Buildables/RMBuildableSignWall.h"
#include "Buildables/RMSignWallSign.h"
#include "Customization/RMRoadLookPaint.h"
#include "RoadMakerModule.h"
#include "RMGeometry.h"
#include "FGCharacterPlayer.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "ProceduralMeshComponent.h"

ARMBuildableSignWall::ARMBuildableSignWall(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Useable (E): IsUseable_Implementation below (mIsUseable itself is private in the game's class).
}

void ARMBuildableSignWall::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableSignWall, mSign);
}

void ARMBuildableSignWall::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		// A moment later: a wall loaded from a save gets its saved sign back first.
		GetWorldTimerManager().SetTimer(mEnsureSignTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableSignWall::EnsureSign), 0.5f, false);
	}
	if (!IsRunningDedicatedServer())
	{
		GetWorldTimerManager().SetTimer(mDesignTimer, FTimerDelegate::CreateUObject(this, &ARMBuildableSignWall::CheckDesignMaterial), 0.2f, true);
	}
	OnSignDesignChanged();
}

void ARMBuildableSignWall::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Gone for good (dismantled, replaced): its sign goes too. Not on unloading a world.
	if (EndPlayReason == EEndPlayReason::Destroyed && HasAuthority() && mSign && !mSign->IsActorBeingDestroyed())
	{
		mSign->Destroy();
	}
	mSign = nullptr;
	Super::EndPlay(EndPlayReason);
}

void ARMBuildableSignWall::EnsureSign()
{
	if (!HasAuthority() || mSign || GetIsDismantled() || IsActorBeingDestroyed())
	{
		if (mSign)
		{
			mSign->mWall = this;
		}
		return;
	}
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 100.f);
	if (Frames.Num() < 2)
	{
		return;
	}
	// Inside the wall, at the middle of its length and height, facing the road side.
	const FRMFrame& Mid = Frames[Frames.Num() / 2];
	const FVector Front = FVector(Mid.Right.X, Mid.Right.Y, 0.f).GetSafeNormal() * GetInwardSign();
	const FTransform At(Front.Rotation(), Mid.Pos + FVector::UpVector * GetWallHeight() * 0.5f);
	ARMSignWallSign* Sign = GetWorld()->SpawnActorDeferred<ARMSignWallSign>(ARMSignWallSign::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Sign)
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("SignWall: %s could not make its sign"), *GetName());
		return;
	}
	Sign->mWall = this;
	Sign->mExtraRange = Frames.Last().Dist * 0.5f;
	mSign = Sign; // before it finishes: it sets up its design right away and tells the wall
	Sign->FinishSpawning(At);
	// A piece of a cut or a join: the design of the wall it came from.
	if (mPendingDesign)
	{
		const bool bOk = IFGFactoryClipboardInterface::Execute_PasteSettings(Sign, mPendingDesign, nullptr);
		mPendingDesign = nullptr;
		UE_LOG(LogRoadMaker, Display, TEXT("SignWall: %s took the design of the wall it was cut / joined from (%s)"), *GetName(), bOk ? TEXT("ok") : TEXT("refused"));
	}
	// Middle click on a sign wall before building this one: its design.
	else if (UFGFactoryClipboardSettings* Design = RMRoadSample::TakeSignDesign())
	{
		const bool bOk = IFGFactoryClipboardInterface::Execute_PasteSettings(Sign, Design, nullptr);
		UE_LOG(LogRoadMaker, Display, TEXT("SignWall: %s took the sampled design (%s)"), *GetName(), bOk ? TEXT("ok") : TEXT("refused"));
	}
	UE_LOG(LogRoadMaker, Display, TEXT("SignWall: %s made its sign %s"), *GetName(), *Sign->GetName());
	OnSignDesignChanged();
}

void ARMBuildableSignWall::CopyCutSettingsTo(ARMBuildableSpline* New) const
{
	Super::CopyCutSettingsTo(New);
	ARMBuildableSignWall* Piece = Cast<ARMBuildableSignWall>(New);
	if (Piece && mSign)
	{
		// The game's own copy (the same as copy settings / middle click), pasted on the piece's new sign.
		Piece->mPendingDesign = IFGFactoryClipboardInterface::Execute_CopySettings(mSign);
	}
}

float ARMBuildableSignWall::GetRepeatLength() const
{
	// The design keeps its proportions: as tall as the wall, as wide as that makes it.
	const FVector2D Size = mSign ? mSign->GetDesignSize() : FVector2D(800.f, 400.f);
	return GetWallHeight() * Size.X / FMath::Max(Size.Y, 1.f);
}

void ARMBuildableSignWall::CheckDesignMaterial()
{
	if (mSign)
	{
		float Seen = 0.f;
		ForEachMeshComponent([&Seen](UProceduralMeshComponent* Mesh)
		{
			Seen = FMath::Max(Seen, Mesh->GetLastRenderTime());
		});
		mSign->MarkSeen(Seen);
	}
	if (mSign && GetFrontMaterial() != mShownFront.Get())
	{
		OnSignDesignChanged();
	}
}

UMaterialInterface* ARMBuildableSignWall::GetFrontMaterial() const
{
	// The sign's own design material (the game's sign material with its background, glow and gloss), once it has one;
	// until then the plain front.
	if (mSign)
	{
		if (UMaterialInterface* Design = mSign->GetSignMaterial())
		{
			return Design;
		}
	}
	return mFrontMaterial;
}

void ARMBuildableSignWall::OnSignDesignChanged()
{
	if (mSign && !mSign->mWall && HasAuthority())
	{
		mSign->mWall = this;
	}
	UMaterialInterface* Front = GetFrontMaterial();
	if (!Front)
	{
		return;
	}
	mShownFront = Front;
	// Section 2 (the face) of every mesh piece, near and far.
	ForEachMeshComponent([Front](UProceduralMeshComponent* Mesh)
	{
		if (Mesh->GetNumSections() > 2)
		{
			Mesh->SetMaterial(2, Front);
		}
	});
}

void ARMBuildableSignWall::RebuildMesh()
{
	Super::RebuildMesh();
	// The sign face on the road side, at every distance level (like a billboard, seen from across the map).
	UMaterialInterface* Front = GetFrontMaterial();
	const float Repeat = GetRepeatLength();
	for (int32 Level = 0; Level < 3; ++Level)
	{
		TArray<FRMFrame> Frames;
		GetLocalFrames(Frames, Level == 0 ? mMeshStep : Level == 1 ? 400.f : 800.f);
		AddChunkBoundaries(Frames);
		if (Frames.Num() < 2)
		{
			continue;
		}
		if (GetInwardSign() < 0.f)
		{
			RMGeometry::MirrorFrames(Frames);
		}
		FRMMesh Face;
		RMGeometry::BuildSignWallFace(Frames, Face, GetWallHeight(), 50.f, Repeat);
		if (Level == 0)
		{
			ApplyNearSection(Face, 2, false, Front);
		}
		else if (Level == 1)
		{
			ApplyFarSection(Face, 2, Front);
		}
		else
		{
			ApplyFar2Section(Face, 2, Front);
		}
	}
	OnSignDesignChanged();
}

bool ARMBuildableSignWall::IsUseable_Implementation() const
{
	return mSign != nullptr;
}

void ARMBuildableSignWall::OnUse_Implementation(AFGCharacterPlayer* byCharacter, const FUseState& state)
{
	if (mSign)
	{
		IFGUseableInterface::Execute_OnUse(mSign, byCharacter, state);
	}
}

FText ARMBuildableSignWall::GetLookAtDecription_Implementation(AFGCharacterPlayer* byCharacter, const FUseState& state) const
{
	return mSign ? IFGUseableInterface::Execute_GetLookAtDecription(mSign, byCharacter, state) : Super::GetLookAtDecription_Implementation(byCharacter, state);
}

void ARMBuildableSignWall::GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const
{
	Super::GetChildDismantleActors_Implementation(out_ChildDismantleActors);
	if (mSign)
	{
		out_ChildDismantleActors.AddUnique(mSign);
	}
}

bool ARMBuildableSignWall::CanUseFactoryClipboard_Implementation()
{
	return mSign != nullptr;
}

TSubclassOf<UObject> ARMBuildableSignWall::GetClipboardMappingClass_Implementation()
{
	if (mSign)
	{
		return IFGFactoryClipboardInterface::Execute_GetClipboardMappingClass(mSign);
	}
	return TSubclassOf<UObject>(GetClass());
}

UFGFactoryClipboardSettings* ARMBuildableSignWall::CopySettings_Implementation()
{
	UFGFactoryClipboardSettings* Settings = mSign ? IFGFactoryClipboardInterface::Execute_CopySettings(mSign) : nullptr;
	UE_LOG(LogRoadMaker, Display, TEXT("SignWall: copy from %s: settings %s, clipboard class %s"), *GetName(), *GetNameSafe(Settings),
		*GetNameSafe(GetClipboardMappingClass_Implementation().Get()));
	return Settings;
}

bool ARMBuildableSignWall::PasteSettings_Implementation(UFGFactoryClipboardSettings* settings, AFGPlayerController* player)
{
	const bool bOk = mSign && IFGFactoryClipboardInterface::Execute_PasteSettings(mSign, settings, player);
	UE_LOG(LogRoadMaker, Display, TEXT("SignWall: paste on %s: settings %s -> %s"), *GetName(), *GetNameSafe(settings), bOk ? TEXT("ok") : TEXT("refused"));
	OnSignDesignChanged();
	return bOk;
}
