// Copyright MCPToolbox. All Rights Reserved.

#include "MCPToolboxWidget.h"
#include "MCPToolboxChatWidget.h"
#include "MCPToolboxHelpWidget.h"
#include "MCPToolboxAPIManager.h"
#include "MCPToolboxSettings.h"
#include "MCPToolbox.h"
#include "MCPToolboxMemoryManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#include "SlateCore.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "EditorStyleSet.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"

#define LOCTEXT_NAMESPACE "MCPToolboxWidget"

// ============================================================================
// API条目列表项
// ============================================================================
struct FMCPToolboxEntryListItem
{
	FMCPToolboxAPIKeyEntry Entry;
};

class SMCPToolboxEntryListRow : public SMultiColumnTableRow<TSharedPtr<FMCPToolboxEntryListItem>>
{
public:
	SLATE_BEGIN_ARGS(SMCPToolboxEntryListRow) {}
		SLATE_ARGUMENT(TSharedPtr<FMCPToolboxEntryListItem>, Item)
		SLATE_EVENT(FSimpleDelegate, OnDelete)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		OnDelete = InArgs._OnDelete;

		SMultiColumnTableRow<TSharedPtr<FMCPToolboxEntryListItem>>::Construct(
			FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid()) return SNullWidget::NullWidget;

		if (ColumnName == TEXT("Provider"))
			return SNew(STextBlock).Text(FText::FromString(Item->Entry.ProviderName));
		if (ColumnName == TEXT("Model"))
			return SNew(STextBlock).Text(FText::FromString(Item->Entry.ModelId));
		if (ColumnName == TEXT("Key"))
			return SNew(STextBlock).Text(FText::FromString(Item->Entry.GetMaskedKey()));
		if (ColumnName == TEXT("Delete"))
		{
			return SNew(SButton)
				.Text(LOCTEXT("DeleteBtn", "删除"))
				.OnClicked_Lambda([this]() {
					if (OnDelete.IsBound()) OnDelete.Execute();
					return FReply::Handled();
				});
		}
		return SNullWidget::NullWidget;
	}

	TSharedPtr<FMCPToolboxEntryListItem> Item;
	FSimpleDelegate OnDelete;
};

