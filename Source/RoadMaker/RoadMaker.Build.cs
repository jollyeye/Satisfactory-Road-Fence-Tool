using UnrealBuildTool;

public class RoadMaker : ModuleRules
{
	public RoadMaker(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"PhysicsCore",
			"NetCore",
			"ProceduralMeshComponent",
			"UMG",
			"Slate",
			"SlateCore",
			"EnhancedInput",
			// Satisfactory + mod loader
			"FactoryGame",
			"SML",
		});
	}
}
