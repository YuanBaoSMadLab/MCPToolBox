// Copyright MCPToolbox. All Rights Reserved.

#include "MCPToolbox.h"
#include "MCPToolboxWidget.h"
#include "MCPToolboxStyle.h"
#include "MCPToolboxCommands.h"
#include "MCPToolboxAPIManager.h"
#include "MCPToolboxSettings.h"
#include "MCPToolboxMemoryManager.h"
#include "MCPToolboxChatSession.h"
#include "MCPToolboxAuxModelManager.h"
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
const FName FMCPToolboxModule::MCPToolboxTabName("MCPToolbox");

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

	// Step 7: Register tab spawner (deferred via OnRegisterTabs — TabManager may not be ready during PostEngineInit)
	if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		OnRegisterTabsHandle = LevelEditorModule.OnRegisterTabs().AddLambda([this](TSharedPtr<FTabManager>)
		{
			RegisterTabSpawner();
		});
		UE_LOG(LogMCPToolbox, Log, TEXT("Tab spawner registration deferred via OnRegisterTabs"));
	}
	else
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("LevelEditor not loaded, cannot register tab spawner"));
	}

	bIsInitialized = true;

	UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox Module Started Successfully"));
}

// ============================================================================
// FMCPToolboxModule::ShutdownModule
// ============================================================================

void FMCPToolboxModule::ShutdownModule()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("MCPToolbox Module Shutting Down..."));

	// Save current session before shutdown
	FMCPToolboxChatSessionManager::Get().SaveCurrentSession();
	FMCPToolboxAuxModelManager::Get().StopServer();

	// Clean up the widget
	MCPToolboxWidget.Reset();

	// Unregister tab spawner
	if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		
		// Remove deferred registration delegate
		if (OnRegisterTabsHandle.IsValid())
		{
			LevelEditorModule.OnRegisterTabs().Remove(OnRegisterTabsHandle);
			OnRegisterTabsHandle.Reset();
		}

		TSharedPtr<FTabManager> TabManager = LevelEditorModule.GetLevelEditorTabManager();
		if (TabManager.IsValid())
		{
			TabManager->UnregisterTabSpawner(MCPToolboxTabName);
		}
	}

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
// FMCPToolboxModule::RegisterTabSpawner
// ============================================================================

void FMCPToolboxModule::RegisterTabSpawner()
{
	if (!FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("LevelEditor module not loaded, cannot register tab spawner"));
		return;
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<FTabManager> TabManager = LevelEditorModule.GetLevelEditorTabManager();
	
	if (!TabManager.IsValid())
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("Level editor tab manager not available"));
		return;
	}

	TabManager->RegisterTabSpawner(MCPToolboxTabName, FOnSpawnTab::CreateRaw(this, &FMCPToolboxModule::OnSpawnMCPToolboxTab))
		.SetDisplayName(LOCTEXT("MCPToolbox_TabTitle", "MCP Toolbox"))
		.SetTooltipText(LOCTEXT("MCPToolbox_TabTooltip", "Open MCP Toolbox AI Assistant"))
		.SetIcon(FSlateIcon(FMCPToolboxStyle::GetStyleSetName(), "MCPToolbox.OpenMCPToolboxWindow"));

	UE_LOG(LogMCPToolbox, Log, TEXT("Tab spawner registered"));
}

// ============================================================================
// FMCPToolboxModule::OnSpawnMCPToolboxTab
// ============================================================================

TSharedRef<SDockTab> FMCPToolboxModule::OnSpawnMCPToolboxTab(const FSpawnTabArgs& Args)
{
	// Create the main widget
	SAssignNew(MCPToolboxWidget, SMCPToolboxWidget);

	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(LOCTEXT("MCPToolbox_TabTitle", "MCP Toolbox"))
		[
			MCPToolboxWidget.ToSharedRef()
		];
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

	if (!FModuleManager::Get().IsModuleLoaded("LevelEditor"))
		return;

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<FTabManager> TabManager = LevelEditorModule.GetLevelEditorTabManager();
	
	if (TabManager.IsValid())
	{
		TabManager->TryInvokeTab(MCPToolboxTabName);
	}
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

	OpenMCPToolboxWindow();
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

	// ponytail: 模块依赖由 Build.cs 保证,跳过冗余验证
	// 原 IsModuleLoaded 在启动早期误报,删掉避免噪音

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