// ============================================================================
// SMCPToolboxWidget
// ============================================================================
void SMCPToolboxWidget::Construct(const FArguments& InArgs)
{
	ActiveTabIndex = 0;
	CurrentLanguage = TEXT("zh-CN");
	CurrentTheme = TEXT("Default");

	ChildSlot
	[
		SNew(SVerticalBox)

		// ---- 标题栏 ----
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("WindowTitle", "MCP Toolbox - AI助手"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
		]

		// ---- Tab栏 ----
		+ SVerticalBox::Slot().AutoHeight().Padding(2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("TabChat", "对话"))
				.OnClicked_Lambda([this]() { OnTabSelected(0); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("TabAPI", "API密钥"))
				.OnClicked_Lambda([this]() { OnTabSelected(1); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("TabSettings", "设置"))
				.OnClicked_Lambda([this]() { OnTabSelected(2); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("TabMemory", "记忆"))
				.OnClicked_Lambda([this]() { OnTabSelected(3); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("StartMCP", "启动MCP"))
				.OnClicked(this, &SMCPToolboxWidget::OnStartMCPServer)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshMCP", "刷新工具"))
				.OnClicked_Lambda([this]()
				{
					if (ChatWidget.IsValid())
						ChatWidget->RefreshMCPTools();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton)
				.Text(LOCTEXT("HelpBtn", "? 帮助"))
				.OnClicked(this, &SMCPToolboxWidget::OnOpenHelp)
			]
		]

		// ---- 内容区 ----
		+ SVerticalBox::Slot().FillHeight(1.0).Padding(2)
		[
			SAssignNew(ContentSwitcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()[CreateChatTab()]
			+ SWidgetSwitcher::Slot()[CreateAPIManagementTab()]
			+ SWidgetSwitcher::Slot()[CreateSettingsTab()]
			+ SWidgetSwitcher::Slot()[CreateMemoryTab()]
		]

		// ---- 状态栏 ----
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SAssignNew(StatusBarText, STextBlock)
			.Text(LOCTEXT("StatusReady", "就绪"))
		]
	];

	// 默认选中对话Tab
	ContentSwitcher->SetActiveWidgetIndex(0);
}

void SMCPToolboxWidget::SetParentWindow(TSharedPtr<SWindow> InWindow)
{
	ParentWindow = InWindow;
}

void SMCPToolboxWidget::RequestDestroyWindow()
{
	if (ParentWindow.IsValid())
	{
		ParentWindow->RequestDestroyWindow();
	}
}

void SMCPToolboxWidget::OnTabSelected(int32 TabIndex)
{
	ActiveTabIndex = TabIndex;
	if (ContentSwitcher.IsValid())
	{
		ContentSwitcher->SetActiveWidgetIndex(TabIndex);
	}
}

// ============================================================================
// 对话Tab
// ============================================================================
TSharedRef<SWidget> SMCPToolboxWidget::CreateChatTab()
{
	SAssignNew(ChatWidget, SMCPToolboxChatWidget);

	return SNew(SBox)
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			ChatWidget.ToSharedRef()
		];
}

// ============================================================================
// API密钥管理Tab（全新设计：选择服务商 → 选择模型 → 填密钥 → 添加）
// ============================================================================
TSharedRef<SWidget> SMCPToolboxWidget::CreateAPIManagementTab()
{
	// 构建服务商下拉选项
	TArray<FString> ProviderIds = FMCPToolboxProviderPreset::GetAllProviderIds();
	ProviderListOptions.Empty();
	for (const FString& Id : ProviderIds)
	{
		const FMCPToolboxProviderPreset* P = FMCPToolboxProviderPreset::Find(Id);
		FString Label = P ? FString::Printf(TEXT("%s (%s)"), *P->Name, *Id) : Id;
		ProviderListOptions.Add(MakeShareable(new FString(Label)));
	}

	// 构建模型下拉选项（初始为空，选服务商后动态更新）
	ModelListOptions.Empty();
	ModelListComboBox = nullptr;
	SelectedModelOption.Reset();

	// 构建已添加条目列表
	RefreshEntryListItems();

	ProviderNameInput = nullptr;
	ProviderBaseURLInput = nullptr;

	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8)
		[
			SNew(SVerticalBox)

			// ---- 标题 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("APIMgmtTitle", "API密钥管理"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
			]

			// ---- 生图模型说明 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SBorder)
				.Padding(FMargin(8.0f))
				.BorderBackgroundColor(FLinearColor(0.1f, 0.2f, 0.4f, 0.3f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ImageGenTitle", "图片生成模型配置"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ImageGenDesc", "支持三种生图方式，请根据环境选择："))
						.Font(FCoreStyle::GetDefaultFontStyle("", 10))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(4, 2, 0, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ImageGenWebUI", "1. SD WebUI (本地) - http://127.0.0.1:7860/"))
						.Font(FCoreStyle::GetDefaultFontStyle("", 10))
						.ColorAndOpacity(FLinearColor(0.6f, 0.8f, 1.0f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(4, 2, 0, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ImageGenComfyUI", "2. ComfyUI (本地) - http://127.0.0.1:8200/"))
						.Font(FCoreStyle::GetDefaultFontStyle("", 10))
						.ColorAndOpacity(FLinearColor(0.6f, 1.0f, 0.8f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(4, 2, 0, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ImageGenCloud", "3. 云端多模态模型 (DALL-E / Replicate / Pollinations)"))
						.Font(FCoreStyle::GetDefaultFontStyle("", 10))
						.ColorAndOpacity(FLinearColor(1.0f, 0.8f, 0.6f))
					]
				]
			]

			// ---- 步骤1：选择服务商 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Step1", "1. 选择服务商"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(ProviderListComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ProviderListOptions)
				.OnSelectionChanged(this, &SMCPToolboxWidget::OnProviderListSelectionChanged)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
					return SNew(STextBlock).Text(FText::FromString(*Item));
				})
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() {
						return SelectedProviderOption.IsValid()
							? FText::FromString(*SelectedProviderOption)
							: LOCTEXT("SelectProvider", "-- 请选择服务商 --");
					})
				]
			]

			// ---- 自动填入的地址 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BaseURL", "API地址（自动填入）"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(ProviderBaseURLInput, SEditableTextBox)
				.HintText(LOCTEXT("BaseURLHint", "选择服务商后自动填入"))
			]

			// ---- 步骤2：选择模型 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Step2", "2. 选择模型（或手动输入）"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(ModelListComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ModelListOptions)
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewValue, ESelectInfo::Type) {
					SelectedModelOption = NewValue;
					if (ModelNameInput.IsValid() && NewValue.IsValid())
						ModelNameInput->SetText(FText::FromString(*NewValue));
				})
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
					return SNew(STextBlock).Text(FText::FromString(*Item));
				})
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() {
						return SelectedModelOption.IsValid()
							? FText::FromString(*SelectedModelOption)
							: LOCTEXT("SelectModel", "-- 选择或输入模型 --");
					})
				]
			]

			// ---- 手动输入模型 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelManual", "手动输入模型ID（可选）"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(ModelNameInput, SEditableTextBox)
				.HintText(LOCTEXT("ModelHint", "例如 gpt-4o, deepseek-chat"))
			]

			// ---- 步骤3：填写API密钥 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Step3", "3. 填写API密钥（本地模型可选）"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SAssignNew(APIKeyInput, SEditableTextBox)
				.HintText(LOCTEXT("APIKeyHint", "sk-xxxxxxxxxxxxxxxx（本地模型可留空）"))
				.IsPassword_Lambda([]() { return true; })
			]

			// ---- 添加按钮 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SButton)
				.Text(LOCTEXT("AddEntryBtn", "添加"))
				.OnClicked(this, &SMCPToolboxWidget::OnAddEntry)
			]

			// ---- 分隔线 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8)
			[
				SNew(SBorder)
				.BorderBackgroundColor(FLinearColor(0.2f, 0.2f, 0.2f))
				.Padding(FMargin(0, 1))
				[
					SNew(SBox).HeightOverride(1)
				]
			]

			// ---- 已添加的密钥列表标题 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SavedEntries", "已添加的API密钥"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
			]

			// ---- 表头 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.25)
				[
					SNew(STextBlock).Text(LOCTEXT("ColProvider", "服务商"))
				]
				+ SHorizontalBox::Slot().FillWidth(0.30)
				[
					SNew(STextBlock).Text(LOCTEXT("ColModel", "模型"))
				]
				+ SHorizontalBox::Slot().FillWidth(0.25)
				[
					SNew(STextBlock).Text(LOCTEXT("ColKey", "密钥"))
				]
				+ SHorizontalBox::Slot().FillWidth(0.20)
				[
					SNew(STextBlock).Text(LOCTEXT("ColAction", "操作"))
				]
			]

			// ---- 条目列表 ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(EntryListView, SListView<TSharedPtr<FMCPToolboxEntryListItem>>)
				.ListItemsSource(&EntryListItems)
				.OnGenerateRow_Lambda([this](TSharedPtr<FMCPToolboxEntryListItem> Item, const TSharedRef<STableViewBase>& OwnerTable) {
					return SNew(SMCPToolboxEntryListRow, OwnerTable)
						.Item(Item)
						.OnDelete(FSimpleDelegate::CreateLambda([this, ItemPtr = Item]() {
							FMCPToolboxAPIManager::Get().RemoveEntry(ItemPtr->Entry.Id);
							RefreshEntryListItems();
						}));
				})
				.HeaderRow(
					SNew(SHeaderRow)
					+ SHeaderRow::Column(TEXT("Provider")).DefaultLabel(LOCTEXT("ColProvider", "服务商")).FillWidth(0.25)
					+ SHeaderRow::Column(TEXT("Model")).DefaultLabel(LOCTEXT("ColModel", "模型")).FillWidth(0.30)
					+ SHeaderRow::Column(TEXT("Key")).DefaultLabel(LOCTEXT("ColKey", "密钥")).FillWidth(0.25)
					+ SHeaderRow::Column(TEXT("Delete")).DefaultLabel(LOCTEXT("ColAction", "操作")).FillWidth(0.20)
				)
			]

			// ---- 无条目提示 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(8)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoEntries", "尚未添加任何API密钥。请在上方选择服务商并填写密钥后点击\"添加\"。"))
				.AutoWrapText(true)
				.Visibility_Lambda([this]() {
					return EntryListItems.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
			]
		];
}

