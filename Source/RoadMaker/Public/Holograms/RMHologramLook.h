// Procedural preview meshes are built after the game set up the hologram's look, so they get the
// game's hologram material and the stencil value that colours it (valid / warning / invalid) here.
#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"

namespace RMHologramLook
{
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
