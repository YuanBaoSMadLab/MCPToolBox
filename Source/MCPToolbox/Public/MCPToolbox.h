// Copyright MCPToolbox. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMCPToolbox, Log, All);

class SMCPToolboxWidget;

class MCPTOOLBOX_API FMCPToolboxModule : public IModuleInterface
{
public:
	// ---- IModuleInterface ----

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// ---- Window Management ----

	/** Open the MCPToolbox tab (creates it if not already open) */
	void OpenMCPToolboxWindow();

	/** Toggle the MCPToolbox tab open/closed */
	void ToggleMCPToolboxWindow();

	// ---- MCP Server ----

	/** Try to start UE's built-in MCP server; returns false if failed */
	static bool StartMCPServer(bool bSilent = false);

	/** Check if MCP server was started successfully */
	static bool IsMCPServerStarted() { return bMCPServerStarted; }

private:
	// ---- Initialization Helpers ----

	/** Create the MCPToolbox style set */
	void CreateStyleSet();

	/** Register the toolbar button in the Level Editor toolbar */
	void RegisterToolbarButton();

	/** Register the menu entry under the Window menu */
	void RegisterMenuEntry();

	/** Register the tab spawner with the level editor */
	void RegisterTabSpawner();

	/** Load configuration from DefaultMCPToolbox.ini */
	void LoadConfiguration();

	/** Validate the runtime environment (check for required modules) */
	void ValidateRuntimeEnvironment();

	/** Spawn the MCPToolbox tab content */
	TSharedRef<SDockTab> OnSpawnMCPToolboxTab(const FSpawnTabArgs& Args);

	// ---- State ----

	/** The MCPToolbox main widget */
	TSharedPtr<SMCPToolboxWidget> MCPToolboxWidget;

	/** Handle for the toolbar button extender */
	TSharedPtr<FExtender> ToolbarExtender;

	/** Handle for the menu extender */
	TSharedPtr<FExtender> MenuExtender;

	/** Delegate handle for the toolbar button registration */
	FDelegateHandle ToolbarButtonHandle;

	/** Delegate handle for the menu entry registration */
	FDelegateHandle MenuEntryHandle;

	/** Delegate handle for OnRegisterTabs (defers tab spawner registration) */
	FDelegateHandle OnRegisterTabsHandle;

	/** Whether the module has been fully initialized */
	bool bIsInitialized;

	/** Whether UE's built-in MCP server was started successfully */
	static bool bMCPServerStarted;

	/** Tab name for the dock tab */
	static const FName MCPToolboxTabName;
};