void SMCPToolboxWidget::RefreshEntryListItems()
{
	EntryListItems.Empty();
	const auto& Entries = FMCPToolboxAPIManager::Get().GetEntries();
	for (const auto& Entry : Entries)
	{
		TSharedPtr<FMCPToolboxEntryListItem> Item = MakeShareable(new FMCPToolboxEntryListItem());
		Item->Entry = Entry;
		EntryListItems.Add(Item);
	}

	if (EntryListView.IsValid())
	{
		EntryListView->RequestListRefresh();
	}
}

void SMCPToolboxWidget::OnProviderListSelectionChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	SelectedProviderOption = NewSelection;

	if (!NewSelection.IsValid()) return;

	// 解析供应商ID（格式: "OpenAI (openai)" -> "openai"）
	FString Sel = *NewSelection;
	FString ProviderId;
	int32 ParenIdx = -1;
	if (Sel.FindLastChar(TCHAR('('), ParenIdx))
	{
		int32 EndIdx = -1;
		if (Sel.FindLastChar(TCHAR(')'), EndIdx) && EndIdx > ParenIdx)
		{
			ProviderId = Sel.Mid(ParenIdx + 1, EndIdx - ParenIdx - 1);
		}
	}
	if (ProviderId.IsEmpty()) ProviderId = Sel;

	const FMCPToolboxProviderPreset* Preset = FMCPToolboxProviderPreset::Find(ProviderId);
	if (Preset)
	{
		// 自动填入URL
		if (ProviderBaseURLInput.IsValid())
		{
			ProviderBaseURLInput->SetText(FText::FromString(Preset->BaseURL));
		}

		// 更新模型列表
		ModelListOptions.Empty();
		for (const FString& Model : Preset->Models)
		{
			ModelListOptions.Add(MakeShareable(new FString(Model)));
		}
		SelectedModelOption.Reset();

		if (ModelListComboBox.IsValid())
		{
			ModelListComboBox->RefreshOptions();
		}

		CurrentProviderId = ProviderId;
	}
}

