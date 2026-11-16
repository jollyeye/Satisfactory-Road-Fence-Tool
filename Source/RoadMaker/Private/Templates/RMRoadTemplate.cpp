#include "Templates/RMRoadTemplate.h"
#include "RoadMakerModule.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableFence.h"
#include "Hologram/FGBlueprintHologram.h"
#include "FGHologramBuildModeDescriptor.h"
#include "Patching/NativeHookManager.h"
#include "RMGeometry.h"

namespace RMRoadTemplate
{
	namespace
	{
		// Road Maker pieces each blueprint hologram shows (they live in the blueprint's own world).
		TMap<TWeakObjectPtr<AFGBlueprintHologram>, TArray<TWeakObjectPtr<ARMBuildableProcedural>>> GPieces;

		UClass* RoadModeClass()
		{
			static TWeakObjectPtr<UClass> Cached;
			if (!Cached.IsValid())
			{
				Cached = LoadClass<UFGHologramBuildModeDescriptor>(nullptr, TEXT("/RoadMaker/Holograms/BM_RoadMode.BM_RoadMode_C"));
			}
			return Cached.Get();
		}

		bool HasRoad(const AFGBlueprintHologram* Hologram)
		{
			const TArray<TWeakObjectPtr<ARMBuildableProcedural>>* Pieces = GPieces.Find(const_cast<AFGBlueprintHologram*>(Hologram));
			if (!Pieces)
			{
				return false;
			}
			for (const TWeakObjectPtr<ARMBuildableProcedural>& P : *Pieces)
			{
				const ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(P.Get());
				if (Road && !Road->IsWalkingPath())
				{
					return true;
				}
			}
			return false;
		}

		/** Signed distance of P from the road's centre line (+ = the road's right) and height above it. */
		bool Measure(const TArray<FRMFrame>& Frames, const FVector& P, float& OutOffset, float& OutLift)
		{
			int32 Best = INDEX_NONE;
			float BestD = TNumericLimits<float>::Max();
			for (int32 I = 0; I < Frames.Num(); ++I)
			{
				const float D = FVector::DistSquared2D(Frames[I].Pos, P);
				if (D < BestD)
				{
					BestD = D;
					Best = I;
				}
			}
			if (Best == INDEX_NONE)
			{
				return false;
			}
			const FRMFrame& F = Frames[Best];
			// Beyond the road's ends: not along it.
			const float Along = FVector::DotProduct(P - F.Pos, F.Fwd);
			if ((Best == 0 && Along < -50.f) || (Best == Frames.Num() - 1 && Along > 50.f))
			{
				return false;
			}
			OutOffset = FVector::DotProduct(P - F.Pos, F.Right);
			OutLift = P.Z - F.Pos.Z;
			return true;
		}
	}

	void NotePiece(AFGBlueprintHologram* Hologram, ARMBuildableProcedural* Piece)
	{
		if (Hologram && Piece)
		{
			GPieces.FindOrAdd(Hologram).AddUnique(Piece);
		}
	}

