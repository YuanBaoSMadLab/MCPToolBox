// Copyright MCPToolbox. All Rights Reserved.

#include "MCPToolbox.h"
#include "MCPToolboxWidget.h"
#include "MCPToolboxStyle.h"
#include "MCPToolboxCommands.h"
#include "MCPToolboxAPIManager.h"
#include "MCPToolboxSettings.h"
#include "MCPToolboxMemoryManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SSpacer.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Interfaces/IMainFrameModule.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenuEntry.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "FMCPToolboxModule"

DEFINE_LOG_CATEGORY(LogMCPToolbox);

bool FMCPToolboxModule::bMCPServerStarted = false;

// ============================================================================
// Static Helpers
// ============================================================================

static const FName MCPToolboxTabName("MCPToolbox");

// ============================================================================
// FMCPToolboxModule::StartupModule
// ============================================================================

void FMCPToolboxModule::StartupModule()
{
	bIsInitialized = false;

	UE_LOG(LogMCPToolbox, Log, TEXT("========================================"));
	UE_LOG(LogMCPToolbox, Log, TEXT(" MCPToolbox Module Starting..."));
	UE_LOG(LogMCPToolbox, Log, TEXT("========================================"));

	// Step 1: Create style set
	CreateStyleSet();

	// Step 2: Load configuration from INI
	LoadConfiguration();

	// Step 2.5: Initialize memory system
	FMCPToolboxMemoryManager::Get().Initialize();

	// Step 3: Validate runtime environment
	ValidateRuntimeEnvironment();

	// Step 4: Register commands
	FMCPToolboxCommands::Register();

	// Step 5: Register toolbar button
	RegisterToolbarButton();

	// Step 6: Register menu entry
	RegisterMenuEntry();

	bIsInitialized = true;

	UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox Module Started Successfully"));
}

// ============================================================================
// FMCPToolboxModule::ShutdownModule
// ============================================================================

void FMCPToolboxModule::ShutdownModule()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox Module Shutting Down..."));

	// Close the window if it is open
	if (MCPToolboxWindow.IsValid())
	{
		MCPToolboxWindow->RequestDestroyWindow();
		MCPToolboxWindow.Reset();
	}

	// Clean up the widget
	MCPToolboxWidget.Reset();

	// Unregister commands
	FMCPToolboxCommands::Unregister();

	// Remove menu entry
	if (MenuEntryHandle.IsValid())
	{
		if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
		{
			FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
			LevelEditorModule.GetMenuExtensibilityManager()->RemoveExtender(MenuExtender);
		}
		MenuEntryHandle.Reset();
	}
	MenuExtender.Reset();

	// Remove toolbar button
	if (ToolbarButtonHandle.IsValid())
	{
		if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
		{
			FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
			LevelEditorModule.GetToolBarExtensibilityManager()->RemoveExtender(ToolbarExtender);
		}
		ToolbarButtonHandle.Reset();
	}
	ToolbarExtender.Reset();

	// Shutdown style
	FMCPToolboxStyle::Shutdown();

	bIsInitialized = false;

	UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox Module Shutdown Complete"));
}

// ============================================================================
// FMCPToolboxModule::CreateMCPToolboxWindow
// ============================================================================

TSharedPtr<SWindow> FMCPToolboxModule::CreateMCPToolboxWindow()
{
	if (MCPToolboxWindow.IsValid())
	{
		// Window already exists, bring it to front
		MCPToolboxWindow->BringToFront();
		return MCPToolboxWindow;
	}

	// Load window dimensions from settings
	const UMCPToolboxSettings* Settings = GetDefault<UMCPToolboxSettings>();
	int32 WindowWidth = Settings ? Settings->WindowWidth : 800;
	int32 WindowHeight = Settings ? Settings->WindowHeight : 600;

	// Create the main widget
	SAssignNew(MCPToolboxWidget, SMCPToolboxWidget);

	// Create the window
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("MCPToolbox_WindowTitle", "MCP Toolbox"))
		.ClientSize(FVector2D(WindowWidth, WindowHeight))
		.SupportsMaximize(true)
		.SupportsMinimize(true)
		.SizingRule(ESizingRule::UserSized)
		.IsTopmostWindow(false)
		[
			MCPToolboxWidget.ToSharedRef()
		];

	// Store the window reference in the widget
	MCPToolboxWidget->SetParentWindow(Window);

	// Add the window to the slate application
	FSlateApplication::Get().AddWindow(Window);

	// Register for window closed notification
	Window->GetOnWindowClosedEvent().AddLambda([this](const TSharedRef<SWindow>&)
	{
		MCPToolboxWindow.Reset();
		MCPToolboxWidget.Reset();
		UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox window closed"));
	});

	MCPToolboxWindow = Window;

	UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox window created (%dx%d)"), WindowWidth, WindowHeight);

	return Window;
}

