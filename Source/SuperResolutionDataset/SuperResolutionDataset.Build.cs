using UnrealBuildTool;

public class SuperResolutionDataset : ModuleRules
{
	public SuperResolutionDataset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ImageCore",
			"LevelSequence",
			"MovieScene",
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Json",
			"JsonUtilities",
			"RenderCore",
			"RHI"
		});
	}
}
