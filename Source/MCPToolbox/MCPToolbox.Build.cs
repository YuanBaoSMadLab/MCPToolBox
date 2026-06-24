using UnrealBuildTool;
using System.IO;

public class MCPToolbox : ModuleRules
{
    public MCPToolbox(ReadOnlyTargetRules Target) : base(Target)
    {
        CppStandard = CppStandardVersion.Cpp20;
        bUseUnity = false;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore",
            "EditorStyle", "UnrealEd", "LevelEditor", "MainFrame",
            "ToolMenus", "InputCore", "HTTP", "WebSockets",
            "Json", "JsonUtilities", "Projects", "ApplicationCore",
            "ImageWrapper", "DesktopPlatform", "AssetRegistry", "EditorSubsystem",
            "DeveloperSettings", "Sockets", "Networking"
        });

        PrivateDependencyModuleNames.AddRange(new string[] { "MCPToolboxScreenshot" });

        // ThirdParty include paths
        string PluginDir = ModuleDirectory;
        string ThirdPartyDir = Path.Combine(PluginDir, "..", "..", "ThirdParty", "assistant");
        PublicIncludePaths.Add(ThirdPartyDir);
        PublicIncludePaths.Add(Path.Combine(ThirdPartyDir, "assistant", "common"));
        PublicIncludePaths.Add(Path.Combine(ThirdPartyDir, "assistant", "cpp-mcp"));

        PublicDefinitions.Add("MCPTOOLBOX_PLUGIN_VERSION=TEXT(\"0.1.0-beta\")");
        PublicDefinitions.Add("MCPTOOLBOX_MCP_DEFAULT_PORT=8000");
        PublicDefinitions.Add("_CRT_SECURE_NO_WARNINGS=1");

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Ws2_32.lib");
        }

        bEnableExceptions = true;
        bUseRTTI = true;
        CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
    }
}
