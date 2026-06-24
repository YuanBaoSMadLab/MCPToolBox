using UnrealBuildTool;

public class MCPToolboxScreenshot : ModuleRules
{
    public MCPToolboxScreenshot(ReadOnlyTargetRules Target) : base(Target)
    {
        CppStandard = CppStandardVersion.Cpp20;
        bUseUnity = false;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Slate", "SlateCore",
            "LevelEditor", "ImageWrapper", "RenderCore", "RHI", "ApplicationCore"
        });
        PrivateDependencyModuleNames.AddRange(new string[] { "EditorStyle", "MainFrame" });

        if (Target.Platform == UnrealTargetPlatform.Win64)
            PublicDefinitions.Add("MCPTOOLBOX_SCREENSHOT_WINDOWS=1");

        OptimizeCode = CodeOptimization.InShippingBuildsOnly;
        bEnableExceptions = true;
    }
}
