#include "RMSound.h"
#include "RoadMakerModule.h"
#include "AkAudioEvent.h"
#include "AkGameplayStatics.h"
#include "AkComponent.h"
#include "AkRtpc.h"
#include "Buildables/FGBuildable.h"
#include "FGBuildEffectActor.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace RMSound
{
	namespace
	{
		// The three events the game's default build effect Blueprint (BuildEffect_Default) plays.
		UAkAudioEvent* GStartEvent = nullptr;  // Play_F_BuildEffect_Switch
		UAkAudioEvent* GSwishEvent = nullptr;  // Play_BuildEffect_SwishComponents
		UAkAudioEvent* GEndEvent = nullptr;    // Play_F_BuildEffect_Rework
		bool GSearched = false;
		// Game parameters the build effect sets on its sounds (found in its Blueprint code).
		UAkRtpc* GSizeRtpc = nullptr;  // BuildEffect_Size_RTPC
		UAkRtpc* GBoxRtpc = nullptr;   // RTPC_BoxVector

		void SoundCollect(UStruct* Struct, const FString& Where, TArray<TPair<FString, UAkAudioEvent*>>& Out)
		{
			for (UObject* Object : Struct->ScriptAndPropertyObjectReferences)
			{
				if (UAkRtpc* Rtpc = Cast<UAkRtpc>(Object))
				{
					UAkRtpc*& Slot = Rtpc->GetName().Contains(TEXT("Size")) ? GSizeRtpc : GBoxRtpc;
					if (!Slot)
					{
						Slot = Rtpc;
						Slot->AddToRoot();
					}
				}
				if (Object && !Object->IsA<UAkAudioEvent>() && Object->GetClass()->GetName().StartsWith(TEXT("Ak")))
				{
					UE_LOG(LogRoadMaker, Display, TEXT("RMSound: also uses %s %s (%s)"), *Object->GetClass()->GetName(), *Object->GetPathName(), *Where);
				}
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
		UAkAudioEvent* SoundFindEvents()
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
			for (const TPair<FString, UAkAudioEvent*>& P : Found)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("RMSound: event %s (%s)"), *P.Value->GetPathName(), *P.Key);
				const FString Name = P.Value->GetName();
				UAkAudioEvent*& Slot = Name.Contains(TEXT("Swish")) ? GSwishEvent
					: Name.Contains(TEXT("Rework")) ? GEndEvent
					: GStartEvent;
				if (!Slot)
				{
					Slot = P.Value;
					Slot->AddToRoot();
				}
			}
			UE_LOG(LogRoadMaker, Display, TEXT("RMSound: start = %s, during = %s, end = %s"),
				*GetNameSafe(GStartEvent), *GetNameSafe(GSwishEvent), *GetNameSafe(GEndEvent));
			return GStartEvent;
		}

		/** Posts Event at the piece with the build effect's parameters set (size = SizeValue). */
		void SoundPostAt(AActor* Piece, UAkAudioEvent* Event, float SizeValue)
		{
			if (!Piece || !Event || !Piece->GetWorld())
			{
				return;
			}
			const FBox Bounds = Piece->GetComponentsBoundingBox();
			const FVector Where = Bounds.IsValid ? Bounds.GetCenter() : Piece->GetActorLocation();
			UAkComponent* Comp = UAkGameplayStatics::SpawnAkComponentAtLocation(Piece, Event, Where, Piece->GetActorRotation(), false, true);
			if (!Comp)
			{
				UAkGameplayStatics::PostAkEventAtLocation(Piece, Event, Where, Piece->GetActorRotation());
				return;
			}
			// The build effect's game parameters (BuildEffect_Size_RTPC, RTPC_BoxVector) are left at their
			// defaults: setting them did not lengthen the sound, and values that sounded right in one test
			// gave the end sound of a big building in the next (2026-11-12). Unset = the normal sounds.
			Comp->PostAkEvent(Event, 0, FOnAkPostEventCallback());
		}
	}

	void PlayBuild(AActor* Piece, float Duration)
	{
		if (!Piece || !Piece->GetWorld() || IsRunningDedicatedServer())
		{
			return;
		}
		if (!GSearched)
		{
			GSearched = true;
			SoundFindEvents();
		}
		const float Size = 0.f; // (unused, see SoundPostAt)
		SoundPostAt(Piece, GStartEvent, Size);
		FTimerManager& Timers = Piece->GetWorld()->GetTimerManager();
		const TWeakObjectPtr<AActor> Weak(Piece);
		// During: the components swish, about every 0.4 s while the animation runs.
		if (GSwishEvent)
		{
			for (float At = 0.15f; At < Duration - 0.2f; At += 0.4f)
			{
				FTimerHandle Handle;
				Timers.SetTimer(Handle, FTimerDelegate::CreateLambda([Weak, Size]() { SoundPostAt(Weak.Get(), GSwishEvent, Size); }), At, false);
			}
		}
		// End: when the animation is done.
		if (GEndEvent)
		{
			FTimerHandle Handle;
			Timers.SetTimer(Handle, FTimerDelegate::CreateLambda([Weak, Size]() { SoundPostAt(Weak.Get(), GEndEvent, Size); }), FMath::Max(Duration, 0.05f), false);
		}
	}
}
