#include "Net/RMRemoteCallObject.h"
#include "Customization/RMRoadLinePaint.h"
#include "Customization/RMRoadLineDescriptor.h"
#include "FGPlayerController.h"
#include "Net/UnrealNetwork.h"

void URMRemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(URMRemoteCallObject, mDummy);
}

void URMRemoteCallObject::ServerApplyRoadLines_Implementation(AActor* Target, TSubclassOf<URMRoadLineDescriptor> Sides, TSubclassOf<URMRoadLineDescriptor> Lanes)
{
	RMRoadLinePaint::ApplyToTarget(Target, Sides, Lanes);
}

void URMRemoteCallObject::RegisterFor(AFGPlayerController* PC)
{
	if (PC && PC->HasAuthority() && !PC->GetRemoteCallObjectOfClass(StaticClass()))
	{
		PC->RegisterRemoteCallObjectClass(StaticClass());
	}
}

URMRemoteCallObject* URMRemoteCallObject::Get(APlayerController* PC)
{
	AFGPlayerController* FGPC = Cast<AFGPlayerController>(PC);
	return FGPC ? Cast<URMRemoteCallObject>(FGPC->GetRemoteCallObjectOfClass(StaticClass())) : nullptr;
}
