#include "Buildables/RMSignWallSign.h"
#include "Buildables/RMBuildableSignWall.h"
#include "RoadMakerModule.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"

namespace
{
	const TCHAR* GGameSignPath = TEXT("/Game/FactoryGame/Buildable/Factory/SignDigital/Build_StandaloneWidgetSign_Large.Build_StandaloneWidgetSign_Large_C");
}

ARMSignWallSign::ARMSignWallSign()
{
}

void ARMSignWallSign::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMSignWallSign, mWall);
	DOREPLIFETIME(ARMSignWallSign, mExtraRange);
}

UClass* ARMSignWallSign::GetGameSignClass()
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = LoadClass<AFGBuildableWidgetSign>(nullptr, GGameSignPath);
		UE_LOG(LogRoadMaker, Display, TEXT("SignWall: game sign class %s"), *GetNameSafe(Cached.Get()));
	}
	return Cached.Get();
}

void ARMSignWallSign::CopyGameSignDefaults()
{
	if (mCopiedDefaults)
	{
		return;
	}
	mCopiedDefaults = true;
	UClass* Game = GetGameSignClass();
	if (!Game)
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("SignWall: %s not found, the sign wall shows no design"), GGameSignPath);
		return;
	}
	const UObject* CDO = Game->GetDefaultObject();
	// The sign's own settings: layouts (sign type), materials, draw size, world size, significance distance...
	// Not what is saved per sign (the design) and not components.
	int32 Copied = 0;
	for (UClass* FromClass : { AFGBuildableWidgetSign::StaticClass(), AFGBuildableSignBase::StaticClass() })
	{
		for (TFieldIterator<FProperty> It(FromClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* P = *It;
			if (!P->HasAnyPropertyFlags(CPF_Edit) || P->HasAnyPropertyFlags(CPF_SaveGame | CPF_Transient | CPF_InstancedReference
				| CPF_ContainsInstancedReference | CPF_Net))
			{
				continue;
			}
			if (const FObjectPropertyBase* O = CastField<FObjectPropertyBase>(P))
			{
				if (O->PropertyClass && O->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
				{
					continue;
				}
			}
			P->CopyCompleteValue_InContainer(this, CDO);
			++Copied;
		}
	}
	// E opens the game's sign editor.
	for (const TCHAR* Name : { TEXT("mInteractWidgetSoftClass"), TEXT("mIsUseable") })
	{
		if (FProperty* P = AFGBuildable::StaticClass()->FindPropertyByName(Name))
		{
			P->CopyCompleteValue_InContainer(this, CDO);
			++Copied;
		}
	}
	// The plane the game draws the design on: its mesh, tiny and inside the wall (only the wall shows the design), with
	// large bounds so it still counts as seen while the wall is (the game updates designs that are being looked at).
	const FObjectProperty* PlaneProp = FindFProperty<FObjectProperty>(AFGBuildableWidgetSign::StaticClass(), TEXT("mSignProxyPlane"));
	const UStaticMeshComponent* GamePlane = PlaneProp ? Cast<UStaticMeshComponent>(PlaneProp->GetObjectPropertyValue_InContainer(CDO)) : nullptr;
	if (mSignProxyPlane)
	{
		if (GamePlane)
		{
			mSignProxyPlane->SetStaticMesh(GamePlane->GetStaticMesh());
		}
		mSignProxyPlane->SetRelativeScale3D(FVector(0.001f));
		mSignProxyPlane->SetBoundsScale(5000.f);
		mSignProxyPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		mSignProxyPlane->SetCastShadow(false);
	}
	SetActorEnableCollision(false);
	UE_LOG(LogRoadMaker, Display, TEXT("SignWall: %s took %d settings from %s (plane mesh %s)"), *GetName(), Copied, *Game->GetName(),
		GamePlane ? *GetNameSafe(GamePlane->GetStaticMesh()) : TEXT("none"));
}

void ARMSignWallSign::OnConstruction(const FTransform& Transform)
{
	CopyGameSignDefaults();
	Super::OnConstruction(Transform);
}

void ARMSignWallSign::BeginPlay()
{
	CopyGameSignDefaults();
	Super::BeginPlay();
	SetActorEnableCollision(false);
}

float ARMSignWallSign::GetSignificanceRange_Implementation() const
{
	return mGainSignificanceDistance + mExtraRange;
}

FVector2D ARMSignWallSign::GetDesignSize() const
{
	return mWorldDimensions.X > 1.f && mWorldDimensions.Y > 1.f ? mWorldDimensions : FVector2D(800.f, 400.f);
}

void ARMSignWallSign::SetupMaterialInstanceForProxyPlane(UMaterialInstanceDynamic* Instance, UTextureRenderTarget2D* RenderTarget)
{
	Super::SetupMaterialInstanceForProxyPlane(Instance, RenderTarget);
	mSignMaterial = Instance;
	mSignTexture = RenderTarget;
	static int32 Logs = 0;
	if (Logs++ < 3 && Instance)
	{
		// The design material's settings, to match the wall's front to it.
		TArray<FString> Parts;
		for (const FTextureParameterValue& V : Instance->TextureParameterValues)
		{
			Parts.Add(FString::Printf(TEXT("tex %s=%s"), *V.ParameterInfo.Name.ToString(), *GetNameSafe(V.ParameterValue)));
		}
		for (const FScalarParameterValue& V : Instance->ScalarParameterValues)
		{
			Parts.Add(FString::Printf(TEXT("%s=%.3f"), *V.ParameterInfo.Name.ToString(), V.ParameterValue));
		}
		for (const FVectorParameterValue& V : Instance->VectorParameterValues)
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *V.ParameterInfo.Name.ToString(), *V.ParameterValue.ToString()));
		}
		UE_LOG(LogRoadMaker, Display, TEXT("SignWall: design of %s: material %s, render target %s (%dx%d); %s"), *GetName(),
			*GetNameSafe(Instance->Parent), *GetNameSafe(RenderTarget), RenderTarget ? RenderTarget->SizeX : 0, RenderTarget ? RenderTarget->SizeY : 0,
			*FString::Join(Parts, TEXT(", ")));
	}
	if (mWall)
	{
		mWall->OnSignDesignChanged();
	}
}