FReply SMCPToolboxWidget::OnAddEntry()
{
	if (CurrentProviderId.IsEmpty())
	{
		FNotificationInfo Info(LOCTEXT("NoProvider", "请先选择一个服务商"));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return FReply::Handled();
	}

	FString ModelId;
	if (ModelNameInput.IsValid())
		ModelId = ModelNameInput->GetText().ToString().TrimStartAndEnd();
	if (ModelId.IsEmpty() && SelectedModelOption.IsValid())
		ModelId = *SelectedModelOption;
	if (ModelId.IsEmpty())
	{
		FNotificationInfo Info(LOCTEXT("NoModel", "请选择或输入模型ID"));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return FReply::Handled();
	}

	FString ApiKey;
	if (APIKeyInput.IsValid())
		ApiKey = APIKeyInput->GetText().ToString().TrimStartAndEnd();

	// Read user-edited BaseURL. Previously this input was visually present but silently
	// ignored — making it impossible to connect to llama.cpp / LM Studio on custom ports
	// or to override any preset's URL. Now passed through to AddEntry.
	FString BaseURLOverride;
	if (ProviderBaseURLInput.IsValid())
		BaseURLOverride = ProviderBaseURLInput->GetText().ToString().TrimStartAndEnd();

	FMCPToolboxAPIManager::Get().AddEntry(CurrentProviderId, ModelId, ApiKey, BaseURLOverride);

	// 清空输入
	if (APIKeyInput.IsValid()) APIKeyInput->SetText(FText::GetEmpty());
	if (ModelNameInput.IsValid()) ModelNameInput->SetText(FText::GetEmpty());
	SelectedModelOption.Reset();

	RefreshEntryListItems();

	FNotificationInfo Info(LOCTEXT("EntryAdded", "API密钥已添加并加密保存"));
	Info.ExpireDuration = 2.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	return FReply::Handled();
}

