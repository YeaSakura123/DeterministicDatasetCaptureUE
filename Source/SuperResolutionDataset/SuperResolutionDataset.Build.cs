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
			"ChaosCaching",
			"ImageCore",
			"LevelSequence",
			"MovieScene",
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Json",
			"JsonUtilities",
			"Projects",
			"RenderCore",
			"Renderer",
			"RHI",
			"Slate",
			"SlateCore",
			"UMG"
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new[]
			{
				"AssetRegistry",
				"NiagaraEditor",
				"UnrealEd"
			});
		}
	}
}
