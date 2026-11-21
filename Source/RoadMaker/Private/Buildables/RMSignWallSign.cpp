#include "Buildables/RMSignWallSign.h"
#include "Buildables/RMBuildableSignWall.h"
#include "RoadMakerModule.h"
#include "TimerManager.h"
#include "EngineUtils.h"
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

UClass* ARMSignWallSignLabel::GetGameSignClass() const
{
	static TWeakObjectPtr<UClass> Cached;
	static bool bSearched = false;
	if (!Cached.IsValid() && !bSearched)
	{
		bSearched = true;
		// The label signs (2, 3 and 4 m): the 4 m one is 8 times as wide as tall.
		const FStructProperty* DimProp = FindFProperty<FStructProperty>(AFGBuildableWidgetSign::StaticClass(), TEXT("mWorldDimensions"));
		for (const TCHAR* Name : { TEXT("SmallVeryWide"), TEXT("SmallWide"), TEXT("Small"), TEXT("Medium") })
		{
			const FString Path = FString::Printf(TEXT("/Game/FactoryGame/Buildable/Factory/SignDigital/Build_StandaloneWidgetSign_%s.Build_StandaloneWidgetSign_%s_C"), Name, Name);
			UClass* Class = LoadClass<AFGBuildableWidgetSign>(nullptr, *Path);
			const FVector2D* Dim = Class && DimProp ? DimProp->ContainerPtrToValuePtr<FVector2D>(Class->GetDefaultObject()) : nullptr;
			UE_LOG(LogRoadMaker, Display, TEXT("SignWall 1 m: game sign %s: %s, %s"), Name, Class ? TEXT("found") : TEXT("missing"),
				Dim ? *Dim->ToString() : TEXT("no size"));
			if (Dim && Dim->Y > 1.f && FMath::IsNearlyEqual(Dim->X / Dim->Y, 8.f, 0.5f))
			{
				Cached = Class;
				UE_LOG(LogRoadMaker, Display, TEXT("SignWall 1 m: uses %s (%.0f x %.0f cm)"), *Class->GetName(), Dim->X, Dim->Y);
				break;
			}
		}
		if (!Cached.IsValid())
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("SignWall 1 m: no 8:1 label sign found, using the billboard"));
		}
	}
	return Cached.IsValid() ? Cached.Get() : Super::GetGameSignClass();
}

UClass* ARMSignWallSign::GetGameSignClass() const
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = LoadClass<AFGBuildableWidgetSign>(nullptr, GGameSignPath);
		UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: game sign class %s"), *GetNameSafe(Cached.Get()));
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
		UE_LOG(LogRoadMaker, Warning, TEXT("SignWall: game sign not found, the sign wall shows no design"));
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
		mSignProxyPlane->SetMobility(EComponentMobility::Movable);
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
	// The small billboard's own resolution (its draw size, copied above): one 8 x 4 m design looks the same on both.
	UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s took %d settings from %s (plane mesh %s)"), *GetName(), Copied, *Game->GetName(),
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
	// A sign no wall owns any more (left behind by 1.4.0 test builds, when a wall made a second one) goes away,
	// once every wall of a loading save has had time to come back and take its sign.
	if (HasAuthority())
	{
		FTimerHandle Check;
		GetWorldTimerManager().SetTimer(Check, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (mWall && mWall->GetSign() == this)
			{
				return;
			}
			// Kept when any wall still points at it (and that wall becomes its owner).
			for (TActorIterator<ARMBuildableSignWall> It(GetWorld()); It; ++It)
			{
				if (It->GetSign() == this)
				{
					mWall = *It;
					return;
				}
			}
			UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: %s belongs to no wall, removed"), *GetName());
			Destroy();
		}), 10.f, false);
	}
}

float ARMSignWallSign::GetSignificanceRange_Implementation() const
{
	return mGainSignificanceDistance + mExtraRange;
}

UMaterialInterface* ARMSignWallSign::GetSignMaterial()
{
	UMaterialInterface* Current = mSignProxyPlane ? mSignProxyPlane->GetMaterial(0) : nullptr;
	UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Current);
	if (!Dynamic)
	{
		return mSignMaterial; // not set up yet (the plane's own default material)
	}
	if (Dynamic != mSignMaterial)
	{
		// A new design material (after a paste): the real sign size on it too, see SetupMaterialInstanceForProxyPlane.
		const FVector2D Size = GetDesignSize();
		Dynamic->SetScalarParameterValue(TEXT("SignWidth"), Size.X);
		Dynamic->SetScalarParameterValue(TEXT("SignHeight"), Size.Y);
		mSignMaterial = Dynamic;
	}
	return Dynamic;
}

FVector2D ARMSignWallSign::GetDesignSize() const
{
	return mWorldDimensions.X > 1.f && mWorldDimensions.Y > 1.f ? mWorldDimensions : FVector2D(800.f, 400.f);
}

TSubclassOf<UObject> ARMSignWallSign::GetClipboardMappingClass_Implementation()
{
	if (UClass* Game = GetGameSignClass())
	{
		return IFGFactoryClipboardInterface::Execute_GetClipboardMappingClass(Game->GetDefaultObject());
	}
	return TSubclassOf<UObject>(GetClass());
}

void ARMSignWallSign::SetupMaterialInstanceForProxyPlane(UMaterialInstanceDynamic* Instance, UTextureRenderTarget2D* RenderTarget)
{
	Super::SetupMaterialInstanceForProxyPlane(Instance, RenderTarget);
	if (Instance)
	{
		// The game sets these from the plane's size, which is tiny here (hidden in the wall): the real sign's size, so
		// background patterns and animations are laid out like on the billboard.
		const FVector2D Size = GetDesignSize();
		Instance->SetScalarParameterValue(TEXT("SignWidth"), Size.X);
		Instance->SetScalarParameterValue(TEXT("SignHeight"), Size.Y);
	}
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
		UE_LOG(LogRoadMaker, Verbose, TEXT("SignWall: design of %s: material %s, render target %s (%dx%d); %s"), *GetName(),
			*GetNameSafe(Instance->Parent), *GetNameSafe(RenderTarget), RenderTarget ? RenderTarget->SizeX : 0, RenderTarget ? RenderTarget->SizeY : 0,
			*FString::Join(Parts, TEXT(", ")));
	}
	if (mWall)
	{
		mWall->OnSignDesignChanged();
	}
}

void ARMSignWallSign::MarkSeen(float RenderTime)
{
	if (mSignProxyPlane && RenderTime > mSignProxyPlane->GetLastRenderTime())
	{
		mSignProxyPlane->SetLastRenderTime(RenderTime);
	}
}