// ============================================================================
// ============================================================================
// 记忆管理Tab
// ============================================================================
TSharedRef<SWidget> SMCPToolboxWidget::CreateMemoryTab()
{
	RefreshMemoryListItems();

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MemoryTitle", "记忆管理"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshMemories", "刷新"))
				.OnClicked_Lambda([this]() { RefreshMemoryListItems(); if (MemoryListView.IsValid()) MemoryListView->RequestListRefresh(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearMemories", "清空全部"))
				.OnClicked_Lambda([this]()
				{
					FMCPToolboxMemoryManager& MM = FMCPToolboxMemoryManager::Get();
					FString Root = MM.GetMemoryRoot();
					if (!Root.IsEmpty())
					{
						TArray<FString> Files;
						IFileManager::Get().FindFiles(Files, *(Root / TEXT("*.md")), true, false);
						for (const FString& F : Files)
						{
							FString Slug = FPaths::GetBaseFilename(F);
							MM.DeleteNote(Slug);
						}
					}
					RefreshMemoryListItems();
					if (MemoryListView.IsValid()) MemoryListView->RequestListRefresh();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(STextBlock)
			.Text_Lambda([]() -> FText
			{
				int32 Count = FMCPToolboxMemoryManager::Get().BuildMemoryContext().IsEmpty() ? 0 :
					[]() -> int32 {
						FString Root = FMCPToolboxMemoryManager::Get().GetMemoryRoot();
						TArray<FString> Files;
						IFileManager::Get().FindFiles(Files, *(Root / TEXT("*.md")), true, false);
						return Files.Num();
					}();
				return FText::FromString(FString::Printf(TEXT("共 %d 条记忆  路径: %s"), Count, *FMCPToolboxMemoryManager::Get().GetMemoryRoot()));
			})
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f))
		]
		+ SVerticalBox::Slot().FillHeight(1.0).Padding(2)
		[
			SAssignNew(MemoryListView, SListView<TSharedPtr<FString>>)
			.ListItemsSource(&MemoryListItems)
			.OnGenerateRow_Lambda([this](TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& OwnerTable)
			{
				return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
					.Padding(FMargin(2))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0)
						[
							SNew(STextBlock)
							.Text(FText::FromString(*Item))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
							.AutoWrapText(true)
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("DeleteMemory", "删除"))
							.OnClicked_Lambda([this, Item]()
							{
								// Extract slug from display: "slug — description"
								FString Slug;
								FString Display = *Item;
								int32 DashPos = Display.Find(TEXT(" — "));
								if (DashPos != INDEX_NONE) Slug = Display.Left(DashPos);
								else Slug = Display;
								FMCPToolboxMemoryManager::Get().DeleteNote(Slug);
								// Refresh the list so the deleted row disappears.
								// Previously this only deleted the file on disk but left
								// the stale row visible — looked like the button did nothing.
								RefreshMemoryListItems();
								if (MemoryListView.IsValid())
									MemoryListView->RequestListRefresh();
								return FReply::Handled();
							})
						]
					];
			})
		];
}

void SMCPToolboxWidget::RefreshMemoryListItems()
{
	MemoryListItems.Empty();
	FMCPToolboxMemoryManager& MM = FMCPToolboxMemoryManager::Get();
	FString Root = MM.GetMemoryRoot();
	if (Root.IsEmpty()) return;

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Root / TEXT("*.md")), true, false);

	for (const FString& File : Files)
	{
		FString Slug = FPaths::GetBaseFilename(File);
		FMCPToolboxMemoryNote Note;
		if (MM.ReadNote(Slug, Note))
		{
			FString Display = FString::Printf(TEXT("%s — %s [%s]"), *Slug, *Note.Description, *Note.Updated.Left(16));
			MemoryListItems.Add(MakeShareable(new FString(Display)));
		}
		else
		{
			MemoryListItems.Add(MakeShareable(new FString(Slug + TEXT(" — (读取失败)"))));
		}
	}
}