// ============================================================================
// FMCPToolboxModule::OpenMCPToolboxWindow
// ============================================================================

void FMCPToolboxModule::OpenMCPToolboxWindow()
{
	if (!bIsInitialized)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("OpenMCPToolboxWindow called before module initialization"));
		return;
	}

	CreateMCPToolboxWindow();
}

// ============================================================================
// FMCPToolboxModule::ToggleMCPToolboxWindow
// ============================================================================

void FMCPToolboxModule::ToggleMCPToolboxWindow()
{
	if (!bIsInitialized)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("ToggleMCPToolboxWindow called before module initialization"));
		return;
	}

	if (MCPToolboxWindow.IsValid())
	{
		if (MCPToolboxWindow->IsVisible())
		{
			MCPToolboxWindow->HideWindow();
			UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox window hidden"));
		}
		else
		{
			MCPToolboxWindow->ShowWindow();
			MCPToolboxWindow->BringToFront();
			UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox window shown"));
		}
	}
	else
	{
		CreateMCPToolboxWindow();
	}
}

// ============================================================================
// FMCPToolboxModule::CreateStyleSet
// ============================================================================

void FMCPToolboxModule::CreateStyleSet()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("Initializing MCPToolbox style set..."));
	FMCPToolboxStyle::Initialize();
}

// ============================================================================
// FMCPToolboxModule::RegisterToolbarButton
// ============================================================================

void FMCPToolboxModule::RegisterToolbarButton()
{
	if (!FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("LevelEditor module not loaded, cannot register toolbar button"));
		return;
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");

	// Create toolbar extender
	ToolbarExtender = MakeShareable(new FExtender);
	ToolbarExtender->AddToolBarExtension(
		"Settings",
		EExtensionHook::After,
		nullptr,
		FToolBarExtensionDelegate::CreateLambda([this](FToolBarBuilder& ToolbarBuilder)
		{
			ToolbarBuilder.AddToolBarButton(
				FMCPToolboxCommands::Get().OpenMCPToolboxWindow,
				NAME_None,
				LOCTEXT("MCPToolbox_ToolbarButtonLabel", "MCPToolbox"),
				LOCTEXT("MCPToolbox_ToolbarButtonTooltip", "打开MCP Toolbox AI助手窗口"),
				FSlateIcon(FMCPToolboxStyle::GetStyleSetName(), "MCPToolbox.OpenMCPToolboxWindow")
			);
		})
	);

	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);

	UE_LOG(LogMCPToolbox, Log, TEXT("Toolbar button registered"));
}

// ============================================================================
// FMCPToolboxModule::RegisterMenuEntry
// ============================================================================

void FMCPToolboxModule::RegisterMenuEntry()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (!ToolMenus) return;

	UToolMenu* WindowMenu = ToolMenus->ExtendMenu("LevelEditor.MainMenu.Window");
	if (!WindowMenu) return;

	FToolMenuSection& Section = WindowMenu->FindOrAddSection("WindowLayout");
	Section.AddMenuEntry(
		"MCPToolbox_Window",
		LOCTEXT("MCPToolbox_MenuEntryLabel", "MCP Toolbox"),
		LOCTEXT("MCPToolbox_MenuEntryTooltip", "Open MCP Toolbox AI Assistant window"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FMCPToolboxModule::OpenMCPToolboxWindow))
	);

	UE_LOG(LogMCPToolbox, Log, TEXT("Menu entry registered under Window menu"));
}

// ============================================================================
// FMCPToolboxModule::LoadConfiguration
// ============================================================================

