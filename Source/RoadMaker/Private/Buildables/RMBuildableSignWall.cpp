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
#include "FGSignTypes.h"
#include "UObject/UnrealType.h"

namespace
{
	/**
	 * A sign's design (FPrefabSignData) read or written through the engine's reflection: the game does not let mods
	 * call GetSignPrefabData / SetPrefabSignData directly (not exported), but both are UFUNCTIONs.
	 */
	bool RMSignData(AFGBuildableWidgetSign* Sign, FPrefabSignData& Data, bool bSet)
	{
		UFunction* Fn = Sign ? Sign->FindFunction(bSet ? FName(TEXT("SetPrefabSignData")) : FName(TEXT("GetSignPrefabData"))) : nullptr;
		if (!Fn)
		{
			return false;
		}
		uint8* Parms = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(Fn->ParmsSize, 1)));
		FMemory::Memzero(Parms, Fn->ParmsSize);
		FStructProperty* DataProp = nullptr;
		for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Parms);
			if (FStructProperty* SP = CastField<FStructProperty>(*It))
			{
				if (SP->Struct == FPrefabSignData::StaticStruct())
				{
					DataProp = SP;
				}
			}
			else if (FBoolProperty* BP = CastField<FBoolProperty>(*It))
			{
				BP->SetPropertyValue_InContainer(Parms, false); // bFilterText: the design as it was
			}
		}
		if (DataProp)
		{
			if (bSet)
			{
				DataProp->CopyCompleteValue(DataProp->ContainerPtrToValuePtr<void>(Parms), &Data);
			}
			Sign->ProcessEvent(Fn, Parms);
			if (!bSet)
			{
				DataProp->CopyCompleteValue(&Data, DataProp->ContainerPtrToValuePtr<void>(Parms));
			}
		}
		for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Parms);
		}
		return DataProp != nullptr;
	}
}

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
	// Never another wall's sign (a wall placed from a blueprint may still link the designer wall's).
	if (mSign && mSign->mWall && mSign->mWall != this)
	{
		mSign = nullptr;
	}
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
	const FTransform At(Front.Rotation(), Mid.Pos + FVector::UpVector * GetHeight() * 0.5f);
	UClass* SignClass = mSignClass ? mSignClass.Get() : ARMSignWallSign::StaticClass();
	ARMSignWallSign* Sign = GetWorld()->SpawnActorDeferred<ARMSignWallSign>(SignClass, At, nullptr, nullptr,
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
	// Placed from a blueprint: the design saved on the wall.
	if (mBlueprintHasDesign)
	{
		FPrefabSignData Data;
		Data.PrefabLayout = TSoftClassPtr<UFGSignPrefabWidget>(FSoftObjectPath(mBlueprintLayout));
		Data.TextElementData = mBlueprintTexts;
		Data.IconElementData = mBlueprintIcons;
		Data.ForegroundColor = mBlueprintForeground;
		Data.BackgroundColor = mBlueprintBackground;
		Data.AuxiliaryColor = mBlueprintAuxiliary;
		Data.Emissive = mBlueprintEmissive;
		Data.Glossiness = mBlueprintGlossiness;
		Data.SignTypeDesc = mBlueprintSignType.IsEmpty() ? nullptr : TSubclassOf<UFGSignTypeDescriptor>(LoadClass<UFGSignTypeDescriptor>(nullptr, *mBlueprintSignType));
		const bool bOk = RMSignData(Sign, Data, true);
		ClearBlueprintDesign();
		UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s took its design from the blueprint (%s)"), *GetName(), bOk ? TEXT("ok") : TEXT("failed"));
	}
	// A piece of a cut or a join: the design of the wall it came from.
	else if (mPendingDesign)
	{
		const bool bOk = IFGFactoryClipboardInterface::Execute_PasteSettings(Sign, mPendingDesign, nullptr);
		mPendingDesign = nullptr;
		UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s took the design of the wall it was cut / joined from (%s)"), *GetName(), bOk ? TEXT("ok") : TEXT("refused"));
	}
	// Middle click on a sign wall before building this one: its design.
	else if (UFGFactoryClipboardSettings* Design = RMRoadSample::TakeSignDesign())
	{
		const bool bOk = IFGFactoryClipboardInterface::Execute_PasteSettings(Sign, Design, nullptr);
		UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s took the sampled design (%s)"), *GetName(), bOk ? TEXT("ok") : TEXT("refused"));
	}
	UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s made its sign %s"), *GetName(), *Sign->GetName());
	OnSignDesignChanged();
}

void ARMBuildableSignWall::PreSerializedToBlueprint()
{
	Super::PreSerializedToBlueprint();
	FPrefabSignData Data;
	if (mSign && RMSignData(mSign, Data, false))
	{
		mBlueprintHasDesign = true;
		mBlueprintLayout = Data.PrefabLayout.ToSoftObjectPath().ToString();
		mBlueprintTexts = Data.TextElementData;
		mBlueprintIcons = Data.IconElementData;
		mBlueprintForeground = Data.ForegroundColor;
		mBlueprintBackground = Data.BackgroundColor;
		mBlueprintAuxiliary = Data.AuxiliaryColor;
		mBlueprintEmissive = Data.Emissive;
		mBlueprintGlossiness = Data.Glossiness;
		mBlueprintSignType = Data.SignTypeDesc ? Data.SignTypeDesc->GetPathName() : FString();
	}
}

