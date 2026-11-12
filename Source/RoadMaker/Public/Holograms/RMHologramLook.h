// Procedural preview meshes are built after the game set up the hologram's look, so they get the
// game's hologram material and the stencil value that colours it (valid / warning / invalid) here.
#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"

namespace RMHologramLook
{
	/**
	 * The hologram version of one of our surface materials: MI_<name>_Holo (made by make_materials.py),
	 * the same material masked, so the mesh's vertex alpha fades it (dithered). Source if there is none.
	 */
	inline UMaterialInterface* HoloVersion(UMaterialInterface* Source)
	{
		if (!Source)
		{
			return nullptr;
		}
		static TMap<FName, UMaterialInterface*> Cache; // entries are rooted (never freed)
		const FName Key = Source->GetFName();
		if (UMaterialInterface** Found = Cache.Find(Key))
		{
			return *Found ? *Found : Source;
		}
		FString Name = Source->GetName();
		Name.RemoveFromStart(TEXT("M_"));
		const FString Holo = FString::Printf(TEXT("MI_%s_Holo"), *Name);
		UMaterialInterface* Loaded = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/RoadMaker/Materials/%s.%s"), *Holo, *Holo));
		if (Loaded)
		{
			Loaded->AddToRoot();
		}
		Cache.Add(Key, Loaded);
		return Loaded ? Loaded : Source;
	}

	inline void Apply(UProceduralMeshComponent* Mesh, UMaterialInterface* Material, uint8 Stencil)
	{
		if (!Mesh)
		{
			return;
		}
		if (Material)
		{
			for (int32 I = 0; I < Mesh->GetNumSections(); ++I)
			{
				Mesh->SetMaterial(I, Material);
			}
		}
		Mesh->SetRenderCustomDepth(true);
		Mesh->SetCustomDepthStencilValue(Stencil);
	}
}
