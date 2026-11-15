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
			"AkAudio", // the game's sounds (Wwise)
			// Satisfactory + mod loader
			"FactoryGame",
			"SML",
			"OnlineIntegration", // the game's sign header (sign wall) includes LocalUserInfo.h
		});
		// The map layer draws roads with Slate instance buffers (GPU map roads, 1.4.0).
		PrivateIncludePathModuleNames.Add("SlateRHIRenderer");
	}
}
