#include "RMSound.h"
#include "RoadMakerModule.h"
#include "AkAudioEvent.h"
#include "AkGameplayStatics.h"
#include "Buildables/FGBuildable.h"
#include "FGBuildEffectActor.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace RMSound
{
	namespace
	{
		UAkAudioEvent* GConstructEvent = nullptr;
		bool GSearched = false;

		void SoundCollect(UStruct* Struct, const FString& Where, TArray<TPair<FString, UAkAudioEvent*>>& Out)
		{
			for (UObject* Object : Struct->ScriptAndPropertyObjectReferences)
			{
				if (UAkAudioEvent* Event = Cast<UAkAudioEvent>(Object))
				{
					if (!Out.ContainsByPredicate([Event](const TPair<FString, UAkAudioEvent*>& P) { return P.Value == Event; }))
					{
						Out.Add(TPair<FString, UAkAudioEvent*>(Where, Event));
					}
				}
			}
		}

		/**
		 * The build sound lives in the game's build effect actor (a Blueprint: its PlayThumbSound event
		 * posts a Wwise event). Find that class (the one a foundation uses) and the sound events its
		 * Blueprint code references; log them all and use the "thump" / build one.
		 */
		UAkAudioEvent* SoundFindConstructEvent()
		{
			UClass* EffectClass = nullptr;
			if (UClass* Foundation = LoadClass<AFGBuildable>(nullptr, TEXT("/Game/FactoryGame/Buildable/Building/Foundation/Build_Foundation_8x4_01.Build_Foundation_8x4_01_C")))
			{
				if (const UFGBuildableSparseDataObject* Sparse = Foundation->GetDefaultObject<AFGBuildable>()->GetBuildableSparseData())
				{
					EffectClass = Sparse->mSparseBuildEffectActorTemplate;
				}
			}
			if (!EffectClass)
			{
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->IsChildOf(AFGBuildEffectActor::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract) && *It != AFGBuildEffectActor::StaticClass())
					{
						EffectClass = *It;
						break;
					}
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("RMSound: build effect class %s"), *GetNameSafe(EffectClass));
			TArray<TPair<FString, UAkAudioEvent*>> Found;
			for (UClass* Class = EffectClass; Class && Class != AFGBuildEffectActor::StaticClass(); Class = Class->GetSuperClass())
			{
				SoundCollect(Class, Class->GetName(), Found);
				for (TFieldIterator<UFunction> F(Class, EFieldIteratorFlags::ExcludeSuper); F; ++F)
				{
					SoundCollect(*F, Class->GetName() + TEXT(".") + F->GetName(), Found);
				}
				UObject* CDO = Class->GetDefaultObject();
				for (TFieldIterator<FObjectProperty> P(Class, EFieldIteratorFlags::ExcludeSuper); P; ++P)
				{
					if (UAkAudioEvent* Event = Cast<UAkAudioEvent>(P->GetObjectPropertyValue_InContainer(CDO)))
					{
						Found.Add(TPair<FString, UAkAudioEvent*>(Class->GetName() + TEXT(" property ") + P->GetName(), Event));
					}
				}
			}
			UAkAudioEvent* Best = nullptr;
			int32 BestScore = -1;
			for (const TPair<FString, UAkAudioEvent*>& P : Found)
			{
				const FString Name = P.Value->GetName();
				UE_LOG(LogRoadMaker, Display, TEXT("RMSound: event %s (%s)"), *P.Value->GetPathName(), *P.Key);
				int32 Score = 0;
				if (Name.Contains(TEXT("Thump")) || Name.Contains(TEXT("Thumb")))
				{
					Score = 3;
				}
				else if (Name.Contains(TEXT("Build")) || Name.Contains(TEXT("Construct")))
				{
					Score = 2;
				}
				else if (P.Key.Contains(TEXT("Thumb")))
				{
					Score = 1;
				}
				if (Name.Contains(TEXT("Dismantle")) || Name.Contains(TEXT("Stop")))
				{
					Score -= 5;
				}
				if (Score > BestScore)
				{
					Best = P.Value;
					BestScore = Score;
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("RMSound: %d events found, build sound = %s"), Found.Num(), *GetPathNameSafe(Best));
			if (Best)
			{
				Best->AddToRoot();
			}
			return Best;
		}
	}

	void PlayConstruct(AActor* Piece)
	{
		if (!Piece || !Piece->GetWorld() || IsRunningDedicatedServer())
		{
			return;
		}
		if (!GSearched)
		{
			GSearched = true;
			GConstructEvent = SoundFindConstructEvent();
		}
		if (GConstructEvent)
		{
			const FBox Bounds = Piece->GetComponentsBoundingBox();
			const FVector Where = Bounds.IsValid ? Bounds.GetCenter() : Piece->GetActorLocation();
			UAkGameplayStatics::PostAkEventAtLocation(Piece, GConstructEvent, Where, Piece->GetActorRotation());
		}
	}
}
