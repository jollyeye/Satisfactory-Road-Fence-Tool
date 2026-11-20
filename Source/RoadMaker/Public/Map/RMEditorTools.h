#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RMEditorTools.generated.h"

class UMaterial;
class UMaterialExpression;
class UStringTable;

/** For make_materials.py: Python cannot name the material's Customized UV inputs, so it wires them through here. */
UCLASS()
class ROADMAKER_API URMEditorTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Road Maker")
	static bool ConnectCustomizedUV(UMaterial* Material, UMaterialExpression* Expression, int32 Index);

	/**
	 * For create_assets.py: Python can only make texts without a key, which the translation gather skips.
	 * Puts Source into the mod's string table under Key (namespace RoadMakerAssets) and returns a text
	 * that reads it from there, so every building name, description and menu name has one fixed key
	 * for translators. Editor only; in a game build it returns plain text.
	 */
	UFUNCTION(BlueprintCallable, Category = "Road Maker")
	static FText TableText(UStringTable* Table, const FString& Key, const FString& Source);
};