	FRMRoadTemplate Read(AFGBlueprintHologram* Hologram)
	{
		FRMRoadTemplate T;
		TArray<TWeakObjectPtr<ARMBuildableProcedural>>* Pieces = GPieces.Find(Hologram);
		if (!Pieces)
		{
			return T;
		}
		// The main road: the longest road (not a walking path).
		ARMBuildableRoad* Main = nullptr;
		float MainLen = 0.f;
		for (const TWeakObjectPtr<ARMBuildableProcedural>& P : *Pieces)
		{
			ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(P.Get());
			if (Road && !Road->IsWalkingPath() && Road->GetLength() > MainLen)
			{
				MainLen = Road->GetLength();
				Main = Road;
			}
		}
		if (!Main)
		{
			return T;
		}
		T.RoadClass = Main->GetClass();
		T.RoadWidth = Main->GetRoadWidth();
		T.Look = Main->GetLook();
		TArray<FRMFrame> Frames;
		Main->GetWorldFrames(Frames, 50.f);
		// Every fence / walking path running along it: the same distance from its centre all the way (within 50 cm).
		for (const TWeakObjectPtr<ARMBuildableProcedural>& P : *Pieces)
		{
			ARMBuildableSpline* Piece = Cast<ARMBuildableSpline>(P.Get());
			if (!Piece || Piece == Main)
			{
				continue;
			}
			ARMBuildableRoad* Path = Cast<ARMBuildableRoad>(Piece);
			ARMBuildableFence* Fence = Cast<ARMBuildableFence>(Piece);
			if (!(Fence || (Path && Path->IsWalkingPath())))
			{
				continue;
			}
			TArray<FRMFrame> Own;
			Piece->GetWorldFrames(Own, 100.f);
			float Sum = 0.f, Min = TNumericLimits<float>::Max(), Max = -TNumericLimits<float>::Max(), LiftSum = 0.f;
			int32 Count = 0;
			for (const FRMFrame& F : Own)
			{
				float Off, Lift;
				if (Measure(Frames, F.Pos, Off, Lift))
				{
					Sum += Off;
					LiftSum += Lift;
					Min = FMath::Min(Min, Off);
					Max = FMath::Max(Max, Off);
					++Count;
				}
			}
			if (Count < 2 || Max - Min > 50.f)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Road template: %s is not along the road (spread %.0f cm), left out"), *Piece->GetName(), Count < 2 ? 0.f : Max - Min);
				continue;
			}
			FRMTemplatePiece TP;
			TP.Class = Piece->GetClass();
			TP.Offset = Sum / Count;
			TP.Lift = LiftSum / Count;
			TP.bWalkingPath = Path != nullptr;
			if (Fence)
			{
				// Facing relative to the road's direction (the fence may run the other way).
				TP.InwardSign = Fence->GetInwardSign() * (FVector::DotProduct(Own[0].Fwd, Frames[0].Fwd) >= 0.f ? 1.f : -1.f);
				TP.Look = Fence->GetWallLook();
			}
			else
			{
				TP.Look = Path->GetLook();
			}
			T.Pieces.Add(TP);
		}
		T.Pieces.Sort([](const FRMTemplatePiece& A, const FRMTemplatePiece& B) { return A.Offset < B.Offset; });
		return T;
	}

	void InstallHooks()
	{
#if !WITH_EDITOR
		// Road Mode next to the blueprint's own modes, when it holds a Road Maker road.
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::GetSupportedBuildModes_Implementation, GetMutableDefault<AFGBlueprintHologram>(),
			[](const AFGBlueprintHologram* Self, TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& OutModes)
			{
				UClass* Mode = RoadModeClass();
				if (Mode && HasRoad(Self))
				{
					OutModes.AddUnique(Mode);
				}
			});
		SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBlueprintHologram::OnBuildModeChanged, GetMutableDefault<AFGBlueprintHologram>(),
			[](AFGBlueprintHologram* Self, TSubclassOf<UFGHologramBuildModeDescriptor> BuildMode)
			{
				if (!BuildMode || BuildMode.Get() != RoadModeClass())
				{
					return;
				}
				const FRMRoadTemplate T = Read(Self);
				UE_LOG(LogRoadMaker, Display, TEXT("Road template: road %s, %.0f cm wide, look %d, %d pieces along it"),
					T.RoadClass ? *T.RoadClass->GetName() : TEXT("none"), T.RoadWidth, T.Look, T.Pieces.Num());
				for (const FRMTemplatePiece& P : T.Pieces)
				{
					UE_LOG(LogRoadMaker, Display, TEXT("Road template:   %s at %.0f cm (%s), %.0f cm up, facing %.0f, look %d"),
						P.Class ? *P.Class->GetName() : TEXT("?"), P.Offset, P.Offset >= 0.f ? TEXT("right") : TEXT("left"), P.Lift, P.InwardSign, P.Look);
				}
			});
#endif
	}
}
