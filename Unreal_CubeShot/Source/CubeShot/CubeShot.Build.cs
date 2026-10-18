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
                "ApplicationCore",
                "AssetRegistry",
                "AssetTools",
                "ContentBrowser",
                "DesktopPlatform",
                "ImageCore",
                "ImageWrapper",
                "InputCore",
                "LevelEditor",
                "Json",
                "JsonUtilities",
                "ModelContextProtocol",
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