// 设置Tab
// ============================================================================
TSharedRef<SWidget> SMCPToolboxWidget::CreateSettingsTab()
{
	UMCPToolboxSettings* Settings = GetMutableDefault<UMCPToolboxSettings>();
	if (!Settings) Settings = GetMutableDefault<UMCPToolboxSettings>();

	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8)
		[
			SNew(SVerticalBox)

			// 标题
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsTitle", "设置"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
			]

			// ---- MCP连接 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MCPSection", "MCP连接"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(LOCTEXT("MCPServerHost", "MCP服务器地址: "))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0)
				[
					SAssignNew(MCPServerHostInput, SEditableTextBox)
					.Text(FText::FromString(Settings ? Settings->MCPServerHost : TEXT("127.0.0.1")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(LOCTEXT("MCPServerPort", "MCP端口: "))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0)
				[
					SAssignNew(MCPServerPortInput, SEditableTextBox)
					.Text(FText::AsNumber(Settings ? Settings->MCPServerPort : 8000))
				]
			]

			// ---- 界面 ----
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("UISection", "界面"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(LOCTEXT("Language", "语言: "))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0)
				[
					SAssignNew(LanguageComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&LanguageOptions)
					.OnSelectionChanged(this, &SMCPToolboxWidget::OnLanguageChanged)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
						return SNew(STextBlock).Text(FText::FromString(*Item));
					})
					.Content()
					[
						SNew(STextBlock).Text(LOCTEXT("LangZH", "中文 (zh-CN)"))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(LOCTEXT("Theme", "主题: "))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0)
				[
					SAssignNew(ThemeComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&ThemeOptions)
					.OnSelectionChanged(this, &SMCPToolboxWidget::OnThemeChanged)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
						return SNew(STextBlock).Text(FText::FromString(*Item));
					})
					.Content()
					[
						SNew(STextBlock).Text(LOCTEXT("ThemeDefault", "默认 (UE风格)"))
					]
				]
			]

			// ---- 保存按钮 ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SButton)
				.Text(LOCTEXT("SaveSettings", "保存设置"))
				.OnClicked_Lambda([this]() {
					UMCPToolboxSettings* Cfg = GetMutableDefault<UMCPToolboxSettings>();
					if (Cfg)
					{
						if (MCPServerHostInput.IsValid())
							Cfg->MCPServerHost = MCPServerHostInput->GetText().ToString();
						if (MCPServerPortInput.IsValid())
						{
							int32 Port = FCString::Atoi(*MCPServerPortInput->GetText().ToString());
							if (Port > 0) Cfg->MCPServerPort = Port;
						}
						Cfg->SaveConfig();
					}

					FNotificationInfo Info(LOCTEXT("SettingsSaved", "设置已保存"));
					Info.ExpireDuration = 2.0f;
					FSlateNotificationManager::Get().AddNotification(Info);
					return FReply::Handled();
				})
			]
		];

	// 初始化语言选项
	LanguageOptions.Empty();
	LanguageOptions.Add(MakeShareable(new FString(TEXT("zh-CN"))));
	LanguageOptions.Add(MakeShareable(new FString(TEXT("en-US"))));
	ThemeOptions.Empty();
	ThemeOptions.Add(MakeShareable(new FString(TEXT("Default"))));
	ThemeOptions.Add(MakeShareable(new FString(TEXT("Dark"))));
	ThemeOptions.Add(MakeShareable(new FString(TEXT("Light"))));
}

// ---- 占位方法已删除（旧接口兼容，从未被 UI 绑定引用） ----

void SMCPToolboxWidget::OnLanguageChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (NewSelection.IsValid())
	{
		CurrentLanguage = *NewSelection;
	}
}

void SMCPToolboxWidget::OnThemeChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (NewSelection.IsValid())
	{
		CurrentTheme = *NewSelection;
	}
}

FReply SMCPToolboxWidget::OnOpenHelp()
{
	TSharedRef<SMCPToolboxHelpWidget> HelpDialog = SNew(SMCPToolboxHelpWidget);

	TSharedRef<SWindow> HelpWindow = SNew(SWindow)
		.Title(LOCTEXT("HelpWindowTitle", "MCP Toolbox 帮助手册"))
		.ClientSize(FVector2D(1000, 700))
		.SupportsMinimize(true)
		.SupportsMaximize(true)
		.IsTopmostWindow(false)
		[
			HelpDialog
		];

	FSlateApplication::Get().AddWindow(HelpWindow);

	return FReply::Handled();
}

FReply SMCPToolboxWidget::OnStartMCPServer()
{
	if (FMCPToolboxModule::StartMCPServer(false))
	{
		if (StatusBarText.IsValid())
		{
			StatusBarText->SetText(LOCTEXT("MCPStarted", "MCP服务器已启动 — 正在连接..."));
			StatusBarText->SetColorAndOpacity(FLinearColor(1.0f, 0.8f, 0.2f));
		}

		// Auto-connect JSON-RPC client
		if (ChatWidget.IsValid())
		{
			ChatWidget->ConnectToMCPServer();
		}

		FNotificationInfo Info(LOCTEXT("MCPStartOK", "MCP服务器启动成功，正在连接..."));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	else
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[MCPToolbox] 无法启动MCP服务器 — 编辑器未就绪"));

		FNotificationInfo Info(LOCTEXT("MCPPluginFail", "MCP失败: 请确保 ModelContextProtocol 插件已启用，并重启编辑器"));
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
