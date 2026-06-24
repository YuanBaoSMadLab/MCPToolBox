// Copyright MCPToolbox. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/SWindow.h"

class SMCPToolboxChatWidget;
class FMCPToolboxMCPClient;
class SMCPToolboxHelpWidget;
class SWidgetSwitcher;
struct FMCPToolboxAPIKeyEntry;

struct FMCPToolboxEntryListItem;

class MCPTOOLBOX_API SMCPToolboxWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPToolboxWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void SetParentWindow(TSharedPtr<SWindow> InWindow);
	void RequestDestroyWindow();

private:
	// ---- Tab创建 ----
	TSharedRef<SWidget> CreateChatTab();
	TSharedRef<SWidget> CreateAPIManagementTab();
	TSharedRef<SWidget> CreateSettingsTab();
	TSharedRef<SWidget> CreateMemoryTab();

	void RefreshMemoryListItems();

	// ---- 旧方法（占位兼容） ----
	TSharedRef<SWidget> CreateProviderDetailPanel();
	TSharedRef<SWidget> CreateAPIKeyPanel();
	TSharedRef<SWidget> CreateVisionDetectionPanel();
	TSharedRef<SWidget> CreateMCPServerPanel();
	TSharedRef<SWidget> CreateStatusBar();

	// ---- 事件处理 ----
	void OnTabSelected(int32 TabIndex);
	void OnProviderListSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void OnLanguageChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void OnThemeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	FReply OnAddEntry();
	FReply OnOpenHelp();
	FReply OnStartMCPServer();

	FReply OnAddProvider();
	FReply OnRemoveProvider();
	FReply OnSaveProvider();
	FReply OnSaveAPIKey();
	FReply OnDetectVision();
	FReply OnRefreshMCPConnection();
	FReply OnExportConfiguration();
	FReply OnImportConfiguration();

	void RefreshEntryListItems();

	// ---- 成员 ----
	TSharedPtr<SMCPToolboxChatWidget> ChatWidget;
	TSharedPtr<SMCPToolboxHelpWidget> HelpWidget;
	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SWidgetSwitcher> ContentSwitcher;

	// 服务商选择
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ProviderListComboBox;
	TArray<TSharedPtr<FString>> ProviderListOptions;
	TSharedPtr<FString> SelectedProviderOption;
	FString CurrentProviderId;

	// 模型选择
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelListComboBox;
	TArray<TSharedPtr<FString>> ModelListOptions;
	TSharedPtr<FString> SelectedModelOption;
	TSharedPtr<SEditableTextBox> ModelNameInput;

	// API密钥输入
	TSharedPtr<SEditableTextBox> APIKeyInput;

	// URL显示
	TSharedPtr<SEditableTextBox> ProviderNameInput;
	TSharedPtr<SEditableTextBox> ProviderBaseURLInput;

	// 条目列表
	TSharedPtr<SListView<TSharedPtr<FMCPToolboxEntryListItem>>> EntryListView;
	TArray<TSharedPtr<FMCPToolboxEntryListItem>> EntryListItems;

	// 设置输入
	TSharedPtr<SEditableTextBox> MCPServerHostInput;
	TSharedPtr<SEditableTextBox> MCPServerPortInput;

	// 语言/主题
	TSharedPtr<SComboBox<TSharedPtr<FString>>> LanguageComboBox;
	TArray<TSharedPtr<FString>> LanguageOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ThemeComboBox;
	TArray<TSharedPtr<FString>> ThemeOptions;

	// 状态栏
	TSharedPtr<STextBlock> StatusBarText;

	// 记忆管理
	TSharedPtr<SListView<TSharedPtr<FString>>> MemoryListView;
	TArray<TSharedPtr<FString>> MemoryListItems;

	// 旧输入（占位）
	TSharedPtr<SEditableTextBox> ProviderIdInput;
	TSharedPtr<SEditableTextBox> ProviderModelInput;
	TSharedPtr<SEditableTextBox> ProviderAPIKeyInput;
	TSharedPtr<SEditableTextBox> APIKeyIdInput;
	TSharedPtr<SEditableTextBox> APIKeyValueInput;
	TSharedPtr<SEditableTextBox> VisionProviderInput;
	TSharedPtr<SEditableTextBox> VisionModelInput;
	TSharedPtr<SEditableTextBox> MCPConnectionTimeoutInput;
	TSharedPtr<SEditableTextBox> MCPHeartbeatIntervalInput;

	TArray<TSharedPtr<SButton>> TabButtons;
	int32 ActiveTabIndex = 0;
	FString CurrentLanguage;
	FString CurrentTheme;
};
