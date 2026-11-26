#include "Buildables/RMBuildableFill.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMSwatchGroupRoads.h"
#include "ProceduralMeshComponent.h"
#include "RoadMakerModule.h"
#include "Net/UnrealNetwork.h"
#include "FGFactoryColoringTypes.h"

ARMBuildableFill::ARMBuildableFill(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Paintable like roads (same "Roads" default swatch).
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();
}

void ARMBuildableFill::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableFill, mOutline);
	DOREPLIFETIME(ARMBuildableFill, mHeight);
}

void ARMBuildableFill::SetOutline(const TArray<FVector>& LocalOutline, float Height)
{
	mOutline = LocalOutline;
	mHeight = FMath::Clamp(Height, 20.f, 1000.f);
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableFill::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Top, Body;
	if (!RMGeometry::BuildSlab(mOutline, mHeight, mUVScale, Top, Body, 250.f, GetShownLook() == 2))
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: could not build the fill from %d outline points"), *GetName(), mOutline.Num());
		return;
	}
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyMaterial(0, TopMat);
	ApplyMaterial(1, BodyMat);
	// Far: fewer points inside.
	FRMMesh FarTop, FarBody;
	if (RMGeometry::BuildSlab(mOutline, mHeight, mUVScale, FarTop, FarBody, 800.f, GetShownLook() == 2))
	{
		ApplyFarSection(FarTop, 0, TopMat);
		ApplyFarSection(FarBody, 1, BodyMat);
	}
	ApplyPaint(mCustomizationData);
}

void ARMBuildableFill::GetWorldOutline(TArray<FVector>& OutOutline) const
{
	OutOutline.Reset();
	const FTransform& T = GetActorTransform();
	for (const FVector& P : mOutline)
	{
		OutOutline.Add(T.TransformPosition(P));
	}
}

uint32 ARMBuildableFill::GetShapeKey() const
{
	uint32 Key = GetTypeHash(GetActorLocation()) ^ (GetTypeHash(GetActorRotation().Yaw) * 31u) ^ (GetLook() * 7919u);
	for (const FVector& P : mOutline)
	{
		Key = HashCombine(Key, GetTypeHash(P));
	}
	return HashCombine(Key, GetTypeHash(mHeight));
}

void ARMBuildableFill::ApplyPaint(const FFactoryCustomizationData& Data)
{
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	FLinearColor P = Resolved.PrimaryColor;
	FLinearColor S = Resolved.SecondaryColor;
	AdjustColorsForLook(Data, P, S);
	ForEachMeshComponent([&P, &S](UProceduralMeshComponent* M)
	{
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
		M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
	});
}

void ARMBuildableFill::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyPaint(customizationData);
}

void ARMBuildableFill::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyPaint(customizationData);
}
