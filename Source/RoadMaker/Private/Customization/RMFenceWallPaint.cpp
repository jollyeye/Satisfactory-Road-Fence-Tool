#include "Customization/RMFenceWallPaint.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableFence.h"
#include "Net/RMRemoteCallObject.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunPaint.h"
#include "FGCharacterPlayer.h"
#include "FGFactoryColoringTypes.h"
#include "Patching/NativeHookManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "UObject/UnrealType.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Holograms/RMFenceHologram.h"
#include "Components/PrimitiveComponent.h"

#if !WITH_EDITOR
namespace
{
	TWeakObjectPtr<ARMBuildableFence> GFenceWallPreview;

	UFGBuildGunStatePaint* FenceWallPaintStateOf(APlayerController* PC)
	{
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
		return Gun && Gun->IsEquipped() ? Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()) : nullptr;
	}

	/** 1 concrete, 2 FICSIT, 3 steel for the game's wall materials; 0 for anything else. */
	uint8 FenceWallLookOfMaterial(TSubclassOf<UFGFactoryCustomizationDescriptor_Material> Material)
	{
		if (!Material)
		{
			return 0;
		}
		const FString Name = Material->GetName();
		if (Name.Contains(TEXT("MaterialDesc_Wall_Concrete")))
		{
			return 1;
		}
		if (Name.Contains(TEXT("MaterialDesc_Wall_Orange")))
		{
			return 2;
		}
		if (Name.Contains(TEXT("MaterialDesc_Wall_Steel")))
		{
			return 3;
		}
		return 0;
	}

	ARMBuildableFence* FenceWallTrace(APlayerController* PC)
	{
		if (!PC || !PC->GetWorld())
		{
			return nullptr;
		}
		FVector Eye;
		FRotator View;
		PC->GetPlayerViewPoint(Eye, View);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMFenceWallTrace), true, PC->GetPawn());
		// Walls just swapped for another material are on their way out: look through them.
		for (int32 Try = 0; Try < 4; ++Try)
		{
			FHitResult Hit;
			if (!PC->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Params))
			{
				return nullptr;
			}
			ARMBuildableFence* Wall = Cast<ARMBuildableFence>(Hit.GetActor());
			if (Wall && (Wall->IsHidden() || Wall->GetIsDismantled() || Wall->IsActorBeingDestroyed()))
			{
				Params.AddIgnoredActor(Wall);
				continue;
			}
			return Wall && Wall->HasLooks() ? Wall : nullptr;
		}
		return nullptr;
	}

	/**
	 * The paint gun outlines the aimed building white if the selected customization can be applied,
	 * red if not; its Blueprint does that in OnHitStatusChanged. The game does not know fence walls
	 * take wall materials (we switch them ourselves), so we report a valid hit for them.
	 */
	void FenceWallReportHit(UFGBuildGunStatePaint* State, const ARMBuildableFence* Wall, bool bValidMaterial)
	{
		static UFunction* Fn = UFGBuildGunStatePaint::StaticClass()->FindFunctionByName(TEXT("OnHitStatusChanged"));
		if (!State || !Fn)
		{
			return;
		}
		struct FParams
		{
			TSubclassOf<UFGBuildingDescriptor> HitBuildingDesc;
			bool bValidSwatch;
			bool bValidPattern;
			bool bValidMaterial;
		} Params;
		Params.HitBuildingDesc = Wall ? TSubclassOf<UFGBuildingDescriptor>(Wall->GetBuiltWithDescriptor<UFGBuildingDescriptor>()) : nullptr;
		Params.bValidSwatch = State->IsSwatchHitValid();
		Params.bValidPattern = State->IsPatternHitValid();
		Params.bValidMaterial = bValidMaterial;
		if (Fn->ParmsSize == sizeof(FParams) || Fn->ParmsSize >= 11)
		{
			State->ProcessEvent(Fn, &Params);
		}
	}

	TWeakObjectPtr<UFGBuildGunStatePaint> GFenceWallReportedState;

	/** Stencil values the game uses to colour holograms / outlines (OK = white, ERROR = red). */
	struct FRMStencilAccess : public AFGHologram
	{
		static uint8 Get(EHologramMaterialState State)
		{
			const AFGHologram* Cdo = GetDefault<ARMFenceHologram>();
			return static_cast<const FRMStencilAccess*>(Cdo)->GetStencilForHologramMaterialState(State);
		}
	};

	/**
	 * The paint gun outlines the aimed building through custom depth with a stencil value; it uses
	 * the "cannot apply" value on fence walls (the game does not know they take wall materials).
	 * Put the "can apply" value on them instead.
	 */
	void FenceWallOutlineValid(ARMBuildableFence* Wall)
	{
		static const uint8 Ok = FRMStencilAccess::Get(EHologramMaterialState::HMS_OK);
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogRoadMaker, Display, TEXT("[Diag] stencil OK=%d WARNING=%d ERROR=%d"), Ok,
				FRMStencilAccess::Get(EHologramMaterialState::HMS_WARNING), FRMStencilAccess::Get(EHologramMaterialState::HMS_ERROR));
		}
		TInlineComponentArray<UPrimitiveComponent*> Comps(Wall);
		for (UPrimitiveComponent* C : Comps)
		{
			if (C->bRenderCustomDepth && C->CustomDepthStencilValue != Ok)
			{
				C->SetCustomDepthStencilValue(Ok);
			}
		}
	}

	void FenceWallClearPreview()
	{
		if (ARMBuildableFence* Old = GFenceWallPreview.Get())
		{
			Old->SetPreviewWallLook(0);
		}
		GFenceWallPreview.Reset();
	}

	void FenceWallClearPaintDisqualifiers(UFGBuildGunStatePaint* State)
	{
		static FArrayProperty* Prop = FindFProperty<FArrayProperty>(UFGBuildGunStatePaint::StaticClass(), TEXT("mConstructDisqualifiers"));
		if (State && Prop)
		{
			FScriptArrayHelper Array(Prop, Prop->ContainerPtrToValuePtr<void>(State));
			if (Array.Num() > 0)
			{
				Array.EmptyValues();
			}
		}
	}

	/** Local player: preview the selected wall material on the aimed fence wall. */
	bool FenceWallTickPreview(float)
	{
		if (!GEngine || IsRunningDedicatedServer())
		{
			return true;
		}
		APlayerController* PC = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.World())
			{
				PC = Context.World()->GetFirstPlayerController();
				break;
			}
		}
		UFGBuildGunStatePaint* State = FenceWallPaintStateOf(PC);
		const uint8 Look = State ? FenceWallLookOfMaterial(State->GetActiveMaterialDesc()) : 0;
		ARMBuildableFence* Wall = Look ? FenceWallTrace(PC) : nullptr;
		if (Wall != GFenceWallPreview.Get())
		{
			FenceWallClearPreview();
		}
		if (Wall)
		{
			Wall->SetPreviewWallLook(Look);
			GFenceWallPreview = Wall;
			FenceWallClearPaintDisqualifiers(State);
			FenceWallReportHit(State, Wall, true);
			FenceWallOutlineValid(Wall); // white outline, like a game wall
			GFenceWallReportedState = State;
		}
		else if (UFGBuildGunStatePaint* Reported = GFenceWallReportedState.Get())
		{
			// Back to what the game itself found for what is aimed at now.
			FenceWallReportHit(Reported, nullptr, Reported->IsMaterialHitValid());
			GFenceWallReportedState.Reset();
		}
		return true;
	}
}

