using UnrealBuildTool;

public class CubeShot : ModuleRules
{
    public CubeShot(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "AppFramework",
                "AssetRegistry",
                "AssetTools",
                "ContentBrowser",
                "ImageCore",
                "ImageWrapper",
                "InputCore",
                "LevelEditor",
                "PropertyEditor",
                "RenderCore",
                "RHI",
                "Slate",
                "SlateCore",
                "SourceControl",
                "ToolMenus",
                "UnrealEd"
            });
    }
}