void FMCPToolboxModule::LoadConfiguration()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("Loading MCPToolbox configuration..."));

	// Configuration is loaded via UDeveloperSettings system
	// (UMCPToolboxSettings with Config=MCPToolbox)
	const UMCPToolboxSettings* Settings = GetDefault<UMCPToolboxSettings>();
	if (Settings)
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("  MCP Server: %s:%d"),
			*Settings->MCPServerHost, Settings->MCPServerPort);
		UE_LOG(LogMCPToolbox, Log, TEXT("  Default Language: %s"), *Settings->DefaultLanguage);
		UE_LOG(LogMCPToolbox, Log, TEXT("  UI Theme: %s"), *Settings->UITheme);
		UE_LOG(LogMCPToolbox, Log, TEXT("  Window Size: %dx%d"), Settings->WindowWidth, Settings->WindowHeight);
		UE_LOG(LogMCPToolbox, Log, TEXT("  Auto Reconnect: %s"), Settings->MCPAutoReconnect ? TEXT("true") : TEXT("false"));
		UE_LOG(LogMCPToolbox, Log, TEXT("  Encryption: %s"), Settings->bEncryptAPIKeys ? TEXT("enabled") : TEXT("disabled"));
		UE_LOG(LogMCPToolbox, Log, TEXT("  Vision Auto-Detect: %s"), Settings->bAutoDetectVisionSupport ? TEXT("enabled") : TEXT("disabled"));
	}
	else
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("MCPToolbox settings not found, using defaults"));
	}

	// Also read from GConfig for programmatic access
	if (GConfig)
	{
		FString ConfigSection = TEXT("MCPToolbox");
		FString Value;

		if (GConfig->GetString(*ConfigSection, TEXT("DefaultLanguage"), Value, GEditorPerProjectIni))
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("Config override - Language: %s"), *Value);
		}
	}
}

// ============================================================================
// FMCPToolboxModule::ValidateRuntimeEnvironment
// ============================================================================

void FMCPToolboxModule::ValidateRuntimeEnvironment()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("Validating MCPToolbox runtime environment..."));

	TArray<FString> RequiredModules = {
		TEXT("Core"),
		TEXT("CoreUObject"),
		TEXT("Engine"),
		TEXT("Slate"),
		TEXT("SlateCore"),
		TEXT("EditorStyle"),
		TEXT("UnrealEd"),
		TEXT("LevelEditor"),
		TEXT("MainFrame"),
		TEXT("InputCore"),
		TEXT("HTTP"),
		TEXT("WebSockets"),
		TEXT("Json"),
		TEXT("JsonUtilities"),
		TEXT("Projects"),
		TEXT("ApplicationCore"),
		TEXT("ImageWrapper"),
		TEXT("DesktopPlatform"),
		TEXT("AssetRegistry")
	};

	bool bAllModulesLoaded = true;
	for (const FString& ModuleName : RequiredModules)
	{
		if (!FModuleManager::Get().IsModuleLoaded(*ModuleName))
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("  Required module not loaded: %s"), *ModuleName);
			bAllModulesLoaded = false;
		}
	}

	if (bAllModulesLoaded)
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("  All required modules are loaded"));
	}
	else
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("  Some required modules are not loaded - MCPToolbox may not function correctly"));
	}

	// Check for screenshot module
	if (FModuleManager::Get().IsModuleLoaded("MCPToolboxScreenshot"))
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("  Screenshot module loaded"));
	}
	else
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("  Screenshot module not loaded (optional)"));
	}

	// Log platform info
	UE_LOG(LogMCPToolbox, Log, TEXT("  Platform: %s"), FPlatformProcess::GetBinariesSubdirectory());
	UE_LOG(LogMCPToolbox, Log, TEXT("  Engine version: %s"), *FEngineVersion::Current().ToString());
}

// ============================================================================
// FMCPToolboxModule::StartMCPServer
// ============================================================================

bool FMCPToolboxModule::StartMCPServer(bool bSilent)
{
	if (!GEditor)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[MCP] GEditor not available, cannot start MCP server"));
		if (!bSilent)
		{
			FNotificationInfo Info(LOCTEXT("MCPNoEditor", "MCP: 编辑器未就绪，请稍后重试"));
			Info.ExpireDuration = 4.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
		return false;
	}

	// Try to start UE's built-in MCP server
	// This requires the "ModelContextProtocol" plugin to be enabled in UE
	GEditor->Exec(nullptr, TEXT("ModelContextProtocol.StartServer"));

	bMCPServerStarted = true;

	UE_LOG(LogMCPToolbox, Log, TEXT("[MCP] UE MCP server start command issued (port 8000)"));

	return true;
}

// ============================================================================
// Module Implementation
// ============================================================================

IMPLEMENT_MODULE(FMCPToolboxModule, MCPToolbox)

#undef LOCTEXT_NAMESPACE