void RMFenceWallPaint::InstallHooks()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FenceWallTickPreview), 0.05f);

	// Fire with a wall material selected while aiming at a fence wall: switch its look in place.
	SUBSCRIBE_METHOD(AFGBuildGun::OnPrimaryFirePressed, [](auto& Scope, AFGBuildGun* Gun)
	{
		UFGBuildGunStatePaint* State = Gun ? Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()) : nullptr;
		const uint8 Look = State ? FenceWallLookOfMaterial(State->GetActiveMaterialDesc()) : 0;
		if (!Look)
		{
			return;
		}
		APawn* Pawn = Cast<APawn>(Gun->GetOwner());
		APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		ARMBuildableFence* Wall = FenceWallTrace(PC);
		UE_LOG(LogRoadMaker, Display, TEXT("[Diag] fire with wall material %d, aimed fence wall %s"), Look, *GetNameSafe(Wall));
		if (!Wall)
		{
			return; // not a fence wall: the game handles it
		}
		Scope.Cancel();
		FenceWallClearPreview();
		if (Wall->HasAuthority())
		{
			Wall->SetWallLook(Look, Cast<AFGCharacterPlayer>(Pawn));
		}
		else if (URMRemoteCallObject* RCO = URMRemoteCallObject::Get(PC))
		{
			RCO->ServerSetFenceWallLook(Wall, Look);
		}
	});
}

#else

void RMFenceWallPaint::InstallHooks() {}

#endif
