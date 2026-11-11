#include "Customization/RMRoadLookPaint.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
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
#include "Components/PrimitiveComponent.h"

#if !WITH_EDITOR
namespace
{
	TWeakObjectPtr<ARMBuildableProcedural> GRoadLookPreview;
	TWeakObjectPtr<UFGBuildGunStatePaint> GRoadLookReportedState;

	UFGBuildGunStatePaint* RoadLookPaintStateOf(APlayerController* PC)
	{
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
		return Gun && Gun->IsEquipped() ? Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()) : nullptr;
	}

	/** 0 asphalt, 1 concrete, 2 cyberpunk for our road materials; -1 for anything else. */
	int32 RoadLookOfMaterial(TSubclassOf<UFGFactoryCustomizationDescriptor_Material> Material)
	{
		if (!Material)
		{
			return -1;
		}
		const FString Name = Material->GetName();
		if (Name.Contains(TEXT("MaterialDesc_Road_Asphalt")))
		{
			return 0;
		}
		if (Name.Contains(TEXT("MaterialDesc_Road_Concrete")))
		{
			return 1;
		}
		if (Name.Contains(TEXT("MaterialDesc_Road_Cyber")))
		{
			return 2;
		}
		return -1;
	}

	ARMBuildableProcedural* RoadLookTrace(APlayerController* PC)
	{
		if (!PC || !PC->GetWorld())
		{
			return nullptr;
		}
		FVector Eye;
		FRotator View;
		PC->GetPlayerViewPoint(Eye, View);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMRoadLookTrace), true, PC->GetPawn());
		FHitResult Hit;
		if (!PC->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Params))
		{
			return nullptr;
		}
		ARMBuildableProcedural* Piece = Cast<ARMBuildableProcedural>(Hit.GetActor());
		if (!Piece || !(Piece->IsA<ARMBuildableRoad>() || Piece->IsA<ARMBuildableIntersection>()))
		{
			return nullptr;
		}
		return Piece->IsHidden() || Piece->GetIsDismantled() || Piece->IsActorBeingDestroyed() ? nullptr : Piece;
	}

	/** The paint gun outlines the aimed building white (can apply) or red; its Blueprint does that in OnHitStatusChanged. */
	void RoadLookReportHit(UFGBuildGunStatePaint* State, const ARMBuildableProcedural* Piece, bool bValidMaterial)
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
		Params.HitBuildingDesc = Piece ? TSubclassOf<UFGBuildingDescriptor>(Piece->GetBuiltWithDescriptor<UFGBuildingDescriptor>()) : nullptr;
		Params.bValidSwatch = State->IsSwatchHitValid();
		Params.bValidPattern = State->IsPatternHitValid();
		Params.bValidMaterial = bValidMaterial;
		if (Fn->ParmsSize == sizeof(FParams) || Fn->ParmsSize >= 11)
		{
			State->ProcessEvent(Fn, &Params);
		}
	}

	void RoadLookClearPreview()
	{
		if (ARMBuildableProcedural* Old = GRoadLookPreview.Get())
		{
			Old->SetPreviewLook(ARMBuildableProcedural::NoPreviewLook);
		}
		GRoadLookPreview.Reset();
	}

	void RoadLookClearPaintDisqualifiers(UFGBuildGunStatePaint* State)
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

	/** Local player: preview the selected road material on the aimed road or intersection. */
	bool RoadLookTickPreview(float)
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
		UFGBuildGunStatePaint* State = RoadLookPaintStateOf(PC);
		const int32 Look = State ? RoadLookOfMaterial(State->GetActiveMaterialDesc()) : -1;
		ARMBuildableProcedural* Piece = Look >= 0 ? RoadLookTrace(PC) : nullptr;
		if (Piece != GRoadLookPreview.Get())
		{
			RoadLookClearPreview();
		}
		if (Piece)
		{
			Piece->SetPreviewLook(static_cast<uint8>(Look));
			GRoadLookPreview = Piece;
			RoadLookClearPaintDisqualifiers(State);
			// Red on a piece that already has this look (nothing to apply), white otherwise.
			const bool bCanApply = Look != Piece->GetLook();
			RoadLookReportHit(State, Piece, bCanApply);
			const uint8 Stencil = bCanApply ? 2 : 7;
			TInlineComponentArray<UPrimitiveComponent*> Comps(Piece);
			for (UPrimitiveComponent* C : Comps)
			{
				if (C->bRenderCustomDepth && C->CustomDepthStencilValue != Stencil)
				{
					C->SetCustomDepthStencilValue(Stencil);
				}
			}
			GRoadLookReportedState = State;
		}
		else if (UFGBuildGunStatePaint* Reported = GRoadLookReportedState.Get())
		{
			RoadLookReportHit(Reported, nullptr, Reported->IsMaterialHitValid());
			GRoadLookReportedState.Reset();
		}
		return true;
	}
}

void RMRoadLookPaint::InstallHooks()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&RoadLookTickPreview), 0.05f);

	// Fire with a road material selected while aiming at a road or intersection: switch its look in place.
	SUBSCRIBE_METHOD(AFGBuildGun::OnPrimaryFirePressed, [](auto& Scope, AFGBuildGun* Gun)
	{
		UFGBuildGunStatePaint* State = Gun ? Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()) : nullptr;
		const int32 Look = State ? RoadLookOfMaterial(State->GetActiveMaterialDesc()) : -1;
		if (Look < 0)
		{
			return;
		}
		APawn* Pawn = Cast<APawn>(Gun->GetOwner());
		APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		ARMBuildableProcedural* Piece = RoadLookTrace(PC);
		if (!Piece)
		{
			return; // not a road: the game handles it
		}
		Scope.Cancel();
		RoadLookClearPreview();
		if (Piece->HasAuthority())
		{
			Piece->SetLook(static_cast<uint8>(Look));
		}
		else if (URMRemoteCallObject* RCO = URMRemoteCallObject::Get(PC))
		{
			RCO->ServerSetRoadLook(Piece, static_cast<uint8>(Look));
		}
	});
}

#else

void RMRoadLookPaint::InstallHooks() {}

#endif
