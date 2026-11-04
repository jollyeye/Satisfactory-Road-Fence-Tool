// Lets clients in multiplayer ask the server to change Road Maker pieces (only the server may).
#pragma once

#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "RMRemoteCallObject.generated.h"

class URMRoadLineDescriptor;

UCLASS()
class ROADMAKER_API URMRemoteCallObject : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Applies Road Lines options to a road or intersection, on the server. */
	UFUNCTION(Server, Reliable)
	void ServerApplyRoadLines(AActor* Target, TSubclassOf<URMRoadLineDescriptor> Sides, TSubclassOf<URMRoadLineDescriptor> Lanes);

	/** Switches a fence wall's look (customizer Material tab), on the server. */
	UFUNCTION(Server, Reliable)
	void ServerSetFenceWallLook(class ARMBuildableFence* Wall, uint8 Look);

	/** Registers this RCO for a player controller (call on the server). */
	static void RegisterFor(class AFGPlayerController* PC);

	/** This player's RCO, if the server registered it. */
	static URMRemoteCallObject* Get(class APlayerController* PC);

private:
	// An RCO needs at least one replicated property for its RPCs to work.
	UPROPERTY(Replicated)
	bool mDummy = true;
};
