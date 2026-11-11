#include "Buildables/RMBuildableIntersection.h"
#include "ProceduralMeshComponent.h"
#include "RoadMakerModule.h"
#include "Net/UnrealNetwork.h"
#include "FGFactoryColoringTypes.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMSwatchGroupRoads.h"

ARMBuildableIntersection::ARMBuildableIntersection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Paintable like roads (same "Roads" default swatch), for the side line colour.
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();
}

void ARMBuildableIntersection::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableIntersection, mEnds);
	DOREPLIFETIME(ARMBuildableIntersection, mHeight);
	DOREPLIFETIME(ARMBuildableIntersection, mSideLines);
}

void ARMBuildableIntersection::SetEnds(const TArray<FRMRoadEnd>& LocalEnds, float Height)
{
	mEnds = LocalEnds;
	mHeight = Height;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::OnRep_Ends()
{
	RebuildMesh();
}

void ARMBuildableIntersection::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Top, Body;
	TArray<FVector> Outline;
	TArray<bool> IsMouth;
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Outline, &IsMouth))
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: could not rebuild intersection from %d saved ends"), *GetName(), mEnds.Num());
		return;
	}
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	UMaterialInterface* TopMat = LookMaterial(mLookTopMaterials, mTopMaterial);
	UMaterialInterface* BodyMat = LookMaterial(mLookBodyMaterials, mBodyMaterial);
	ApplyMaterial(0, TopMat);
	ApplyMaterial(1, BodyMat);

	if (mPreviewSideLines.Get(mSideLines))
	{
		TArray<TArray<FVector>> Curbs;
		RMGeometry::GetIntersectionCurbs(Outline, IsMouth, Curbs);
		FRMMesh Lines;
		RMGeometry::BuildCurbLines(Curbs, mEdgeLineInset, Top, Lines);
		Lines.ApplyToSection(mMesh, 2, false);
		ApplyMaterial(2, TopMat);
	}

	// Far version: 3 points per curb fillet instead of mFilletSegments.
	FRMMesh FarTop, FarBody;
	TArray<FVector> FarOutline;
	TArray<bool> FarIsMouth;
	if (RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, FMath::Min(mFilletSegments, 3), FarTop, FarBody, &FarOutline, &FarIsMouth))
	{
		ApplyFarSection(FarTop, 0, TopMat);
		ApplyFarSection(FarBody, 1, BodyMat);
		if (mPreviewSideLines.Get(mSideLines))
		{
			TArray<TArray<FVector>> FarCurbs;
			RMGeometry::GetIntersectionCurbs(FarOutline, FarIsMouth, FarCurbs);
			FRMMesh FarLines;
			RMGeometry::BuildCurbLines(FarCurbs, mEdgeLineInset, FarTop, FarLines);
			ApplyFarSection(FarLines, 2, TopMat);
		}
	}
	ApplyMarkingColors(mCustomizationData);
}

void ARMBuildableIntersection::SetSideLines(bool bOn)
{
	if (mSideLines == bOn)
	{
		return;
	}
	mSideLines = bOn;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::SetPreviewSideLines(TOptional<bool> Preview)
{
	if (Preview == mPreviewSideLines)
	{
		return;
	}
	mPreviewSideLines = Preview;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::ApplyMarkingColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh)
	{
		return;
	}
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	FLinearColor P = Resolved.PrimaryColor;
	FLinearColor S = Resolved.SecondaryColor;
	AdjustColorsForLook(Data, P, S);
	for (UProceduralMeshComponent* M : { mMesh, mFarMesh })
	{
		if (M)
		{
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		}
	}
}

void ARMBuildableIntersection::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableIntersection::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableIntersection::GetWorldEnds(TArray<FRMRoadEnd>& OutEnds) const
{
	OutEnds.Reset();
	const FTransform& T = GetActorTransform();
	for (const FRMRoadEnd& E : mEnds)
	{
		FRMRoadEnd W = E;
		W.Center = T.TransformPosition(E.Center);
		W.Inward = T.TransformVectorNoScale(E.Inward);
		OutEnds.Add(W);
	}
}

void ARMBuildableIntersection::GetWorldCurbs(TArray<TArray<FVector>>& OutCurbs) const
{
	OutCurbs.Reset();
	FRMMesh Top, Body;
	TArray<FVector> Outline;
	TArray<bool> IsMouth;
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body, &Outline, &IsMouth))
	{
		return;
	}
	RMGeometry::GetIntersectionCurbs(Outline, IsMouth, OutCurbs);
	const FTransform& T = GetActorTransform();
	for (TArray<FVector>& Curb : OutCurbs)
	{
		for (FVector& P : Curb)
		{
			P = T.TransformPosition(P);
		}
	}
}
