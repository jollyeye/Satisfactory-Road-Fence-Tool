#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RMEditorTools.generated.h"

class UMaterial;
class UMaterialExpression;

/** For make_materials.py: Python cannot name the material's Customized UV inputs, so it wires them through here. */
UCLASS()
class ROADMAKER_API URMEditorTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Road Maker")
	static bool ConnectCustomizedUV(UMaterial* Material, UMaterialExpression* Expression, int32 Index);
};