void ARMBuildableSignWall::PostSerializedToBlueprint()
{
	Super::PostSerializedToBlueprint();
	ClearBlueprintDesign(); // only the blueprint keeps it (the game may not call this: EnsureSign clears it on use too)
}

void ARMBuildableSignWall::PostSerializedFromBlueprint(bool isBlueprintWorld)
{
	Super::PostSerializedFromBlueprint(isBlueprintWorld);
	// Never another wall's sign (blueprints saved before 1.4.0 may still link one).
	if (mSign && mSign->mWall && mSign->mWall != this)
	{
		mSign = nullptr;
	}
}

void ARMBuildableSignWall::ClearBlueprintDesign()
{
	mBlueprintHasDesign = false;
	mBlueprintLayout.Reset();
	mBlueprintTexts.Reset();
	mBlueprintIcons.Reset();
	mBlueprintSignType.Reset();
}

void ARMBuildableSignWall::CopyCutSettingsTo(ARMBuildableSpline* New) const
{
	Super::CopyCutSettingsTo(New);
	ARMBuildableSignWall* Piece = Cast<ARMBuildableSignWall>(New);
	if (!Piece)
	{
		return;
	}
	if (mSign && (!mSign->mWall || mSign->mWall == this))
	{
		// The game's own copy (the same as copy settings / middle click), pasted on the piece's new sign.
		Piece->mPendingDesign = IFGFactoryClipboardInterface::Execute_CopySettings(mSign);
	}
	else if (mBlueprintHasDesign)
	{
		// A wall of a blueprint (Road Mode stretches it over the curve): it has no sign of its own, only the
		// design saved with the blueprint. The new wall gives it to its sign like a wall placed from the blueprint.
		Piece->mBlueprintHasDesign = true;
		Piece->mBlueprintLayout = mBlueprintLayout;
		Piece->mBlueprintTexts = mBlueprintTexts;
		Piece->mBlueprintIcons = mBlueprintIcons;
		Piece->mBlueprintForeground = mBlueprintForeground;
		Piece->mBlueprintBackground = mBlueprintBackground;
		Piece->mBlueprintAuxiliary = mBlueprintAuxiliary;
		Piece->mBlueprintEmissive = mBlueprintEmissive;
		Piece->mBlueprintGlossiness = mBlueprintGlossiness;
		Piece->mBlueprintSignType = mBlueprintSignType;
	}
	UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s passes its design to %s (%s)"), *GetName(), *Piece->GetName(),
		Piece->mPendingDesign ? TEXT("from its sign") : Piece->mBlueprintHasDesign ? TEXT("from the blueprint") : TEXT("none"));
}

float ARMBuildableSignWall::GetRepeatLength() const
{
	// The design keeps its proportions: as tall as the wall, as wide as that makes it.
	// 8 m on both walls: the billboard (8 x 4 m) on 4 m, the 4 m label sign (4 x 0.5 m) on 1 m. Before the hidden sign
	// is there, 8 m too; when it reports another size the face is rebuilt (OnSignDesignChanged).
	if (!mSign)
	{
		return 800.f;
	}
	const FVector2D Size = mSign->GetDesignSize();
	return GetHeight() * Size.X / FMath::Max(Size.Y, 1.f);
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
	// The face was built for another repeat length (before the sign told its size, 2026-11-21: a 1 m wall showed the
	// label design every 2 m): build it again.
	if (mBuiltRepeat > 0.f && !FMath::IsNearlyEqual(mBuiltRepeat, GetRepeatLength(), 1.f))
	{
		RebuildMesh();
		return;
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
	mBuiltRepeat = Repeat;
	for (int32 Level = 0; Level < 3; ++Level)
	{
		TArray<FRMFrame> Frames;
		GetLocalFrames(Frames, Level == 0 ? mMeshStep : Level == 1 ? 400.f : 800.f);
		if (Level == 0)
		{
			RMGeometry::SimplifyFrames(Frames, SimplifyTurnDeg, 400.f); // as the wall body (repeats get their own cross-sections)
		}
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
		RMGeometry::BuildSignWallFace(Frames, Face, GetHeight(), 50.f, Repeat);
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
	UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: copy from %s: settings %s, clipboard class %s"), *GetName(), *GetNameSafe(Settings),
		*GetNameSafe(GetClipboardMappingClass_Implementation().Get()));
	return Settings;
}

bool ARMBuildableSignWall::PasteSettings_Implementation(UFGFactoryClipboardSettings* settings, AFGPlayerController* player)
{
	const bool bOk = mSign && IFGFactoryClipboardInterface::Execute_PasteSettings(mSign, settings, player);
	UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: paste on %s: settings %s -> %s"), *GetName(), *GetNameSafe(settings), bOk ? TEXT("ok") : TEXT("refused"));
	OnSignDesignChanged();
	return bOk;
}
