// Road Maker's own sign icons (chevrons for curves...), offered in the game's sign icon picker next to the game's.
// They live in an icon library asset (/RoadMaker/Icons/IL_RoadMaker, a UFGIconLibrary) that the game finds with its
// own libraries. The library's entries have fields the editor shows as read-only (ID, type), so make_materials.py fills
// it through SetIcons here.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RMIconLibraryTools.generated.h"

class UFGIconLibrary;

UCLASS()
class ROADMAKER_API URMIconLibraryTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Editor (make_materials.py): replaces the library's icons with these, as monochrome icons (tinted with the sign's
	 * colour), IDs 0, 1, 2... in this order. Keep the order once released: saved signs refer to the IDs.
	 * An icon is a texture, or a UI material for an animated one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Road Maker")
	static void SetIcons(UFGIconLibrary* Library, const TArray<UObject*>& Icons, const TArray<FString>& Names, const TArray<bool>& Animated,
		bool bBackgrounds);

	/** Game start: tells the asset manager about our library (the game looks for icon libraries there). */
	static void RegisterLibrary();

	/**
	 * Game, every second: makes sure the world's icon database has our icons. If the game did not find the library by
	 * itself, it is added to the database's list of libraries and the database rebuilt. Logs once what happened, and
	 * once how many icons each category has (with a few names), to see where the game's own categories are.
	 */
	static void EnsureOurIcons(UWorld* World);

	static constexpr const TCHAR* LibraryPath = TEXT("/RoadMaker/Icons/IL_RoadMaker.IL_RoadMaker");
};
