#include "Buildables/RMBuildableSignWall.h"
#include "Buildables/RMSignWallSign.h"
#include "RoadMakerModule.h"
#include "RMGeometry.h"
#include "FGCharacterPlayer.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

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
	Sign->FinishSpawning(At);
	mSign = Sign;
	UE_LOG(LogRoadMaker, Display, TEXT("SignWall: %s made its sign %s"), *GetName(), *Sign->GetName());
	OnSignDesignChanged();
}

float ARMBuildableSignWall::GetRepeatLength() const
{
	// The design keeps its proportions: as tall as the wall, as wide as that makes it.
	const FVector2D Size = mSign ? mSign->GetDesignSize() : FVector2D(800.f, 400.f);
	return GetWallHeight() * Size.X / FMath::Max(Size.Y, 1.f);
}

UMaterialInstanceDynamic* ARMBuildableSignWall::GetFrontMaterial()
{
	if (!mFrontMID && mFrontMaterial)
	{
		mFrontMID = UMaterialInstanceDynamic::Create(mFrontMaterial, this);
	}
	return mFrontMID;
}

void ARMBuildableSignWall::OnSignDesignChanged()
{
	if (mSign && !mSign->mWall && HasAuthority())
	{
		mSign->mWall = this;
	}
	UMaterialInstanceDynamic* Front = GetFrontMaterial();
	if (!Front || !mSign)
	{
		return;
	}
	// The design's render target, and the settings of its material that the front material also has (same names).
	if (UMaterialInstanceDynamic* Design = mSign->GetSignMaterial())
	{
		for (const FScalarParameterValue& V : Design->ScalarParameterValues)
		{
			Front->SetScalarParameterValue(V.ParameterInfo.Name, V.ParameterValue);
		}
		for (const FVectorParameterValue& V : Design->VectorParameterValues)
		{
			Front->SetVectorParameterValue(V.ParameterInfo.Name, V.ParameterValue);
		}
	}
	if (UTextureRenderTarget2D* Texture = mSign->GetSignTexture())
	{
		Front->SetTextureParameterValue(TEXT("SignTexture"), Texture);
	}
}

void ARMBuildableSignWall::RebuildMesh()
{
	Super::RebuildMesh();
	// The sign face on the road side, near and first distance level (further away: the plain slab).
	UMaterialInstanceDynamic* Front = GetFrontMaterial();
	const float Repeat = GetRepeatLength();
	for (int32 Level = 0; Level < 2; ++Level)
	{
		TArray<FRMFrame> Frames;
		GetLocalFrames(Frames, Level == 0 ? mMeshStep : 400.f);
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
		else
		{
			ApplyFarSection(Face, 2, Front);
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
