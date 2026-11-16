// Console commands to look at Road Maker meshes in the game (the engine's own wireframe view mode is not in the
// release game):
//   RM.ShowLOD <-1|0|1|2>   every Road Maker piece shows that detail level (0 detailed, 1 far, 2 second far); -1 = by distance
//   RM.Wireframe [metres]   toggles the edges of the triangles drawn by Road Maker pieces near the aim (default 40 m),
//                           green = detailed, yellow = far, red = second far, and logs the triangle counts per level
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableProcedural.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"

namespace
{
	TWeakObjectPtr<AActor> GWireActor;

	bool AimPoint(UWorld* World, FVector& Out)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (!PC)
		{
			return false;
		}
		FVector Eye;
		FRotator View;
		PC->GetPlayerViewPoint(Eye, View);
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RMWireframe), true, PC->GetPawn());
		Out = World->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 20000.f, ECC_Visibility, Params)
			? Hit.Location : Eye + View.Vector() * 2000.f;
		return true;
	}

	/** A line as two thin crossing strips (seen from any side). */
	void AddLine(const FVector& A, const FVector& B, float Half, const FLinearColor& Color,
		TArray<FVector>& V, TArray<int32>& T, TArray<FLinearColor>& C)
	{
		const FVector Dir = (B - A).GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			return;
		}
		const FVector U = FVector::CrossProduct(Dir, FMath::Abs(Dir.Z) < 0.9f ? FVector::UpVector : FVector::ForwardVector).GetSafeNormal() * Half;
		const FVector W = FVector::CrossProduct(Dir, U).GetSafeNormal() * Half;
		for (const FVector& Side : { U, W })
		{
			const int32 Base = V.Num();
			V.Add(A - Side); V.Add(A + Side); V.Add(B + Side); V.Add(B - Side);
			for (int32 K = 0; K < 4; ++K)
			{
				C.Add(Color);
			}
			T.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
			T.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GRMShowLOD(
		TEXT("RM.ShowLOD"),
		TEXT("Road Maker: every piece shows one detail level (0 detailed, 1 far, 2 second far); -1 = by distance (normal)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ARMBuildableProcedural::sForcedLOD = Args.Num() > 0 ? FMath::Clamp(FCString::Atoi(*Args[0]), -1, 2) : -1;
			UE_LOG(LogRoadMaker, Display, TEXT("RM.ShowLOD %d"), ARMBuildableProcedural::sForcedLOD);
		}));

	FAutoConsoleCommandWithWorldAndArgs GRMWireframe(
		TEXT("RM.Wireframe"),
		TEXT("Road Maker: toggle the triangle edges of Road Maker pieces near the aim (optional radius in metres, default 40)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (AActor* Old = GWireActor.Get())
			{
				Old->Destroy();
				GWireActor.Reset();
				UE_LOG(LogRoadMaker, Display, TEXT("RM.Wireframe off"));
				return;
			}
			FVector At;
			if (!World || !AimPoint(World, At))
			{
				return;
			}
			const float Radius = (Args.Num() > 0 ? FMath::Max(1.f, FCString::Atof(*Args[0])) : 40.f) * 100.f;
			static const FLinearColor Colors[3] = { FLinearColor(0.f, 1.f, 0.f), FLinearColor(1.f, 0.9f, 0.f), FLinearColor(1.f, 0.f, 0.f) };
			TArray<FVector> V;
			TArray<int32> T;
			TArray<FLinearColor> C;
			int32 Edges = 0;
			bool bCapped = false;
			for (TActorIterator<ARMBuildableProcedural> It(World); It; ++It)
			{
				ARMBuildableProcedural* Piece = *It;
				TArray<TPair<UProceduralMeshComponent*, int32>> Meshes;
				Piece->GetMeshesByLevel(Meshes);
				int32 Tris[3] = { 0, 0, 0 };
				bool bNear = false;
				for (const TPair<UProceduralMeshComponent*, int32>& M : Meshes)
				{
					UProceduralMeshComponent* Comp = M.Key;
					if (!Comp || !Comp->IsRegistered())
					{
						continue;
					}
					for (int32 S = 0; S < Comp->GetNumSections(); ++S)
					{
						if (const FProcMeshSection* Sec = Comp->GetProcMeshSection(S))
						{
							Tris[M.Value] += Sec->ProcIndexBuffer.Num() / 3;
						}
					}
					if (!Comp->IsVisible() || Comp->Bounds.GetBox().ComputeSquaredDistanceToPoint(At) > Radius * Radius)
					{
						continue;
					}
					bNear = true;
					const FTransform X = Comp->GetComponentTransform();
					const float Half = 1.5f;
					for (int32 S = 0; S < Comp->GetNumSections() && !bCapped; ++S)
					{
						const FProcMeshSection* Sec = Comp->GetProcMeshSection(S);
						if (!Sec)
						{
							continue;
						}
						TSet<uint64> Done;
						const TArray<uint32>& I = Sec->ProcIndexBuffer;
						for (int32 K = 0; K + 2 < I.Num(); K += 3)
						{
							for (int32 E = 0; E < 3; ++E)
							{
								uint32 A = I[K + E], B = I[K + (E + 1) % 3];
								if (A > B) { Swap(A, B); }
								bool bSeen = false;
								Done.Add((uint64(A) << 32) | B, &bSeen);
								if (bSeen)
								{
									continue;
								}
								const FVector PA = X.TransformPosition(FVector(Sec->ProcVertexBuffer[A].Position));
								const FVector PB = X.TransformPosition(FVector(Sec->ProcVertexBuffer[B].Position));
								if (FVector::DistSquared(PA, At) > Radius * Radius && FVector::DistSquared(PB, At) > Radius * Radius)
								{
									continue;
								}
								AddLine(PA, PB, Half, Colors[M.Value], V, T, C);
								if (++Edges >= 60000)
								{
									bCapped = true;
									break;
								}
							}
							if (bCapped)
							{
								break;
							}
						}
					}
				}
				if (bNear)
				{
					UE_LOG(LogRoadMaker, Display, TEXT("RM.Wireframe: %s triangles detailed %d, far %d, second far %d"), *Piece->GetName(), Tris[0], Tris[1], Tris[2]);
				}
			}
			if (V.Num() == 0)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("RM.Wireframe: no Road Maker piece drawn within %.0f m of the aim"), Radius / 100.f);
				return;
			}
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
			if (!Actor)
			{
				return;
			}
			UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Actor, TEXT("RMWire"), RF_Transient);
			Actor->SetRootComponent(Mesh);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetCastShadow(false);
			Mesh->RegisterComponent();
			Mesh->CreateMeshSection_LinearColor(0, V, T, TArray<FVector>(), TArray<FVector2D>(), C, TArray<FProcMeshTangent>(), false);
			static UMaterialInterface* Overlay = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (M) { M->AddToRoot(); } return M; }();
			if (Overlay)
			{
				Mesh->SetMaterial(0, Overlay);
			}
			GWireActor = Actor;
			UE_LOG(LogRoadMaker, Display, TEXT("RM.Wireframe on: %d edges within %.0f m%s (green detailed, yellow far, red second far)"), Edges, Radius / 100.f,
				bCapped ? TEXT(", capped") : TEXT(""));
		}));
}
