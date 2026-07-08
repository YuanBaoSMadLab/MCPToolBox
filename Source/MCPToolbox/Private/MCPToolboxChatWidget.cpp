#include "MCPToolboxChatWidget.h"
#include "MCPToolbox.h"
#include "MCPToolboxAPIManager.h"
#include "MCPToolboxMemoryManager.h"
#include "MCPToolboxScreenshot.h"
#include "MCPToolboxAuxModelManager.h"
#include "MCPToolboxErrorCodes.h"
#include "MCPToolboxJsonValueHelper.h"
#include "MCPToolboxTransactionService.h"
#include "MCPToolboxPerformanceService.h"
#include "MCPToolboxServiceRegistry.h"
#include "Interfaces/IPluginManager.h"

// Assistant library — only FunctionBuilder/FunctionTable (no HTTP layer)
#include "assistant/function.hpp"

#include "Layout/Margin.h"
#include "Layout/Geometry.h"
#include "Input/Reply.h"
#include "Input/Events.h"
#include "Types/SlateEnums.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Styling/SlateBrush.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SWindow.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Base64.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DDynamic.h"
#include "HttpModule.h"
#include "Containers/Ticker.h"
#include "Async/Async.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
// Python infrastructure (移植自 VibeUE)
#include "MCPToolboxPythonExecutionService.h"
#include "MCPToolboxPythonDiscoveryService.h"

#define LOCTEXT_NAMESPACE "MCPToolboxChat"

// Forward declare helper
static void InjectSpeculationHint(TArray<TSharedPtr<FJsonValue>>& PendingMsgs, const FSpeculativeResult& Spec, bool& bSpeculationPending);

// ============================================================================
// Construction
// ============================================================================
void SMCPToolboxChatWidget::Construct(const FArguments& InArgs)
{
	Messages.Empty();
	bIsStreaming = false;
	bIsWaiting = false;

	// Restore persisted widget state (vision/sidebar/more-expanded) before any UI is built.
	// bVisionModeEnabled etc. are reset above; LoadWidgetState overrides them from disk.
	LoadWidgetState();

	// 阶段 A1: 注册 SkillService 状态变更回调 — ToggleSkillEnabled 后触发 SaveWidgetState
	// 解耦:SkillService 不依赖 widget,通过回调通知持久化
	SkillService.SetStateChangedCallback([this]() { this->SaveWidgetState(); bSystemPromptDirty = true; });

	// Load cached conversation summary (if user previously generated one) — injected into BuildSystemPrompt.
	{
		FString ToolsSum, MemSum;
		if (FMCPToolboxMemoryManager::Get().LoadConversationSummary(ToolsSum, MemSum))
		{
			CachedToolsSummary = MoveTemp(ToolsSum);
			CachedMemorySummary = MoveTemp(MemSum);
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Loaded conversation summary: tools=%d / memory=%d chars"),
				CachedToolsSummary.Len(), CachedMemorySummary.Len());
		}
	}

	// Refresh auxiliary model status and start llama-server
	FMCPToolboxAuxModelManager& AuxMgr = FMCPToolboxAuxModelManager::Get();
	AuxMgr.RefreshStatus();
	if (AuxMgr.GetStatus() == EMCPToolboxAuxModelStatus::Ready)
	{
		AuxMgr.StartServer();
	}

	ToolFunctionTable = MakeUnique<assistant::FunctionTable>();
	RegisterMCPTools();
	RefreshSessionList();

	TSharedRef<SScrollBox> ChatBox = BuildChatArea();
	TSharedRef<SWidget> Sidebar = BuildSessionSidebar();

	ChildSlot
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride_Lambda([this]() -> float
			{
				return bSidebarCollapsed ? 0.0f : 220.0f;
			})
			.Visibility_Lambda([this]() -> EVisibility
			{
				return bSidebarCollapsed ? EVisibility::Collapsed : EVisibility::Visible;
			})
			[
				Sidebar
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(4))
			[
				BuildToolbar()
			]

			+ SVerticalBox::Slot().FillHeight(1.0).Padding(FMargin(4))
			[
				ChatBox
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4, 0, 0))
			[
				BuildInputArea()
			]
		]
	];
}

TSharedRef<SScrollBox> SMCPToolboxChatWidget::BuildChatArea()
{
	SAssignNew(ChatScrollBox, SScrollBox);

	FMCPToolboxChatSession* CurrentSession = FMCPToolboxChatSessionManager::Get().GetCurrentSession();
	if (CurrentSession && CurrentSession->Messages.Num() > 0)
	{
		Messages = CurrentSession->Messages;
		// Defensive: clear stale bIsStreaming flags from any interrupted past session.
		// A streaming message saved with bIsStreaming=true would otherwise render as a
		// forever-streaming bubble after restart, which is one cause of the
		// "context inconsistent after editor restart" report.
		for (FMCPToolboxChatMessage& Msg : Messages)
		{
			Msg.bIsStreaming = false;
		}
	}
	else
	{
		FMCPToolboxChatMessage WelcomeMsg;
		WelcomeMsg.Role = EMCPToolboxMessageRole::Assistant;
		WelcomeMsg.Content = TEXT("**欢迎使用 MCP Toolbox！**\n\n"
			"- 先在 \"API密钥\" Tab 添加密钥（选择服务商→选模型→填密钥→添加）\n"
			"- 点击 \"启动MCP\" 启动MCP服务器\n"
			"- AI会自动调用 UE5 编辑器工具（截图、命令执行、Actor选择等）\n\n"
			"支持 22+ 国内外服务商。");
		Messages.Add(WelcomeMsg);
		if (CurrentSession)
		{
			UpdateCurrentSessionWithMessage(WelcomeMsg);
		}
	}
	RebuildChatDisplay();
	return ChatScrollBox.ToSharedRef();
}

TSharedRef<SWidget> SMCPToolboxChatWidget::BuildSessionSidebar()
{
	RefreshSessionList();

	TSharedPtr<SListView<TSharedPtr<FString>>> ListView;
	SAssignNew(ListView, SListView<TSharedPtr<FString>>)
		.ListItemsSource(&SessionOptions)
		.OnSelectionChanged(this, &SMCPToolboxChatWidget::OnSessionSelected)
		.OnGenerateRow(this, &SMCPToolboxChatWidget::GenerateSessionRow);
	SessionListView = ListView;

	TSharedRef<SWidget> NewChatBtn = SNew(SButton)
		.Text(LOCTEXT("NewChatBtn", "新对话"))
		.OnClicked(this, &SMCPToolboxChatWidget::OnNewChat)
		.HAlign(HAlign_Center);

	// (Removed the in-sidebar ">>" collapse button — it duplicated the toolbar's
	// "显示/隐藏对话" toggle and, worse, disappeared along with the sidebar when
	// clicked, leaving no in-pane way to expand again. The toolbar button remains.)

	return SNew(SBox)
		.WidthOverride(220.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(4))
			[
				NewChatBtn
			]

			+ SVerticalBox::Slot().FillHeight(1.0).Padding(FMargin(2))
			[
				ListView.ToSharedRef()
			]
		];
}

TSharedRef<ITableRow> SMCPToolboxChatWidget::GenerateSessionRow(TSharedPtr<FString> SessionId, const TSharedRef<STableViewBase>& OwnerTable)
{
	FString* TitlePtr = SessionIdToTitle.Find(*SessionId);
	FString* PreviewPtr = SessionIdToPreview.Find(*SessionId);
	FString DisplayTitle = TitlePtr ? *TitlePtr : TEXT("未知");
	FString DisplayPreview = PreviewPtr ? *PreviewPtr : TEXT("");

	FMCPToolboxChatSession* CurrentSession = FMCPToolboxChatSessionManager::Get().GetCurrentSession();
	bool bIsSelected = CurrentSession && CurrentSession->SessionId == *SessionId;

	FLinearColor BgColor = bIsSelected ? FLinearColor(0.25f, 0.4f, 0.7f) : FLinearColor(0.1f, 0.1f, 0.12f);

	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
		.Padding(FMargin(0))
		[
			SNew(SBorder)
			.BorderBackgroundColor(BgColor)
			.Padding(FMargin(4, 2))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().FillWidth(1.0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(DisplayTitle))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
						.ColorAndOpacity(bIsSelected ? FLinearColor::White : FLinearColor(0.8f, 0.8f, 0.8f))
						.AutoWrapText(true)
					]

					+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
					[
						SNew(SButton)
						.Text(LOCTEXT("DeleteSession", "×"))
						.OnClicked_Lambda([this, SessionId]() {
							OnDeleteSession(*SessionId);
							return FReply::Handled();
						})
						.ButtonColorAndOpacity(FLinearColor(0.3f, 0.15f, 0.15f))
						.HAlign(HAlign_Center)
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 2, 0, 0))
				[
					SNew(STextBlock)
					.Text(FText::FromString(DisplayPreview))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f))
					.AutoWrapText(true)
				]
			]
		];
}

void SMCPToolboxChatWidget::RebuildChatDisplay()
{
	if (!ChatScrollBox.IsValid()) return;
	ChatScrollBox->ClearChildren();
	for (const auto& Msg : Messages)
		ChatScrollBox->AddSlot().Padding(FMargin(4))[CreateMessageBubble(Msg)];
	ChatScrollBox->ScrollToEnd();
}

void SMCPToolboxChatWidget::AddMessage(const FMCPToolboxChatMessage& Message)
{
	Messages.Add(Message);
	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->AddSlot().Padding(FMargin(4))[CreateMessageBubble(Message)];
		ChatScrollBox->ScrollToEnd();
	}
	
	UpdateCurrentSessionWithMessage(Message);
}

// ============================================================================
// Message Bubble - selectable, readable
// ============================================================================
TSharedRef<SWidget> SMCPToolboxChatWidget::CreateMessageBubble(const FMCPToolboxChatMessage& Message)
{
	TSharedPtr<SMultiLineEditableTextBox> Dummy;
	return CreateMessageBubble(Message, Dummy);
}

TSharedRef<SWidget> SMCPToolboxChatWidget::CreateMessageBubble(const FMCPToolboxChatMessage& Message, TSharedPtr<SMultiLineEditableTextBox>& OutTextBlock)
{
	FLinearColor BgColor = GetMessageColor(Message.Role);
	FString RoleLabel;
	bool bAlignRight = false;

	switch (Message.Role)
	{
	case EMCPToolboxMessageRole::User:      RoleLabel = TEXT("你"); bAlignRight = true; break;
	case EMCPToolboxMessageRole::Assistant:  RoleLabel = TEXT("AI助手"); break;
	case EMCPToolboxMessageRole::System:     RoleLabel = TEXT("系统"); break;
	case EMCPToolboxMessageRole::Thinking:   RoleLabel = TEXT("思考中"); break;
	}

	// Clean markdown for display
	FString DisplayContent = CleanMarkdownToText(Message.Content);

	// --- Tool execution card (Thinking or System with result) ---
	if (Message.Role == EMCPToolboxMessageRole::Thinking || 
		(Message.Role == EMCPToolboxMessageRole::System && Message.Content.Contains(TEXT("结果:"))))
	{
		bool bIsResult = Message.Role == EMCPToolboxMessageRole::System;
		if (DisplayContent.Len() > 500) DisplayContent = DisplayContent.Left(500) + TEXT("...");

		TSharedPtr<SMultiLineEditableTextBox> TextBlock;
		auto Result = SNew(SBox)
			.HAlign(HAlign_Left)
			[
				SNew(SBorder)
				.BorderBackgroundColor(FLinearColor(0.06f, 0.08f, 0.12f))
				.Padding(FMargin(8, 4))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(STextBlock)
							.Text(FText::FromString(bIsResult ? TEXT("[OK]") : TEXT("[...]")))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
							.ColorAndOpacity(bIsResult ? FLinearColor(0.3f, 0.85f, 0.4f) : FLinearColor(1.0f, 0.8f, 0.2f))
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(6, 0, 0, 0))
						[
							SNew(STextBlock)
							.Text(FText::FromString(bIsResult ? TEXT("工具结果") : TEXT("工具调用")))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
							.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f))
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
						[
							SNew(STextBlock)
							.Text(FText::FromString(Message.Timestamp.ToString(TEXT("%H:%M:%S"))))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
							.ColorAndOpacity(FLinearColor(0.3f, 0.3f, 0.3f))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4, 0, 0))
					[
						SAssignNew(TextBlock, SMultiLineEditableTextBox)
						.Text(FText::FromString(DisplayContent))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.IsReadOnly(true)
						.AllowMultiLine(true)
						.AllowContextMenu(true)
						.AutoWrapText(true)
						.WrappingPolicy(ETextWrappingPolicy::DefaultWrapping)
						.BackgroundColor(FLinearColor::Transparent)
					]
				]
			];
		OutTextBlock = TextBlock;
		return Result;
	}

	TSharedPtr<SMultiLineEditableTextBox> TextBlock;
	auto Result = SNew(SBox)
		.HAlign(bAlignRight ? HAlign_Right : HAlign_Left)
		[
			SNew(SBorder)
			.BorderBackgroundColor(BgColor)
			.Padding(FMargin(10, 6))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock)
						.Text(FText::FromString(RoleLabel))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(10, 0, 0, 0))
					[
						SNew(STextBlock)
						.Text(FText::FromString(Message.Timestamp.ToString(TEXT("%H:%M"))))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
						.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f))
					]
				]
				// Image attachments preview
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 4, 0, 0))
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([MsgCopy = Message]() -> EVisibility {
					return (MsgCopy.bHasImageAttachment && MsgCopy.ImageDataURIs.Num() > 0)
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox)
					.WidthOverride(300.0f)
					[
						SNew(SImage)
						.Image_Lambda([MsgCopy = Message]() -> const FSlateBrush*
						{
							static TMap<FString, TSharedPtr<FSlateDynamicImageBrush>> ImageBrushCache;
							if (MsgCopy.ImageDataURIs.Num() > 0)
							{
								FString ImageURI = MsgCopy.ImageDataURIs[0];
								FString CacheKey = ImageURI;
								FString Base64Data;

								if (ImageURI.StartsWith(TEXT("data:image/")))
								{
									int32 CommaIdx = ImageURI.Find(TEXT(","));
									if (CommaIdx != INDEX_NONE)
										Base64Data = ImageURI.RightChop(CommaIdx + 1);
								}
								else if (ImageURI.Len() > 100 && ImageURI.MatchesWildcard(TEXT("*[A-Za-z0-9+/=]*")) && !ImageURI.Contains(TEXT("://")))
								{
									bool bIsBase64 = true;
									for (TCHAR C : ImageURI)
									{
										if (!FCString::Strchr(TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="), C))
										{
											bIsBase64 = false;
											break;
										}
									}
									if (bIsBase64)
									{
										Base64Data = ImageURI;
									}
								}

								if (!Base64Data.IsEmpty())
								{
									if (!ImageBrushCache.Contains(CacheKey))
									{
										TArray<uint8> RawData;
										if (FBase64::Decode(Base64Data, RawData))
										{
											FString TempPath = FPaths::ProjectSavedDir() / TEXT("MCPToolbox") / TEXT("TempImages");
											IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
											PlatformFile.CreateDirectoryTree(*TempPath);
											FString TempFileName = FString::Printf(TEXT("img_%d.png"), FDateTime::Now().GetTicks());
											FString TempFilePath = TempPath / TempFileName;
											if (FFileHelper::SaveArrayToFile(RawData, *TempFilePath))
											{
												FString FullPath = FPaths::ConvertRelativePathToFull(TempFilePath);
												TSharedPtr<FSlateDynamicImageBrush> Brush = MakeShareable(new FSlateDynamicImageBrush(
													FName(*FullPath),
													FVector2D(400, 400)));
												ImageBrushCache.Add(CacheKey, Brush);
											}
										}
									}
									return ImageBrushCache[CacheKey].Get();
								}
								else if (ImageURI.StartsWith(TEXT("file://")))
								{
									FString FilePath = ImageURI.RightChop(7);
									if (!ImageBrushCache.Contains(CacheKey))
									{
										TSharedPtr<FSlateDynamicImageBrush> Brush = MakeShareable(new FSlateDynamicImageBrush(
											FName(*FilePath),
											FVector2D(400, 400)));
										ImageBrushCache.Add(CacheKey, Brush);
									}
									return ImageBrushCache[CacheKey].Get();
								}
							}
							return nullptr;
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([MsgCopy = Message]() -> EVisibility {
						return (MsgCopy.ImageFileNames.Num() > 0)
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text_Lambda([MsgCopy = Message]() -> FText
					{
						FString Names;
						for (int32 i = 0; i < MsgCopy.ImageFileNames.Num(); ++i)
						{
							if (i > 0) Names += TEXT(", ");
							Names += MsgCopy.ImageFileNames[i];
						}
						return FText::FromString(FString::Printf(TEXT("[图片: %s]"), *Names));
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
					.ColorAndOpacity(FLinearColor(0.5f, 0.7f, 1.0f))
				]
			]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 6, 0, 0))
				[
					SAssignNew(TextBlock, SMultiLineEditableTextBox)
					.Text(FText::FromString(DisplayContent))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.IsReadOnly(true)
					.AllowMultiLine(true)
					.AllowContextMenu(true)
					.AutoWrapText(true)
					.WrappingPolicy(ETextWrappingPolicy::DefaultWrapping)
					.BackgroundColor(FLinearColor::Transparent)
				]
			]
		];
	OutTextBlock = TextBlock;
	return Result;
}

// ============================================================================
// Input Area
// ============================================================================
TSharedRef<SWidget> SMCPToolboxChatWidget::BuildInputArea()
{
	// ponytail: 单行布局 — 输入框 + 所有操作按钮同一行
	// 原 Row 2(优化/退回/上传/清空)合并到输入行,2 行 → 1 行
	return SNew(SBorder).Padding(FMargin(4))
		[
			SNew(SHorizontalBox)
			// 输入框(占满宽度)
			+ SHorizontalBox::Slot().FillWidth(1.0).VAlign(VAlign_Center).Padding(FMargin(0, 0, 4, 0))
			[
				SAssignNew(InputTextBox, SMultiLineEditableTextBox)
				.HintText(LOCTEXT("InputHint", "输入消息... 回车发送, Shift+回车换行"))
				.OnKeyDownHandler(this, &SMCPToolboxChatWidget::OnInputKeyDown)
			]
			// 优化按钮
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 2, 0))
			[
				SNew(SButton)
				.Text_Lambda([this]() {
					if (bOptimizingPrompt) return LOCTEXT("OptimizingBtn", "优化中...");
					return LOCTEXT("OptimizeBtn", "优化");
				})
				.OnClicked(this, &SMCPToolboxChatWidget::OnOptimizePrompt)
				.IsEnabled_Lambda([this]() {
				return !bIsWaiting && !bOptimizingPrompt;
			})
			.ToolTipText_Lambda([this]() {
				return LOCTEXT("OptimizeTooltipRemote", "用远程大模型优化提示词(去冗余+明确意图)");
			})
				.ButtonColorAndOpacity_Lambda([this]() {
				if (bOptimizingPrompt) return FLinearColor(0.4f, 0.5f, 0.9f);
				return FLinearColor(0.3f, 0.15f, 0.5f);
			})
				.ContentPadding(FMargin(6, 2))
			]
			// 退回按钮(仅 bPromptOptimized 时显示)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 2, 0))
			[
				SNew(SButton)
				.Text(LOCTEXT("UndoBtn", "退回"))
				.OnClicked(this, &SMCPToolboxChatWidget::OnUndoOptimization)
				.IsEnabled_Lambda([this]() { return bPromptOptimized && !bOptimizingPrompt; })
				.ToolTipText(LOCTEXT("UndoTooltip", "撤销优化，恢复原始提示词"))
				.Visibility_Lambda([this]() { return bPromptOptimized ? EVisibility::Visible : EVisibility::Collapsed; })
				.ButtonColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.2f))
				.ContentPadding(FMargin(6, 2))
			]
			// 上传按钮
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 2, 0))
			[
				SNew(SButton)
				.Text(LOCTEXT("UploadBtn", "上传"))
				.OnClicked(this, &SMCPToolboxChatWidget::OnUploadFile)
				.IsEnabled_Lambda([this]() {
					return bVisionModeEnabled || FMCPToolboxAuxModelManager::Get().IsReady();
				})
				.ToolTipText_Lambda([this]() {
					if (bVisionModeEnabled)
						return LOCTEXT("UploadTooltip", "上传图片 (云端视觉)");
					if (FMCPToolboxAuxModelManager::Get().IsReady())
						return LOCTEXT("UploadVLMode", "上传图片 (本地 VL 分析)");
					return LOCTEXT("UploadDisabledTooltip", "请先开启视觉模式或确保辅助模型可用");
				})
				.ButtonColorAndOpacity_Lambda([this]() {
					return (bVisionModeEnabled || FMCPToolboxAuxModelManager::Get().IsReady())
						? FLinearColor::White : FLinearColor(0.3f, 0.3f, 0.3f);
				})
				.ContentPadding(FMargin(6, 2))
			]
			// 清空按钮
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 2, 0))
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearBtn", "清空"))
				.OnClicked(this, &SMCPToolboxChatWidget::OnClearChat)
				.ContentPadding(FMargin(6, 2))
			]
			// 发送 / 停止按钮
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text_Lambda([this]() { return bIsWaiting ? LOCTEXT("StopBtn", "停止") : LOCTEXT("SendBtn", "发送"); })
				.OnClicked_Lambda([this]() { return bIsWaiting ? OnInterrupt() : OnSendMessage(); })
				.ButtonColorAndOpacity_Lambda([this]() { return bIsWaiting ? FLinearColor(0.8f, 0.2f, 0.2f) : FLinearColor::White; })
				.ContentPadding(FMargin(8, 4))
			]
		];
}



// ============================================================================
// Send Message — builds messages, then delegates to SendAIRequest
// ============================================================================
FReply SMCPToolboxChatWidget::OnSendMessage()
{
	if (!InputTextBox.IsValid() || bIsWaiting) return FReply::Handled();

	// Reset pseudo-tool-call retry counter at the start of each user turn.
	PseudoToolCallRetries = 0;

	FString UserText = InputTextBox->GetText().ToString().TrimStartAndEnd();
	if (UserText.IsEmpty()) return FReply::Handled();

	const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
	if (!ActiveEntry)
	{
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = TEXT("请先在 \"API密钥\" Tab中添加API密钥。\n\n操作步骤：选择服务商 → 选择模型 → 填写密钥 → 点击\"添加\"。");
		AddMessage(Err);
		return FReply::Handled();
	}

	bool bModelSupportsVision = ActiveEntry->ModelId.Contains(TEXT("gpt-4o")) ||
		ActiveEntry->ModelId.Contains(TEXT("claude-3")) || ActiveEntry->ModelId.Contains(TEXT("claude-4")) ||
		ActiveEntry->ModelId.Contains(TEXT("gemini")) ||
		ActiveEntry->ModelId.Contains(TEXT("vl")) || ActiveEntry->ModelId.Contains(TEXT("vision"));

	if (UserText.StartsWith(TEXT("/test_image")))
	{
		FString TestPrompt = UserText.Mid(11).TrimStartAndEnd();
		if (TestPrompt.IsEmpty())
			TestPrompt = TEXT("A cute little girl, about 5-6 years old, wearing a pretty floral dress, smiling while standing in a sunny flower field with butterflies around, soft warm lighting, high quality digital art style");

		FMCPToolboxChatMessage UserMsg;
		UserMsg.Role = EMCPToolboxMessageRole::User;
		UserMsg.Content = TEXT("[测试生图工具] " + TestPrompt);
		AddMessage(UserMsg);
		InputTextBox->SetText(FText::GetEmpty());

		bIsWaiting = true;

		FMCPToolboxChatMessage ToolMsg;
		ToolMsg.Role = EMCPToolboxMessageRole::Thinking;
		ToolMsg.Content = TEXT("调用工具: generate_image (异步执行)");
		AddMessage(ToolMsg);

		Async(EAsyncExecution::Thread,
			[this, TestPrompt]()
		{
			FString Result = GenerateImageSync(TestPrompt, TEXT(""), 512, 512, 20, 7.0f, TEXT("saved:/GeneratedImages/"));

			AsyncTask(ENamedThreads::GameThread,
				[this, Result]()
			{
				FMCPToolboxChatMessage ResultMsg;
				ResultMsg.Role = EMCPToolboxMessageRole::System;
				ResultMsg.Content = FString::Printf(TEXT("**generate_image** 结果:\n```\n%s\n```"), *Result.Left(2000));
				AddMessage(ResultMsg);

				if (Result.Contains(TEXT("\"status\":\"ok\"")))
				{
					TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
					TSharedPtr<FJsonObject> ResultObj;
					if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
					{
						FString ImageURL, ImageData, Status;
						ResultObj->TryGetStringField(TEXT("status"), Status);
						ResultObj->TryGetStringField(TEXT("image_url"), ImageURL);
						ResultObj->TryGetStringField(TEXT("image_data"), ImageData);

						FString DisplayURL = ImageURL.IsEmpty() ? ImageData : ImageURL;
						if (!DisplayURL.IsEmpty() && Status == TEXT("ok"))
						{
							FMCPToolboxChatMessage ImageMsg;
							ImageMsg.Role = EMCPToolboxMessageRole::Assistant;
							ImageMsg.Content = TEXT("**生图成功！**");
							ImageMsg.bHasImageAttachment = true;
							ImageMsg.ImageDataURIs.Add(DisplayURL);
							ImageMsg.ImageFileNames.Add(TEXT("generated.png"));
							AddMessage(ImageMsg);
						}
					}
				}

				bIsWaiting = false;
			});
		});

		return FReply::Handled();
	}

	FString ApiKey;
	FBase64::Decode(ActiveEntry->EncryptedKey, ApiKey);

	// Add user message to chat
	FMCPToolboxChatMessage UserMsg;
	UserMsg.Role = EMCPToolboxMessageRole::User;
	UserMsg.Content = UserText;
	// Attach any pending uploaded images to this user message, then clear the queue.
	if (PendingUploadURIs.Num() > 0)
	{
		UserMsg.bHasImageAttachment = true;
		UserMsg.ImageDataURIs = PendingUploadURIs;
		UserMsg.ImageFileNames = PendingUploadFileNames;
		PendingUploadURIs.Empty();
		PendingUploadFileNames.Empty();
	}
	AddMessage(UserMsg);
	InputTextBox->SetText(FText::GetEmpty());

	bIsWaiting = true;
	bInterrupted = false;
	ToolCallIteration = 0;
	ToolCallHistory.Empty();
	ConsecutiveSameToolCount = 0;

	// Image generation mode: route to dedicated image generation endpoint
	// Route when: (1) user explicitly enabled image generation mode, OR (2) active entry is an image generation model
	if (bImageGenerationMode || ActiveEntry->bIsImageGeneration)
	{
		SendImageGenerationRequest(UserText);
		return FReply::Handled();
	}

	// Build messages array for API — PRESERVE conversation history!
	// NOTE: UserMsg has already been pushed into Messages via AddMessage() above,
	// so iterating Messages naturally includes it. Do NOT append it again.
	TArray<TSharedPtr<FJsonValue>> Msgs;

	// ponytail: cache system prompt — only rebuild when memory/skills/rules change
	// BuildSystemPrompt reads three disk files (index.md, rules.md) which are expensive.
	FString MemoryCtx = FMCPToolboxMemoryManager::Get().BuildMemoryContext();
	if (bSystemPromptDirty || CachedSystemPromptMemoryKey != MemoryCtx)
	{
		CachedSystemPrompt = BuildSystemPrompt(MemoryCtx);
		CachedSystemPromptMemoryKey = MemoryCtx;
		bSystemPromptDirty = false;
	}
	TSharedPtr<FJsonObject> SysMsg = MakeShareable(new FJsonObject());
	SysMsg->SetStringField(TEXT("role"), TEXT("system"));
	SysMsg->SetStringField(TEXT("content"), CachedSystemPrompt);
	Msgs.Add(MakeShareable(new FJsonValueObject(SysMsg)));

	// Append conversation history (includes the user message just added above)
	for (int32 MsgIdx = 0; MsgIdx < Messages.Num(); ++MsgIdx)
	{
		const auto& Msg = Messages[MsgIdx];

		// Skip the welcome message — it's UI-only, not for LLM context
		if (Msg.Role == EMCPToolboxMessageRole::Assistant &&
			Msg.Content.StartsWith(TEXT("**欢迎使用 MCP Toolbox")))
			continue;

		// Skip assistant messages with image attachments — these are generated images
		// that have been displayed in UI. Non-vision models like DeepSeek don't support
		// image_url content type, and AI should describe the image in text instead.
		// ponytail: also skip User messages with bHasImageAttachment for non-vision models,
		// preventing generated-image content from leaking into follow-up requests.
		if (Msg.bHasImageAttachment && (Msg.Role == EMCPToolboxMessageRole::Assistant || !bModelSupportsVision))
			continue;

		TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject());
		FString RoleStr;
		switch (Msg.Role)
		{
		case EMCPToolboxMessageRole::User: RoleStr = TEXT("user"); break;
		case EMCPToolboxMessageRole::Assistant: RoleStr = TEXT("assistant"); break;
		case EMCPToolboxMessageRole::System: RoleStr = TEXT("system"); break;
		case EMCPToolboxMessageRole::Thinking: RoleStr = TEXT("assistant"); break;
		default: RoleStr = TEXT("user");
		}
		MsgObj->SetStringField(TEXT("role"), RoleStr);

		// ponytail: image-to-text conversion — if this image has been analyzed
		// (followed by an assistant message), replace base64 with text marker
		// to avoid token waste and model-switching issues
		bool bImageAlreadyAnalyzed = false;
		if (Msg.bHasImageAttachment && Msg.ImageDataURIs.Num() > 0 && Msg.Role == EMCPToolboxMessageRole::User)
		{
			for (int32 j = MsgIdx + 1; j < Messages.Num(); ++j)
			{
				if (Messages[j].Role == EMCPToolboxMessageRole::Assistant &&
					!Messages[j].Content.IsEmpty())
				{
					bImageAlreadyAnalyzed = true;
					break;
				}
			}
		}

		if (Msg.bHasImageAttachment && Msg.ImageDataURIs.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ContentArray;

			if (bImageAlreadyAnalyzed)
			{
				// Replace image with text note — safe for non-vision models
				FString TextContent = Msg.Content;
				TextContent += TEXT("\n\n[图片已由AI分析");
				if (Msg.ImageFileNames.Num() > 0)
				{
					TextContent += TEXT(": ");
					for (int32 i = 0; i < Msg.ImageFileNames.Num(); ++i)
					{
						if (i > 0) TextContent += TEXT(", ");
						TextContent += Msg.ImageFileNames[i];
					}
				}
				TextContent += TEXT("]");

				TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
				TextPart->SetStringField(TEXT("type"), TEXT("text"));
				TextPart->SetStringField(TEXT("text"), TextContent);
				ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));
			}
			else
			{
				TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
				TextPart->SetStringField(TEXT("type"), TEXT("text"));
				TextPart->SetStringField(TEXT("text"), Msg.Content);
				ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));

				for (const FString& ImageURI : Msg.ImageDataURIs)
				{
					TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject());
					ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));

					TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject());
					ImageUrlObj->SetStringField(TEXT("url"), ImageURI);
					ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);

					ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));
				}
			}

			MsgObj->SetArrayField(TEXT("content"), ContentArray);
		}
		else
		{
			MsgObj->SetStringField(TEXT("content"), Msg.Content);
		}

		// DeepSeek thinking mode: round-trip reasoning_content for assistant messages.
		// We always attach the field (even if empty) so the API sees a consistent shape.
		// This prevents HTTP 400 "The reasoning_content in the thinking mode must be passed back".
		if (RoleStr == TEXT("assistant"))
		{
			MsgObj->SetStringField(TEXT("reasoning_content"), Msg.ReasoningContent);
		}

		Msgs.Add(MakeShareable(new FJsonValueObject(MsgObj)));
	}

	SendAIRequest(Msgs);
	return FReply::Handled();
}

// ============================================================================
// Image Generation Request
// ============================================================================
void SMCPToolboxChatWidget::SendImageGenerationRequest(const FString& Prompt)
{
	if (bInterrupted) { bIsWaiting = false; return; }

	const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
	if (!ActiveEntry) { bIsWaiting = false; return; }

	FString ApiKey;
	FBase64::Decode(ActiveEntry->EncryptedKey, ApiKey);

	FString ApiUrl = ActiveEntry->BaseURL;
	FString ModelId = ActiveEntry->ModelId;

	// Provider-specific URL construction
	FString Endpoint = TEXT("");
	if (ActiveEntry->ProviderId == TEXT("openai-image"))
	{
		Endpoint = ApiUrl + TEXT("/images/generations");
	}
	else if (ActiveEntry->ProviderId == TEXT("sd-local"))
	{
		Endpoint = ApiUrl + TEXT("/sdapi/v1/txt2img");
	}
	else if (ActiveEntry->ProviderId == TEXT("comfyui"))
	{
		Endpoint = ApiUrl + TEXT("/prompt");
	}
	else if (ActiveEntry->ProviderId == TEXT("replicate-img"))
	{
		Endpoint = ApiUrl + TEXT("/predictions");
	}
	else if (ActiveEntry->ProviderId == TEXT("pollinations"))
	{
		Endpoint = ApiUrl + TEXT("/api/text2image");
	}
	else
	{
		bIsWaiting = false;
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = FString::Printf(TEXT("不支持的生图服务商: %s"), *ActiveEntry->ProviderName);
		AddMessage(Err);
		return;
	}

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());

	if (ActiveEntry->ProviderId == TEXT("openai-image"))
	{
		Body->SetStringField(TEXT("model"), ModelId);
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("n"), TEXT("1"));
		Body->SetStringField(TEXT("size"), TEXT("1024x1024"));
	}
	else if (ActiveEntry->ProviderId == TEXT("sd-local"))
	{
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("negative_prompt"), TEXT(""));
		Body->SetNumberField(TEXT("steps"), 20);
		Body->SetNumberField(TEXT("width"), 1024);
		Body->SetNumberField(TEXT("height"), 1024);
		Body->SetNumberField(TEXT("cfg_scale"), 7.0);
	}
	else if (ActiveEntry->ProviderId == TEXT("comfyui"))
	{
		FString PromptId = FGuid::NewGuid().ToString();
		Body->SetStringField(TEXT("prompt_id"), PromptId);
		
		TSharedPtr<FJsonObject> Workflow = MakeShareable(new FJsonObject());
		
		TSharedPtr<FJsonObject> CheckpointLoader = MakeShareable(new FJsonObject());
		FString CkptName = ModelId.Contains(TEXT("flux")) ? TEXT("flux-schnell.safetensors") : (ModelId.Contains(TEXT("sdxl")) ? TEXT("sd_xl_base_1.0.safetensors") : TEXT("v1-5-pruned-emaonly.safetensors"));
		FString CheckpointInputs = FString::Printf(TEXT("{\"ckpt_name\":\"%s\"}"), *CkptName);
		CheckpointLoader->SetStringField(TEXT("inputs"), CheckpointInputs);
		CheckpointLoader->SetStringField(TEXT("class_type"), TEXT("CheckpointLoaderSimple"));
		Workflow->SetObjectField(TEXT("1"), CheckpointLoader);
		
		TSharedPtr<FJsonObject> CLIPTextEncode = MakeShareable(new FJsonObject());
		FString CLIPInputs = FString::Printf(TEXT("{\"text\":\"%s\",\"clip\":[\"1\",0]}"), *Prompt);
		CLIPTextEncode->SetStringField(TEXT("inputs"), CLIPInputs);
		CLIPTextEncode->SetStringField(TEXT("class_type"), TEXT("CLIPTextEncode"));
		Workflow->SetObjectField(TEXT("2"), CLIPTextEncode);
		
		TSharedPtr<FJsonObject> CLIPTextEncodeNeg = MakeShareable(new FJsonObject());
		CLIPTextEncodeNeg->SetStringField(TEXT("inputs"), TEXT("{\"text\":\"\",\"clip\":[\"1\",0]}"));
		CLIPTextEncodeNeg->SetStringField(TEXT("class_type"), TEXT("CLIPTextEncode"));
		Workflow->SetObjectField(TEXT("3"), CLIPTextEncodeNeg);
		
		TSharedPtr<FJsonObject> KSampler = MakeShareable(new FJsonObject());
		KSampler->SetStringField(TEXT("inputs"), TEXT("{\"seed\":-1,\"steps\":20,\"cfg\":7.0,\"sampler_name\":\"euler\",\"scheduler\":\"normal\",\"denoise\":1.0,\"model\":[\"1\",0],\"positive\":[\"2\",0],\"negative\":[\"3\",0],\"latent_image\":[\"5\",0]}"));
		KSampler->SetStringField(TEXT("class_type"), TEXT("KSampler"));
		Workflow->SetObjectField(TEXT("4"), KSampler);
		
		TSharedPtr<FJsonObject> EmptyLatentImage = MakeShareable(new FJsonObject());
		EmptyLatentImage->SetStringField(TEXT("inputs"), TEXT("{\"width\":1024,\"height\":1024,\"batch_size\":1}"));
		EmptyLatentImage->SetStringField(TEXT("class_type"), TEXT("EmptyLatentImage"));
		Workflow->SetObjectField(TEXT("5"), EmptyLatentImage);
		
		TSharedPtr<FJsonObject> VAEDecode = MakeShareable(new FJsonObject());
		VAEDecode->SetStringField(TEXT("inputs"), TEXT("{\"samples\":[\"4\",0],\"vae\":[\"1\",0]}"));
		VAEDecode->SetStringField(TEXT("class_type"), TEXT("VAEDecode"));
		Workflow->SetObjectField(TEXT("6"), VAEDecode);
		
		TSharedPtr<FJsonObject> SaveImage = MakeShareable(new FJsonObject());
		SaveImage->SetStringField(TEXT("inputs"), TEXT("{\"filename_prefix\":\"mcp_gen\",\"images\":[\"6\",0]}"));
		SaveImage->SetStringField(TEXT("class_type"), TEXT("SaveImage"));
		Workflow->SetObjectField(TEXT("7"), SaveImage);
		
		Body->SetObjectField(TEXT("prompt"), Workflow);
	}
	else if (ActiveEntry->ProviderId == TEXT("replicate-img"))
	{
		FString Version;
		if (ModelId.Contains(TEXT("sdxl")))
			Version = TEXT("stability-ai/stable-diffusion-xl:7bf3a1b0cf148149d5270340caf046486809454464c6ab31258e875874c72071");
		else if (ModelId.Contains(TEXT("flux")))
			Version = TEXT("black-forest-labs/flux-schnell:5955196189e4c2e6b8e1c062977892415f132619925d2e658b7f997b60a92005");
		else
			Version = TEXT("stability-ai/stable-diffusion:ac732df83cea7fff18b8472768c88ad041fa750ff7682a21affe81863cbe77e4");

		Body->SetStringField(TEXT("version"), Version);

		TSharedPtr<FJsonObject> Input = MakeShareable(new FJsonObject());
		Input->SetStringField(TEXT("prompt"), Prompt);
		Body->SetObjectField(TEXT("input"), Input);
	}
	else if (ActiveEntry->ProviderId == TEXT("pollinations"))
	{
		Body->SetStringField(TEXT("prompt"), Prompt);
	}

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 生图请求: url=%s, model=%s, prompt=%s"), *Endpoint, *ModelId, *Prompt.Left(100));

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		if (ActiveEntry->ProviderId == TEXT("replicate-img"))
		{
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Token %s"), *ApiKey));
		}
		else
		{
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		}
	}
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(120.0f);

	FString PromptCopy = Prompt;
	FString ProviderIdCopy = ActiveEntry->ProviderId;
	Request->OnProcessRequestComplete().BindLambda(
		[this, PromptCopy, ProviderIdCopy](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (bInterrupted || !bIsWaiting) return;
			if (bSuccess && Resp.IsValid())
				HandleImageGenerationResponse(Resp, PromptCopy, ProviderIdCopy);
			else
			{
				bIsWaiting = false;
				FMCPToolboxChatMessage Err;
				Err.Role = EMCPToolboxMessageRole::System;
				Err.Content = TEXT("生图请求失败。请检查网络连接和API配置。");
				AddMessage(Err);
			}
			ActiveHttpRequest.Reset();
		});

	ActiveHttpRequest = Request;
	Request->ProcessRequest();
}

// ============================================================================
FString SMCPToolboxChatWidget::GenerateImage_WebUI(const FMCPToolboxAPIKeyEntry* Entry, const FString& Prompt, const FString& NegativePrompt, int32 Width, int32 Height, int32 Steps, float CfgScale, const FString& SavePath)
{
	FString ApiKey;
	FBase64::Decode(Entry->EncryptedKey, ApiKey);

	FString Endpoint = Entry->BaseURL + TEXT("/sdapi/v1/txt2img");
	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	Body->SetStringField(TEXT("prompt"), Prompt);
	Body->SetStringField(TEXT("negative_prompt"), NegativePrompt.IsEmpty() ? TEXT("") : NegativePrompt);
	Body->SetNumberField(TEXT("steps"), Steps);
	Body->SetNumberField(TEXT("width"), Width);
	Body->SetNumberField(TEXT("height"), Height);
	Body->SetNumberField(TEXT("cfg_scale"), CfgScale);

	if (!Entry->ModelId.IsEmpty())
	{
		TSharedPtr<FJsonObject> OverrideSettings = MakeShareable(new FJsonObject());
		OverrideSettings->SetStringField(TEXT("sd_model_checkpoint"), Entry->ModelId);
		Body->SetObjectField(TEXT("override_settings"), OverrideSettings);
	}

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(120.0f);

	bool bDone = false;
	FHttpResponsePtr Response;
	bool bSuccess = false;

	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
		{
			bDone = true;
			bSuccess = bSuc;
			Response = Resp;
		});

	Request->ProcessRequest();
	while (!bDone)
		FPlatformProcess::Sleep(0.01f);

	if (!bSuccess || !Response.IsValid())
		return TEXT("{\"error\":\"SD WebUI request failed.\"}");

	int32 Code = Response->GetResponseCode();
	FString RespBody = Response->GetContentAsString();

	if (Code != 200)
		return FString::Printf(TEXT("{\"error\":\"SD WebUI error (HTTP %d): %s\"}"), Code, *RespBody.Left(500));

	TSharedPtr<FJsonObject> Result;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
	if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
		return TEXT("{\"error\":\"Failed to parse SD WebUI response.\"}");

	const TArray<TSharedPtr<FJsonValue>>* Images;
	if (Result->TryGetArrayField(TEXT("images"), Images) && Images->Num() > 0)
	{
		FString ImageData;
		(*Images)[0]->TryGetString(ImageData);

		FString SavedPath;
		if (!SavePath.IsEmpty())
		{
			FString FinalPath = SavePath;
			if (FinalPath.StartsWith(TEXT("project:/")))
				FinalPath = FPaths::ProjectContentDir() / FinalPath.RightChop(9);
			else if (FinalPath.StartsWith(TEXT("saved:/")))
				FinalPath = FPaths::ProjectSavedDir() / FinalPath.RightChop(8);

			IPlatformFile& FileManager = FPlatformFileManager::Get().GetPlatformFile();
			FString Folder = FPaths::GetPath(FinalPath);
			if (!FileManager.DirectoryExists(*Folder))
				FileManager.CreateDirectoryTree(*Folder);

			FString Filename = FPaths::GetCleanFilename(FinalPath);
			if (Filename.IsEmpty() || FPaths::GetExtension(Filename).IsEmpty())
				Filename = FString::Printf(TEXT("gen_%lld.png"), FDateTime::Now().ToUnixTimestamp());

			FString FullPath = FPaths::Combine(Folder, Filename);

			TArray<uint8> RawData;
			if (FBase64::Decode(ImageData, RawData))
			{
				FFileHelper::SaveArrayToFile(RawData, *FullPath);
				SavedPath = FullPath;
			}
		}

		if (!SavedPath.IsEmpty())
			return FString::Printf(TEXT("{\"status\":\"ok\",\"image_data\":\"data:image/png;base64,%s\",\"saved_path\":\"%s\"}"), *ImageData, *SavedPath);
		return FString::Printf(TEXT("{\"status\":\"ok\",\"image_data\":\"data:image/png;base64,%s\"}"), *ImageData);
	}

	return TEXT("{\"error\":\"SD WebUI returned no images.\"}");
}

FString SMCPToolboxChatWidget::GenerateImage_ComfyUI(const FMCPToolboxAPIKeyEntry* Entry, const FString& Prompt, const FString& NegativePrompt, int32 Width, int32 Height, int32 Steps, float CfgScale, const FString& SavePath)
{
	// ponytail: auto-detect SD WebUI vs ComfyUI on the same port
	// Many users run AUTOMATIC1111 WebUI on the same port they intended for ComfyUI.
	// Probe /sdapi/v1/sd-models — if it responds, this is SD WebUI, not ComfyUI.
	{
		auto ProbeReq = FHttpModule::Get().CreateRequest();
		ProbeReq->SetURL(Entry->BaseURL + TEXT("/sdapi/v1/sd-models"));
		ProbeReq->SetVerb(TEXT("GET"));
		ProbeReq->SetTimeout(2.0f);
		bool bProbeDone = false; bool bProbeOk = false;
		ProbeReq->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr, FHttpResponsePtr R, bool b) { bProbeDone = true; bProbeOk = b && R.IsValid() && R->GetResponseCode() == 200; });
		ProbeReq->ProcessRequest();
		while (!bProbeDone) FPlatformProcess::Sleep(0.01f);
		if (bProbeOk)
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[ComfyUI] Auto-detected SD WebUI at %s — redirecting to WebUI path"), *Entry->BaseURL);
			return GenerateImage_WebUI(Entry, Prompt, NegativePrompt, Width, Height, Steps, CfgScale, SavePath);
		}
	}

	FString ApiKey;
	FBase64::Decode(Entry->EncryptedKey, ApiKey);

	FString Endpoint = Entry->BaseURL + TEXT("/prompt");
	FString PromptId = FGuid::NewGuid().ToString();

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	Body->SetStringField(TEXT("prompt_id"), PromptId);

	TSharedPtr<FJsonObject> Workflow;
	FString WorkflowPath = FPaths::ProjectSavedDir() / TEXT("ComfyUIWorkflows") / (Entry->ModelId + TEXT(".json"));
	if (FPaths::FileExists(WorkflowPath))
	{
		FString WorkflowContent;
		if (FFileHelper::LoadFileToString(WorkflowContent, *WorkflowPath))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WorkflowContent);
			if (FJsonSerializer::Deserialize(Reader, Workflow) && Workflow.IsValid())
			{
				// Find KSampler to identify which CLIPTextEncode nodes are positive vs negative
				FString PosNodeId, NegNodeId;
				for (auto& Pair : Workflow->Values)
				{
					TSharedPtr<FJsonObject> Node = Pair.Value->AsObject();
					if (!Node.IsValid()) continue;
					FString ClassType;
					Node->TryGetStringField(TEXT("class_type"), ClassType);
					if (ClassType == TEXT("KSampler") || ClassType == TEXT("KSamplerAdvanced"))
					{
						const TSharedPtr<FJsonObject>* InputsPtr;
						if (Node->TryGetObjectField(TEXT("inputs"), InputsPtr) && InputsPtr)
						{
							// "positive" input links to the positive CLIPTextEncode node
							const TArray<TSharedPtr<FJsonValue>>* PosLink;
							if ((*InputsPtr)->TryGetArrayField(TEXT("positive"), PosLink) && PosLink->Num() > 0)
						{
							const TArray<TSharedPtr<FJsonValue>>* LinkArr;
							if ((*PosLink)[0]->TryGetArray(LinkArr) && LinkArr->Num() > 0)
								(*LinkArr)[0]->TryGetString(PosNodeId);
						}
							// "negative" input links to the negative CLIPTextEncode node
							const TArray<TSharedPtr<FJsonValue>>* NegLink;
							if ((*InputsPtr)->TryGetArrayField(TEXT("negative"), NegLink) && NegLink->Num() > 0)
						{
							const TArray<TSharedPtr<FJsonValue>>* LinkArr;
							if ((*NegLink)[0]->TryGetArray(LinkArr) && LinkArr->Num() > 0)
								(*LinkArr)[0]->TryGetString(NegNodeId);
						}
						}
						break;
					}
				}

				// Inject prompts into identified CLIPTextEncode nodes
				int32 ClipEncodeCount = 0;
				for (auto& Pair : Workflow->Values)
				{
					TSharedPtr<FJsonObject> Node = Pair.Value->AsObject();
					if (!Node.IsValid()) continue;
					FString ClassType;
					Node->TryGetStringField(TEXT("class_type"), ClassType);
					if (ClassType.Contains(TEXT("CLIPTextEncode")))
					{
						const TSharedPtr<FJsonObject>* InputsPtr;
						if (Node->TryGetObjectField(TEXT("inputs"), InputsPtr) && InputsPtr && (*InputsPtr).IsValid())
						{
							TSharedPtr<FJsonObject> NewInputs = MakeShareable(new FJsonObject());
							*NewInputs = *(*InputsPtr);
							bool bIsNegative = NegNodeId == Pair.Key;
							NewInputs->SetStringField(TEXT("text"), bIsNegative
								? (NegativePrompt.IsEmpty() ? TEXT("") : NegativePrompt)
								: Prompt);
							Node->SetObjectField(TEXT("inputs"), NewInputs);
							ClipEncodeCount++;
						}
					}
					else if (ClassType == TEXT("EmptyLatentImage"))
					{
						const TSharedPtr<FJsonObject>* InputsPtr;
						if (Node->TryGetObjectField(TEXT("inputs"), InputsPtr) && InputsPtr && (*InputsPtr).IsValid())
						{
							TSharedPtr<FJsonObject> Inputs = *InputsPtr;
							Inputs->SetNumberField(TEXT("width"), Width);
							Inputs->SetNumberField(TEXT("height"), Height);
						}
					}
					else if (ClassType == TEXT("KSampler") || ClassType == TEXT("KSamplerAdvanced"))
					{
						const TSharedPtr<FJsonObject>* InputsPtr;
						if (Node->TryGetObjectField(TEXT("inputs"), InputsPtr) && InputsPtr && (*InputsPtr).IsValid())
						{
							TSharedPtr<FJsonObject> Inputs = *InputsPtr;
							Inputs->SetNumberField(TEXT("steps"), Steps);
							Inputs->SetNumberField(TEXT("cfg"), CfgScale);
						}
					}
				}
				UE_LOG(LogMCPToolbox, Log, TEXT("[ComfyUI] Workflow loaded: %s, CLIPTextEncode nodes: %d, positive=node_%s negative=node_%s, prompt=%s, size=%dx%d"),
					*WorkflowPath, ClipEncodeCount, *PosNodeId, *NegNodeId, *Prompt.Left(50), Width, Height);
				if (ClipEncodeCount == 0)
					UE_LOG(LogMCPToolbox, Warning, TEXT("[ComfyUI] No CLIPTextEncode nodes found in workflow! Prompt may not be injected. Check workflow JSON for correct class_type."));
			}
		}
	}

	if (!Workflow.IsValid())
	{
		Workflow = MakeShareable(new FJsonObject());

		TSharedPtr<FJsonObject> CheckpointLoader = MakeShareable(new FJsonObject());
		FString CkptName = Entry->ModelId.Contains(TEXT("flux")) ? TEXT("flux-schnell.safetensors") : (Entry->ModelId.Contains(TEXT("sdxl")) ? TEXT("sd_xl_base_1.0.safetensors") : TEXT("v1-5-pruned-emaonly.safetensors"));
		TSharedPtr<FJsonObject> CheckpointInputs = MakeShareable(new FJsonObject());
		CheckpointInputs->SetStringField(TEXT("ckpt_name"), CkptName);
		CheckpointLoader->SetObjectField(TEXT("inputs"), CheckpointInputs);
		CheckpointLoader->SetStringField(TEXT("class_type"), TEXT("CheckpointLoaderSimple"));
		Workflow->SetObjectField(TEXT("1"), CheckpointLoader);

		TSharedPtr<FJsonObject> CLIPTextEncode = MakeShareable(new FJsonObject());
		TSharedPtr<FJsonObject> CLIPInputs = MakeShareable(new FJsonObject());
		CLIPInputs->SetStringField(TEXT("text"), Prompt);
		TArray<TSharedPtr<FJsonValue>> ClipLink;
		ClipLink.Add(MakeShareable(new FJsonValueString(TEXT("1"))));
		ClipLink.Add(MakeShareable(new FJsonValueNumber(0)));
		CLIPInputs->SetArrayField(TEXT("clip"), ClipLink);
		CLIPTextEncode->SetObjectField(TEXT("inputs"), CLIPInputs);
		CLIPTextEncode->SetStringField(TEXT("class_type"), TEXT("CLIPTextEncode"));
		Workflow->SetObjectField(TEXT("2"), CLIPTextEncode);

		TSharedPtr<FJsonObject> CLIPTextEncodeNeg = MakeShareable(new FJsonObject());
		TSharedPtr<FJsonObject> CLIPNegInputs = MakeShareable(new FJsonObject());
		CLIPNegInputs->SetStringField(TEXT("text"), NegativePrompt.IsEmpty() ? TEXT("") : NegativePrompt);
		TArray<TSharedPtr<FJsonValue>> ClipNegLink;
		ClipNegLink.Add(MakeShareable(new FJsonValueString(TEXT("1"))));
		ClipNegLink.Add(MakeShareable(new FJsonValueNumber(0)));
		CLIPNegInputs->SetArrayField(TEXT("clip"), ClipNegLink);
		CLIPTextEncodeNeg->SetObjectField(TEXT("inputs"), CLIPNegInputs);
		CLIPTextEncodeNeg->SetStringField(TEXT("class_type"), TEXT("CLIPTextEncode"));
		Workflow->SetObjectField(TEXT("3"), CLIPTextEncodeNeg);

		TSharedPtr<FJsonObject> KSampler = MakeShareable(new FJsonObject());
		TSharedPtr<FJsonObject> KSamplerInputs = MakeShareable(new FJsonObject());
		KSamplerInputs->SetNumberField(TEXT("seed"), -1);
		KSamplerInputs->SetNumberField(TEXT("steps"), Steps);
		KSamplerInputs->SetNumberField(TEXT("cfg"), CfgScale);
		KSamplerInputs->SetStringField(TEXT("sampler_name"), TEXT("euler"));
		KSamplerInputs->SetStringField(TEXT("scheduler"), TEXT("normal"));
		KSamplerInputs->SetNumberField(TEXT("denoise"), 1.0);
		TArray<TSharedPtr<FJsonValue>> ModelLink, PositiveLink, NegativeLink, LatentLink;
		ModelLink.Add(MakeShareable(new FJsonValueString(TEXT("1"))));
		ModelLink.Add(MakeShareable(new FJsonValueNumber(0)));
		PositiveLink.Add(MakeShareable(new FJsonValueString(TEXT("2"))));
		PositiveLink.Add(MakeShareable(new FJsonValueNumber(0)));
		NegativeLink.Add(MakeShareable(new FJsonValueString(TEXT("3"))));
		NegativeLink.Add(MakeShareable(new FJsonValueNumber(0)));
		LatentLink.Add(MakeShareable(new FJsonValueString(TEXT("5"))));
		LatentLink.Add(MakeShareable(new FJsonValueNumber(0)));
		KSamplerInputs->SetArrayField(TEXT("model"), ModelLink);
		KSamplerInputs->SetArrayField(TEXT("positive"), PositiveLink);
		KSamplerInputs->SetArrayField(TEXT("negative"), NegativeLink);
		KSamplerInputs->SetArrayField(TEXT("latent_image"), LatentLink);
		KSampler->SetObjectField(TEXT("inputs"), KSamplerInputs);
		KSampler->SetStringField(TEXT("class_type"), TEXT("KSampler"));
		Workflow->SetObjectField(TEXT("4"), KSampler);

		TSharedPtr<FJsonObject> EmptyLatentImage = MakeShareable(new FJsonObject());
		TSharedPtr<FJsonObject> LatentInputs = MakeShareable(new FJsonObject());
		LatentInputs->SetNumberField(TEXT("width"), Width);
		LatentInputs->SetNumberField(TEXT("height"), Height);
		LatentInputs->SetNumberField(TEXT("batch_size"), 1);
		EmptyLatentImage->SetObjectField(TEXT("inputs"), LatentInputs);
		EmptyLatentImage->SetStringField(TEXT("class_type"), TEXT("EmptyLatentImage"));
		Workflow->SetObjectField(TEXT("5"), EmptyLatentImage);

		TSharedPtr<FJsonObject> VAEDecode = MakeShareable(new FJsonObject());
		TSharedPtr<FJsonObject> VAEDecodeInputs = MakeShareable(new FJsonObject());
		TArray<TSharedPtr<FJsonValue>> SamplesLink, VAELink;
		SamplesLink.Add(MakeShareable(new FJsonValueString(TEXT("4"))));
		SamplesLink.Add(MakeShareable(new FJsonValueNumber(0)));
		VAELink.Add(MakeShareable(new FJsonValueString(TEXT("1"))));
		VAELink.Add(MakeShareable(new FJsonValueNumber(0)));
		VAEDecodeInputs->SetArrayField(TEXT("samples"), SamplesLink);
		VAEDecodeInputs->SetArrayField(TEXT("vae"), VAELink);
		VAEDecode->SetObjectField(TEXT("inputs"), VAEDecodeInputs);
		VAEDecode->SetStringField(TEXT("class_type"), TEXT("VAEDecode"));
		Workflow->SetObjectField(TEXT("6"), VAEDecode);

		TSharedPtr<FJsonObject> SaveImage = MakeShareable(new FJsonObject());
		TSharedPtr<FJsonObject> SaveImageInputs = MakeShareable(new FJsonObject());
		SaveImageInputs->SetStringField(TEXT("filename_prefix"), TEXT("mcp_gen"));
		TArray<TSharedPtr<FJsonValue>> ImagesLink;
		ImagesLink.Add(MakeShareable(new FJsonValueString(TEXT("6"))));
		ImagesLink.Add(MakeShareable(new FJsonValueNumber(0)));
		SaveImageInputs->SetArrayField(TEXT("images"), ImagesLink);
		SaveImage->SetObjectField(TEXT("inputs"), SaveImageInputs);
		SaveImage->SetStringField(TEXT("class_type"), TEXT("SaveImage"));
		Workflow->SetObjectField(TEXT("7"), SaveImage);
	}

	Body->SetObjectField(TEXT("prompt"), Workflow);

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(120.0f);

	bool bDone = false;
	FHttpResponsePtr Response;
	bool bSuccess = false;

	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
		{
			bDone = true;
			bSuccess = bSuc;
			Response = Resp;
		});

	Request->ProcessRequest();
	while (!bDone)
		FPlatformProcess::Sleep(0.01f);

	if (!bSuccess || !Response.IsValid())
		return TEXT("{\"error\":\"ComfyUI request failed.\"}");

	int32 Code = Response->GetResponseCode();
	FString RespBody = Response->GetContentAsString();

	if (Code != 200)
		return FString::Printf(TEXT("{\"error\":\"ComfyUI error (HTTP %d): %s\"}"), Code, *RespBody.Left(500));

	TSharedPtr<FJsonObject> Result;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
	if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
		return TEXT("{\"error\":\"Failed to parse ComfyUI response.\"}");

	FString RespPromptId;
	if (!Result->TryGetStringField(TEXT("prompt_id"), RespPromptId) || RespPromptId.IsEmpty())
		return TEXT("{\"error\":\"ComfyUI did not return a prompt_id.\"}");

	FString HistoryUrl = Entry->BaseURL + TEXT("/history/") + RespPromptId;
	float PollTime = 0.0f;
	const float MaxPollTime = 120.0f;

	while (PollTime < MaxPollTime)
	{
		FPlatformProcess::Sleep(1.0f);
		PollTime += 1.0f;

		auto PollReq = FHttpModule::Get().CreateRequest();
		PollReq->SetURL(HistoryUrl);
		PollReq->SetVerb(TEXT("GET"));
		PollReq->SetTimeout(30.0f);

		bool bPollDone = false;
		FHttpResponsePtr PollResp;
		bool bPollSuccess = false;

		PollReq->OnProcessRequestComplete().BindLambda(
			[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
			{
				bPollDone = true;
				bPollSuccess = bSuc;
				PollResp = Resp;
			});

		PollReq->ProcessRequest();
		while (!bPollDone)
			FPlatformProcess::Sleep(0.01f);

		if (!bPollSuccess || !PollResp.IsValid() || PollResp->GetResponseCode() != 200)
			continue;

		FString HistoryBody = PollResp->GetContentAsString();
		TSharedPtr<FJsonObject> HistoryResult;
		TSharedRef<TJsonReader<>> HistoryReader = TJsonReaderFactory<>::Create(HistoryBody);
		if (!FJsonSerializer::Deserialize(HistoryReader, HistoryResult) || !HistoryResult.IsValid())
			continue;

		const TSharedPtr<FJsonObject>* Outputs;
		if (HistoryResult->TryGetObjectField(RespPromptId, Outputs) && Outputs && (*Outputs).IsValid())
		{
			const TSharedPtr<FJsonObject>* OutputNode;
			if ((*Outputs)->TryGetObjectField(TEXT("outputs"), OutputNode) && OutputNode && (*OutputNode).IsValid())
			{
				for (const auto& OutputPair : (*OutputNode)->Values)
				{
					TSharedPtr<FJsonObject> NodeObj = OutputPair.Value->AsObject();
					if (NodeObj.IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Images;
						if (NodeObj->TryGetArrayField(TEXT("images"), Images) && Images->Num() > 0)
						{
							TSharedPtr<FJsonObject> ImageObj = (*Images)[0]->AsObject();
							if (ImageObj.IsValid())
							{
								FString Filename;
								ImageObj->TryGetStringField(TEXT("filename"), Filename);
								if (!Filename.IsEmpty())
								{
									FString ImageURL = Entry->BaseURL + TEXT("/output/") + Filename;

									FString SavedPath;
									if (!SavePath.IsEmpty())
									{
										FString FinalPath = SavePath;
										if (FinalPath.StartsWith(TEXT("project:/")))
											FinalPath = FPaths::ProjectContentDir() / FinalPath.RightChop(9);
										else if (FinalPath.StartsWith(TEXT("saved:/")))
											FinalPath = FPaths::ProjectSavedDir() / FinalPath.RightChop(8);

										IPlatformFile& FileManager = FPlatformFileManager::Get().GetPlatformFile();
										FString Folder = FPaths::GetPath(FinalPath);
										if (!FileManager.DirectoryExists(*Folder))
											FileManager.CreateDirectoryTree(*Folder);

										FString DestFilename = FPaths::GetCleanFilename(FinalPath);
										if (DestFilename.IsEmpty() || FPaths::GetExtension(DestFilename).IsEmpty())
											DestFilename = Filename.IsEmpty()
												? FString::Printf(TEXT("gen_%lld.png"), FDateTime::Now().ToUnixTimestamp())
												: Filename;

										FString FullPath = FPaths::Combine(Folder, DestFilename);

										auto DownloadReq = FHttpModule::Get().CreateRequest();
										DownloadReq->SetURL(ImageURL);
										DownloadReq->SetVerb(TEXT("GET"));
										DownloadReq->SetTimeout(30.0f);

										bool bDownloadDone = false;
										FHttpResponsePtr DownloadResp;
										bool bDownloadSuccess = false;

										DownloadReq->OnProcessRequestComplete().BindLambda(
											[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
											{
												bDownloadDone = true;
												bDownloadSuccess = bSuc;
												DownloadResp = Resp;
											});

										DownloadReq->ProcessRequest();
										while (!bDownloadDone)
											FPlatformProcess::Sleep(0.01f);

										if (bDownloadSuccess && DownloadResp.IsValid() && DownloadResp->GetResponseCode() == 200)
										{
											FFileHelper::SaveArrayToFile(DownloadResp->GetContent(), *FullPath);
											SavedPath = FullPath;
										}
									}

									if (!SavedPath.IsEmpty())
										return FString::Printf(TEXT("{\"status\":\"ok\",\"image_url\":\"%s\",\"saved_path\":\"%s\"}"), *ImageURL, *SavedPath);
									return FString::Printf(TEXT("{\"status\":\"ok\",\"image_url\":\"%s\"}"), *ImageURL);
								}
							}
						}
					}
				}
			}
		}
	}

	return TEXT("{\"error\":\"ComfyUI generation timed out or no image was produced.\"}");
}

FString SMCPToolboxChatWidget::GenerateImage_MultimodalLLM(const FMCPToolboxAPIKeyEntry* Entry, const FString& Prompt, const FString& NegativePrompt, int32 Width, int32 Height, int32 Steps, float CfgScale, const FString& SavePath)
{
	FString ApiKey;
	FBase64::Decode(Entry->EncryptedKey, ApiKey);

	FString Endpoint;
	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());

	if (Entry->ProviderId == TEXT("openai-image"))
	{
		Endpoint = Entry->BaseURL + TEXT("/images/generations");
		Body->SetStringField(TEXT("model"), Entry->ModelId);
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("n"), TEXT("1"));
		FString Size = FString::Printf(TEXT("%dx%d"), Width, Height);
		Body->SetStringField(TEXT("size"), Size);
	}
	else if (Entry->ProviderId == TEXT("replicate-img"))
	{
		Endpoint = Entry->BaseURL + TEXT("/predictions");
		FString Version;
		if (Entry->ModelId.Contains(TEXT("sdxl")))
			Version = TEXT("stability-ai/stable-diffusion-xl:7bf3a1b0cf148149d5270340caf046486809454464c6ab31258e875874c72071");
		else if (Entry->ModelId.Contains(TEXT("flux")))
			Version = TEXT("black-forest-labs/flux-schnell:5955196189e4c2e6b8e1c062977892415f132619925d2e658b7f997b60a92005");
		else
			Version = TEXT("stability-ai/stable-diffusion:ac732df83cea7fff18b8472768c88ad041fa750ff7682a21affe81863cbe77e4");
		Body->SetStringField(TEXT("version"), Version);
		TSharedPtr<FJsonObject> Input = MakeShareable(new FJsonObject());
		Input->SetStringField(TEXT("prompt"), Prompt);
		Body->SetObjectField(TEXT("input"), Input);
	}
	else if (Entry->ProviderId == TEXT("pollinations"))
	{
		Endpoint = Entry->BaseURL + TEXT("/api/text2image");
		Body->SetStringField(TEXT("prompt"), Prompt);
	}
	else
	{
		return FString::Printf(TEXT("{\"error\":\"Unsupported multimodal provider: %s\"}"), *Entry->ProviderName);
	}

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		if (Entry->ProviderId == TEXT("replicate-img"))
			Request->SetHeader(TEXT("Authorization"), TEXT("Token ") + ApiKey);
		else
			Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
	}
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(120.0f);

	bool bDone = false;
	FHttpResponsePtr Response;
	bool bSuccess = false;

	Request->OnProcessRequestComplete().BindLambda(
		[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
		{
			bDone = true;
			bSuccess = bSuc;
			Response = Resp;
		});

	Request->ProcessRequest();
	while (!bDone)
		FPlatformProcess::Sleep(0.01f);

	if (!bSuccess || !Response.IsValid())
		return TEXT("{\"error\":\"Multimodal LLM request failed.\"}");

	int32 Code = Response->GetResponseCode();
	FString RespBody = Response->GetContentAsString();

	if (Code != 200)
		return FString::Printf(TEXT("{\"error\":\"Multimodal LLM error (HTTP %d): %s\"}"), Code, *RespBody.Left(500));

	TSharedPtr<FJsonObject> Result;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
	if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
		return TEXT("{\"error\":\"Failed to parse multimodal LLM response.\"}");

	FString ImageURL;

	if (Entry->ProviderId == TEXT("openai-image"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Data;
		if (Result->TryGetArrayField(TEXT("data"), Data) && Data->Num() > 0)
		{
			TSharedPtr<FJsonObject> ImageObj = (*Data)[0]->AsObject();
			if (ImageObj.IsValid())
				ImageObj->TryGetStringField(TEXT("url"), ImageURL);
		}
	}
	else if (Entry->ProviderId == TEXT("replicate-img"))
	{
		const TSharedPtr<FJsonObject>* OutputPtr;
		if (Result->TryGetObjectField(TEXT("output"), OutputPtr) && OutputPtr && (*OutputPtr).IsValid())
		{
			TSharedPtr<FJsonObject> Output = *OutputPtr;
			const TArray<TSharedPtr<FJsonValue>>* Images;
			if (Output->TryGetArrayField(TEXT(""), Images) && Images->Num() > 0)
				(*Images)[0]->TryGetString(ImageURL);
		}
	}
	else if (Entry->ProviderId == TEXT("pollinations"))
	{
		Result->TryGetStringField(TEXT("url"), ImageURL);
	}

	if (!ImageURL.IsEmpty())
	{
		FString SavedPath;
		if (!SavePath.IsEmpty())
		{
			FString FinalPath = SavePath;
			if (FinalPath.StartsWith(TEXT("project:/")))
				FinalPath = FPaths::ProjectContentDir() / FinalPath.RightChop(9);
			else if (FinalPath.StartsWith(TEXT("saved:/")))
				FinalPath = FPaths::ProjectSavedDir() / FinalPath.RightChop(8);

			IPlatformFile& FileManager = FPlatformFileManager::Get().GetPlatformFile();
			FString Folder = FPaths::GetPath(FinalPath);
			if (!FileManager.DirectoryExists(*Folder))
				FileManager.CreateDirectoryTree(*Folder);

			FString DestFilename = FPaths::GetCleanFilename(FinalPath);
			if (DestFilename.IsEmpty() || FPaths::GetExtension(DestFilename).IsEmpty())
				DestFilename = FString::Printf(TEXT("gen_%lld.png"), FDateTime::Now().ToUnixTimestamp());

			FString FullPath = FPaths::Combine(Folder, DestFilename);

			if (ImageURL.StartsWith(TEXT("data:")))
			{
				FString Base64Data = ImageURL.RightChop(ImageURL.Find(TEXT(",") + 1));
				TArray<uint8> RawData;
				if (FBase64::Decode(Base64Data, RawData))
				{
					FFileHelper::SaveArrayToFile(RawData, *FullPath);
					SavedPath = FullPath;
				}
			}
			else
			{
				auto DownloadReq = FHttpModule::Get().CreateRequest();
				DownloadReq->SetURL(ImageURL);
				DownloadReq->SetVerb(TEXT("GET"));
				DownloadReq->SetTimeout(30.0f);

				bool bDownloadDone = false;
				FHttpResponsePtr DownloadResp;
				bool bDownloadSuccess = false;

				DownloadReq->OnProcessRequestComplete().BindLambda(
					[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
					{
						bDownloadDone = true;
						bDownloadSuccess = bSuc;
						DownloadResp = Resp;
					});

				DownloadReq->ProcessRequest();
				while (!bDownloadDone)
					FPlatformProcess::Sleep(0.01f);

				if (bDownloadSuccess && DownloadResp.IsValid() && DownloadResp->GetResponseCode() == 200)
				{
					FFileHelper::SaveArrayToFile(DownloadResp->GetContent(), *FullPath);
					SavedPath = FullPath;
				}
			}
		}

		if (!SavedPath.IsEmpty())
		{
			if (ImageURL.StartsWith(TEXT("data:")))
				return FString::Printf(TEXT("{\"status\":\"ok\",\"image_data\":\"%s\",\"saved_path\":\"%s\"}"), *ImageURL, *SavedPath);
			return FString::Printf(TEXT("{\"status\":\"ok\",\"image_url\":\"%s\",\"saved_path\":\"%s\"}"), *ImageURL, *SavedPath);
		}
		if (ImageURL.StartsWith(TEXT("data:")))
			return FString::Printf(TEXT("{\"status\":\"ok\",\"image_data\":\"%s\"}"), *ImageURL);
		return FString::Printf(TEXT("{\"status\":\"ok\",\"image_url\":\"%s\"}"), *ImageURL);
	}

	return TEXT("{\"error\":\"Multimodal LLM returned no image URL.\"}");
}

// GenerateImageSync - Synchronous image generation for tool call
// ============================================================================
FString SMCPToolboxChatWidget::GenerateImageSync(const FString& Prompt, const FString& NegativePrompt, int32 Width, int32 Height, int32 Steps, float CfgScale, const FString& SavePath)
{
	TArray<const FMCPToolboxAPIKeyEntry*> ImgEntries = FMCPToolboxAPIManager::Get().GetImageGenerationEntries();
	if (ImgEntries.Num() == 0)
	{
		return TEXT("{\"error\":\"No image generation models configured. Please add image generation providers (DALL-E, SD WebUI, etc.) in API settings.\"}");
	}

	const FMCPToolboxAPIKeyEntry* Entry = ImgEntries[0];

	FString Result;
	switch (Entry->ImageGenType)
	{
	case EMCPToolboxImageGenType::WebUI:
		Result = GenerateImage_WebUI(Entry, Prompt, NegativePrompt, Width, Height, Steps, CfgScale, SavePath);
		break;
	case EMCPToolboxImageGenType::ComfyUI:
		Result = GenerateImage_ComfyUI(Entry, Prompt, NegativePrompt, Width, Height, Steps, CfgScale, SavePath);
		break;
	case EMCPToolboxImageGenType::MultimodalLLM:
		Result = GenerateImage_MultimodalLLM(Entry, Prompt, NegativePrompt, Width, Height, Steps, CfgScale, SavePath);
		break;
	default:
		return TEXT("{\"error\":\"Unsupported image generation type.\"}");
	}

	if (Result.Contains(TEXT("\"status\":\"ok\"")) && !SavePath.IsEmpty())
	{
		TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
		TSharedPtr<FJsonObject> ResultObj;
		if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
		{
			FString ImageData;
			ResultObj->TryGetStringField(TEXT("image_data"), ImageData);
			if (ImageData.StartsWith(TEXT("data:image/")))
			{
				FString Base64Data = ImageData.RightChop(ImageData.Find(TEXT(",") + 1));
				TArray<uint8> RawData;
				if (FBase64::Decode(Base64Data, RawData))
				{
					FString FinalPath = SavePath;
					if (FinalPath.StartsWith(TEXT("project:/")))
					{
						FinalPath = FPaths::ProjectContentDir() / FinalPath.RightChop(9);
					}
					else if (FinalPath.StartsWith(TEXT("saved:/")))
					{
						FinalPath = FPaths::ProjectSavedDir() / FinalPath.RightChop(8);
					}

					if (!FinalPath.EndsWith(TEXT("/")))
						FinalPath += TEXT("/");

					FString FileName = FString::Printf(TEXT("generated_%d.png"), FDateTime::Now().GetTicks());
					FString FullPath = FinalPath + FileName;

					IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
					FString Directory = FPaths::GetPath(FullPath);
					PlatformFile.CreateDirectoryTree(*Directory);

					if (FFileHelper::SaveArrayToFile(RawData, *FullPath))
					{
						ResultObj->SetStringField(TEXT("saved_path"), FullPath);
						FString UpdatedResult;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&UpdatedResult);
						FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
						return UpdatedResult;
					}
				}
			}
		}
	}

	return Result;
}

// ============================================================================
// Handle Image Generation Response
// ============================================================================
void SMCPToolboxChatWidget::HandleImageGenerationResponse(FHttpResponsePtr Resp, const FString& Prompt, const FString& ProviderId)
{
	if (bInterrupted) { bIsWaiting = false; return; }

	int32 Code = Resp->GetResponseCode();
	FString RespBody = Resp->GetContentAsString();

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 生图响应: HTTP %d, BodyLen=%d"), Code, RespBody.Len());

	if (Code != 200)
	{
		bIsWaiting = false;
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = FString::Printf(TEXT("生图服务错误 (HTTP %d):\n\n```\n%s\n```"), Code, *RespBody.Left(500));
		AddMessage(Err);
		return;
	}

	TSharedPtr<FJsonObject> Result;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
	if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
	{
		bIsWaiting = false;
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = TEXT("无法解析生图响应。");
		AddMessage(Err);
		return;
	}

	FString ImageURL;

	if (ProviderId == TEXT("openai-image"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Data;
		if (Result->TryGetArrayField(TEXT("data"), Data) && Data->Num() > 0)
		{
			TSharedPtr<FJsonObject> ImageObj = (*Data)[0]->AsObject();
			if (ImageObj.IsValid())
			{
				ImageObj->TryGetStringField(TEXT("url"), ImageURL);
			}
		}
	}
	else if (ProviderId == TEXT("sd-local"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Images;
		if (Result->TryGetArrayField(TEXT("images"), Images) && Images->Num() > 0)
		{
			(*Images)[0]->TryGetString(ImageURL);
		}
	}
	else if (ProviderId == TEXT("comfyui"))
	{
		FString PromptId;
		if (Result->TryGetStringField(TEXT("prompt_id"), PromptId) && !PromptId.IsEmpty())
		{
			const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
			if (ActiveEntry)
			{
				FString HistoryUrl = ActiveEntry->BaseURL + TEXT("/history/") + PromptId;
				FString BaseURL = ActiveEntry->BaseURL;
				FString PromptCopy = Prompt;
				
				FPlatformProcess::Sleep(2.0f);
				
				float PollTime = 0.0f;
				const float MaxPollTime = 120.0f;
				while (PollTime < MaxPollTime)
				{
					FPlatformProcess::Sleep(2.0f);
					PollTime += 2.0f;
					
					auto PollReq = FHttpModule::Get().CreateRequest();
					PollReq->SetURL(HistoryUrl);
					PollReq->SetVerb(TEXT("GET"));
					PollReq->SetTimeout(30.0f);
					
					bool bPollDone = false;
					FHttpResponsePtr PollResp;
					bool bPollSuccess = false;
					
					PollReq->OnProcessRequestComplete().BindLambda(
						[&](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSuc)
						{
							bPollDone = true;
							bPollSuccess = bSuc;
							PollResp = Resp;
						});
					
					PollReq->ProcessRequest();
					while (!bPollDone)
						FPlatformProcess::Sleep(0.01f);
					
					if (!bPollSuccess || !PollResp.IsValid() || PollResp->GetResponseCode() != 200)
						continue;
					
					FString HistoryBody = PollResp->GetContentAsString();
					TSharedPtr<FJsonObject> HistoryResult;
					TSharedRef<TJsonReader<>> HistoryReader = TJsonReaderFactory<>::Create(HistoryBody);
					if (!FJsonSerializer::Deserialize(HistoryReader, HistoryResult) || !HistoryResult.IsValid())
						continue;
					
					for (const auto& Pair : HistoryResult->Values)
					{
						TSharedPtr<FJsonObject> EntryObj = Pair.Value->AsObject();
						if (!EntryObj.IsValid()) continue;
						
						const TSharedPtr<FJsonObject>* Outputs;
						if (!EntryObj->TryGetObjectField(TEXT("outputs"), Outputs) || !Outputs || !(*Outputs).IsValid())
							continue;
						
						const TSharedPtr<FJsonObject>* SaveImageNode;
						if (!(*Outputs)->TryGetObjectField(TEXT("7"), SaveImageNode) || !SaveImageNode || !(*SaveImageNode).IsValid())
							continue;
						
						const TArray<TSharedPtr<FJsonValue>>* Images;
						if (!(*SaveImageNode)->TryGetArrayField(TEXT("images"), Images) || Images->Num() == 0)
							continue;
						
						TSharedPtr<FJsonObject> ImageObj = (*Images)[0]->AsObject();
						if (!ImageObj.IsValid()) continue;
						
						FString Filename;
						if (!ImageObj->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
							continue;
						
						FString GeneratedImageURL = BaseURL + TEXT("/output/") + Filename;
						
						FMCPToolboxChatMessage ImageMsg;
						ImageMsg.Role = EMCPToolboxMessageRole::Assistant;
						ImageMsg.Content = TEXT("**生图成功！**");
						ImageMsg.bHasImageAttachment = true;
						ImageMsg.ImageDataURIs.Add(GeneratedImageURL);
						ImageMsg.ImageFileNames.Add(Filename);
						AddMessage(ImageMsg);
						bIsWaiting = false;
						return;
					}
				}
				
				bIsWaiting = false;
				FMCPToolboxChatMessage Err;
				Err.Role = EMCPToolboxMessageRole::System;
				Err.Content = TEXT("ComfyUI 生图超时，未生成图片。");
				AddMessage(Err);
				return;
			}
		}
	}
	else if (ProviderId == TEXT("replicate-img"))
	{
		const TSharedPtr<FJsonObject>* OutputPtr;
		if (Result->TryGetObjectField(TEXT("output"), OutputPtr) && OutputPtr && (*OutputPtr).IsValid())
		{
			TSharedPtr<FJsonObject> Output = *OutputPtr;
			const TArray<TSharedPtr<FJsonValue>>* Images;
			if (Output->TryGetArrayField(TEXT(""), Images) && Images->Num() > 0)
			{
				(*Images)[0]->TryGetString(ImageURL);
			}
		}
	}
	else if (ProviderId == TEXT("pollinations"))
	{
		Result->TryGetStringField(TEXT("url"), ImageURL);
	}

	if (!ImageURL.IsEmpty())
	{
		FMCPToolboxChatMessage ImageMsg;
		ImageMsg.Role = EMCPToolboxMessageRole::Assistant;
		
		if (ImageURL.StartsWith(TEXT("data:image/")) || ImageURL.Len() > 1000)
		{
			TArray<uint8> RawData;
			FString Base64Data = ImageURL;
			if (Base64Data.StartsWith(TEXT("data:image/")))
			{
				int32 CommaIdx = Base64Data.Find(TEXT(","));
				if (CommaIdx != INDEX_NONE)
					Base64Data = Base64Data.RightChop(CommaIdx + 1);
			}

			if (FBase64::Decode(Base64Data, RawData))
			{
				FString TempDir = FPaths::ProjectSavedDir() / TEXT("MCPToolbox/GeneratedImages");
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				PlatformFile.CreateDirectoryTree(*TempDir);

				FString FileName = FString::Printf(TEXT("generated_%d.png"), FDateTime::Now().GetTicks());
				FString FullPath = TempDir / FileName;

				if (FFileHelper::SaveArrayToFile(RawData, *FullPath))
				{
					ImageMsg.Content = TEXT("**生图成功！**");
					ImageMsg.bHasImageAttachment = true;
					ImageMsg.ImageDataURIs.Add(TEXT("file://") + FullPath);
					ImageMsg.ImageFileNames.Add(FileName);
				}
				else
				{
					ImageMsg.Content = TEXT("**生图成功，但保存图片失败。**");
				}
			}
			else
			{
				ImageMsg.Content = TEXT("**生图成功，但无法解析图片数据。**");
			}
		}
		else
		{
			ImageMsg.Content = TEXT("**生图成功！**");
			ImageMsg.bHasImageAttachment = true;
			ImageMsg.ImageDataURIs.Add(ImageURL);
			ImageMsg.ImageFileNames.Add(TEXT("generated_image.png"));
		}

		AddMessage(ImageMsg);
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 生图成功"));
	}
	else
	{
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = TEXT("生图失败，未返回图片URL。");
		AddMessage(Err);
	}

	bIsWaiting = false;
}

// ============================================================================
// Send AI Request — reusable, called from OnSendMessage and tool loops
// ============================================================================
void SMCPToolboxChatWidget::SendAIRequest(const TArray<TSharedPtr<FJsonValue>>& ApiMessages)
{
	if (bInterrupted) { bIsWaiting = false; return; }

	// ── Determine if current model supports vision ──
	// Previously this was hardcoded to only openai/google/anthropic providers,
	// which (a) excluded vision-capable models from other providers (Qwen-VL,
	// GLM-4V, Yi-Vision, Moonshot, etc.) forcing them through the slower local
	// VL path, and (b) incorrectly flagged non-vision models from those providers
	// (e.g. gpt-3.5-turbo) as vision-capable, causing API 400 errors.
	// Now: check by model name pattern in addition to provider whitelist.
	const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
	bool bModelSupportsVision = false;
	if (ActiveEntry)
	{
		// Provider whitelist (most common vision-capable providers)
		static const TSet<FString> VisionProviders = {
			TEXT("openai"), TEXT("google"), TEXT("anthropic"),
			TEXT("qwen"), TEXT("zhipu"), TEXT("yi"), TEXT("moonshot"),
			TEXT("minimax"), TEXT("stepfun"), TEXT("sense"), TEXT("mistral"),
			TEXT("groq"),
		};
		if (VisionProviders.Contains(ActiveEntry->ProviderId))
		{
			// Within vision-capable providers, still verify by model name —
			// older/cheaper variants (gpt-3.5-turbo, claude-3-haiku text-only, etc.)
			// may not accept image_url.
			const FString& M = ActiveEntry->ModelId;
			bModelSupportsVision = M.Contains(TEXT("gpt-4o")) ||
				M.Contains(TEXT("gpt-4-turbo")) || M.Contains(TEXT("gpt-4-vision")) ||
				M.Contains(TEXT("o1")) || M.Contains(TEXT("o3")) ||
				M.Contains(TEXT("claude-3")) || M.Contains(TEXT("claude-4")) ||
				M.Contains(TEXT("gemini")) ||
				M.Contains(TEXT("vl")) || M.Contains(TEXT("vision")) ||
				M.Contains(TEXT("image")) ||
				// Qwen/GLM/Yi vision model naming
				M.Contains(TEXT("qwen-vl")) || M.Contains(TEXT("glm-4v")) ||
				M.Contains(TEXT("yi-vision")) || M.Contains(TEXT("step-1v")) ||
				M.Contains(TEXT("pixtral")) || M.Contains(TEXT("sonar"));
		}
	}

	// ponytail: when model doesn't support vision, strip ALL content arrays to text-only
	// and remove base64 image_data from tool results — prevents HTTP 400 and huge payloads.
	if (!bModelSupportsVision)
	{
		TArray<TSharedPtr<FJsonValue>> StrippedMsgs;
		for (int32 Vi = 0; Vi < ApiMessages.Num(); ++Vi)
		{
			const auto& V = ApiMessages[Vi];
			TSharedPtr<FJsonObject> Obj = V->AsObject();
			if (!Obj.IsValid()) { StrippedMsgs.Add(V); continue; }

			// Handle array-type content: keep only text parts, produce plain text string
			const TArray<TSharedPtr<FJsonValue>>* Content;
			if (Obj->TryGetArrayField(TEXT("content"), Content))
			{
				TSharedPtr<FJsonObject> NewObj = MakeShareable(new FJsonObject());
				for (const auto& Pair : Obj->Values)
				{
					if (Pair.Key != TEXT("content"))
						NewObj->SetField(Pair.Key, Pair.Value);
				}
				FString PlainText;
				for (int32 Ci = 0; Ci < Content->Num(); ++Ci)
				{
					const auto& C = (*Content)[Ci];
					TSharedPtr<FJsonObject> CObj = C->AsObject();
					FString CType;
					if (CObj.IsValid() && CObj->TryGetStringField(TEXT("type"), CType) && CType == TEXT("text"))
					{
						FString Txt;
						if (CObj->TryGetStringField(TEXT("text"), Txt) && !Txt.IsEmpty())
						{
							if (!PlainText.IsEmpty()) PlainText += TEXT("\n");
							PlainText += Txt;
						}
					}
				}
				NewObj->SetStringField(TEXT("content"), PlainText.IsEmpty() ? TEXT("[非文本内容已移除]") : PlainText);
				StrippedMsgs.Add(MakeShareable(new FJsonValueObject(NewObj)));
			}
			else
			{
				// Handle string-type content: strip base64 image_data from tool results
				FString StrContent;
				if (Obj->TryGetStringField(TEXT("content"), StrContent) && StrContent.Contains(TEXT("\"image_data\":\"")))
				{
					// Parse result JSON, remove image_data, re-serialize
					TSharedPtr<FJsonObject> NewObj = MakeShareable(new FJsonObject());
					for (const auto& Pair : Obj->Values)
					{
						if (Pair.Key != TEXT("content"))
							NewObj->SetField(Pair.Key, Pair.Value);
					}
					TSharedRef<TJsonReader<>> Rdr = TJsonReaderFactory<>::Create(StrContent);
					TSharedPtr<FJsonObject> ResultObj;
					if (FJsonSerializer::Deserialize(Rdr, ResultObj) && ResultObj.IsValid())
					{
						ResultObj->SetStringField(TEXT("image_data"), TEXT("[base64 removed, see chat for image]"));
						FString CleanJson;
						TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&CleanJson);
						FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Wr);
						NewObj->SetStringField(TEXT("content"), CleanJson);
					}
					else
					{
						NewObj->SetStringField(TEXT("content"), TEXT("{\"status\":\"ok\",\"note\":\"image generated successfully\"}"));
					}
					StrippedMsgs.Add(MakeShareable(new FJsonValueObject(NewObj)));
				}
				else
				{
					StrippedMsgs.Add(V);
				}
			}
		}
		SendAIRequestInternal(StrippedMsgs);
		return;
	}

	SendAIRequestInternal(ApiMessages);
}

void SMCPToolboxChatWidget::SendAIRequestInternal(const TArray<TSharedPtr<FJsonValue>>& ApiMessages)
{
	if (bInterrupted) { bIsWaiting = false; return; }

	const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
	if (!ActiveEntry) { bIsWaiting = false; return; }

	FString ApiKey;
	FBase64::Decode(ActiveEntry->EncryptedKey, ApiKey);

	FString ApiUrl = ActiveEntry->BaseURL;

	// Provider-specific URL mapping
	if (ActiveEntry->ProviderId == TEXT("google"))
	{
		// Gemini: https://generativelanguage.googleapis.com/v1beta/models/{model}:streamGenerateContent
		if (!ApiUrl.EndsWith(TEXT(":streamGenerateContent")))
		{
			if (ApiUrl.EndsWith(TEXT("/"))) ApiUrl.RemoveFromEnd(TEXT("/"));
			ApiUrl += TEXT("/models/") + ActiveEntry->ModelId + TEXT(":streamGenerateContent?alt=sse");
		}
	}
	else if (ActiveEntry->ProviderId == TEXT("baidu"))
	{
		// Baidu Wenxin: needs access_token in URL param
		if (!ApiUrl.Contains(TEXT("access_token=")))
		{
			ApiUrl += TEXT("?access_token=") + ApiKey;
		}
	}
	else if (!ApiUrl.EndsWith(TEXT("/chat/completions")))
	{
		if (ApiUrl.EndsWith(TEXT("/"))) ApiUrl.RemoveFromEnd(TEXT("/"));
		ApiUrl += TEXT("/chat/completions");
	}

	// Fast/Deep thinking hybrid: detect tool-calling loops → suppress reasoning
	const TArray<TSharedPtr<FJsonValue>>* EffectiveMessages = &ApiMessages;
	TArray<TSharedPtr<FJsonValue>> FastModeMsgs;
	{
		int32 ConsecutiveToolPairs = 0;
		for (int32 i = ApiMessages.Num() - 1; i >= 0; --i)
		{
			TSharedPtr<FJsonObject> Obj = ApiMessages[i]->AsObject();
			if (!Obj.IsValid()) break;
			FString Role;
			Obj->TryGetStringField(TEXT("role"), Role);
			if (Role == TEXT("tool")) { ConsecutiveToolPairs++; continue; }
			if (Role == TEXT("assistant"))
			{
				const TArray<TSharedPtr<FJsonValue>>* TCs;
				if (Obj->TryGetArrayField(TEXT("tool_calls"), TCs) && TCs->Num() > 0) continue;
			}
			break;
		}
		if (ConsecutiveToolPairs >= 2)
		{
			// DeepSeek thinking mode requires reasoning_content on ALL assistant msgs.
			// Ensure every assistant message has it (empty = fast mode, no actual reasoning).
			for (const auto& V : ApiMessages)
			{
				TSharedPtr<FJsonObject> Obj = V->AsObject();
				FString Role;
				Obj->TryGetStringField(TEXT("role"), Role);
				if (Role == TEXT("assistant") && !Obj->Values.Contains(TEXT("reasoning_content")))
				{
					// Need to modify: deep-copy and add empty reasoning
					FString Ser;
					TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Ser);
					FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
					TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Ser);
					TSharedPtr<FJsonObject> Copy = MakeShareable(new FJsonObject());
					FJsonSerializer::Deserialize(R, Copy);
					Copy->SetStringField(TEXT("reasoning_content"), TEXT(""));
					FastModeMsgs.Add(MakeShareable(new FJsonValueObject(Copy)));
				}
				else
				{
					FastModeMsgs.Add(V);
				}
			}
			EffectiveMessages = &FastModeMsgs;
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Fast-thinking mode: %d tool pairs, ensured reasoning fields"), ConsecutiveToolPairs);
		}
	}

	// Build request body
	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	Body->SetStringField(TEXT("model"), ActiveEntry->ModelId);
	Body->SetBoolField(TEXT("stream"), true);
	Body->SetArrayField(TEXT("messages"), *EffectiveMessages);

	// Add tools (cached, rebuilt when tools change)
	if (CachedToolsArray.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), CachedToolsArray);
		FString ToolsLog;
		for (int32 Ti = 0; Ti < CachedToolsArray.Num(); ++Ti)
		{
			const auto& ToolVal = CachedToolsArray[Ti];
			if (ToolVal.IsValid())
			{
				TSharedPtr<FJsonObject> ToolObj = ToolVal->AsObject();
				if (ToolObj.IsValid())
				{
					FString ToolName;
					ToolObj->TryGetStringField(TEXT("name"), ToolName);
					if (!ToolsLog.IsEmpty()) ToolsLog += TEXT(", ");
					ToolsLog += ToolName;
				}
			}
		}
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] tools array contains: %s"), *ToolsLog);
	}

	// Add tool_choice to encourage tool use
	Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 发送请求: url=%s, model=%s, msgs=%d"), *ApiUrl, *ActiveEntry->ModelId, ApiMessages.Num());

	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ApiUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(120.0f); // 2 minute timeout for tool calls

	TArray<TSharedPtr<FJsonValue>> MsgsCopy = ApiMessages;
	Request->OnProcessRequestComplete().BindLambda(
		[this, MsgsCopy](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (bInterrupted || !bIsWaiting) return;
			if (bSuccess && Resp.IsValid())
				HandleAIResponse(Resp, MsgsCopy);
			else
			{
				bIsWaiting = false;
				FMCPToolboxChatMessage Err;
				Err.Role = EMCPToolboxMessageRole::System;
				Err.Content = TEXT("网络请求失败。\n请检查网络连接和API地址。");
				AddMessage(Err);
			}
			ActiveHttpRequest.Reset();
		});

	ActiveHttpRequest = Request;
	Request->ProcessRequest();
}

// ============================================================================
// Handle AI Response — parse content + tool_calls, loop if needed
// ============================================================================
void SMCPToolboxChatWidget::HandleAIResponse(FHttpResponsePtr Resp, const TArray<TSharedPtr<FJsonValue>>& SentMessages)
{
	if (bInterrupted) { bIsWaiting = false; return; }

	int32 Code = Resp->GetResponseCode();
	FString RespBody = Resp->GetContentAsString();

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] HTTP %d, BodyLen=%d"), Code, RespBody.Len());

	if (Code != 200)
	{
		bIsWaiting = false;
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = FString::Printf(TEXT("AI服务错误 (HTTP %d):\n\n```\n%s\n```"), Code, *RespBody.Left(500));
		AddMessage(Err);
		UE_LOG(LogMCPToolbox, Error, TEXT("[Chat] HTTP error %d: %s"), Code, *RespBody.Left(300));
		return;
	}

	// Detect SSE streaming response (starts with "data: ")
	TSharedPtr<FJsonObject> RespObj;
	FString FinishReason = TEXT("stop");
	FString AccumulatedContent;
	FString AccumulatedReasoning;  // DeepSeek thinking mode: delta.reasoning_content
	TArray<TSharedPtr<FJsonValue>> AccumulatedToolCalls;
	TMap<int32, TSharedPtr<FJsonValue>> ToolCallByIndex; // Dedup by index

	if (RespBody.TrimStartAndEnd().StartsWith(TEXT("data: ")))
	{
		// Parse SSE stream into chunks for progressive display
		TArray<FString> TextChunks;
		TArray<FString> Lines;
		RespBody.ParseIntoArrayLines(Lines);

		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || Trimmed == TEXT("data: [DONE]")) continue;
			if (!Trimmed.StartsWith(TEXT("data: "))) continue;

			FString JsonStr = Trimmed.RightChop(6);
			if (JsonStr == TEXT("[DONE]")) break;

			TSharedPtr<FJsonObject> Chunk;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
			if (!FJsonSerializer::Deserialize(Reader, Chunk) || !Chunk.IsValid()) continue;

			const TArray<TSharedPtr<FJsonValue>>* Choices;
			if (!Chunk->TryGetArrayField(TEXT("choices"), Choices) || Choices->Num() == 0) continue;

			TSharedPtr<FJsonObject> Ch = (*Choices)[0]->AsObject();
			if (!Ch.IsValid()) continue;

			Ch->TryGetStringField(TEXT("finish_reason"), FinishReason);

			const TSharedPtr<FJsonObject>* Delta;
			if (Ch->TryGetObjectField(TEXT("delta"), Delta) && Delta->IsValid())
			{
				FString DeltaContent;
				if ((*Delta)->TryGetStringField(TEXT("content"), DeltaContent))
				{
					AccumulatedContent += DeltaContent;
					TextChunks.Add(DeltaContent);
				}

				// DeepSeek thinking mode: accumulate reasoning_content so we can pass
				// it back on subsequent requests (API enforces this in thinking mode).
				FString DeltaReasoning;
				if ((*Delta)->TryGetStringField(TEXT("reasoning_content"), DeltaReasoning))
				{
					AccumulatedReasoning += DeltaReasoning;
				}

				const TArray<TSharedPtr<FJsonValue>>* DeltaTC;
			if ((*Delta)->TryGetArrayField(TEXT("tool_calls"), DeltaTC))
			{
				for (const auto& TC : *DeltaTC)
				{
					TSharedPtr<FJsonObject> TCObj = TC->AsObject();
					if (!TCObj.IsValid()) continue;

					// Merge by index: SSE deltas repeat full arrays, use index to dedup
					int32 Idx = 0;
					if (TCObj->TryGetNumberField(TEXT("index"), Idx))
					{
						TSharedPtr<FJsonValue>* Existing = ToolCallByIndex.Find(Idx);
						if (Existing && Existing->IsValid())
						{
							// Merge fields individually (NOT replace whole object, which loses name)
							TSharedPtr<FJsonObject> ExObj = (*Existing)->AsObject();
							TSharedPtr<FJsonObject> NewObj = TC->AsObject();
							if (ExObj.IsValid() && NewObj.IsValid())
							{
								// Merge id
								FString NewId;
								if (NewObj->TryGetStringField(TEXT("id"), NewId) && !NewId.IsEmpty())
									ExObj->SetStringField(TEXT("id"), NewId);
								// Merge function fields one by one
								const TSharedPtr<FJsonObject>* NewFunc;
								const TSharedPtr<FJsonObject>* OldFunc;
								if (NewObj->TryGetObjectField(TEXT("function"), NewFunc))
								{
									if (ExObj->TryGetObjectField(TEXT("function"), OldFunc))
							{
								TSharedPtr<FJsonObject> MergedFunc = MakeShareable(new FJsonObject(*OldFunc->Get()));
								for (const auto& Pair : (*NewFunc)->Values)
								{
									// Arguments are streamed INCREMENTALLY — concatenate!
									if (Pair.Key == TEXT("arguments") && MergedFunc->HasField(TEXT("arguments")))
									{
										FString OldArgs = MergedFunc->GetStringField(TEXT("arguments"));
										FString NewArgs;
										Pair.Value->TryGetString(NewArgs);
										MergedFunc->SetStringField(TEXT("arguments"), OldArgs + NewArgs);
									}
									else
									{
										MergedFunc->SetField(Pair.Key, Pair.Value);
									}
								}
								ExObj->SetObjectField(TEXT("function"), MergedFunc);
							}
									else
									{
										ExObj->SetObjectField(TEXT("function"), *NewFunc);
									}
								}
							}
						}
						else
						{
							ToolCallByIndex.Add(Idx, TC);
						}
					}
				}
			}
			}
		}

		// Convert deduped map back to array, sorted by index
		ToolCallByIndex.KeySort([](int32 A, int32 B) { return A < B; });
		for (auto& Pair : ToolCallByIndex)
			AccumulatedToolCalls.Add(Pair.Value);

		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] SSE: content=%d chars(%d chunks), tool_calls=%d (deduped from map), finish=%s, reasoning=%d chars"),
			AccumulatedContent.Len(), TextChunks.Num(), AccumulatedToolCalls.Num(), *FinishReason, AccumulatedReasoning.Len());

		// Progressive display for text-only responses (no tool_calls)
		if (AccumulatedToolCalls.Num() == 0 && TextChunks.Num() > 0)
		{
			// Create streaming placeholder
			FMCPToolboxChatMessage StreamMsg;
			StreamMsg.Role = EMCPToolboxMessageRole::Assistant;
			StreamMsg.Content = TEXT("");
			StreamMsg.bIsStreaming = true;
			StreamMsg.ReasoningContent = AccumulatedReasoning;  // Persist for next request
			Messages.Add(StreamMsg);
			FMCPToolboxChatMessage* StreamPtr = &Messages.Last();
			
			// Add the message bubble to the chat area and get the text box reference
			TSharedPtr<SMultiLineEditableTextBox> TextBlock;
			if (ChatScrollBox.IsValid())
			{
				TSharedRef<SWidget> Bubble = CreateMessageBubble(*StreamPtr, TextBlock);
				ChatScrollBox->AddSlot().Padding(FMargin(4))[Bubble];
				ChatScrollBox->ScrollToEnd();
			}
			StreamingMessageBox = TextBlock;

			// Use ticker to progressively show chunks
			TSharedPtr<int32> ChunkIndex = MakeShareable(new int32(0));
			TSharedPtr<FString> DisplayBuffer = MakeShareable(new FString());
			TSharedPtr<TArray<FString>> Chunks = MakeShareable(new TArray<FString>(MoveTemp(TextChunks)));
			// Incremental auto-save counter — save every 20 ticks (~2 seconds)
			TSharedPtr<int32> SaveCounter = MakeShareable(new int32(0));

			FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, StreamPtr, ChunkIndex, DisplayBuffer, Chunks, SaveCounter](float DeltaTime) -> bool
			{
				if (bInterrupted || *ChunkIndex >= Chunks->Num())
				{
					StreamPtr->bIsStreaming = false;
					StreamPtr->Content = *DisplayBuffer;
					FMCPToolboxMemoryManager::Get().ExtractMemoriesFromResponse(*DisplayBuffer);
				bSystemPromptDirty = true;  // memory changed, rebuild system prompt next request
					bIsWaiting = false;
					// Final rebuild to show cleaned markdown
					RebuildChatDisplay();
					// Persist the completed streaming message to the session.
					UpdateCurrentSessionWithMessage(*StreamPtr);
					RefreshSessionList();
					StreamingMessageBox.Reset();
					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Stream done, %d chars"), DisplayBuffer->Len());

					FString FinalContent = *DisplayBuffer;
					TWeakPtr<SMCPToolboxChatWidget> WeakSelf = SharedThis(this);
					AsyncTask(ENamedThreads::GameThread, [WeakSelf, FinalContent]()
					{
						if (TSharedPtr<SMCPToolboxChatWidget> Self = WeakSelf.Pin())
						{
							Self->HandleStreamingTextCompletion(FinalContent);
						}
					});

					TWeakPtr<SMCPToolboxChatWidget> ArchiveWeakSelf = SharedThis(this);
					AsyncTask(ENamedThreads::GameThread, [ArchiveWeakSelf]()
					{
						if (TSharedPtr<SMCPToolboxChatWidget> Self = ArchiveWeakSelf.Pin())
						{
							Self->TryAutoArchiveWhenIdle();
						}
					});
					return false;
				}

					// Batch: append up to 5 chunks at once to reduce rebuild frequency
					int32 BatchCount = FMath::Min(5, Chunks->Num() - *ChunkIndex);
					for (int32 i = 0; i < BatchCount; ++i)
					{
						*DisplayBuffer += (*Chunks)[*ChunkIndex];
						(*ChunkIndex)++;
					}
					StreamPtr->Content = *DisplayBuffer;

				// Update the text box directly instead of rebuilding everything
				if (StreamingMessageBox.IsValid())
				{
					StreamingMessageBox->SetText(FText::FromString(*DisplayBuffer));
					if (ChatScrollBox.IsValid())
						ChatScrollBox->ScrollToEnd();
				}
				else
				{
					// Fallback: rebuild if text box reference is lost
					RebuildChatDisplay();
				}

				// Incremental auto-save: persist streaming content every 20 ticks
				(*SaveCounter)++;
				if (*SaveCounter % 20 == 0)
				{
					UpdateCurrentSessionWithMessage(*StreamPtr);
				}

				return true;
				}),
				0.15f // ~150ms between batches, much smoother than 30ms
			);

			return; // Progressive display handles completion
		}

		// Build a message object from accumulated SSE data (for tool_calls or empty)
		RespObj = MakeShareable(new FJsonObject());
		TArray<TSharedPtr<FJsonValue>> ChoicesArray;
		TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject());
		MsgObj->SetStringField(TEXT("role"), TEXT("assistant"));
		MsgObj->SetStringField(TEXT("content"), AccumulatedContent);
		// DeepSeek thinking mode: attach reasoning_content so it round-trips back on
		// the next request (API rejects assistant messages without this field in thinking mode).
		// Use empty string when no reasoning was streamed (preserves field presence).
		MsgObj->SetStringField(TEXT("reasoning_content"), AccumulatedReasoning);
		if (AccumulatedToolCalls.Num() > 0)
			MsgObj->SetArrayField(TEXT("tool_calls"), AccumulatedToolCalls);
		TSharedPtr<FJsonObject> ChoiceObj = MakeShareable(new FJsonObject());
		ChoiceObj->SetObjectField(TEXT("message"), MsgObj);
		ChoiceObj->SetStringField(TEXT("finish_reason"), FinishReason);
		ChoicesArray.Add(MakeShareable(new FJsonValueObject(ChoiceObj)));
		RespObj->SetArrayField(TEXT("choices"), ChoicesArray);
	}
	else
	{
		// Non-streaming response
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
		if (!FJsonSerializer::Deserialize(Reader, RespObj) || !RespObj.IsValid())
		{
			bIsWaiting = false;
			FMCPToolboxChatMessage Err;
			Err.Role = EMCPToolboxMessageRole::System;
			Err.Content = FString::Printf(TEXT("无法解析AI响应:\n\n```\n%s\n```"), *RespBody.Left(300));
			AddMessage(Err);
			return;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Choices;
	if (!RespObj->TryGetArrayField(TEXT("choices"), Choices) || Choices->Num() == 0)
	{
		bIsWaiting = false;
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = FString::Printf(TEXT("AI返回空响应:\n\n```\n%s\n```"), *RespBody.Left(300));
		AddMessage(Err);
		return;
	}

	TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
	if (!Choice.IsValid()) { bIsWaiting = false; return; }

	// FinishReason already parsed from SSE or set below
	if (FinishReason.IsEmpty())
		Choice->TryGetStringField(TEXT("finish_reason"), FinishReason);

	const TSharedPtr<FJsonObject>* Msg;
	if (!Choice->TryGetObjectField(TEXT("message"), Msg) || !Msg->IsValid()) { bIsWaiting = false; return; }

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] finish_reason=%s, has_content=%d"), *FinishReason, (*Msg)->HasField(TEXT("content")) ? 1 : 0);

	// --- Check for tool_calls ---
	const TArray<TSharedPtr<FJsonValue>>* ToolCalls;
	bool bHasToolCalls = (*Msg)->TryGetArrayField(TEXT("tool_calls"), ToolCalls) && ToolCalls->Num() > 0;
	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] has_tool_calls=%d, count=%d"), bHasToolCalls ? 1 : 0, bHasToolCalls ? ToolCalls->Num() : 0);

	if (bHasToolCalls)
	{
		// Reset pseudo-tool-call retry counter — LLM generated real tool_calls this turn.
		PseudoToolCallRetries = 0;

		// Loop detection: abort if same tool name repeats more than 10 times
		++ToolCallIteration;
		FString CurrentToolNames;
		for (const auto& TC : *ToolCalls)
		{
			TSharedPtr<FJsonObject> TCObj = TC->AsObject();
			if (TCObj.IsValid())
			{
				FString Name;
				const TSharedPtr<FJsonObject>* Func;
				if (TCObj->TryGetObjectField(TEXT("function"), Func) && Func->IsValid())
				{
					(*Func)->TryGetStringField(TEXT("name"), Name);
				}
				else
				{
					TCObj->TryGetStringField(TEXT("name"), Name);
				}
				if (!CurrentToolNames.IsEmpty()) CurrentToolNames += TEXT(",");
				CurrentToolNames += Name;
			}
		}
		if (ToolCallHistory.Num() > 0 && ToolCallHistory.Last() == CurrentToolNames)
		{
			++ConsecutiveSameToolCount;
		}
		else
		{
			ConsecutiveSameToolCount = 1;
		}
		ToolCallHistory.Add(CurrentToolNames);
		if (ConsecutiveSameToolCount > 10)
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Tool loop detected (%d consecutive same calls), aborting"), ConsecutiveSameToolCount);
			bIsWaiting = false;
			FMCPToolboxChatMessage LimitMsg;
			LimitMsg.Role = EMCPToolboxMessageRole::System;
			LimitMsg.Content = TEXT("检测到工具调用循环，已终止。请检查工具定义或调整请求。");
			AddMessage(LimitMsg);
			return;
		}

		// Show AI's text if present alongside tool calls
		FString ContentAlongside;
		(*Msg)->TryGetStringField(TEXT("content"), ContentAlongside);
		if (!ContentAlongside.IsEmpty())
		{
			FMCPToolboxChatMessage ThinkingMsg;
			ThinkingMsg.Role = EMCPToolboxMessageRole::Assistant;
			ThinkingMsg.Content = ContentAlongside;
			AddMessage(ThinkingMsg);
		}

		// Show tool usage to user
		FString ToolNames;
		TArray<FString> ToolArgsList;
		for (const auto& TC : *ToolCalls)
		{
			TSharedPtr<FJsonObject> TCObj = TC->AsObject();
			if (TCObj.IsValid())
			{
				FString Name, Args;
				const TSharedPtr<FJsonObject>* Func;
				if (TCObj->TryGetObjectField(TEXT("function"), Func) && Func->IsValid())
				{
					(*Func)->TryGetStringField(TEXT("name"), Name);
					(*Func)->TryGetStringField(TEXT("arguments"), Args);
				}
				else
				{
					TCObj->TryGetStringField(TEXT("name"), Name);
					const TSharedPtr<FJsonObject>* ArgsObj;
					if (TCObj->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && (*ArgsObj).IsValid())
					{
						FString OutputJson;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
						FJsonSerializer::Serialize((*ArgsObj).ToSharedRef(), Writer);
						Args = OutputJson;
					}
				}
				if (!ToolNames.IsEmpty()) ToolNames += TEXT(", ");
				ToolNames += Name;
				ToolArgsList.Add(Args);
			}
		}

		FMCPToolboxChatMessage ToolMsg;
		ToolMsg.Role = EMCPToolboxMessageRole::Thinking;
		ToolMsg.Content = FString::Printf(TEXT("调用工具: %s"), *ToolNames);
		AddMessage(ToolMsg);

		// Check if tool calls have DAG dependencies (LLMCompiler-style)
		if (HasDAGDependencies(*ToolCalls))
		{
			ExecuteToolCallsDAG(*ToolCalls, SentMessages, *Msg);
			return;
		}

		// Build new messages: old messages + assistant(tool_calls) + tool results
		TArray<TSharedPtr<FJsonValue>> NewMsgs = SentMessages;

		// Add assistant message with tool_calls (use the original response message)
		NewMsgs.Add(MakeShareable(new FJsonValueObject(*Msg)));

		// Execute each tool call and add tool result messages
		TSharedPtr<int32> PendingMCP = MakeShared<int32>(0);
		TSharedPtr<TArray<TSharedPtr<FJsonValue>>> PendingMsgs = MakeShared<TArray<TSharedPtr<FJsonValue>>>(MoveTemp(NewMsgs));
		TSharedPtr<TArray<TSharedPtr<FJsonObject>>> PendingToolCallObjs = MakeShared<TArray<TSharedPtr<FJsonObject>>>();
		
		for (const auto& TC : *ToolCalls)
		{
			TSharedPtr<FJsonObject> TCObj = TC->AsObject();
			if (TCObj.IsValid()) PendingToolCallObjs->Add(TCObj);
		}

		for (const auto& TCObj : *PendingToolCallObjs)
		{
			if (!TCObj.IsValid()) continue;
			FString TCId;
			TCObj->TryGetStringField(TEXT("id"), TCId);
			
			FString FuncName, FuncArgs;
			const TSharedPtr<FJsonObject>* Func;
			if (TCObj->TryGetObjectField(TEXT("function"), Func) && Func->IsValid())
			{
				(*Func)->TryGetStringField(TEXT("name"), FuncName);
				(*Func)->TryGetStringField(TEXT("arguments"), FuncArgs);
			}
			else
			{
				TCObj->TryGetStringField(TEXT("name"), FuncName);
				const TSharedPtr<FJsonObject>* ArgsObj;
				if (TCObj->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && (*ArgsObj).IsValid())
				{
					FString OutputJson;
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
					FJsonSerializer::Serialize((*ArgsObj).ToSharedRef(), Writer);
					FuncArgs = OutputJson;
				}
			}
			
			if (FuncName.IsEmpty()) continue;

			// ── call_tool 特殊处理 ──
			// call_tool 是本地注册的元工具，用于调用 MCP server 上的真实工具
			// 需要解析 toolset_name 和 tool_name，然后通过 MCP server 调用真实工具
			if (FuncName == TEXT("call_tool"))
			{
				if (!MCPServerClient.IsConnected())
				{
					FMCPToolboxChatMessage ErrorMsg;
					ErrorMsg.Role = EMCPToolboxMessageRole::System;
					ErrorMsg.Content = TEXT("⚠ MCP 服务器未连接，无法调用 call_tool。请检查 MCP 服务器是否正常运行。");
					AddMessage(ErrorMsg);
					continue;
				}

				TSharedPtr<FJsonObject> ArgsObj;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FuncArgs);
				if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
				{
					FString ToolsetName, ToolName, ArgumentsJson;
					ArgsObj->TryGetStringField(TEXT("toolset_name"), ToolsetName);
					ArgsObj->TryGetStringField(TEXT("tool_name"), ToolName);
					
					const TSharedPtr<FJsonObject>* ArgsPtr;
					if (ArgsObj->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr->IsValid())
					{
						FString OutputJson;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
						FJsonSerializer::Serialize((*ArgsPtr).ToSharedRef(), Writer);
						ArgumentsJson = OutputJson;
					}

					if (!ToolsetName.IsEmpty() && !ToolName.IsEmpty())
					{
						(*PendingMCP)++;
						FString NameCap = ToolName;
						FString IdCap = TCId;
						FString FullToolName = ToolsetName + TEXT(".") + ToolName;

						UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] call_tool -> MCP tool: %s"), *FullToolName);

						MCPServerClient.ExecuteTool(FullToolName, ArgumentsJson,
							[this, PendingMCP, PendingMsgs, NameCap, IdCap](bool bOk, const FString& R)
							{
								AsyncTask(ENamedThreads::GameThread,
									[this, PendingMCP, PendingMsgs, NameCap, IdCap, R]()
								{
									TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
									ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
									ToolResultMsg->SetStringField(TEXT("tool_call_id"), IdCap);
									ToolResultMsg->SetStringField(TEXT("name"), NameCap);
									ToolResultMsg->SetStringField(TEXT("content"), R);
									PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

									FMCPToolboxChatMessage ResultMsg;
									ResultMsg.Role = EMCPToolboxMessageRole::System;
									ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *R.Left(2000));
									AddMessage(ResultMsg);

									(*PendingMCP)--;
									if (*PendingMCP <= 0)
									{
										if (bSpeculationPending && LastSpeculation.IsValid() == false)
										{
											bPendingToolCompletion = true;
										}
										else
										{
											TrySpeculativeOrContinue(PendingMsgs);
										}
									}
								});
							});
						continue;
					}
					else
					{
						FMCPToolboxChatMessage ErrorMsg;
						ErrorMsg.Role = EMCPToolboxMessageRole::System;
						ErrorMsg.Content = TEXT("⚠ call_tool 参数错误：缺少 toolset_name 或 tool_name");
						AddMessage(ErrorMsg);
					}
				}
				else
				{
					FMCPToolboxChatMessage ErrorMsg;
					ErrorMsg.Role = EMCPToolboxMessageRole::System;
					ErrorMsg.Content = TEXT("⚠ call_tool 参数解析失败");
					AddMessage(ErrorMsg);
				}
				continue;
			}

			// Check if this is an MCP tool that needs async execution
			bool bIsMCP = MCPServerClient.IsConnected() && MCPServerClient.IsMCPTool(FuncName);
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Tool routing: FuncName=%s, IsMCP=%d, MCPConnected=%d"), 
				*FuncName, bIsMCP ? 1 : 0, MCPServerClient.IsConnected() ? 1 : 0);

			if (bIsMCP)
			{
				// MCP tool: execute asynchronously
				(*PendingMCP)++;
				FString NameCap = FuncName;
				FString IdCap = TCId;

				// IdleSpec: launch speculative execution while tool is busy
				LaunchIdleSpec(FuncName);

				MCPServerClient.ExecuteTool(FuncName, FuncArgs,
					[this, PendingMCP, PendingMsgs, NameCap, IdCap](bool bOk, const FString& R)
					{
						// HTTP callback runs on a background thread. Three unsafe operations
						// must be marshalled back to the game thread:
						//   1. PendingMCP-- is a non-atomic read-modify-write that races when
						//      multiple MCP tools finish concurrently (lost update → counter
						//      never reaches zero → conversation hangs).
						//   2. PendingMsgs->Add() pushes to a TArray that is not thread-safe
						//      under concurrent callers (corrupts internal state / crash).
						//   3. AddMessage() touches Slate UI state (Messages array, scrollbox)
						//      which UE requires to run on the game thread.
						AsyncTask(ENamedThreads::GameThread,
							[this, PendingMCP, PendingMsgs, NameCap, IdCap, R]()
						{
							TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
							ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
							ToolResultMsg->SetStringField(TEXT("tool_call_id"), IdCap);
							ToolResultMsg->SetStringField(TEXT("name"), NameCap);
							ToolResultMsg->SetStringField(TEXT("content"), R);
							PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

							FMCPToolboxChatMessage ResultMsg;
							ResultMsg.Role = EMCPToolboxMessageRole::System;
							ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *R.Left(2000));
							AddMessage(ResultMsg);

							(*PendingMCP)--;

							// All MCP calls done → continue when speculation resolves
							if (*PendingMCP <= 0)
							{
								if (bSpeculationPending && LastSpeculation.IsValid() == false)
								{
									bPendingToolCompletion = true;
								}
								else
								{
									TrySpeculativeOrContinue(PendingMsgs);
								}
							}
						});
					});
			}
			else
			{
				if (FuncName == TEXT("generate_image"))
				{
					(*PendingMCP)++;
					FString NameCap = FuncName;
					FString IdCap = TCId;
					FString ArgsCap = FuncArgs;

					Async(EAsyncExecution::Thread,
						[this, PendingMCP, PendingMsgs, NameCap, IdCap, ArgsCap]()
					{
						FString Result = ExecuteToolCall(NameCap, ArgsCap);

						AsyncTask(ENamedThreads::GameThread,
							[this, PendingMCP, PendingMsgs, NameCap, IdCap, Result]()
						{
							FMCPToolboxChatMessage ResultMsg;
							ResultMsg.Role = EMCPToolboxMessageRole::System;
							ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *Result.Left(500));
							AddMessage(ResultMsg);

							TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
							TSharedPtr<FJsonObject> ResultObj;
							FString CleanedResult = Result;

							if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
							{
								FString ImageData;
								if (ResultObj->TryGetStringField(TEXT("image_data"), ImageData) && !ImageData.IsEmpty())
								{
									ResultObj->SetStringField(TEXT("image_data"), TEXT("[图片数据已省略，图片已在UI中显示]"));
									TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&CleanedResult);
									FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);
								}
							}

							TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
							ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
							ToolResultMsg->SetStringField(TEXT("tool_call_id"), IdCap);
							ToolResultMsg->SetStringField(TEXT("name"), NameCap);
							ToolResultMsg->SetStringField(TEXT("content"), CleanedResult);
							PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

							TSharedRef<TJsonReader<>> ResultReader2 = TJsonReaderFactory<>::Create(Result);
							TSharedPtr<FJsonObject> ResultObj2;
							if (FJsonSerializer::Deserialize(ResultReader2, ResultObj2) && ResultObj2.IsValid())
							{
								FString ImageURL;
								FString ImageData;
								FString Status;
								ResultObj2->TryGetStringField(TEXT("status"), Status);
								ResultObj2->TryGetStringField(TEXT("image_url"), ImageURL);
								ResultObj2->TryGetStringField(TEXT("image_data"), ImageData);

								FString DisplayURL = ImageURL.IsEmpty() ? ImageData : ImageURL;
								if (!DisplayURL.IsEmpty() && Status == TEXT("ok"))
								{
									FMCPToolboxChatMessage ImageMsg;
									ImageMsg.Role = EMCPToolboxMessageRole::Assistant;
									ImageMsg.Content = TEXT("**生图成功！**");
									ImageMsg.bHasImageAttachment = true;
									ImageMsg.ImageDataURIs.Add(DisplayURL);
									ImageMsg.ImageFileNames.Add(TEXT("generated.png"));
									AddMessage(ImageMsg);
								}
							}

							(*PendingMCP)--;
							if (*PendingMCP <= 0)
							{
								if (bSpeculationPending && LastSpeculation.IsValid() == false)
								{
									bPendingToolCompletion = true;
								}
								else
								{
									TrySpeculativeOrContinue(PendingMsgs);
								}
							}
						});
					});
				}
				else
				{
					FString Result = ExecuteToolCall(FuncName, FuncArgs);

					FMCPToolboxChatMessage ResultMsg;
					ResultMsg.Role = EMCPToolboxMessageRole::System;
					ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *FuncName, *Result.Left(500));
					AddMessage(ResultMsg);

					TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
					ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
					ToolResultMsg->SetStringField(TEXT("tool_call_id"), TCId);
					ToolResultMsg->SetStringField(TEXT("name"), FuncName);
					ToolResultMsg->SetStringField(TEXT("content"), Result);
					PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

					if (FuncName == TEXT("screenshot"))
					{
						TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
						TSharedPtr<FJsonObject> ResultObj;
						if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
						{
							FString DataURI;
							if (ResultObj->TryGetStringField(TEXT("data_uri"), DataURI) && !DataURI.IsEmpty())
							{
								TArray<TSharedPtr<FJsonValue>> ContentArray;

								TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
								TextPart->SetStringField(TEXT("type"), TEXT("text"));
								TextPart->SetStringField(TEXT("text"), TEXT("Here is the screenshot you requested. Please analyze it."));
								ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));

								TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject());
								ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));

								TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject());
								ImageUrlObj->SetStringField(TEXT("url"), DataURI);
								ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);

								ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));

								TSharedPtr<FJsonObject> ScreenshotUserMsg = MakeShareable(new FJsonObject());
								ScreenshotUserMsg->SetStringField(TEXT("role"), TEXT("user"));
								ScreenshotUserMsg->SetArrayField(TEXT("content"), ContentArray);
								PendingMsgs->Add(MakeShareable(new FJsonValueObject(ScreenshotUserMsg)));

								FMCPToolboxChatMessage ScreenshotMsg;
								ScreenshotMsg.Role = EMCPToolboxMessageRole::User;
								ScreenshotMsg.Content = TEXT("（截图已捕获，正在分析...）");
								ScreenshotMsg.bHasImageAttachment = true;
								ScreenshotMsg.ImageDataURIs.Add(DataURI);
								ScreenshotMsg.ImageFileNames.Add(TEXT("screenshot.png"));
								AddMessage(ScreenshotMsg);
							}
						}
					}
				}
			}
		}

		// If no MCP calls pending (all local tools), continue immediately
		if (*PendingMCP <= 0)
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 工具调用完成(本地), 继续对话"));
			SendAIRequest(*PendingMsgs);
		}
		return;
	}

	// --- Normal content response (no tool_calls) ---
	FString Content;
	(*Msg)->TryGetStringField(TEXT("content"), Content);

	if (!Content.IsEmpty())
	{
		// ── Pseudo-tool-call detection ──
		// DeepSeek thinking mode sometimes makes LLM state its tool-call intent in content
		// (e.g. "先查看 Panner 的引脚：调用工具 call_tool") but doesn't actually generate
		// the tool_calls field, so finish=stop and the conversation stalls. Detect this
		// and auto-retry once by appending a user message telling it to generate tool_calls.
		if (FinishReason == TEXT("stop") && PseudoToolCallRetries == 0)
		{
			bool bPseudoToolCall = false;
			// Detect intent keywords — LLM said it will call a tool but didn't.
			// Exclude past tense ("已调用"/"调用了") to avoid false positives.
			bool bHasIntent = Content.Contains(TEXT("调用工具")) ||
			                  Content.Contains(TEXT("call_tool")) ||
			                  Content.Contains(TEXT("先查看")) ||
			                  Content.Contains(TEXT("先读取")) ||
			                  Content.Contains(TEXT("先调用")) ||
			                  Content.Contains(TEXT("接下来调用")) ||
			                  Content.Contains(TEXT("接下来查看")) ||
			                  Content.Contains(TEXT("生图工具")) ||
			                  Content.Contains(TEXT("generate_image")) ||
			                  Content.Contains(TEXT("生成图片")) ||
			                  Content.Contains(TEXT("画一张")) ||
			                  Content.Contains(TEXT("画个")) ||
			                  Content.Contains(TEXT("正在调用生图")) ||
			                  Content.Contains(TEXT("正在调用绘图")) ||
			                  Content.Contains(TEXT("让我为你生成")) ||
			                  Content.Contains(TEXT("让我来生成")) ||
			                  Content.Contains(TEXT("正在生成图片")) ||
			                  Content.Contains(TEXT("即将调用")) ||
			                  Content.Contains(TEXT("准备调用"));
			bool bHasPast = Content.Contains(TEXT("已调用")) ||
			                Content.Contains(TEXT("调用了")) ||
			                Content.Contains(TEXT("已完成")) ||
			                Content.Contains(TEXT("已生成")) ||
			                Content.Contains(TEXT("已画"));
			if (bHasIntent && !bHasPast)
			{
				bPseudoToolCall = true;
			}

			if (!bPseudoToolCall)
			{
				FString UserMessage;
				for (int32 i = SentMessages.Num() - 1; i >= 0; i--)
				{
					TSharedPtr<FJsonObject> MsgObj = SentMessages[i]->AsObject();
					if (MsgObj.IsValid())
					{
						FString Role;
						MsgObj->TryGetStringField(TEXT("role"), Role);
						if (Role == TEXT("user"))
						{
							MsgObj->TryGetStringField(TEXT("content"), UserMessage);
							break;
						}
					}
				}

				if (!UserMessage.IsEmpty())
				{
					bool bUserWantsImage = UserMessage.Contains(TEXT("画")) ||
					                       UserMessage.Contains(TEXT("生成图片")) ||
					                       UserMessage.Contains(TEXT("生图")) ||
					                       UserMessage.Contains(TEXT("绘图")) ||
					                       UserMessage.Contains(TEXT("image")) ||
					                       UserMessage.Contains(TEXT("picture")) ||
					                       UserMessage.Contains(TEXT("photo")) ||
					                       UserMessage.Contains(TEXT("生成图片")) ||
					                       UserMessage.Contains(TEXT("帮我画")) ||
					                       UserMessage.Contains(TEXT("画一个")) ||
					                       UserMessage.Contains(TEXT("画一张")) ||
					                       UserMessage.Contains(TEXT("给我画"));

					if (bUserWantsImage)
					{
						bPseudoToolCall = true;
						UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] User intent detected: wants image generation, auto-triggering generate_image"));
					}
				}
			}

			if (bPseudoToolCall)
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Pseudo tool-call detected: LLM said it would call a tool but tool_calls=0."));

				// ── 尝试从 AI 的文字描述中提取工具调用信息 ──
				FString DetectedToolName;
				if (Content.Contains(TEXT("generate_image")))
					DetectedToolName = TEXT("generate_image");
				else if (Content.Contains(TEXT("screenshot")))
					DetectedToolName = TEXT("screenshot");
				else if (Content.Contains(TEXT("call_tool")))
					DetectedToolName = TEXT("call_tool");
				else if (Content.Contains(TEXT("list_directory")))
					DetectedToolName = TEXT("list_directory");
				else if (Content.Contains(TEXT("search_codebase")))
					DetectedToolName = TEXT("search_codebase");
				else if (Content.Contains(TEXT("batch_read_files")))
					DetectedToolName = TEXT("batch_read_files");
				else if (Content.Contains(TEXT("glob_search")))
					DetectedToolName = TEXT("glob_search");
				else if (Content.Contains(TEXT("command")))
					DetectedToolName = TEXT("command");

				if (DetectedToolName.IsEmpty())
				{
					FString UserMessage;
					for (int32 i = SentMessages.Num() - 1; i >= 0; i--)
					{
						TSharedPtr<FJsonObject> MsgObj = SentMessages[i]->AsObject();
						if (MsgObj.IsValid())
						{
							FString Role;
							MsgObj->TryGetStringField(TEXT("role"), Role);
							if (Role == TEXT("user"))
							{
								MsgObj->TryGetStringField(TEXT("content"), UserMessage);
								break;
							}
						}
					}

					bool bUserWantsImage = UserMessage.Contains(TEXT("画")) ||
					                       UserMessage.Contains(TEXT("生成图片")) ||
					                       UserMessage.Contains(TEXT("生图")) ||
					                       UserMessage.Contains(TEXT("绘图")) ||
					                       UserMessage.Contains(TEXT("image")) ||
					                       UserMessage.Contains(TEXT("picture")) ||
					                       UserMessage.Contains(TEXT("photo")) ||
					                       UserMessage.Contains(TEXT("帮我画")) ||
					                       UserMessage.Contains(TEXT("画一个")) ||
					                       UserMessage.Contains(TEXT("画一张")) ||
					                       UserMessage.Contains(TEXT("给我画"));

					if (bUserWantsImage)
					{
						DetectedToolName = TEXT("generate_image");
						UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] User wants image, auto-setting tool: generate_image"));
					}
				}

				if (!DetectedToolName.IsEmpty())
				{
					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Extracted tool from content: %s"), *DetectedToolName);

					// Show the LLM's text reply
					FMCPToolboxChatMessage Reply;
					Reply.Role = EMCPToolboxMessageRole::Assistant;
					Reply.Content = Content;
					AddMessage(Reply);

					// ── 构造工具调用参数 ──
					FString FuncArgs;
					
					if (DetectedToolName == TEXT("generate_image"))
					{
						// 从用户最近的消息中提取提示词
						FString UserPrompt;
						if (SentMessages.Num() > 0)
						{
							TSharedPtr<FJsonObject> LastMsg = SentMessages.Last()->AsObject();
							if (LastMsg.IsValid())
							{
								FString Role;
								LastMsg->TryGetStringField(TEXT("role"), Role);
								if (Role == TEXT("user"))
								{
									LastMsg->TryGetStringField(TEXT("content"), UserPrompt);
								}
							}
						}

						// 如果没有提取到用户消息，使用 AI 回复中的描述
						if (UserPrompt.IsEmpty())
						{
							UserPrompt = Content;
						}

						// 构造 generate_image 的参数
						TSharedPtr<FJsonObject> ArgsObj = MakeShareable(new FJsonObject());
						ArgsObj->SetStringField(TEXT("prompt"), UserPrompt);
						FString OutputJson;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
						FJsonSerializer::Serialize(ArgsObj.ToSharedRef(), Writer);
						FuncArgs = OutputJson;
					}
					else
					{
						// 其他工具暂时用空参数，或尝试从内容中提取
						FuncArgs = TEXT("{}");
					}

					// ── 执行工具调用 ──
					TSharedPtr<int32> PendingMCP = MakeShared<int32>(0);
					TSharedPtr<TArray<TSharedPtr<FJsonValue>>> PendingMsgs = MakeShared<TArray<TSharedPtr<FJsonValue>>>(SentMessages);

					bool bIsMCP = MCPServerClient.IsConnected() && MCPServerClient.IsMCPTool(DetectedToolName);

					if (bIsMCP)
					{
						(*PendingMCP)++;
						FString NameCap = DetectedToolName;
						FString TCId = FGuid::NewGuid().ToString();
						LaunchIdleSpec(DetectedToolName);

						MCPServerClient.ExecuteTool(DetectedToolName, FuncArgs,
							[this, PendingMCP, PendingMsgs, NameCap, TCId](bool bOk, const FString& R)
							{
								AsyncTask(ENamedThreads::GameThread,
									[this, PendingMCP, PendingMsgs, NameCap, TCId, R]()
								{
									TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
									ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
									ToolResultMsg->SetStringField(TEXT("tool_call_id"), TCId);
									ToolResultMsg->SetStringField(TEXT("name"), NameCap);
									ToolResultMsg->SetStringField(TEXT("content"), R);
									PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

									FMCPToolboxChatMessage ResultMsg;
									ResultMsg.Role = EMCPToolboxMessageRole::System;
									ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *R.Left(2000));
									AddMessage(ResultMsg);

									(*PendingMCP)--;
									if (*PendingMCP <= 0)
									{
										if (bSpeculationPending && LastSpeculation.IsValid() == false)
										{
											bPendingToolCompletion = true;
										}
										else
										{
											TrySpeculativeOrContinue(PendingMsgs);
										}
									}
								});
							});
					}
					else
					{
						FString Result = ExecuteToolCall(DetectedToolName, FuncArgs);

						FMCPToolboxChatMessage ResultMsg;
						ResultMsg.Role = EMCPToolboxMessageRole::System;
						ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *DetectedToolName, *Result.Left(500));
						AddMessage(ResultMsg);

						TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
						ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
						ToolResultMsg->SetStringField(TEXT("tool_call_id"), TEXT(""));
						ToolResultMsg->SetStringField(TEXT("name"), DetectedToolName);
						ToolResultMsg->SetStringField(TEXT("content"), Result);
						PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

						if (DetectedToolName == TEXT("generate_image"))
						{
							TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
							TSharedPtr<FJsonObject> ResultObj;
							if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
							{
								FString ImageURL, ImageData, Status;
								ResultObj->TryGetStringField(TEXT("status"), Status);
								ResultObj->TryGetStringField(TEXT("image_url"), ImageURL);
								ResultObj->TryGetStringField(TEXT("image_data"), ImageData);

								if (Status == TEXT("ok") && (!ImageURL.IsEmpty() || !ImageData.IsEmpty()))
								{
									FString DisplayURL = ImageURL.IsEmpty() ? ImageData : ImageURL;

									TArray<TSharedPtr<FJsonValue>> ContentArray;

									TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
									TextPart->SetStringField(TEXT("type"), TEXT("text"));
									TextPart->SetStringField(TEXT("text"), TEXT("图片已生成，请分析。"));
									ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));

									TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject());
									ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));

									TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject());
									ImageUrlObj->SetStringField(TEXT("url"), DisplayURL);
									ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);

									ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));

									TSharedPtr<FJsonObject> ImageUserMsg = MakeShareable(new FJsonObject());
									ImageUserMsg->SetStringField(TEXT("role"), TEXT("user"));
									ImageUserMsg->SetArrayField(TEXT("content"), ContentArray);
									PendingMsgs->Add(MakeShareable(new FJsonValueObject(ImageUserMsg)));

									FMCPToolboxChatMessage ImageMsg;
									ImageMsg.Role = EMCPToolboxMessageRole::User;
									ImageMsg.Content = TEXT("（图片已生成）");
									ImageMsg.bHasImageAttachment = true;
									ImageMsg.ImageDataURIs.Add(DisplayURL);
									ImageMsg.ImageFileNames.Add(TEXT("generated.png"));
									AddMessage(ImageMsg);
								}
							}
						}

						SendAIRequest(*PendingMsgs);
					}
					return;
				}

				// 如果没有提取到工具名称，回退到重试模式
				PseudoToolCallRetries++;

				FMCPToolboxChatMessage Reply;
				Reply.Role = EMCPToolboxMessageRole::Assistant;
				Reply.Content = Content;
				AddMessage(Reply);

				TArray<TSharedPtr<FJsonValue>> RetryMsgs = SentMessages;
				TSharedPtr<FJsonObject> AsstMsg = MakeShareable(new FJsonObject());
				AsstMsg->SetStringField(TEXT("role"), TEXT("assistant"));
				AsstMsg->SetStringField(TEXT("content"), Content);
				AsstMsg->SetStringField(TEXT("reasoning_content"), TEXT(""));
				RetryMsgs.Add(MakeShareable(new FJsonValueObject(AsstMsg)));

				TSharedPtr<FJsonObject> NudgeMsg = MakeShareable(new FJsonObject());
				NudgeMsg->SetStringField(TEXT("role"), TEXT("user"));
				NudgeMsg->SetStringField(TEXT("content"),
					TEXT("你刚才说要调用工具，但没有在 tool_calls 字段中生成实际的工具调用（finish_reason=stop）。"
					     "请立即用 tool_calls 字段调用你刚才提到的工具，不要再用文字描述。"
					     "如果你已经完成了任务不需要调用工具，请直接给出最终答复。"));
				RetryMsgs.Add(MakeShareable(new FJsonValueObject(NudgeMsg)));

				bIsWaiting = true;
				SendAIRequest(RetryMsgs);
				return;
			}

			// ── JSON tool-call extraction from content ──
			// Some LLMs output tool_calls as JSON text in content field instead of structured tool_calls.
			// Try to extract and parse them.
			// Support formats:
			// 1. {"tool_calls": [...]} - standard format
			// 2. [json][{...}] - AI wrapped format
			// 3. [{...}] - direct array format
			if (Content.Contains(TEXT("tool_calls")) && Content.Contains(TEXT("[")) && Content.Contains(TEXT("]")))
			{
				TArray<TSharedPtr<FJsonValue>> ExtractedToolCalls;
				FString TextPart = Content;

				// Try to find the JSON array
				int32 ArrayStart = Content.Find(TEXT("["));
				int32 ArrayEnd = Content.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);

				if (ArrayStart != INDEX_NONE && ArrayEnd != INDEX_NONE && ArrayStart < ArrayEnd)
				{
					FString JsonStr = Content.Mid(ArrayStart, ArrayEnd - ArrayStart + 1);
					
					// First try to parse as array directly
					TArray<TSharedPtr<FJsonValue>> JsonArray;
					TSharedRef<TJsonReader<>> ArrayReader = TJsonReaderFactory<>::Create(JsonStr);
					
					if (FJsonSerializer::Deserialize(ArrayReader, JsonArray) && JsonArray.Num() > 0)
					{
						// Success! Found a direct array format
						ExtractedToolCalls = JsonArray;
						TextPart = Content.Left(ArrayStart).TrimEnd();
						UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Extracted tool_calls from content (array format): %d calls"), ExtractedToolCalls.Num());
					}
					else
					{
						// Try to parse as object with tool_calls field
						TSharedPtr<FJsonObject> JsonObj;
						TSharedRef<TJsonReader<>> ObjReader = TJsonReaderFactory<>::Create(JsonStr);
						if (FJsonSerializer::Deserialize(ObjReader, JsonObj) && JsonObj.IsValid())
						{
							const TArray<TSharedPtr<FJsonValue>>* ToolCallsPtr;
							if (JsonObj->TryGetArrayField(TEXT("tool_calls"), ToolCallsPtr) && ToolCallsPtr->Num() > 0)
							{
								ExtractedToolCalls = *ToolCallsPtr;
								TextPart = Content.Left(ArrayStart).TrimEnd();
								UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Extracted tool_calls from content (object format): %d calls"), ExtractedToolCalls.Num());
							}
						}
					}
				}

				if (ExtractedToolCalls.Num() > 0)
				{
					UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Extracted tool_calls from content: %d calls"), ExtractedToolCalls.Num());

					// Show the LLM's text reply first
					if (!TextPart.IsEmpty())
					{
						FMCPToolboxChatMessage Reply;
						Reply.Role = EMCPToolboxMessageRole::Assistant;
						Reply.Content = TextPart;
						AddMessage(Reply);
					}

					// Build new messages: old messages + assistant(extracted tool_calls)
					TArray<TSharedPtr<FJsonValue>> NewMsgs = SentMessages;

					TSharedPtr<FJsonObject> AsstMsg = MakeShareable(new FJsonObject());
					AsstMsg->SetStringField(TEXT("role"), TEXT("assistant"));
					AsstMsg->SetStringField(TEXT("content"), TEXT(""));
					AsstMsg->SetArrayField(TEXT("tool_calls"), ExtractedToolCalls);
					NewMsgs.Add(MakeShareable(new FJsonValueObject(AsstMsg)));

					// Execute the extracted tool calls
					TSharedPtr<int32> PendingMCP = MakeShared<int32>(0);
					TSharedPtr<TArray<TSharedPtr<FJsonValue>>> PendingMsgs = MakeShared<TArray<TSharedPtr<FJsonValue>>>(MoveTemp(NewMsgs));
					TSharedPtr<TArray<TSharedPtr<FJsonObject>>> PendingToolCallObjs = MakeShared<TArray<TSharedPtr<FJsonObject>>>();

					for (int32 TIdx = 0; TIdx < ExtractedToolCalls.Num(); ++TIdx)
					{
						TSharedPtr<FJsonObject> TCObj = ExtractedToolCalls[TIdx]->AsObject();
						if (TCObj.IsValid()) PendingToolCallObjs->Add(TCObj);
					}

					for (const auto& TCObj : *PendingToolCallObjs)
					{
						if (!TCObj.IsValid()) continue;
						FString TCId;
						TCObj->TryGetStringField(TEXT("id"), TCId);

						FString FuncName, FuncArgs;
						const TSharedPtr<FJsonObject>* Func;
						if (TCObj->TryGetObjectField(TEXT("function"), Func) && Func->IsValid())
						{
							(*Func)->TryGetStringField(TEXT("name"), FuncName);
							(*Func)->TryGetStringField(TEXT("arguments"), FuncArgs);
						}
						else
						{
							TCObj->TryGetStringField(TEXT("name"), FuncName);
							const TSharedPtr<FJsonObject>* ArgsObj;
							if (TCObj->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && (*ArgsObj).IsValid())
							{
								FString OutputJson;
								TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
								FJsonSerializer::Serialize((*ArgsObj).ToSharedRef(), Writer);
								FuncArgs = OutputJson;
							}
						}

						if (FuncName.IsEmpty()) continue;

						bool bIsMCP = MCPServerClient.IsConnected() && MCPServerClient.IsMCPTool(FuncName);

						if (bIsMCP)
						{
							(*PendingMCP)++;
							FString NameCap = FuncName;
							FString IdCap = TCId;
							LaunchIdleSpec(FuncName);

							MCPServerClient.ExecuteTool(FuncName, FuncArgs,
								[this, PendingMCP, PendingMsgs, NameCap, IdCap](bool bOk, const FString& R)
								{
									AsyncTask(ENamedThreads::GameThread,
										[this, PendingMCP, PendingMsgs, NameCap, IdCap, R]()
									{
										TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
										ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
										ToolResultMsg->SetStringField(TEXT("tool_call_id"), IdCap);
										ToolResultMsg->SetStringField(TEXT("name"), NameCap);
										ToolResultMsg->SetStringField(TEXT("content"), R);
										PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

										FMCPToolboxChatMessage ResultMsg;
										ResultMsg.Role = EMCPToolboxMessageRole::System;
										ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *R.Left(2000));
										AddMessage(ResultMsg);

										(*PendingMCP)--;
										if (*PendingMCP <= 0)
										{
											if (bSpeculationPending && LastSpeculation.IsValid() == false)
											{
												bPendingToolCompletion = true;
											}
											else
											{
												TrySpeculativeOrContinue(PendingMsgs);
											}
										}
									});
								});
						}
						else
						{
							FString Result = ExecuteToolCall(FuncName, FuncArgs);

							FMCPToolboxChatMessage ResultMsg;
							ResultMsg.Role = EMCPToolboxMessageRole::System;
							ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *FuncName, *Result.Left(500));
							AddMessage(ResultMsg);

							TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
							ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
							ToolResultMsg->SetStringField(TEXT("tool_call_id"), TCId);
							ToolResultMsg->SetStringField(TEXT("name"), FuncName);
							ToolResultMsg->SetStringField(TEXT("content"), Result);
							PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

							if (FuncName == TEXT("generate_image"))
							{
								TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
								TSharedPtr<FJsonObject> ResultObj;
								if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
								{
									FString ImageURL, ImageData, Status;
									ResultObj->TryGetStringField(TEXT("status"), Status);
									ResultObj->TryGetStringField(TEXT("image_url"), ImageURL);
									ResultObj->TryGetStringField(TEXT("image_data"), ImageData);

									FString DisplayURL = ImageURL.IsEmpty() ? ImageData : ImageURL;
									if (!DisplayURL.IsEmpty() && Status == TEXT("ok"))
									{
										FMCPToolboxChatMessage ImageMsg;
										ImageMsg.Role = EMCPToolboxMessageRole::Assistant;
										ImageMsg.Content = TEXT("**生图成功！**");
										ImageMsg.bHasImageAttachment = true;
										ImageMsg.ImageDataURIs.Add(DisplayURL);
										ImageMsg.ImageFileNames.Add(TEXT("generated.png"));
										AddMessage(ImageMsg);
									}
								}
							}
						}
					}

					if (*PendingMCP <= 0)
					{
						SendAIRequest(*PendingMsgs);
					}
					return;
				}
			}
		}

		FMCPToolboxChatMessage Reply;
		Reply.Role = EMCPToolboxMessageRole::Assistant;
		Reply.Content = Content;
		AddMessage(Reply);
		FMCPToolboxMemoryManager::Get().ExtractMemoriesFromResponse(Content);
		bSystemPromptDirty = true;  // memory changed
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 响应完成, %d chars, finish=%s"), Content.Len(), *FinishReason);

		// If finish_reason is "length", AI was cut off — ask it to continue
		if (FinishReason == TEXT("length"))
		{
			TArray<TSharedPtr<FJsonValue>> ContinueMsgs = SentMessages;
			TSharedPtr<FJsonObject> AsstMsg = MakeShareable(new FJsonObject());
			AsstMsg->SetStringField(TEXT("role"), TEXT("assistant"));
			AsstMsg->SetStringField(TEXT("content"), Content);
			AsstMsg->SetStringField(TEXT("reasoning_content"), TEXT(""));
			ContinueMsgs.Add(MakeShareable(new FJsonValueObject(AsstMsg)));

			TSharedPtr<FJsonObject> ContMsg = MakeShareable(new FJsonObject());
			ContMsg->SetStringField(TEXT("role"), TEXT("user"));
			ContMsg->SetStringField(TEXT("content"), TEXT("请继续。"));
			ContinueMsgs.Add(MakeShareable(new FJsonValueObject(ContMsg)));

			bIsWaiting = true;
			SendAIRequest(ContinueMsgs);
			return;
		}

		bIsWaiting = false;
	}
	else
	{
		// Unknown: show raw for debugging
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = FString::Printf(TEXT("AI返回空内容 (finish_reason=%s):\n\n```\n%s\n```"), *FinishReason, *RespBody.Left(400));
		AddMessage(Err);
		bIsWaiting = false;
	}
}

// ============================================================================
// Execute Tool Call — local FunctionTable or sync MCP bridge
// ============================================================================
FString SMCPToolboxChatWidget::ExecuteToolCall(const FString& ToolName, const FString& ArgsJson)
{
	// ── Stage 4: 结构化错误码 ──
	if (!ToolFunctionTable.IsValid())
		return MCPToolboxErrorFormat::FormatGenericError(EMCPToolboxErrorCode::InternalError, TEXT("FunctionTable not initialized"));

	// ── Stage 5: JsonValueHelper 容错层 ──
	// 先用 UE 的 JSON 解析器解析 ArgsJson → FJsonObject
	// 然后对 FJsonObject 应用 CoerceObject(字符串→强类型自动升级)
	// 最后序列化回字符串,再用 nlohmann::json 解析传给 FunctionTable
	FString CoercedArgsJson = ArgsJson;
	if (!ArgsJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> ParsedObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(Reader, ParsedObj) && ParsedObj.IsValid())
		{
			// 应用 CoerceObject — 递归将字符串值升级为强类型
			TSharedPtr<FJsonObject> CoercedObj = FMCPToolboxJsonValueHelper::CoerceObject(ParsedObj);

			// 序列化回字符串
			FString OutputJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
			FJsonSerializer::Serialize(CoercedObj.ToSharedRef(), Writer);
			CoercedArgsJson = OutputJson;
		}
		// 如果 UE JSON 解析失败,回退到原始 ArgsJson(nlohmann 会处理错误)
	}

	// Parse args
	assistant::json Args;
	try
	{
		if (!CoercedArgsJson.IsEmpty())
			Args = assistant::json::parse(TCHAR_TO_UTF8(*CoercedArgsJson));
		else
			Args = assistant::json::object();
	}
	catch (const std::exception&)
	{
		return MCPToolboxErrorFormat::FormatParamError(
			EMCPToolboxErrorCode::ParamInvalidType,
			TEXT("arguments"),
			TEXT("object"),
			ArgsJson,
			TEXT("{\"example\":\"{\\\"name\\\":\\\"value\\\"}\"}")
		);
	}
	catch (...)
	{
		return MCPToolboxErrorFormat::FormatParamError(
			EMCPToolboxErrorCode::ParamInvalidType,
			TEXT("arguments"),
			TEXT("object"),
			ArgsJson
		);
	}

	// ── Stage 4: 工具调用(FunctionTable.Call 内部处理不存在情况) ──
	assistant::FunctionCall FC;
	FC.name = TCHAR_TO_UTF8(*ToolName);
	FC.args = Args;
	assistant::FunctionResult Result = ToolFunctionTable->Call(FC);

	// 如果工具返回错误,包装为结构化错误码(如果还不是结构化的)
	if (Result.isError)
	{
		// 检查是否已经是结构化错误(含 error_code 字段)
		FString ResultStr = UTF8_TO_TCHAR(Result.text.c_str());
		if (!ResultStr.Contains(TEXT("error_code")))
		{
			// 包装为 TOOL_EXECUTION_FAILED
			return MCPToolboxErrorFormat::FormatToolError(
				EMCPToolboxErrorCode::ToolExecutionFailed,
				ToolName,
				ResultStr
			);
		}
	}

	return UTF8_TO_TCHAR(Result.text.c_str());
}

// ============================================================================
// MCP Server Connection
// ============================================================================
void SMCPToolboxChatWidget::ConnectToMCPServer()
{
	if (MCPServerClient.IsConnected()) return;

	MCPServerClient.OnConnected.AddLambda([this]()
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] MCP connected, listing tools..."));
		RefreshMCPTools();
	});

	MCPServerClient.OnDisconnected.AddLambda([]()
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] MCP disconnected"));
	});

	MCPServerClient.Connect();
}

void SMCPToolboxChatWidget::RefreshMCPTools()
{
	if (!MCPServerClient.IsConnected())
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] RefreshMCPTools: MCP not connected"));
		return;
	}

	// Send tools/list to get all available tools
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
	// We call SendJsonRpc directly — simplest path, SSE parsing handles response
	MCPServerClient.CallDirectMethod(TEXT("tools/list"), Params,
		[this](bool bSuccess, const TSharedPtr<FJsonObject>& Result)
		{
			if (!bSuccess || !Result.IsValid())
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] tools/list failed"));
				return;
			}

			// Parse tools array from result
			TArray<TSharedPtr<FJsonObject>> DiscoveredTools;
			const TArray<TSharedPtr<FJsonValue>>* ToolArray;
			if (Result->TryGetArrayField(TEXT("tools"), ToolArray))
			{
				for (const auto& Val : *ToolArray)
				{
					TSharedPtr<FJsonObject> ToolObj = Val->AsObject();
					if (ToolObj.IsValid())
						DiscoveredTools.Add(ToolObj);
				}
			}

			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] tools/list: %d tools"), DiscoveredTools.Num());

			if (DiscoveredTools.Num() > 0)
			{
				MCPServerClient.GetTools().Empty();
				MCPServerClient.GetTools().Append(DiscoveredTools);
				MCPServerClient.RebuildToolNameSet();
			}

			MergeMCPTools();

			// Build cached MCP tool descriptions markdown — avoids repeated list_toolsets/describe_toolset queries
			CachedMCPToolDescriptionsMD.Empty();
			if (DiscoveredTools.Num() > 0)
			{
				CachedMCPToolDescriptionsMD += TEXT("## 已发现MCP工具\n\n");
				CachedMCPToolDescriptionsMD += TEXT("以下工具已可用，直接调用 call_tool 即可。\n\n");
				for (const auto& Tool : DiscoveredTools)
				{
					FString Name, Desc;
					Tool->TryGetStringField(TEXT("name"), Name);
					if (const TSharedPtr<FJsonObject>* Schema; Tool->TryGetObjectField(TEXT("inputSchema"), Schema))
						(*Schema)->TryGetStringField(TEXT("description"), Desc);
					if (Name.IsEmpty()) continue;
					CachedMCPToolDescriptionsMD += FString::Printf(TEXT("- **%s**"), *Name);
					if (!Desc.IsEmpty())
						CachedMCPToolDescriptionsMD += FString::Printf(TEXT(": %s"), *Desc);
					CachedMCPToolDescriptionsMD += TEXT("\n");
				}
				UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] MCP tool cache built (%d tools, %d chars)"), DiscoveredTools.Num(), CachedMCPToolDescriptionsMD.Len());
			}
		});
}

void SMCPToolboxChatWidget::MergeMCPTools()
{
	if (!ToolFunctionTable.IsValid()) return;

	// ══ Top-level meta-tools that should NOT be exposed to the LLM ══
	// list_toolsets/describe_toolset waste ~14s per call — tools are already cached in system prompt
	static const TSet<FString> BlockedMetaTools = {
		TEXT("list_toolsets"),
		TEXT("describe_toolset"),
	};

	const TArray<TSharedPtr<FJsonObject>>& McpTools = MCPServerClient.GetTools();
	for (const auto& ToolDef : McpTools)
	{
		FString ToolName;
		ToolDef->TryGetStringField(TEXT("name"), ToolName);
		if (ToolName.IsEmpty()) continue;

		// Skip meta-tools — LLM should NOT call these (tools are pre-cached in system prompt)
		if (BlockedMetaTools.Contains(ToolName))
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Skipped meta-tool: %s"), *ToolName);
			continue;
		}

		FString ToolDesc;
		ToolDef->TryGetStringField(TEXT("description"), ToolDesc);

		// MCP tools are now dispatched directly in HandleAIResponse (fully async).
		// This FunctionTable callback provides schema info only.
		if (ToolName == TEXT("call_tool"))
		{
			// ── 关键修复：为 call_tool 注册精确的参数 schema ──
			// 之前仅 SetDescription，LLM 看不到参数结构 → 把 tool_name 误并入 toolset_name
			// (日志: Toolset 'editor_toolset.toolsets.material.MaterialTools.create_material' not found)
			FString CallToolDesc = ToolDesc;
			CallToolDesc += TEXT("\n\n**关键**: toolset_name 是 toolset 的完整路径(如 'editor_toolset.toolsets.material.MaterialTools')");
			CallToolDesc += TEXT("，tool_name 是该 toolset 中的具体工具名(如 'CreateMaterial')。");
			CallToolDesc += TEXT("不要把 tool_name 拼进 toolset_name。所有可用 toolset 和 tool 见下方'已发现MCP工具'章节或 .mcptoolbox/ 缓存。");

			ToolFunctionTable->Add(assistant::FunctionBuilder("call_tool")
				.SetDescription(TCHAR_TO_UTF8(*CallToolDesc))
				.AddRequiredParam("toolset_name",
					"完整 toolset 路径，如 'editor_toolset.toolsets.material.MaterialTools'。不含 tool_name。",
					"string")
				.AddRequiredParam("tool_name",
					"toolset 中的具体工具名，如 'CreateMaterial'。",
					"string")
				.AddRequiredParam("arguments",
					"工具参数对象。具体 schema 见下方'已发现MCP工具'章节或 .mcptoolbox/ 缓存。",
					"object")
				.SetCallback([](const assistant::json& Args) -> assistant::FunctionResult
				{
					return assistant::FunctionResult{
						.isError = false,
						.text = R"({"status":"dispatched"})"
					};
				}).Build());
		}
		else
		{
			ToolFunctionTable->Add(assistant::FunctionBuilder(TCHAR_TO_UTF8(*ToolName))
				.SetDescription(TCHAR_TO_UTF8(*ToolDesc))
				.SetCallback([](const assistant::json& Args) -> assistant::FunctionResult
				{
					return assistant::FunctionResult{
						.isError = false,
						.text = R"({"status":"dispatched"})"
					};
				}).Build());
		}

		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Registered MCP tool: %s"), *ToolName);
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Total tools after MCP merge: %d"), ToolFunctionTable->GetFunctionsCount());

	// Rebuild cached tools array
	CachedToolsArray.Empty();
	assistant::json ToolsJson = ToolFunctionTable->ToJSON(assistant::EndpointKind::ollama, assistant::CachePolicy::kNone);
	for (const auto& tool : ToolsJson)
	{
		FString ToolStr = UTF8_TO_TCHAR(tool.dump().c_str());
		TSharedPtr<FJsonObject> ToolObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolStr);
		if (FJsonSerializer::Deserialize(Reader, ToolObj) && ToolObj.IsValid())
			CachedToolsArray.Add(MakeShareable(new FJsonValueObject(ToolObj)));
	}
}
FReply SMCPToolboxChatWidget::OnInterrupt()
{
	if (!bIsWaiting) return FReply::Handled();

	bInterrupted = true;
	bIsWaiting = false;

	if (ActiveHttpRequest.IsValid())
	{
		ActiveHttpRequest->CancelRequest();
		ActiveHttpRequest.Reset();
	}

	FMCPToolboxChatMessage Info;
	Info.Role = EMCPToolboxMessageRole::System;
	Info.Content = TEXT("已中断当前请求。");
	AddMessage(Info);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Request interrupted by user"));
	return FReply::Handled();
}

// ============================================================================
// System Prompt Builder
// ============================================================================
FString SMCPToolboxChatWidget::BuildSystemPrompt(const FString& MemoryContext)
{
	FString Prompt;
	Prompt.Reserve(8192);

	FString ContentPath = FPaths::ProjectContentDir();
	FString ProjectName = FApp::GetProjectName();
	FString PluginDir = FPaths::GetPath(FPaths::GetPath(FModuleManager::Get().GetModuleFilename("MCPToolbox")));

	// ponytail: resolve PluginDir from uplugin file for reliability
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MCPToolbox"));
	if (Plugin.IsValid()) PluginDir = Plugin->GetBaseDir();

	// ── Load prompt fragments from MD files (ponytail: keep prompts out of C++) ──
	auto LoadPromptFile = [&](const FString& Filename) -> FString {
		FString Path = PluginDir / TEXT("Prompts") / Filename;
		FString Content;
		if (FFileHelper::LoadFileToString(Content, *Path))
		{
			Content.ReplaceInline(TEXT("{{PROJECT_NAME}}"), *ProjectName);
			Content.ReplaceInline(TEXT("{{CONTENT_PATH}}"), *ContentPath);
		}
		return Content;
	};

	Prompt += LoadPromptFile(TEXT("system_base.md"));
	Prompt += TEXT("\n\n");
	Prompt += LoadPromptFile(TEXT("image_gen.md"));
	Prompt += TEXT("\n\n");
	Prompt += LoadPromptFile(TEXT("tools_local.md"));
	Prompt += TEXT("\n\n");

	// ── MCP Toolset 索引(从 .mcptoolbox/index.md 加载,~2K tokens) ──
	// 这是唯一注入的 toolset 信息源。详细 schema 由 get_skills 懒加载(阶段 2)。
	Prompt += TEXT("## MCP Toolset 索引\n\n");
	Prompt += SkillService.LoadSkillIndex();
	Prompt += TEXT("\n\n");

	// ── 调用规则(从 .mcptoolbox/rules.md 加载,~3K tokens,WRONG/CORRECT 范式) ──
	// 含 call_tool 约定、路径格式、批量调用、DAG 并行、记忆触发词、AI 行为准则(八荣八耻)
	Prompt += TEXT("## 调用规则与行为准则\n\n");
	Prompt += SkillService.LoadRulesMD();
	Prompt += TEXT("\n\n");

	// Inject cached MCP tool descriptions (built once, avoids repeated list_toolsets queries)
	// 保留作为 fallback — 即使 .mcptoolbox/ 缓存不存在,LLM 仍能基于此调用 call_tool
	if (!CachedMCPToolDescriptionsMD.IsEmpty())
	{
		Prompt += CachedMCPToolDescriptionsMD;
		Prompt += TEXT("\n");
	}

	// ── Inject cached conversation summary (loaded from ~/.mcptoolbox/conversation_summary.md) ──
	// This is the "archive" generated by the user via the "归档总结" button, persisted across restarts.
	if (!CachedToolsSummary.IsEmpty() || !CachedMemorySummary.IsEmpty())
	{
		Prompt += TEXT("## 历史会话归档（从 ~/.mcptoolbox/conversation_summary.md 加载）\n");
		Prompt += TEXT("> 这是用户上次手动归档的会话总结，作为本会话的上下文延续，无需用户重复说明。\n\n");
		if (!CachedToolsSummary.IsEmpty())
		{
			Prompt += TEXT("### 工具使用归档\n");
			Prompt += CachedToolsSummary;
			Prompt += TEXT("\n\n");
		}
		if (!CachedMemorySummary.IsEmpty())
		{
			Prompt += TEXT("### 记忆归档\n");
			Prompt += CachedMemorySummary;
			Prompt += TEXT("\n\n");
		}
	}

	if (!MemoryContext.IsEmpty())
	{
		Prompt += TEXT("## 用户记忆（从持久存储加载）\n");
		Prompt += MemoryContext;
		Prompt += TEXT("\n");
	}

	return Prompt;
}

// ============================================================================
// Register MCP tools via assistant library FunctionBuilder
// ============================================================================
void SMCPToolboxChatWidget::RegisterMCPTools()
{
	if (!ToolFunctionTable.IsValid()) return;

	// Screenshot tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("screenshot")
		.SetDescription("Capture a screenshot of the screen and return the image data. Only available when vision mode is enabled. The AI can see and analyze the screenshot.")
		.AddOptionalParam("mode", "Capture mode: 'desktop' for full screen (default), 'editor' for editor window", "string")
		.AddOptionalParam("width", "Target width (default: 1920, max: 1920)", "number")
		.AddOptionalParam("height", "Target height (default: 1080, max: 1080)", "number")
		.SetCallback([this](const assistant::json& args) -> assistant::FunctionResult {
			if (!bVisionModeEnabled && !FMCPToolboxAuxModelManager::Get().IsReady())
			{
				return {.isError = true, .text = R"RAW({"error":"Vision mode is not enabled. Please enable vision mode first."})RAW"};
			}

			if (!FModuleManager::Get().IsModuleLoaded(TEXT("MCPToolboxScreenshot")))
			{
				FModuleManager::Get().LoadModule(TEXT("MCPToolboxScreenshot"));
			}

			FMCPToolboxScreenshotModule& ScreenshotModule = 
				FModuleManager::GetModuleChecked<FMCPToolboxScreenshotModule>(TEXT("MCPToolboxScreenshot"));

			std::string Mode = args.value("mode", "desktop");
			int32 Width = args.value("width", 1280);
			int32 Height = args.value("height", 720);

			Width = FMath::Clamp(Width, 320, 1920);
			Height = FMath::Clamp(Height, 240, 1080);

			FString Base64Image;
			if (Mode == "desktop" || Mode == "fullscreen")
			{
				Base64Image = ScreenshotModule.CaptureFullDesktop();
			}
			else
			{
				Base64Image = ScreenshotModule.CaptureScreenshot(true, false, Width, Height);
			}

			if (Base64Image.IsEmpty())
			{
				return {.isError = true, .text = R"RAW({"error":"Failed to capture screenshot"})RAW"};
			}

			// Avoid FString::Printf copying large base64; concat manually
			FString ResultJson;
			ResultJson.Reserve(Base64Image.Len() + 128);
			ResultJson += TEXT("{\"status\":\"ok\",\"format\":\"jpeg\",\"width\":");
			ResultJson.AppendInt(Width);
			ResultJson += TEXT(",\"height\":");
			ResultJson.AppendInt(Height);
			ResultJson += TEXT(",\"data_uri\":\"data:image/jpeg;base64,");
			ResultJson += Base64Image;
			ResultJson += TEXT("\"}");

			return {.isError = false, .text = TCHAR_TO_UTF8(*ResultJson)};
		}).Build());

	// Generate image tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("generate_image")
		.SetDescription("Generate images using configured image generation models (DALL-E, SD WebUI, Replicate, Pollinations). "
			"Call this when the user asks to create, draw, or generate images. Returns the generated image URL, base64 data, and saved file path.")
		.AddRequiredParam("prompt", "Detailed description of the image to generate", "string")
		.AddOptionalParam("negative_prompt", "What to exclude from the image (default: empty)", "string")
		.AddOptionalParam("width", "Image width in pixels (default: 512 for local, 1024 for cloud)", "number")
		.AddOptionalParam("height", "Image height in pixels (default: 512 for local, 1024 for cloud)", "number")
		.AddOptionalParam("steps", "Number of generation steps (default: 20, SD WebUI only)", "number")
		.AddOptionalParam("cfg_scale", "CFG scale for guidance (default: 7.0, SD WebUI only)", "number")
		.AddOptionalParam("save_path", "Path to save the generated image. Use \"project:/Textures/\" for project Content directory, \"saved:/Images/\" for project Saved directory, or an absolute path. Must be specified; if omitted, image may be lost after session.", "string")
		.SetCallback([this](const assistant::json& args) -> assistant::FunctionResult {
			std::string Prompt = args.value("prompt", "");
			if (Prompt.empty())
				return {.isError = true, .text = R"({"error":"Missing prompt parameter"})"};

			std::string NegativePrompt = args.value("negative_prompt", "");
			std::string SavePath = args.value("save_path", "saved:/GeneratedImages/");
			int32 Width = args.value("width", 512);
			int32 Height = args.value("height", 512);
			int32 Steps = args.value("steps", 20);
			float CfgScale = args.value("cfg_scale", 7.0f);

			FString Result = GenerateImageSync(
				UTF8_TO_TCHAR(Prompt.c_str()),
				UTF8_TO_TCHAR(NegativePrompt.c_str()),
				Width, Height, Steps, CfgScale,
				UTF8_TO_TCHAR(SavePath.c_str()));

			if (Result.Contains(TEXT("\"error\"")))
				return {.isError = true, .text = TCHAR_TO_UTF8(*Result)};

			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	// Command tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("command")
		.SetDescription("Execute a console command in Unreal Engine")
		.AddRequiredParam("cmd", "The console command to execute", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string cmd = args.value("cmd", "");
			if (cmd.empty()) return {.isError = true, .text = R"({"error":"Missing cmd parameter"})"};
			if (GEditor)
			{
				GEditor->Exec(nullptr, UTF8_TO_TCHAR(cmd.c_str()));
				return {.isError = false, .text = R"({"cmd":")" + cmd + R"(","output":"Command executed."})"};
			}
			return {.isError = true, .text = R"({"error":"Editor not available"})"};
		}).Build());

	// Select tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("select")
		.SetDescription("Select an Actor in the level by name")
		.AddRequiredParam("name", "Name of the Actor to select", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string name = args.value("name", "");
			if (name.empty()) return {.isError = true, .text = R"({"error":"Missing name parameter"})"};

			FString ActorName = UTF8_TO_TCHAR(name.c_str());
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
				return {.isError = true, .text = R"({"error":"No world available"})"};

			// Find actor by name
			bool bFound = false;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetName().Contains(ActorName))
				{
					GEditor->SelectNone(false, true);
					GEditor->SelectActor(*It, true, true);
					bFound = true;
					break;
				}
			}

			if (bFound)
				return {.isError = false, .text = R"({"status":"ok","selected":")" + name + "\"}"};
			return {.isError = true, .text = R"({"error":"Actor not found: ")" + name + "\"}"};
		}).Build());

	// Inspect tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("inspect")
		.SetDescription("Inspect properties of the currently selected Actor(s)")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			if (!GEditor)
				return {.isError = true, .text = R"({"error":"Editor not available"})"};

			int32 SelCount = GEditor->GetSelectedActorCount();
			if (SelCount == 0)
				return {.isError = true, .text = R"({"error":"No Actor selected. Use select tool first."})"};

			std::string Result = R"({"status":"ok","selected_count":)" + std::to_string(SelCount) + R"(,"actors":[)";

			USelection* Sel = GEditor->GetSelectedActors();
			for (int32 i = 0; i < Sel->Num(); ++i)
			{
				AActor* Actor = Cast<AActor>(Sel->GetSelectedObject(i));
				if (Actor)
				{
					FString Name = Actor->GetName();
					FVector Loc = Actor->GetActorLocation();
					FString ClassName = Actor->GetClass()->GetName();

					if (i > 0) Result += ",";
					Result += R"({"name":")" + std::string(TCHAR_TO_UTF8(*Name)) + R"(",)"
						+ R"("class":")" + std::string(TCHAR_TO_UTF8(*ClassName)) + R"(",)"
						+ R"("location":{)" + R"("x":)" + std::to_string(Loc.X)
						+ R"(,"y":)" + std::to_string(Loc.Y)
						+ R"(,"z":)" + std::to_string(Loc.Z) + "}}";
				}
			}
			Result += "]}";
			return {.isError = false, .text = Result};
		}).Build());

	// Batch read files tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("batch_read_files")
		.SetDescription("Read multiple files at once. Returns contents of all files in a single response.")
		.AddRequiredParam("file_paths", "Array of absolute file paths to read", "array")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			if (!args.contains("file_paths") || !args["file_paths"].is_array())
				return {.isError = true, .text = R"({"error":"Missing file_paths parameter (must be array)"})"};

			assistant::json Result;
			Result["status"] = "ok";
			Result["files"] = assistant::json::array();

			for (const auto& PathItem : args["file_paths"])
			{
				if (!PathItem.is_string()) continue;
				FString FilePath = UTF8_TO_TCHAR(PathItem.get<std::string>().c_str());
				FString Content;
				bool bSuccess = FFileHelper::LoadFileToString(Content, *FilePath);

				assistant::json FileResult;
				FileResult["path"] = PathItem.get<std::string>();
				if (bSuccess)
				{
					FileResult["success"] = true;
					FileResult["content"] = TCHAR_TO_UTF8(*Content);
				}
				else
				{
					FileResult["success"] = false;
					FileResult["error"] = "Failed to read file";
				}
				Result["files"].push_back(FileResult);
			}
			return {.isError = false, .text = Result.dump()};
		}).Build());

	// Search codebase tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("search_codebase")
		.SetDescription("Search for code patterns across the codebase using substring match. Returns matching files and line numbers.")
		.AddRequiredParam("pattern", "Substring to search for", "string")
		.AddOptionalParam("path", "Directory to search in (defaults to project source directory)", "string")
		.AddOptionalParam("file_pattern", "File glob pattern, e.g. *.cpp, *.h (defaults to *.cpp,*.h)", "string")
		.AddOptionalParam("max_results", "Maximum number of results to return (default 50)", "number")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Pattern = args.value("pattern", "");
			if (Pattern.empty())
				return {.isError = true, .text = R"({"error":"Missing pattern parameter"})"};

			FString BaseDir = args.contains("path") ? UTF8_TO_TCHAR(args["path"].get<std::string>().c_str()) : FPaths::ProjectDir() / TEXT("Source");
			FString FilePatternStr = UTF8_TO_TCHAR(args.value("file_pattern", "*.cpp,*.h").c_str());
			int32 MaxResults = args.value("max_results", 50);

			TArray<FString> FileTypes;
			FilePatternStr.ParseIntoArray(FileTypes, TEXT(","), true);

			FString SearchStr = UTF8_TO_TCHAR(Pattern.c_str());
			assistant::json Result;
			Result["status"] = "ok";
			Result["pattern"] = Pattern;
			Result["results"] = assistant::json::array();
			int32 Count = 0;

			for (const FString& Ext : FileTypes)
			{
				TArray<FString> FoundFiles;
				IFileManager::Get().FindFilesRecursive(FoundFiles, *BaseDir, *Ext.TrimStartAndEnd(), true, false);
				for (const FString& Fp : FoundFiles)
				{
					if (Count >= MaxResults) break;
					FString Content;
					if (!FFileHelper::LoadFileToString(Content, *Fp)) continue;
					TArray<FString> Lines;
					Content.ParseIntoArrayLines(Lines);
					for (int32 Li = 0; Li < Lines.Num() && Count < MaxResults; ++Li)
					{
						if (Lines[Li].Contains(SearchStr))
						{
							assistant::json Hit;
							Hit["file"] = TCHAR_TO_UTF8(*Fp);
							Hit["line"] = Li + 1;
							Hit["content"] = TCHAR_TO_UTF8(*Lines[Li].TrimStartAndEnd());
							Result["results"].push_back(Hit);
							Count++;
						}
					}
				}
			}
			Result["total"] = Count;
			return {.isError = false, .text = Result.dump()};
		}).Build());

	// Glob search tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("glob_search")
		.SetDescription("Search for files matching a glob pattern. Returns list of matching file paths.")
		.AddRequiredParam("pattern", "Glob pattern to match, e.g. *.cpp, **/*.h", "string")
		.AddOptionalParam("path", "Base directory to search from (defaults to project directory)", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Pattern = args.value("pattern", "");
			if (Pattern.empty())
				return {.isError = true, .text = R"({"error":"Missing pattern parameter"})"};

			FString BaseDir = args.contains("path") ? UTF8_TO_TCHAR(args["path"].get<std::string>().c_str()) : FPaths::ProjectDir();
			FString SearchPattern = UTF8_TO_TCHAR(Pattern.c_str());

			TArray<FString> FoundFiles;
			if (SearchPattern.Contains(TEXT("**")))
			{
				FString LeafPattern = FPaths::GetCleanFilename(SearchPattern);
				if (LeafPattern.IsEmpty()) LeafPattern = TEXT("*");
				IFileManager::Get().FindFilesRecursive(FoundFiles, *BaseDir, *LeafPattern, true, false);
			}
			else
			{
				IFileManager::Get().FindFiles(FoundFiles, *(BaseDir / SearchPattern), true, false);
			}

			assistant::json Result;
			Result["status"] = "ok";
			Result["pattern"] = Pattern;
			Result["files"] = assistant::json::array();
			for (int32 i = 0; i < FoundFiles.Num(); ++i)
				Result["files"].push_back(TCHAR_TO_UTF8(*(BaseDir / FoundFiles[i])));
			Result["count"] = FoundFiles.Num();
			return {.isError = false, .text = Result.dump()};
		}).Build());

	// List directory tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("list_directory")
		.SetDescription("List contents of a directory. Returns files and subdirectories.")
		.AddRequiredParam("path", "Directory path to list", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Path = args.value("path", "");
			if (Path.empty())
				return {.isError = true, .text = R"({"error":"Missing path parameter"})"};

			FString DirPath = UTF8_TO_TCHAR(Path.c_str());

			TArray<FString> Files;
			TArray<FString> Dirs;

			IFileManager::Get().FindFiles(Files, *(DirPath / TEXT("*")), true, false);
			IFileManager::Get().FindFiles(Dirs, *(DirPath / TEXT("*")), false, true);

			assistant::json J;
			J["status"] = "ok";
			J["path"] = Path;
			J["directories"] = assistant::json::array();
			J["files"] = assistant::json::array();
			for (int32 i = 0; i < Dirs.Num(); ++i)
				J["directories"].push_back(TCHAR_TO_UTF8(*Dirs[i]));
			for (int32 i = 0; i < Files.Num(); ++i)
				J["files"].push_back(TCHAR_TO_UTF8(*Files[i]));
			return {.isError = false, .text = J.dump()};
		}).Build());

	// ── Stage 2: Skill lazy-loading virtual tools (VibeUE ListSkills/GetSkills protocol) ──
	// list_skills: 列出所有可用 skill 的轻量摘要(name + description + toolset + keywords)
	// 不返回 body — LLM 看完摘要后决定是否需要 get_skills 加载详细内容。
	ToolFunctionTable->Add(assistant::FunctionBuilder("list_skills")
		.SetDescription("List all available MCP toolset skills (lightweight summaries only). "
			"Returns name + description + toolset + keywords for each skill in .mcptoolbox/toolsets/. "
			"Does NOT include the skill body — use get_skills to load detailed content on demand. "
			"Call this FIRST when facing an unfamiliar task to discover available toolsets.")
		.SetCallback([this](const assistant::json& args) -> assistant::FunctionResult {
			FString Result = SkillService.ListSkills();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	// get_skills: 按需加载指定 skill 的完整内容(frontmatter 已剥离)
	// 会话级 LRU 缓存(上限 5),避免重复磁盘 IO。
	ToolFunctionTable->Add(assistant::FunctionBuilder("get_skills")
		.SetDescription("Load detailed content of a specific skill by name. "
			"Returns the full skill body (frontmatter stripped) with complete tool schemas, "
			"examples, and Critical Rules. Results are cached per-session (LRU, max 5). "
			"Use after list_skills to get the detailed documentation for a specific toolset.")
		.AddRequiredParam("skill_name", "Name of the skill to load (from list_skills result)", "string")
		.SetCallback([this](const assistant::json& args) -> assistant::FunctionResult {
			std::string StdName = args.value("skill_name", "");
			if (StdName.empty())
				return {.isError = true, .text = R"({"error":"Missing skill_name parameter"})"};
			FString SkillName = UTF8_TO_TCHAR(StdName.c_str());
			FString Result = SkillService.GetSkill(SkillName);
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	// =====================================================================
	// Transaction Service — 编辑器 Undo/Redo 工具 (Stage 6.1)
	// 移植自 VibeUE UTransactionService,封装 GEditor->Trans
	// 让 agent 可驱动 undo/redo、组合编辑、检查历史、重置缓冲区
	// =====================================================================
	ToolFunctionTable->Add(assistant::FunctionBuilder("undo")
		.SetDescription("Undo the most recent editor transaction (Ctrl+Z equivalent). "
			"Returns {success, undone} where undone=true if an undo was performed.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxTransactionService::Undo();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("redo")
		.SetDescription("Redo the most recently undone transaction (Ctrl+Y equivalent). "
			"Returns {success, redone} where redone=true if a redo was performed.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxTransactionService::Redo();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("undo_multiple")
		.SetDescription("Undo the last N transactions. Returns {success, undone_count}.")
		.AddRequiredParam("count", "Number of transactions to undo (must be positive)", "number")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			int32 Count = args.value("count", 1);
			FString Result = FMCPToolboxTransactionService::UndoMultiple(Count);
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("redo_multiple")
		.SetDescription("Redo the last N undone transactions. Returns {success, redone_count}.")
		.AddRequiredParam("count", "Number of transactions to redo (must be positive)", "number")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			int32 Count = args.value("count", 1);
			FString Result = FMCPToolboxTransactionService::RedoMultiple(Count);
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("begin_transaction")
		.SetDescription("Begin a named transaction so subsequent edits collapse into a single undo step. "
			"Must pair with end_transaction or cancel_transaction. Returns {success, transaction_index}.")
		.AddOptionalParam("description", "Human-readable description of the transaction", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Desc = args.value("description", "");
			FString Result = FMCPToolboxTransactionService::BeginTransaction(UTF8_TO_TCHAR(Desc.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("end_transaction")
		.SetDescription("End the active transaction opened with begin_transaction. Returns {success, transaction_index}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxTransactionService::EndTransaction();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("cancel_transaction")
		.SetDescription("Cancel the active transaction, rolling back everything since begin_transaction. "
			"Implementation: End + Undo trick (GEditor->CancelTransaction only discards the record). "
			"The reverted transaction remains as a redo candidate. Returns {success, rolled_back}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxTransactionService::CancelTransaction();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("get_transaction_state")
		.SetDescription("Get a snapshot of the editor's undo/redo state. "
			"Returns {success, can_undo, can_redo, undo_count, redo_count, next_undo_title, next_redo_title}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxTransactionService::GetState();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("get_transaction_history")
		.SetDescription("List the undo history (most recent last). Each entry has title, queue_index, is_undone. "
			"Returns {success, total_count, returned_count, history:[...]}.")
		.AddOptionalParam("max_entries", "Maximum number of entries to return (default 20, 0=all)", "number")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			int32 Max = args.value("max_entries", 20);
			FString Result = FMCPToolboxTransactionService::GetHistory(Max);
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("reset_transaction_buffer")
		.SetDescription("Reset (clear) the entire undo/redo buffer. Clears HISTORY ONLY — does NOT change "
			"anything in the level/world. Returns {success}.")
		.AddOptionalParam("reason", "Reason for resetting the buffer (logged)", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Reason = args.value("reason", "");
			FString Result = FMCPToolboxTransactionService::ResetBuffer(UTF8_TO_TCHAR(Reason.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	// =====================================================================
	// PerformanceService 工具注册 (Stage 6.2)
	// 移植自 VibeUE UPerformanceService — 帧率诊断 + Unreal Insights trace
	// 推荐流程: frame_timing 第一步(bound 判定) → start_trace → 复现 → stop_trace → analyse
	// =====================================================================

	ToolFunctionTable->Add(assistant::FunctionBuilder("frame_timing")
		.SetDescription("Report Game/Render/RHI/GPU thread ms + a CPU-vs-GPU bound verdict and optimization hint "
			"for the most recently rendered frame (the same data as the on-screen 'stat unit'). "
			"RUN THIS FIRST in any frame-rate investigation — optimising the GPU does nothing if the frame is "
			"game- or render-thread bound. Returns {game_thread_ms, render_thread_ms, rhi_thread_ms, gpu_ms, "
			"frame_ms, fps, bound:GPU|RenderThread|GameThread, hint, pie_running, note}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxPerformanceService::FrameTiming();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("start_trace")
		.SetDescription("Start an Unreal Insights trace to file. Call StopTrace when done, then Analyse to read results. "
			"Returns {status:tracing, trace_file, channels, hint}.")
		.AddOptionalParam("name", "Trace file name (without extension). Empty uses 'mcp_capture'.", "string")
		.AddOptionalParam("channels", "Comma-separated trace channels (frame,cpu,gpu,log,loadtime,object,stats,bookmark,region). Empty uses defaults.", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Name = args.value("name", "");
			std::string Channels = args.value("channels", "");
			FString Result = FMCPToolboxPerformanceService::StartTrace(
				UTF8_TO_TCHAR(Name.c_str()), UTF8_TO_TCHAR(Channels.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("stop_trace")
		.SetDescription("Stop the active trace. Returns the trace file path and size in MB. "
			"Returns {status:stopped, trace_file, file_size_mb, hint}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxPerformanceService::StopTrace();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("get_trace_status")
		.SetDescription("Report whether a trace is active and which channels are enabled. "
			"Returns {tracing, destination, active_channels, last_trace_file}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxPerformanceService::GetTraceStatus();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("trace_bookmark")
		.SetDescription("Drop a named bookmark in the active trace. Useful for marking moments during profiling. "
			"Returns {bookmark, status:ok}.")
		.AddRequiredParam("name", "Bookmark name (visible in Unreal Insights)", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Name = args.value("name", "");
			FString Result = FMCPToolboxPerformanceService::Bookmark(UTF8_TO_TCHAR(Name.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("trace_region_start")
		.SetDescription("Begin a named region in the active trace. Pair with trace_region_end. "
			"Useful for enclosing a workload you want to isolate in the trace timeline. "
			"Returns {region, status:started}.")
		.AddRequiredParam("name", "Region name", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Name = args.value("name", "");
			FString Result = FMCPToolboxPerformanceService::RegionStart(UTF8_TO_TCHAR(Name.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("trace_region_end")
		.SetDescription("End a named region in the active trace. Returns {region, status:ended}.")
		.AddRequiredParam("name", "Region name (must match a region started by trace_region_start)", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Name = args.value("name", "");
			FString Result = FMCPToolboxPerformanceService::RegionEnd(UTF8_TO_TCHAR(Name.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("analyse_performance")
		.SetDescription("Read back a trace and/or the log and return a perf summary. "
			"For trace: frame_count, avg_frame_ms, avg_fps, max_frame_ms, p95_frame_ms, worst_frames[10]. "
			"For logs: total_lines, errors, warnings, pso_hitches, notable_lines[<=40]. "
			"Returns combined object when source='both'.")
		.AddOptionalParam("source", "Analysis source: 'trace', 'logs', or 'both' (default).", "string")
		.AddOptionalParam("file", "Optional file path override; empty uses the last trace/log from start_trace/stop_trace.", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Source = args.value("source", "both");
			std::string File = args.value("file", "");
			FString Result = FMCPToolboxPerformanceService::Analyse(
				UTF8_TO_TCHAR(Source.c_str()), UTF8_TO_TCHAR(File.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("start_standalone_trace")
		.SetDescription("Launch the game as a separate standalone process with a trace attached "
			"(representative readings that the editor viewport can't give). Connects back to the editor's "
			"Unreal Trace Server. Returns {status, trace_file, log_file, channels, hint}.")
		.AddOptionalParam("name", "Trace file name (without extension). Empty uses 'standalone_capture'.", "string")
		.AddOptionalParam("channels", "Comma-separated trace channels. Empty uses defaults.", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Name = args.value("name", "");
			std::string Channels = args.value("channels", "");
			FString Result = FMCPToolboxPerformanceService::StartStandalone(
				UTF8_TO_TCHAR(Name.c_str()), UTF8_TO_TCHAR(Channels.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("stop_standalone_trace")
		.SetDescription("Stop the standalone process and finalise its trace/log. "
			"Returns {status, trace_file, log_file, uts_store, hint}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxPerformanceService::StopStandalone();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("get_standalone_status")
		.SetDescription("Report whether a standalone session is running and which trace/log it is writing. "
			"Returns {running, last_trace_file, last_log_file}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxPerformanceService::GetStandaloneStatus();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	// =====================================================================
	// Python 执行工具 (已禁用 — AI 总是绕过 MCP 直接用 Python,导致冲突和循环)
	// 保留代码以备将来通过 command(cmd="py ...") 手动调用时使用
	// =====================================================================
	/*
	ToolFunctionTable->Add(assistant::FunctionBuilder("execute_python_code")
		.SetDescription("Execute Python code in the Unreal Editor. "
			"WARNING: This is a LAST RESORT — only use when call_tool has NO matching MCP toolset. "
			"Python cannot handle asset conflicts (will fail on existing objects), lacks transaction support, "
			"and is slower than MCP. For materials, blueprints, actors, levels etc. ALWAYS prefer call_tool. "
			"Returns: {success:true/false, output, error_message, execution_time_ms}.")
		.AddRequiredParam("code", "Python code to execute", "string")
		.AddOptionalParam("scope", "Execution scope: 'private' (isolated, default) or 'public' (shared console state)", "string")
		.AddOptionalParam("timeout_ms", "Timeout in milliseconds (default: 30000)", "number")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Code = args.value("code", "");
			if (Code.empty())
				return {.isError = true, .text = R"({"error":"Missing code parameter"})"};

			std::string ScopeStr = args.value("scope", "private");
			EPythonFileExecutionScope Scope = (ScopeStr == "public")
				? EPythonFileExecutionScope::Public : EPythonFileExecutionScope::Private;

			int32 Timeout = args.value("timeout_ms", 30000);

			auto ExecResult = FMCPToolboxPythonExecutionService::ExecuteCode(
				UTF8_TO_TCHAR(Code.c_str()), Scope, Timeout);

			if (ExecResult.IsError())
			{
				FString ErrJson = FString::Printf(TEXT("{\"error\":\"%s\",\"error_code\":\"%s\"}"),
					*ExecResult.GetErrorMessage(), *ExecResult.GetErrorCode());
				return {.isError = true, .text = TCHAR_TO_UTF8(*ErrJson)};
			}

			const auto& Data = ExecResult.GetValue();
			FString OutJson = FString::Printf(
				TEXT("{\"success\":true,\"output\":\"%s\",\"execution_time_ms\":%.1f}"),
				*Data.Output, Data.ExecutionTimeMs);
			return {.isError = false, .text = TCHAR_TO_UTF8(*OutJson)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("evaluate_python")
		.SetDescription("Evaluate a single Python expression. LAST RESORT — prefer call_tool. "
			"Use only for quick value lookups when no MCP tool provides the info. "
			"Returns {success:true, result:...}.")
		.AddRequiredParam("expression", "Python expression to evaluate", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Expr = args.value("expression", "");
			if (Expr.empty())
				return {.isError = true, .text = R"({"error":"Missing expression parameter"})"};

			auto EvalResult = FMCPToolboxPythonExecutionService::EvaluateExpression(
				UTF8_TO_TCHAR(Expr.c_str()));

			if (EvalResult.IsError())
			{
				FString ErrJson = FString::Printf(TEXT("{\"error\":\"%s\"}"), *EvalResult.GetErrorMessage());
				return {.isError = true, .text = TCHAR_TO_UTF8(*ErrJson)};
			}

			const auto& Data = EvalResult.GetValue();
			FString OutJson = FString::Printf(TEXT("{\"success\":true,\"result\":\"%s\"}"),
				*Data.Result);
			return {.isError = false, .text = TCHAR_TO_UTF8(*OutJson)};
		}).Build());
	*/

	// =====================================================================
	// Python 内省工具 (移植自 VibeUE PythonDiscoveryService)
	// 让 LLM 发现 Python API、内省类/函数、搜索源码
	// =====================================================================

	ToolFunctionTable->Add(assistant::FunctionBuilder("discover_python_module")
		.SetDescription("Discover members of the unreal Python module. Returns classes, functions, and constants "
			"filtered by name pattern. Use this to understand what's available before calling specific APIs.")
		.AddRequiredParam("filter", "Name pattern to filter (case-insensitive, e.g., 'Editor' or 'Blueprint')", "string")
		.AddOptionalParam("include_classes", "Include class names (default: true)", "boolean")
		.AddOptionalParam("include_functions", "Include function names (default: false)", "boolean")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Filter = args.value("filter", "");
			bool bClasses = args.value("include_classes", true);
			bool bFuncs = args.value("include_functions", false);

			auto Result = FMCPToolboxPythonDiscoveryService::DiscoverUnrealModule(
				1, UTF8_TO_TCHAR(Filter.c_str()), 200, bClasses, bFuncs);

			if (Result.IsError())
				return {.isError = true, .text = TCHAR_TO_UTF8(
					*FString::Printf(TEXT("{\"error\":\"%s\"}"), *Result.GetErrorMessage()))};

			const auto& Info = Result.GetValue();
			FString Json;
			Json.Reserve(4096);
			Json += TEXT("{\"module\":\"unreal\",\"total_members\":");
			Json.AppendInt(Info.TotalMembers);

			Json += TEXT(",\"classes\":[");
			for (int32 i = 0; i < Info.Classes.Num(); ++i)
			{
				if (i > 0) Json += TEXT(",");
				Json += TEXT("\"") + Info.Classes[i] + TEXT("\"");
			}
			Json += TEXT("],\"functions\":[");
			for (int32 i = 0; i < Info.Functions.Num(); ++i)
			{
				if (i > 0) Json += TEXT(",");
				Json += TEXT("\"") + Info.Functions[i] + TEXT("\"");
			}
			Json += TEXT("]}");
			return {.isError = false, .text = TCHAR_TO_UTF8(*Json)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("discover_python_class")
		.SetDescription("Introspect a Python class in the unreal module. Returns methods with signatures, "
			"properties, base classes, and docstring. Use before calling class methods to verify they exist.")
		.AddRequiredParam("class_name", "Class name (e.g., 'EditorActorSubsystem')", "string")
		.AddOptionalParam("method_filter", "Filter methods by name (e.g., 'get|set|spawn')", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string ClassName = args.value("class_name", "");
			std::string MethodFilter = args.value("method_filter", "");

			auto Result = FMCPToolboxPythonDiscoveryService::DiscoverClass(
				UTF8_TO_TCHAR(ClassName.c_str()),
				UTF8_TO_TCHAR(MethodFilter.c_str()));

			if (Result.IsError())
				return {.isError = true, .text = TCHAR_TO_UTF8(
					*FString::Printf(TEXT("{\"error\":\"%s\"}"), *Result.GetErrorMessage()))};

			const auto& Info = Result.GetValue();
			FString Json;
			Json.Reserve(8192);
			Json += FString::Printf(TEXT("{\"class\":\"%s\",\"full_path\":\"%s\",\"docstring\":\"%s\",\"is_abstract\":%s,"),
				*Info.Name, *Info.FullPath, *Info.Docstring,
				Info.bIsAbstract ? TEXT("true") : TEXT("false"));

			Json += TEXT("\"base_classes\":[");
			for (int32 i = 0; i < Info.BaseClasses.Num(); ++i)
			{
				if (i > 0) Json += TEXT(",");
				Json += TEXT("\"") + Info.BaseClasses[i] + TEXT("\"");
			}
			Json += TEXT("],\"methods\":[");
			for (int32 i = 0; i < Info.Methods.Num(); ++i)
			{
				if (i > 0) Json += TEXT(",");
				const auto& M = Info.Methods[i];
				Json += FString::Printf(TEXT("{\"name\":\"%s\",\"sig\":\"%s\"}"),
					*M.Name, *M.Signature);
			}
			Json += TEXT("],\"properties\":[");
			for (int32 i = 0; i < Info.Properties.Num(); ++i)
			{
				if (i > 0) Json += TEXT(",");
				Json += TEXT("\"") + Info.Properties[i] + TEXT("\"");
			}
			Json += TEXT("]}");
			return {.isError = false, .text = TCHAR_TO_UTF8(*Json)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("discover_python_function")
		.SetDescription("Introspect a Python function or method. Returns full signature with parameter names, "
			"types, return type, and docstring. Use for 'class.method' paths like 'EditorActorSubsystem.spawn_actor'.")
		.AddRequiredParam("function_path", "Function path (e.g., 'load_asset' or 'EditorActorSubsystem.spawn_actor')", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string FuncPath = args.value("function_path", "");
			auto Result = FMCPToolboxPythonDiscoveryService::DiscoverFunction(
				UTF8_TO_TCHAR(FuncPath.c_str()));

			if (Result.IsError())
				return {.isError = true, .text = TCHAR_TO_UTF8(
					*FString::Printf(TEXT("{\"error\":\"%s\"}"), *Result.GetErrorMessage()))};

			const auto& Info = Result.GetValue();
			FString Json;
			Json.Reserve(2048);
			Json += FString::Printf(
				TEXT("{\"name\":\"%s\",\"signature\":\"%s\",\"return_type\":\"%s\",\"docstring\":\"%s\",\"is_method\":%s}"),
				*Info.Name, *Info.Signature, *Info.ReturnType,
				*Info.Docstring,
				Info.bIsMethod ? TEXT("true") : TEXT("false"));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Json)};
		}).Build());

	// =====================================================================
	// Service Registry 内省工具 (Stage 6.4)
	// 让 LLM 查询当前已注册的所有 Service,了解工具来源和归属
	// =====================================================================

	// 注册服务元数据到 Registry
	FMCPToolboxServiceRegistry::Get().Register(FMCPToolboxTransactionService::GetServiceInfo());
	FMCPToolboxServiceRegistry::Get().Register(FMCPToolboxPerformanceService::GetServiceInfo());

	ToolFunctionTable->Add(assistant::FunctionBuilder("list_services")
		.SetDescription("List all registered Services with their metadata (name, description, source, tool_count). "
			"Use this to understand which Services are available and how tools are grouped. "
			"Returns {status:ok, count:N, services:[{name, description, source, tool_count}, ...]}.")
		.SetCallback([](const assistant::json&) -> assistant::FunctionResult {
			FString Result = FMCPToolboxServiceRegistry::Get().ListServices();
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	ToolFunctionTable->Add(assistant::FunctionBuilder("get_service_info")
		.SetDescription("Get detailed info about a specific Service, including its full tool list. "
			"Returns {status:ok, name, description, source, tool_count, tools:[...]}. "
			"On unknown service returns SERVICE_NOT_FOUND with valid_services list for self-correction.")
		.AddRequiredParam("service", "Service name (e.g., 'transaction', 'performance')", "string")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Svc = args.value("service", "");
			FString Result = FMCPToolboxServiceRegistry::Get().GetServiceInfo(UTF8_TO_TCHAR(Svc.c_str()));
			return {.isError = false, .text = TCHAR_TO_UTF8(*Result)};
		}).Build());

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 已注册 %d 个MCP工具 (%d 个 Service)"),
		ToolFunctionTable->GetFunctionsCount(), FMCPToolboxServiceRegistry::Get().GetServiceCount());
}

assistant::FunctionTable& SMCPToolboxChatWidget::GetFunctionTable()
{
	return *ToolFunctionTable;
}

// ============================================================================
// Key Handlers
// ============================================================================
FReply SMCPToolboxChatWidget::OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Enter && !InKeyEvent.IsShiftDown())
		return OnSendMessage();
	return FReply::Unhandled();
}

FReply SMCPToolboxChatWidget::OnUploadFile()
{
	TArray<FString> Files;
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform && DesktopPlatform->OpenFileDialog(
		nullptr, TEXT("选择图片或视频"), FPaths::ProjectDir(), TEXT(""),
		TEXT("Images (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg|All (*.*)|*.*"),
		EFileDialogFlags::Multiple, Files) && Files.Num() > 0)
	{
		// Collect image URIs into PendingUploadURIs — they will be attached to the
		// next user message inside OnSendMessage. This avoids the old design where
		// OnUploadFile called AddMessage(FileMsg) + OnSendMessage(), which either
		// sent a duplicate user message (when input had text) or skipped sending
		// entirely (when input was empty, because OnSendMessage early-returns).
		PendingUploadURIs.Empty();
		FString FileList;
		for (const FString& F : Files)
		{
			FileList += FPaths::GetCleanFilename(F) + TEXT("\n");
			FString URI = EncodeFileToDataURI(F);
			if (!URI.IsEmpty())
			{
				PendingUploadURIs.Add(URI);
				PendingUploadFileNames.Add(FPaths::GetCleanFilename(F));
			}
		}

		// If input box is empty, inject a default prompt so OnSendMessage actually sends.
		if (InputTextBox.IsValid())
		{
			FString Current = InputTextBox->GetText().ToString().TrimStartAndEnd();
			if (Current.IsEmpty())
				InputTextBox->SetText(FText::FromString(TEXT("请分析这张图片")));
		}

		// Trigger send — OnSendMessage picks up PendingUploadURIs and attaches them.
		OnSendMessage();
	}
	return FReply::Handled();
}

void SMCPToolboxChatWidget::ClearMessages()
{
	Messages.Empty();
	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ClearChildren();
	}
}

FReply SMCPToolboxChatWidget::OnClearChat()
{
	ClearMessages();
	FMCPToolboxChatMessage W;
	W.Role = EMCPToolboxMessageRole::Assistant;
	W.Content = TEXT("对话已清空。有什么可以帮你的？");
	AddMessage(W);
	return FReply::Handled();
}

// ============================================================================
// Prompt Optimization — use local auxiliary model to refine user input
// Semantic optimization: clarify goal, add constraints, structure steps, remove
// redundancy. Output may legitimately expand or shrink. Post-processing strips
// Markdown fences and rejects obvious model echoes / heavy repetition.
// ============================================================================

FReply SMCPToolboxChatWidget::OnOptimizePrompt()
{
	if (!InputTextBox.IsValid() || bOptimizingPrompt) return FReply::Handled();

	FString CurrentText = InputTextBox->GetText().ToString().TrimStartAndEnd();
	if (CurrentText.IsEmpty()) return FReply::Handled();

	const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
	if (!ActiveEntry)
	{
		FNotificationInfo Info(FText::FromString(TEXT("请先在 API密钥 页添加模型")));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return FReply::Handled();
	}

	// Save original for undo
	UndoBuffer = CurrentText;
	bOptimizingPrompt = true;
	bPromptOptimized = false;

	// Build API URL (same logic as SendAIRequestInternal for provider compatibility)
	FString ApiKey;
	FBase64::Decode(ActiveEntry->EncryptedKey, ApiKey);
	FString ApiUrl = ActiveEntry->BaseURL;

	if (ActiveEntry->ProviderId == TEXT("google"))
	{
		// Gemini: https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent
		if (ApiUrl.EndsWith(TEXT("/"))) ApiUrl.RemoveFromEnd(TEXT("/"));
		ApiUrl += TEXT("/models/") + ActiveEntry->ModelId + TEXT(":generateContent?key=") + ApiKey;
	}
	else if (ActiveEntry->ProviderId == TEXT("baidu"))
	{
		if (!ApiUrl.Contains(TEXT("access_token=")))
			ApiUrl += TEXT("?access_token=") + ApiKey;
		if (!ApiUrl.EndsWith(TEXT("/chat/completions")))
		{
			if (ApiUrl.EndsWith(TEXT("/"))) ApiUrl.RemoveFromEnd(TEXT("/"));
			ApiUrl += TEXT("/chat/completions");
		}
	}
	else
	{
		if (!ApiUrl.EndsWith(TEXT("/chat/completions")))
		{
			if (ApiUrl.EndsWith(TEXT("/"))) ApiUrl.RemoveFromEnd(TEXT("/"));
			ApiUrl += TEXT("/chat/completions");
		}
	}

	// Build request: single system message + user text, no tools
	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	if (ActiveEntry->ProviderId != TEXT("google"))
		Body->SetStringField(TEXT("model"), ActiveEntry->ModelId);
	Body->SetNumberField(TEXT("max_tokens"), 512);
	Body->SetNumberField(TEXT("temperature"), 0.3);

	TArray<TSharedPtr<FJsonValue>> Msgs;
	if (ActiveEntry->ProviderId == TEXT("google"))
	{
		// Gemini uses "contents" array with "parts"
		TSharedPtr<FJsonObject> SysPart = MakeShareable(new FJsonObject());
		SysPart->SetStringField(TEXT("text"), TEXT("你是文案优化助手。将用户输入改写为更清晰简洁的表达,只做去冗余和明确意图,不添加原文没有的信息。直接输出改写结果。"));
		TArray<TSharedPtr<FJsonValue>> SysParts;
		SysParts.Add(MakeShareable(new FJsonValueObject(SysPart)));
		TSharedPtr<FJsonObject> SysContent = MakeShareable(new FJsonObject());
		SysContent->SetStringField(TEXT("role"), TEXT("user"));
		SysContent->SetArrayField(TEXT("parts"), SysParts);
		Msgs.Add(MakeShareable(new FJsonValueObject(SysContent)));

		TSharedPtr<FJsonObject> UsrPart = MakeShareable(new FJsonObject());
		UsrPart->SetStringField(TEXT("text"), CurrentText);
		TArray<TSharedPtr<FJsonValue>> UsrParts;
		UsrParts.Add(MakeShareable(new FJsonValueObject(UsrPart)));
		TSharedPtr<FJsonObject> UsrContent = MakeShareable(new FJsonObject());
		UsrContent->SetStringField(TEXT("role"), TEXT("user"));
		UsrContent->SetArrayField(TEXT("parts"), UsrParts);
		Msgs.Add(MakeShareable(new FJsonValueObject(UsrContent)));

		Body->SetArrayField(TEXT("contents"), Msgs);
	}
	else
	{
		TSharedPtr<FJsonObject> Sys = MakeShareable(new FJsonObject());
		Sys->SetStringField(TEXT("role"), TEXT("system"));
		Sys->SetStringField(TEXT("content"), TEXT("你是文案优化助手。将用户输入改写为更清晰简洁的表达,只做去冗余和明确意图,不添加原文没有的信息。直接输出改写结果。"));
		Msgs.Add(MakeShareable(new FJsonValueObject(Sys)));

		TSharedPtr<FJsonObject> Usr = MakeShareable(new FJsonObject());
		Usr->SetStringField(TEXT("role"), TEXT("user"));
		Usr->SetStringField(TEXT("content"), CurrentText);
		Msgs.Add(MakeShareable(new FJsonValueObject(Usr)));

		Body->SetArrayField(TEXT("messages"), Msgs);
	}

	FString BodyStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), W);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(ApiUrl);
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Content-Length"), FString::FromInt(BodyStr.Len()));
	if (ActiveEntry->ProviderId != TEXT("google") && ActiveEntry->ProviderId != TEXT("baidu"))
		Req->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
	Req->SetContentAsString(BodyStr);
	Req->SetTimeout(30.0f);

	TWeakPtr<SMultiLineEditableTextBox> WeakInput = InputTextBox;
	FString OriginalText = CurrentText;
	FString Provider = ActiveEntry->ProviderId;

	Req->OnProcessRequestComplete().BindLambda(
		[this, WeakInput, OriginalText, Provider](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			bOptimizingPrompt = false;

			if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{
				FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("优化失败: HTTP %d"), Resp.IsValid() ? Resp->GetResponseCode() : 0)));
				Info.ExpireDuration = 3.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				UndoBuffer.Empty();
				return;
			}

			// Parse response
			TSharedPtr<FJsonObject> RespObj;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(R, RespObj))
			{
				FNotificationInfo Info(FText::FromString(TEXT("优化失败:响应解析错误")));
				Info.ExpireDuration = 3.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				UndoBuffer.Empty();
				return;
			}

			FString Content;
			if (Provider == TEXT("google"))
			{
				// Gemini: candidates[0].content.parts[0].text
				const TArray<TSharedPtr<FJsonValue>>* Candidates;
				if (RespObj->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* ContentObj;
					if ((*Candidates)[0]->AsObject()->TryGetObjectField(TEXT("content"), ContentObj))
					{
						const TArray<TSharedPtr<FJsonValue>>* Parts;
						if ((*ContentObj)->TryGetArrayField(TEXT("parts"), Parts) && Parts->Num() > 0)
							(*Parts)[0]->AsObject()->TryGetStringField(TEXT("text"), Content);
					}
				}
			}
			else
			{
				// OpenAI-compatible: choices[0].message.content
				const TArray<TSharedPtr<FJsonValue>>* Choices;
				if (RespObj->TryGetArrayField(TEXT("choices"), Choices) && Choices->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* MsgObj;
					if ((*Choices)[0]->AsObject()->TryGetObjectField(TEXT("message"), MsgObj))
						(*MsgObj)->TryGetStringField(TEXT("content"), Content);
				}
			}

			FString Cleaned = Content.TrimStartAndEnd();
			// Strip Markdown fences
			if (Cleaned.StartsWith(TEXT("```")))
			{
				int32 Nl = Cleaned.Find(TEXT("\n"));
				if (Nl != INDEX_NONE) Cleaned = Cleaned.Mid(Nl + 1);
				if (Cleaned.EndsWith(TEXT("```"))) Cleaned = Cleaned.LeftChop(3).TrimEnd();
			}

			if (Cleaned.IsEmpty() || Cleaned.Len() > OriginalText.Len() * 4)
			{
				FNotificationInfo Info(FText::FromString(TEXT("优化结果不可用,已保留原文")));
				Info.ExpireDuration = 3.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				UndoBuffer.Empty();
				return;
			}

			if (WeakInput.IsValid())
			{
				WeakInput.Pin()->SetText(FText::FromString(Cleaned));
			}
			bPromptOptimized = true;

			FNotificationInfo Info(FText::FromString(FString::Printf(
				TEXT("已优化(%d→%d字符),不满意可点「退回」"), OriginalText.Len(), Cleaned.Len())));
			Info.ExpireDuration = 3.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		});

	Req->ProcessRequest();
	return FReply::Handled();
}


FReply SMCPToolboxChatWidget::OnUndoOptimization()
{
	if (!InputTextBox.IsValid() || !bPromptOptimized) return FReply::Handled();

	// Restore original text
	InputTextBox->SetText(FText::FromString(UndoBuffer));
	UndoBuffer.Empty();
	bPromptOptimized = false;

	FNotificationInfo Info(FText::FromString(TEXT("已恢复原始提示词")));
	Info.ExpireDuration = 2.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Prompt optimization undone"));
	return FReply::Handled();
}

// ============================================================================
// MCP Toolset Cache (Refresh Tool Cache button)
// ============================================================================
FReply SMCPToolboxChatWidget::OnRefreshToolCache()
{
	if (!MCPServerClient.IsConnected())
	{
		FNotificationInfo Err(FText::FromString(TEXT("MCP 服务器未连接,请先点击 \"连接 MCP\"")));
		Err.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Err);
		return FReply::Handled();
	}

	// Create a persistent notification that we can update with progress
	FNotificationInfo ProgressInfo(FText::FromString(TEXT("正在获取工具集列表...")));
	ProgressInfo.bFireAndForget = false;
	ProgressInfo.ExpireDuration = 0.0f;
	TSharedPtr<SNotificationItem> ProgressNotif = FSlateNotificationManager::Get().AddNotification(ProgressInfo);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] OnRefreshToolCache: calling DiscoverRealTools..."));

	MCPServerClient.DiscoverRealTools(
		[this, ProgressNotif](const TArray<TSharedPtr<FJsonObject>>& Tools)
		{
			// Per project rules, MCP callbacks must execute on the game thread
			// to prevent cross-thread Slate/UObject access.
			AsyncTask(ENamedThreads::GameThread,
				[this, Tools, ProgressNotif]()
				{
					if (Tools.Num() == 0)
					{
						if (ProgressNotif.IsValid())
						{
							ProgressNotif->SetText(FText::FromString(TEXT("未感知到任何 MCP 工具,请确认 UE5 MCP 服务器已启用")));
							ProgressNotif->SetCompletionState(SNotificationItem::CS_Fail);
							ProgressNotif->ExpireAndFadeout();
						}
						return;
					}

					WriteToolsetCacheToDisk(Tools);

					// Rebuild the cached tool descriptions MD so the next system
					// prompt construction picks up the refreshed tools immediately.
					TMap<FString, TArray<TSharedPtr<FJsonObject>>> Grouped;
					for (const auto& Tool : Tools)
					{
						if (!Tool.IsValid()) continue;
						FString Ts;
						if (Tool->TryGetStringField(TEXT("_toolset"), Ts) && !Ts.IsEmpty())
							Grouped.FindOrAdd(Ts).Add(Tool);
					}

					FString Md;
					Md += TEXT("##  MCP Toolset 缓存(运行时感知)\n\n");
					Md += TEXT("使用 `call_tool` 调用,toolset_name 是完整路径(如 `editor_toolset.toolsets.material.MaterialTools`),tool_name 是工具名(如 `CreateMaterial`)。**禁止**调用 list_toolsets/describe_toolset。\n\n");
					for (const auto& Kvp : Grouped)
					{
						Md += FString::Printf(TEXT("### Toolset: `%s` (%d 个工具)\n"), *Kvp.Key, Kvp.Value.Num());
						for (const auto& Tool : Kvp.Value)
						{
							FString Name;  Tool->TryGetStringField(TEXT("name"), Name);
							FString Desc;  Tool->TryGetStringField(TEXT("description"), Desc);
							FString OneLine = Desc.Replace(TEXT("\n"), TEXT(" ")).Left(140);
							Md += FString::Printf(TEXT("- **%s**: %s\n"), *Name, *OneLine);
						}
						Md += TEXT("\n");
					}
					CachedMCPToolDescriptionsMD = Md;

					FString OkText = FString::Printf(
						TEXT("已缓存 %d 个 toolset / %d 个工具到 .mcptoolbox/ (下次系统提示词将自动加载)"),
						Grouped.Num(), Tools.Num());
					if (ProgressNotif.IsValid())
					{
						ProgressNotif->SetText(FText::FromString(OkText));
						ProgressNotif->SetCompletionState(SNotificationItem::CS_Success);
						ProgressNotif->ExpireAndFadeout();
					}

					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Toolset cache refreshed: %d toolsets, %d tools"),
						Grouped.Num(), Tools.Num());
				});
		},
		// OnProgress callback — called after each describe_toolset completes
		[ProgressNotif](int32 Done, int32 Total, const FString& CurrentToolset)
		{
			AsyncTask(ENamedThreads::GameThread,
				[ProgressNotif, Done, Total, CurrentToolset]()
				{
					if (ProgressNotif.IsValid())
					{
						FString ProgressText = FString::Printf(
							TEXT("正在获取工具集 %d/%d: %s"),
							Done, Total, *CurrentToolset);
						ProgressNotif->SetText(FText::FromString(ProgressText));
					}
					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Refresh progress: %d/%d (%s)"), Done, Total, *CurrentToolset);
				});
		});

	return FReply::Handled();
}

void SMCPToolboxChatWidget::WriteToolsetCacheToDisk(const TArray<TSharedPtr<FJsonObject>>& Tools)
{
	// ── VibeUE-style three-tier structure ──
	// .mcptoolbox/
	//   ├── index.md       (lean index ~2K tokens, ONLY file injected into system prompt)
	//   ├── rules.md       (WRONG/CORRECT rules + scientific vibe, created once, user-editable)
	//   └── toolsets/      (per-toolset detailed docs, lazy-loaded via get_skills)
	//       ├── <toolset>.md (with YAML frontmatter)
	//       └── ...
	FString ToolboxDir = FPaths::ProjectDir() / TEXT(".mcptoolbox");
	FString ToolsetsDir = ToolboxDir / TEXT("toolsets");

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.CreateDirectoryTree(*ToolsetsDir))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] WriteToolsetCacheToDisk: failed to create dir %s"), *ToolsetsDir);
		return;
	}

	// Convert toolset name to safe file name (replace . / \ : etc with _)
	auto SafeFileName = [](const FString& Ts) -> FString
	{
		FString S = Ts;
		S.ReplaceInline(TEXT("."), TEXT("_"));
		S.ReplaceInline(TEXT("/"),  TEXT("_"));
		S.ReplaceInline(TEXT("\\"), TEXT("_"));
		S.ReplaceInline(TEXT(":"),  TEXT("_"));
		return S;
	};

	// Extract short name from full toolset path (last segment after final '.')
	auto ShortName = [](const FString& Ts) -> FString
	{
		int32 LastDot;
		if (Ts.FindLastChar(TEXT('.'), LastDot))
		{
			return Ts.RightChop(LastDot + 1);
		}
		return Ts;
	};

	// Group tools by _toolset field (set by ExtractToolsFromDescribeResult)
	TMap<FString, TArray<TSharedPtr<FJsonObject>>> Grouped;
	TArray<FString> ToolsetOrder; // preserve first-seen order
	for (const auto& Tool : Tools)
	{
		if (!Tool.IsValid()) continue;
		FString Ts;
		if (!Tool->TryGetStringField(TEXT("_toolset"), Ts) || Ts.IsEmpty()) continue;
		if (!Grouped.Contains(Ts))
		{
			Grouped.Add(Ts);
			ToolsetOrder.Add(Ts);
		}
		Grouped[Ts].Add(Tool);
	}

	// ── Write index.md (with YAML frontmatter, VibeUE-style) ──
	// This is the ONLY file injected into system prompt by BuildSystemPrompt.
	// Kept lean (~2K tokens) — detailed docs are lazy-loaded via get_skills().
	FString Index;
	Index += TEXT("---\n");
	Index += FString::Printf(TEXT("generated_at: %s\n"), *FDateTime::Now().ToString());
	Index += FString::Printf(TEXT("toolset_count: %d\n"), ToolsetOrder.Num());
	Index += FString::Printf(TEXT("tool_count: %d\n"), Tools.Num());
	Index += TEXT("---\n\n");
	Index += TEXT("# MCP Toolset 索引\n\n");
	Index += TEXT("## 工具集列表\n\n");
	Index += TEXT("| Toolset | 工具数 | 描述 |\n");
	Index += TEXT("|---------|-------|------|\n");
	for (const FString& Ts : ToolsetOrder)
	{
		const TArray<TSharedPtr<FJsonObject>>& TsTools = Grouped[Ts];
		FString Summary;
		if (TsTools.Num() > 0)
		{
			TsTools[0]->TryGetStringField(TEXT("description"), Summary);
			Summary = Summary.Replace(TEXT("\n"), TEXT(" ")).Left(80);
		}
		Index += FString::Printf(TEXT("| `%s` | %d | %s |\n"),
			*Ts, TsTools.Num(), *Summary);
	}
	Index += TEXT("\n## 调用约定\n\n");
	Index += TEXT("使用 `call_tool` 工具调用,参数:\n");
	Index += TEXT("- `toolset_name`: toolset 完整路径(如 `editor_toolset.toolsets.material.MaterialTools`),**不含** tool_name\n");
	Index += TEXT("- `tool_name`: 工具名(如 `CreateMaterial`)\n");
	Index += TEXT("- `arguments`: 工具参数对象,具体 schema 通过 `get_skills(skills=[\"<toolset_name>\"])` 按需加载\n\n");
	Index += TEXT("**禁止**调用 `list_toolsets` 或 `describe_toolset` — 本索引已包含全部可用 toolset。\n");
	Index += TEXT("**需要详细 schema 时**调用 `get_skills` 加载对应 toolset 文档(懒加载,节省 token)。\n");

	FString IndexPath = ToolboxDir / TEXT("index.md");
	if (!FFileHelper::SaveStringToFile(Index, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Failed to write %s"), *IndexPath);
	}

	// ── Write rules.md (only if missing — user may have customized it) ──
	// Contains WRONG/CORRECT paradigm rules + scientific vibe (AI 行为准则).
	// BuildSystemPrompt loads this file and injects it verbatim.
	FString RulesPath = ToolboxDir / TEXT("rules.md");
	if (!PF.FileExists(*RulesPath))
	{
		FString DefaultRules = FMCPToolboxSkillService::GenerateDefaultRulesMD();
		if (!FFileHelper::SaveStringToFile(DefaultRules, *RulesPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Failed to write default %s"), *RulesPath);
		}
		else
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Created default rules.md at %s"), *RulesPath);
		}
	}

	// ── Write one MD per toolset (with YAML frontmatter, in toolsets/ subdir) ──
	// These are lazy-loaded by get_skills() — NOT injected into system prompt.
	int32 WrittenFiles = 1; // index.md already written
	for (const FString& Ts : ToolsetOrder)
	{
		const TArray<TSharedPtr<FJsonObject>>& TsTools = Grouped[Ts];
		FString Short = ShortName(Ts);

		FString Md;
		// YAML frontmatter (VibeUE-style)
		Md += TEXT("---\n");
		Md += FString::Printf(TEXT("name: %s\n"), *Short);
		Md += FString::Printf(TEXT("toolset: %s\n"), *Ts);
		Md += FString::Printf(TEXT("tool_count: %d\n"), TsTools.Num());
		Md += TEXT("keywords: []\n");
		Md += TEXT("---\n\n");

		Md += FString::Printf(TEXT("# Toolset: `%s`\n\n"), *Ts);
		Md += FString::Printf(TEXT("工具数: %d\n\n"), TsTools.Num());
		Md += TEXT("## 工具列表\n\n");

		for (const auto& Tool : TsTools)
		{
			FString Name; Tool->TryGetStringField(TEXT("name"), Name);
			FString Desc; Tool->TryGetStringField(TEXT("description"), Desc);
			Md += FString::Printf(TEXT("### %s\n\n"), *Name);
			Md += FString::Printf(TEXT("**描述**: %s\n\n"), *Desc);

			// inputSchema block
			const TSharedPtr<FJsonObject>* SchemaObj;
			if (Tool->TryGetObjectField(TEXT("inputSchema"), SchemaObj) && SchemaObj->IsValid())
			{
				FString SchemaStr;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&SchemaStr);
				FJsonSerializer::Serialize(SchemaObj->ToSharedRef(), W);
				Md += TEXT("**参数 schema**:\n```json\n");
				Md += SchemaStr;
				Md += TEXT("\n```\n\n");
			}

			// example call (fills in required property names as placeholders)
			Md += TEXT("**调用示例**:\n```json\n");
			Md += FString::Printf(TEXT("{\n  \"toolset_name\": \"%s\",\n  \"tool_name\": \"%s\",\n  \"arguments\": {"),
				*Ts, *Name);

			const TSharedPtr<FJsonObject>* Schema2;
			if (Tool->TryGetObjectField(TEXT("inputSchema"), Schema2) && Schema2->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* RequiredArr;
				if ((*Schema2)->TryGetArrayField(TEXT("required"), RequiredArr))
				{
					bool bFirst = true;
					for (const auto& Req : *RequiredArr)
					{
						FString ReqName;
						if (Req->TryGetString(ReqName))
						{
							Md += FString::Printf(TEXT("%s\n    \"%s\": \"<value>\""),
								bFirst ? TEXT("") : TEXT(","),
								*ReqName);
							bFirst = false;
						}
					}
				}
			}
			Md += TEXT("\n  }\n}\n```\n\n---\n\n");
		}

		FString FileName = SafeFileName(Ts) + TEXT(".md");
		FString FilePath = ToolsetsDir / FileName;
		if (FFileHelper::SaveStringToFile(Md, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			WrittenFiles++;
		}
		else
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Failed to write %s"), *FilePath);
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] WriteToolsetCacheToDisk: wrote %d files to %s (index.md + rules.md + toolsets/)"),
		WrittenFiles, *ToolboxDir);
}

// ============================================================================
// VibeUE-style Skill system helpers (Stage 1 migration)
// ============================================================================
// 设计参考: e:\YBAI\VibeUE\Content\Python\init_unreal.py (_parse_frontmatter)
// 设计参考: e:\YBAI\VibeUE\Content\Skills\materials\SKILL.md (WRONG/CORRECT 范式)
// ============================================================================

// ============================================================================
// Skill 相关方法已移至 FMCPToolboxSkillService (God Object 拆分阶段 A1)
// - ParseFrontmatter / GenerateDefaultRulesMD / LoadRulesMD / LoadSkillIndex
// - ListSkills / GetSkill / TouchSkillCache
// - IsSkillDisabled / ToggleSkillEnabled
// widget 通过 SkillService 成员委托调用,详见 MCPToolboxSkillService.h
// ============================================================================

// ============================================================================
// Stubs & helpers
// ============================================================================
FReply SMCPToolboxChatWidget::OnToggleVisionMode()
{
	bVisionModeEnabled = !bVisionModeEnabled;
	SaveWidgetState();

	bool bHasLocalVL = FMCPToolboxAuxModelManager::Get().IsReady();
	FString Note;
	if (bVisionModeEnabled)
		Note = TEXT("视觉模式已开启 — 图片由云端 AI 处理");
	else if (bHasLocalVL)
		Note = TEXT("视觉模式已关闭 — 图片由本地辅助模型 (Qwen3VL) 分析");
	else
		Note = TEXT("视觉模式已关闭 — 图片功能不可用");

	FNotificationInfo Info(FText::FromString(Note));
	Info.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Vision mode %s"), bVisionModeEnabled ? TEXT("enabled") : TEXT("disabled"));
	return FReply::Handled();
}

FReply SMCPToolboxChatWidget::OnToggleSidebar()
{
	bSidebarCollapsed = !bSidebarCollapsed;
	SaveWidgetState();
	return FReply::Handled();
}
// ============================================================================
// Toolbar — Entry Selector + Status
// ============================================================================
TSharedRef<SWidget> SMCPToolboxChatWidget::BuildToolbar()
{
	// ponytail: 单行极简布局 — 原 3 行(模型/状态/按钮)压缩为 1 行
	// 5 个状态 TextBlock 合并为单个紧凑标签(●/○ 前缀 + 悬停 tooltip)
	RefreshEntryList();

	return SNew(SBorder)
		.Padding(FMargin(2))
		[
			SNew(SHorizontalBox)
			// 模型下拉(占满宽度)
			+ SHorizontalBox::Slot().FillWidth(1.0).VAlign(VAlign_Center)
			[
				SAssignNew(EntryComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&EntryOptions)
				.OnSelectionChanged(this, &SMCPToolboxChatWidget::OnEntrySelected)
				.OnGenerateWidget(this, &SMCPToolboxChatWidget::GenerateEntryRow)
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						if (SelectedEntry.IsValid())
							return FText::FromString(*SelectedEntry);
						const FMCPToolboxAPIKeyEntry* Active = FMCPToolboxAPIManager::Get().GetActiveEntry();
						if (Active)
							return FText::FromString(FString::Printf(TEXT("%s - %s"), *Active->ProviderName, *Active->ModelId));
						return LOCTEXT("NoModel", "未选择模型");
					})
				]
			]
			// 紧凑状态指示器(单 TextBlock,● 已就绪 / ○ 未就绪)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8, 0, 4, 0))
			[
				SNew(STextBlock)
				.Text_Lambda([this]() -> FText
				{
					auto Ch = [](bool b) -> TCHAR { return b ? 0x25CF : 0x25CB; }; // ● / ○
					FString S;
					S += Ch(MCPServerClient.IsConnected());
					S += TEXT("MCP  ");
					S += Ch(FMCPToolboxAuxModelManager::Get().IsReady());
					S += TEXT("Aux  ");
					IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
					S += Ch(Py && Py->IsPythonAvailable());
					S += TEXT("Py  ");
					const bool bHasMem = !CachedMemorySummary.IsEmpty() || !CachedToolsSummary.IsEmpty();
					S += Ch(bHasMem);
					S += TEXT("Mem");
					if (bIsWaiting) { S += TEXT("  "); S += Ch(true); S += TEXT(" AI..."); }
					return FText::FromString(S);
				})
				.ToolTipText_Lambda([this]() -> FText
				{
					FString T;
					T += FString::Printf(TEXT("MCP: %s\n"), MCPServerClient.IsConnected() ? TEXT("已连接") : TEXT("未连接"));
					T += FString::Printf(TEXT("Aux: %s\n"), FMCPToolboxAuxModelManager::Get().IsReady() ? TEXT("就绪") : TEXT("未就绪"));
					IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
					T += FString::Printf(TEXT("Py:  %s\n"), (Py && Py->IsPythonAvailable()) ? TEXT("可用") : TEXT("不可用"));
					const bool bHasMem = !CachedMemorySummary.IsEmpty() || !CachedToolsSummary.IsEmpty();
					T += FString::Printf(TEXT("Mem: %s"), bHasMem ? TEXT("有内容") : TEXT("为空"));
					return FText::FromString(T);
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
				.ColorAndOpacity_Lambda([this]() -> FLinearColor
				{
					return bIsWaiting ? FLinearColor(1.0f, 0.8f, 0.2f) : FLinearColor(0.55f, 0.7f, 0.6f);
				})
			]
			// 侧栏按钮
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.ButtonColorAndOpacity(FLinearColor(0.13f, 0.13f, 0.15f, 1.0f))
				.ContentPadding(FMargin(6, 2))
				.OnClicked(this, &SMCPToolboxChatWidget::OnToggleSidebar)
				.ToolTipText(LOCTEXT("SidebarTooltip", "显示/隐藏会话历史侧边栏"))
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						return bSidebarCollapsed ? LOCTEXT("SidebarShow", "侧栏") : LOCTEXT("SidebarHide", "侧栏");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
				]
			]
			// 更多按钮(展开后显示:视觉/获取工具/归档总结/Skill)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.ButtonColorAndOpacity(FLinearColor(0.13f, 0.13f, 0.15f, 1.0f))
				.ContentPadding(FMargin(6, 2))
				.OnClicked_Lambda([this]() -> FReply
				{
					bMoreExpanded = !bMoreExpanded;
					SaveWidgetState();
					return FReply::Handled();
				})
				.ToolTipText(LOCTEXT("MoreTooltip", "展开/收起更多操作"))
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						return bMoreExpanded ? LOCTEXT("MoreCollapse", "收起") : LOCTEXT("MoreExpand", "更多");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
				]
			]
			// 视觉(展开时显示)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.Visibility_Lambda([this]() -> EVisibility
				{
					return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ButtonColorAndOpacity_Lambda([this]() -> FLinearColor
				{
					return bVisionModeEnabled ? FLinearColor(0.2f, 0.45f, 0.65f, 1.0f) : FLinearColor(0.13f, 0.13f, 0.15f, 1.0f);
				})
				.ContentPadding(FMargin(6, 2))
				.OnClicked(this, &SMCPToolboxChatWidget::OnToggleVisionMode)
				.ToolTipText(LOCTEXT("VisionTooltip", "切换视觉模式"))
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						return bVisionModeEnabled ? LOCTEXT("VisionEnabled", "视觉开") : LOCTEXT("VisionDisabled", "视觉");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						return bVisionModeEnabled ? FLinearColor(0.9f, 0.95f, 1.0f) : FLinearColor(0.7f, 0.7f, 0.7f);
					})
				]
			]
			// 获取工具(展开时显示)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.Visibility_Lambda([this]() -> EVisibility
				{
					return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ButtonColorAndOpacity(FLinearColor(0.15f, 0.28f, 0.18f, 1.0f))
				.ContentPadding(FMargin(6, 2))
				.OnClicked(this, &SMCPToolboxChatWidget::OnRefreshToolCache)
				.ToolTipText(LOCTEXT("RefreshToolCacheTooltip", "获取 MCP 服务器所有工具集用法并缓存为 .mcptoolbox/*.md"))
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("RefreshToolCacheBtn", "获取工具"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FLinearColor(0.85f, 0.95f, 0.85f))
				]
			]
			// 归档总结(展开时显示)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.Visibility_Lambda([this]() -> EVisibility
				{
					return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ButtonColorAndOpacity(FLinearColor(0.25f, 0.20f, 0.35f, 1.0f))
				.ContentPadding(FMargin(6, 2))
				.OnClicked(this, &SMCPToolboxChatWidget::OnArchiveSummary)
				.ToolTipText(LOCTEXT("ArchiveSummaryTooltip", "总结当前会话并归档，下次启动自动加载"))
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ArchiveSummaryBtn", "归档总结"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FLinearColor(0.90f, 0.85f, 0.95f))
				]
			]
			// Skill 管理(展开时显示)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.Visibility_Lambda([this]() -> EVisibility
				{
					return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ButtonColorAndOpacity(FLinearColor(0.20f, 0.30f, 0.35f, 1.0f))
				.ContentPadding(FMargin(6, 2))
				.OnClicked(this, &SMCPToolboxChatWidget::OnOpenSkillManager)
				.ToolTipText(LOCTEXT("SkillManagerTooltip", "管理 skill 启用/禁用"))
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("SkillManagerBtn", "Skill"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FLinearColor(0.85f, 0.95f, 0.95f))
				]
			]
			// 生图模式(展开时显示)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2, 0, 0, 0))
			[
				SNew(SButton)
				.Visibility_Lambda([this]() -> EVisibility
				{
					return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ButtonColorAndOpacity_Lambda([this]() -> FLinearColor
				{
					return bImageGenerationMode ? FLinearColor(0.5f, 0.2f, 0.6f, 1.0f) : FLinearColor(0.13f, 0.13f, 0.15f, 1.0f);
				})
				.ContentPadding(FMargin(6, 2))
				.OnClicked_Lambda([this]() -> FReply
				{
					bImageGenerationMode = !bImageGenerationMode;
					SaveWidgetState();
					return FReply::Handled();
				})
				.ToolTipText(LOCTEXT("ImageGenTooltip", "切换生图模式(使用生图模型生成图片)"))
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						return bImageGenerationMode ? LOCTEXT("ImageGenEnabled", "生图开") : LOCTEXT("ImageGenDisabled", "生图");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						return bImageGenerationMode ? FLinearColor(0.95f, 0.85f, 1.0f) : FLinearColor(0.7f, 0.7f, 0.7f);
					})
				]
			]
		];
	}

// ============================================================================
// Entry Selection
// ============================================================================
void SMCPToolboxChatWidget::RefreshEntryList()
{
	EntryOptions.Empty();
	EntryDisplayToId.Empty();

	const TArray<FMCPToolboxAPIKeyEntry>& Entries = FMCPToolboxAPIManager::Get().GetEntries();
	for (const auto& Entry : Entries)
	{
		FString DisplayStr = FString::Printf(TEXT("%s - %s"), *Entry.ProviderName, *Entry.ModelId);
		EntryOptions.Add(MakeShareable(new FString(DisplayStr)));
		EntryDisplayToId.Add(DisplayStr, Entry.Id);
	}

	// Select current active entry
	const FMCPToolboxAPIKeyEntry* Active = FMCPToolboxAPIManager::Get().GetActiveEntry();
	if (Active)
	{
		FString ActiveDisplay = FString::Printf(TEXT("%s - %s"), *Active->ProviderName, *Active->ModelId);
		for (auto& Opt : EntryOptions)
		{
			if (*Opt == ActiveDisplay)
			{
				SelectedEntry = Opt;
				break;
			}
		}
	}

	if (EntryComboBox.IsValid())
		EntryComboBox->RefreshOptions();
}

void SMCPToolboxChatWidget::OnEntrySelected(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (!NewSelection.IsValid()) return;
	SelectedEntry = NewSelection;

	FString* FoundId = EntryDisplayToId.Find(*NewSelection);
	if (FoundId)
	{
		FMCPToolboxAPIManager::Get().SetActiveEntry(*FoundId);
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 已选择模型: %s"), **NewSelection);
	}
}

TSharedRef<SWidget> SMCPToolboxChatWidget::GenerateEntryRow(TSharedPtr<FString> InItem)
{
	return SNew(STextBlock)
		.Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("")))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
		.Margin(FMargin(4, 2));
}

void SMCPToolboxChatWidget::RefreshProviderList() {}
void SMCPToolboxChatWidget::OnProviderSelected(TSharedPtr<FString>, ESelectInfo::Type) {}
void SMCPToolboxChatWidget::RefreshModelList() {}
void SMCPToolboxChatWidget::OnModelSelected(TSharedPtr<FString>, ESelectInfo::Type) {}
TSharedRef<SWidget> SMCPToolboxChatWidget::GenerateProviderRow(TSharedPtr<FString>) { return SNullWidget::NullWidget; }
TSharedRef<SWidget> SMCPToolboxChatWidget::GenerateModelRow(TSharedPtr<FString>) { return SNullWidget::NullWidget; }

void SMCPToolboxChatWidget::SetCurrentProvider(const FString&) {}
FString SMCPToolboxChatWidget::GetCurrentProviderId() const { return TEXT(""); }
FString SMCPToolboxChatWidget::GetCurrentModelId() const
{
	const FMCPToolboxAPIKeyEntry* E = FMCPToolboxAPIManager::Get().GetActiveEntry();
	return E ? E->ModelId : TEXT("");
}

FLinearColor SMCPToolboxChatWidget::GetMessageColor(EMCPToolboxMessageRole Role) const
{
	switch (Role)
	{
	case EMCPToolboxMessageRole::User:      return FLinearColor(0.15f, 0.35f, 0.7f);
	case EMCPToolboxMessageRole::Assistant:  return FLinearColor(0.12f, 0.12f, 0.14f);
	case EMCPToolboxMessageRole::System:     return FLinearColor(0.4f, 0.25f, 0.05f);
	case EMCPToolboxMessageRole::Thinking:   return FLinearColor(0.08f, 0.08f, 0.12f);
	default:                                 return FLinearColor(0.1f, 0.1f, 0.1f);
	}
}

// ============================================================================
// DAG Parallel Execution
// ============================================================================

bool SMCPToolboxChatWidget::HasDAGDependencies(const TArray<TSharedPtr<FJsonValue>>& ToolCalls) const
{
	// 有多个工具调用就启用 DAG 路径（用于可视化+并行执行）
	// 无依赖的任务会在同一批次并行执行，和原来效率一样
	if (ToolCalls.Num() >= 2)
	{
		return true;
	}

	// 单个工具调用也检查是否有显式依赖
	for (const TSharedPtr<FJsonValue>& TC : ToolCalls)
	{
		TSharedPtr<FJsonObject> TCObj = TC->AsObject();
		if (!TCObj.IsValid()) continue;

		const TSharedPtr<FJsonObject>* Func = nullptr;
		if (!TCObj->TryGetObjectField(TEXT("function"), Func) || !Func->IsValid())
			continue;

		FString ArgsStr;
		(*Func)->TryGetStringField(TEXT("arguments"), ArgsStr);

		if (ArgsStr.Contains(TEXT("$t")))
		{
			return true;
		}

		if (TCObj->HasField(TEXT("depends_on")) || TCObj->HasField(TEXT("task_id")))
		{
			return true;
		}
	}

	return false;
}

void SMCPToolboxChatWidget::ConvertToolCallsToDAGFormat(
	const TArray<TSharedPtr<FJsonValue>>& ToolCalls,
	TArray<TSharedPtr<FJsonObject>>& OutDAGCalls) const
{
	OutDAGCalls.Empty();

	for (int32 i = 0; i < ToolCalls.Num(); ++i)
	{
		TSharedPtr<FJsonObject> TCObj = ToolCalls[i]->AsObject();
		if (!TCObj.IsValid()) continue;

		const TSharedPtr<FJsonObject>* Func = nullptr;
		if (!TCObj->TryGetObjectField(TEXT("function"), Func) || !Func->IsValid())
			continue;

		FString FuncName;
		FString ArgsStr;
		(*Func)->TryGetStringField(TEXT("name"), FuncName);
		(*Func)->TryGetStringField(TEXT("arguments"), ArgsStr);

		TSharedPtr<FJsonObject> DAGCall = MakeShareable(new FJsonObject());

		FString TaskId;
		if (!TCObj->TryGetStringField(TEXT("id"), TaskId) || TaskId.IsEmpty())
		{
			TaskId = FString::Printf(TEXT("t%d"), i + 1);
		}
		DAGCall->SetStringField(TEXT("task_id"), TaskId);
		DAGCall->SetStringField(TEXT("tool_id"), FuncName);

		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
		if (!ArgsStr.IsEmpty())
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsStr);
			TSharedPtr<FJsonObject> ParsedArgs;
			if (FJsonSerializer::Deserialize(Reader, ParsedArgs) && ParsedArgs.IsValid())
			{
				Params = ParsedArgs;
			}
		}
		DAGCall->SetObjectField(TEXT("parameters"), Params);

		TArray<FString> Dependencies;
		const TArray<TSharedPtr<FJsonValue>>* DepsArray = nullptr;
		if (TCObj->TryGetArrayField(TEXT("depends_on"), DepsArray))
		{
			for (const TSharedPtr<FJsonValue>& DepVal : *DepsArray)
			{
				Dependencies.Add(DepVal->AsString());
			}
		}
		else
		{
			for (const auto& Pair : Params->Values)
			{
				if (Pair.Value->Type == EJson::String)
				{
					FString ValStr = Pair.Value->AsString();
					if (ValStr.StartsWith(TEXT("$")))
					{
						FString Ref = ValStr.Mid(1);
						int32 DotIdx;
						if (Ref.FindChar(TEXT('.'), DotIdx))
						{
							Ref = Ref.Left(DotIdx);
						}
						if (!Dependencies.Contains(Ref) && Ref.StartsWith(TEXT("t")))
						{
							Dependencies.Add(Ref);
						}
					}
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> DepsJson;
		for (const FString& Dep : Dependencies)
		{
			DepsJson.Add(MakeShareable(new FJsonValueString(Dep)));
		}
		DAGCall->SetArrayField(TEXT("depends_on"), DepsJson);

		OutDAGCalls.Add(DAGCall);
	}
}

// ============================================================================
// DAG $tN.field.path Resolution — Stage 5 subtask
// ============================================================================
TSharedPtr<FJsonValue> SMCPToolboxChatWidget::ResolveDAGFieldPath(const FString& ResultJsonStr, const FString& FieldPath)
{
	// 无字段路径 → 返回整个结果解析为 FJsonValue
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJsonStr);
	TSharedPtr<FJsonObject> RootObj;
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		// 结果不是合法 JSON 对象 — 作为字符串值返回
		return MakeShared<FJsonValueString>(ResultJsonStr);
	}

	TSharedPtr<FJsonValue> Root = MakeShared<FJsonValueObject>(RootObj);

	// FieldPath 为空 → 返回整个根值
	if (FieldPath.IsEmpty())
	{
		return Root;
	}

	TSharedPtr<FJsonValue> Current = Root;
	FString Remaining = FieldPath;

	// 按 '.' 分割路径,每段支持 "field" 或 "field[index]" 或 "[index]"
	while (!Remaining.IsEmpty() && Current.IsValid())
	{
		FString Token;
		int32 DotIdx;
		if (Remaining.FindChar(TEXT('.'), DotIdx))
		{
			Token = Remaining.Left(DotIdx);
			Remaining = Remaining.Mid(DotIdx + 1);
		}
		else
		{
			Token = Remaining;
			Remaining = TEXT("");
		}

		if (Token.IsEmpty())
			continue;

		// 分离字段名与数组索引后缀: "field[0][1]" → FieldName="field", Indices=[0,1]
		FString FieldName;
		TArray<int32> ArrayIndices;
		int32 FirstBracket = Token.Find(TEXT("["));
		if (FirstBracket != INDEX_NONE)
		{
			FieldName = Token.Left(FirstBracket);
			FString IndexPart = Token.Mid(FirstBracket);
			// 解析所有 [N] 后缀
			int32 Pos = 0;
			while (Pos < IndexPart.Len())
			{
				int32 Open = IndexPart.Find(TEXT("["), ESearchCase::IgnoreCase, ESearchDir::FromStart, Pos);
				if (Open == INDEX_NONE) break;
				int32 Close = IndexPart.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Open + 1);
				if (Close == INDEX_NONE) break;
				FString IdxStr = IndexPart.Mid(Open + 1, Close - Open - 1);
				int32 ArrIdx = FCString::Atoi(*IdxStr);
				ArrayIndices.Add(ArrIdx);
				Pos = Close + 1;
			}
		}
		else
		{
			FieldName = Token;
		}

		// 1) 取字段(如果 FieldName 非空)
		if (!FieldName.IsEmpty())
		{
			if (Current->Type != EJson::Object)
				return nullptr;
			Current = Current->AsObject()->TryGetField(FieldName);
			if (!Current.IsValid())
				return nullptr;
		}

		// 2) 按数组索引依次取元素
		for (int32 ArrIdx : ArrayIndices)
		{
			if (!Current.IsValid() || Current->Type != EJson::Array)
				return nullptr;
			const TArray<TSharedPtr<FJsonValue>>& Arr = Current->AsArray();
			if (ArrIdx < 0 || ArrIdx >= Arr.Num())
				return nullptr;
			Current = Arr[ArrIdx];
		}
	}

	return Current;
}

void SMCPToolboxChatWidget::SetResolvedParam(TSharedPtr<FJsonObject>& Target, const FString& Key, const TSharedPtr<FJsonValue>& Value)
{
	if (!Target.IsValid() || !Value.IsValid())
		return;

	switch (Value->Type)
	{
	case EJson::String:
		Target->SetStringField(Key, Value->AsString());
		break;
	case EJson::Number:
		Target->SetNumberField(Key, Value->AsNumber());
		break;
	case EJson::Boolean:
		Target->SetBoolField(Key, Value->AsBool());
		break;
	case EJson::Object:
		Target->SetObjectField(Key, Value->AsObject());
		break;
	case EJson::Array:
		Target->SetArrayField(Key, Value->AsArray());
		break;
	case EJson::Null:
		Target->SetField(Key, MakeShared<FJsonValueNull>());
		break;
	default:
		// 兜底:序列化为字符串
		{
			FString JsonStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
			if (Value->Type == EJson::Array)
			{
				FJsonSerializer::Serialize(Value->AsArray(), Writer);
			}
			else if (Value->Type == EJson::Object && Value->AsObject().IsValid())
			{
				FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
			}
			Writer->Close();
			Target->SetStringField(Key, JsonStr);
			break;
		}
	}
}

// ============================================================================
// Execute Tool Calls DAG — parallel scheduling with $tN.field.path resolution
// ============================================================================
void SMCPToolboxChatWidget::ExecuteToolCallsDAG(
	const TArray<TSharedPtr<FJsonValue>>& ToolCalls,
	const TArray<TSharedPtr<FJsonValue>>& SentMessages,
	const TSharedPtr<FJsonObject>& AssistantMsg)
{
	UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] Starting DAG execution for %d tool calls"), ToolCalls.Num());

	TArray<TSharedPtr<FJsonObject>> DAGCalls;
	ConvertToolCallsToDAGFormat(ToolCalls, DAGCalls);

	TSet<FString> AvailableToolIds;
	if (MCPServerClient.IsConnected())
	{
		for (const TSharedPtr<FJsonObject>& Tool : MCPServerClient.GetTools())
		{
			FString ToolName;
			if (Tool->TryGetStringField(TEXT("name"), ToolName))
				AvailableToolIds.Add(ToolName);
		}
	}

	// 添加本地工具到可用列表
	{
		// Use GetAllFunctions() directly (avoids JSON serialization round-trip)
		for (const auto& Pair : ToolFunctionTable->GetAllFunctions())
			AvailableToolIds.Add(FString(Pair.first.c_str()));
	}

	FExecutionPlan Plan;
	FString PlanError;
	if (!ExecutionPlanner.CreatePlan(DAGCalls, AvailableToolIds, Plan, PlanError))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[DAG] Plan failed: %s, fallback to original path"), *PlanError);
		return;
	}

	// 显示 DAG 可视化
	FString DAGVisual = ExecutionPlanner.VisualizeDAG(Plan);
	UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] Plan:\n%s"), *DAGVisual);

	FMCPToolboxChatMessage DAGMsg;
	DAGMsg.Role = EMCPToolboxMessageRole::Thinking;
	DAGMsg.Content = FString::Printf(TEXT("DAG 并行执行 (加速比: %.1fx)\n```\n%s\n```"),
		ExecutionPlanner.EstimateSpeedup(Plan), *DAGVisual);
	AddMessage(DAGMsg);

	// 准备消息列表
	TSharedPtr<TArray<TSharedPtr<FJsonValue>>> NewMsgs = MakeShared<TArray<TSharedPtr<FJsonValue>>>(SentMessages);
	NewMsgs->Add(MakeShareable(new FJsonValueObject(AssistantMsg)));

	// 构建 ID 映射
	TMap<FString, FString> DAGToOrigId;
	TMap<FString, FString> DAGToToolName;
	TMap<FString, FString> DAGToArgs;
	for (int32 i = 0; i < ToolCalls.Num() && i < DAGCalls.Num(); ++i)
	{
		FString OrigId;
		ToolCalls[i]->AsObject()->TryGetStringField(TEXT("id"), OrigId);
		FString DAGId;
		DAGCalls[i]->TryGetStringField(TEXT("task_id"), DAGId);
		FString ToolName;
		DAGCalls[i]->TryGetStringField(TEXT("tool_id"), ToolName);

		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		FString ArgsStr;
		if (DAGCalls[i]->TryGetObjectField(TEXT("parameters"), ParamsObj) && ParamsObj->IsValid())
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsStr);
			FJsonSerializer::Serialize(ParamsObj->ToSharedRef(), Writer);
		}

		DAGToOrigId.Add(DAGId, OrigId.IsEmpty() ? DAGId : OrigId);
		DAGToToolName.Add(DAGId, ToolName);
		DAGToArgs.Add(DAGId, ArgsStr);
	}

	// 获取并行批次
	TArray<TArray<FDAGTaskNode>> Batches;
	ExecutionPlanner.GetParallelBatches(Plan, Batches);

	UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] %d batches to execute"), Batches.Num());

	// 批次执行状态
	TSharedPtr<TMap<FString, FTaskExecutionResult>> AllResults = MakeShared<TMap<FString, FTaskExecutionResult>>();
	TSharedPtr<int32> CurrentBatch = MakeShared<int32>(0);

	TSharedPtr<TFunction<void()>> ExecuteNextBatchPtr = MakeShared<TFunction<void()>>();
	TWeakPtr<TFunction<void()>> WeakNextBatch = ExecuteNextBatchPtr;

	*ExecuteNextBatchPtr = [this, Batches, CurrentBatch, AllResults, NewMsgs, DAGToOrigId, DAGToToolName, WeakNextBatch]()
	{
		TSharedPtr<TFunction<void()>> ExecuteNextBatch = WeakNextBatch.Pin();
		if (!ExecuteNextBatch)
			return;

		if (*CurrentBatch >= Batches.Num())
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] All batches complete, total tasks: %d"), AllResults->Num());

			for (const auto& Pair : *AllResults)
			{
				const FTaskExecutionResult& Res = Pair.Value;
				const FString& DAGId = Res.TaskId;

				const FString* OrigId = DAGToOrigId.Find(DAGId);
				const FString* ToolName = DAGToToolName.Find(DAGId);

				FString CallId = OrigId ? *OrigId : DAGId;
				FString Name = ToolName ? *ToolName : DAGId;

				// 结果消息
				FMCPToolboxChatMessage ResultMsg;
				ResultMsg.Role = EMCPToolboxMessageRole::System;
				FString Status = Res.bSuccess ? TEXT("[OK]") : TEXT("[FAIL]");
				ResultMsg.Content = FString::Printf(
					TEXT("**%s %s** (%dms)\n```\n%s\n```"),
					*Status, *Name,
					FMath::RoundToInt(Res.LatencyMs),
					*Res.ResultJson.Left(500));
				AddMessage(ResultMsg);

				// 构建 tool 消息
				TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
				ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
				ToolResultMsg->SetStringField(TEXT("tool_call_id"), CallId);
				ToolResultMsg->SetStringField(TEXT("name"), Name);
				ToolResultMsg->SetStringField(TEXT("content"), Res.ResultJson);
				NewMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

				// 截图结果处理：添加用户消息包含图片
				if (Name == TEXT("screenshot") && Res.bSuccess)
				{
					TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Res.ResultJson);
					TSharedPtr<FJsonObject> ResultObj;
					if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
					{
						FString DataURI;
						if (ResultObj->TryGetStringField(TEXT("data_uri"), DataURI) && !DataURI.IsEmpty())
						{
							TArray<TSharedPtr<FJsonValue>> ContentArray;

							TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
							TextPart->SetStringField(TEXT("type"), TEXT("text"));
							TextPart->SetStringField(TEXT("text"), TEXT("Here is the screenshot you requested. Please analyze it."));
							ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));

							TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject());
							ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));
							TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject());
							ImageUrlObj->SetStringField(TEXT("url"), DataURI);
							ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);
							ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));

							TSharedPtr<FJsonObject> ScreenshotUserMsg = MakeShareable(new FJsonObject());
							ScreenshotUserMsg->SetStringField(TEXT("role"), TEXT("user"));
							ScreenshotUserMsg->SetArrayField(TEXT("content"), ContentArray);
							NewMsgs->Add(MakeShareable(new FJsonValueObject(ScreenshotUserMsg)));

							FMCPToolboxChatMessage ScreenshotMsg;
							ScreenshotMsg.Role = EMCPToolboxMessageRole::User;
							ScreenshotMsg.Content = TEXT("（截图已捕获，正在分析...）");
							ScreenshotMsg.bHasImageAttachment = true;
							ScreenshotMsg.ImageDataURIs.Add(DataURI);
						ScreenshotMsg.ImageFileNames.Add(TEXT("screenshot.png"));
						AddMessage(ScreenshotMsg);
					}
				}
			}
		}

			// IdleSpec: inject speculation hint to accelerate next round
			InjectSpeculationHint(*NewMsgs, LastSpeculation, bSpeculationPending);

			UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] Done, continuing conversation"));
			SendAIRequest(*NewMsgs);
			return;
		}

		const TArray<FDAGTaskNode>& Batch = Batches[*CurrentBatch];
		(*CurrentBatch)++;

		UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] Executing batch %d with %d tasks"), *CurrentBatch, Batch.Num());

		if (Batch.Num() == 0)
		{
			(*ExecuteNextBatch)();
			return;
		}

		TSharedPtr<int32> Pending = MakeShared<int32>(Batch.Num());

		for (const FDAGTaskNode& Task : Batch)
		{
			FString TaskId = Task.TaskId;
			FString ToolId = Task.ToolId;

			// 解析参数依赖 ($tN.field.path 语法 — Stage 5: 真正字段路径解析)
			TSharedPtr<FJsonObject> ResolvedParams = MakeShareable(new FJsonObject());
			if (Task.Parameters.IsValid())
			{
				for (const auto& ParamPair : Task.Parameters->Values)
				{
					const FString Key(ParamPair.Key);
					const TSharedPtr<FJsonValue>& Val = ParamPair.Value;

					// 仅字符串值可能是 $tN.field 引用
					if (Val->Type == EJson::String)
					{
						FString StrVal = Val->AsString();
						if (StrVal.StartsWith(TEXT("$")))
						{
							// 解析 $tN.field.path 语法
							FString Ref = StrVal.Mid(1);             // 去掉前缀 $
							FString RefTaskId = Ref;
							FString FieldPath;

							int32 DotIdx;
							if (Ref.FindChar(TEXT('.'), DotIdx))
							{
								RefTaskId = Ref.Left(DotIdx);
								FieldPath = Ref.Mid(DotIdx + 1);
							}

							const FTaskExecutionResult* DepResult = AllResults->Find(RefTaskId);
							if (DepResult)
							{
								// 用 ResolveDAGFieldPath 真正解析字段路径
								TSharedPtr<FJsonValue> ResolvedValue = ResolveDAGFieldPath(DepResult->ResultJson, FieldPath);
								if (ResolvedValue.IsValid())
								{
									// 按原始类型设置(保留 string/number/bool/array/object)
									SetResolvedParam(ResolvedParams, Key, ResolvedValue);
									UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] Resolved $%s.%s -> type=%d"), *RefTaskId, *FieldPath, (int32)ResolvedValue->Type);
								}
								else
								{
									// 字段路径无法解析 — 返回结构化错误 + 回退到整个结果
									UE_LOG(LogMCPToolbox, Warning, TEXT("[DAG] Unresolved field path '%s' from task %s, fallback to whole result"), *FieldPath, *RefTaskId);
									ResolvedParams->SetStringField(Key, MCPToolboxErrorFormat::FormatGenericError(
										EMCPToolboxErrorCode::DagRefUnresolved,
										FString::Printf(TEXT("Cannot resolve field path '%s' from task '%s' result"), *FieldPath, *RefTaskId)
									));
								}
								continue;
							}
							else
							{
								// 引用任务不存在 — 返回结构化错误
								UE_LOG(LogMCPToolbox, Warning, TEXT("[DAG] Unresolved task ref '%s' in param '%s'"), *RefTaskId, *Key);
								ResolvedParams->SetStringField(Key, MCPToolboxErrorFormat::FormatGenericError(
									EMCPToolboxErrorCode::DagRefUnresolved,
									FString::Printf(TEXT("Task ref '$%s' not found in completed results"), *RefTaskId)
								));
								continue;
							}
						}
					}
					// 普通值 — 原样保留
					ResolvedParams->SetField(Key, Val);
				}

				// Stage 5: 对解析后的参数应用 JsonValueHelper CoerceObject(字符串→强类型自动升级)
				ResolvedParams = FMCPToolboxJsonValueHelper::CoerceObject(ResolvedParams);
			}

			FString ArgsJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
			FJsonSerializer::Serialize(ResolvedParams.ToSharedRef(), Writer);

			double StartTime = FDateTime::Now().ToUnixTimestamp() * 1000.0;

			// ── call_tool 特殊处理 (DAG 模式) ──
			if (ToolId == TEXT("call_tool"))
			{
				if (!MCPServerClient.IsConnected())
				{
					FTaskExecutionResult Res;
					Res.TaskId = TaskId;
					Res.bSuccess = false;
					Res.ResultJson = TEXT("{\"error\":\"MCP server not connected\"}");
					Res.LatencyMs = 0;
					Res.Attempts = 1;
					AllResults->Add(TaskId, Res);
					(*Pending)--;
					if (*Pending <= 0) (*ExecuteNextBatch)();
					continue;
				}

				TSharedPtr<FJsonObject> ArgsObj;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
				if (FJsonSerializer::Deserialize(Reader, ArgsObj) && ArgsObj.IsValid())
				{
					FString ToolsetName, RealToolName, RealArgsJson;
					ArgsObj->TryGetStringField(TEXT("toolset_name"), ToolsetName);
					ArgsObj->TryGetStringField(TEXT("tool_name"), RealToolName);

					const TSharedPtr<FJsonObject>* ArgsPtr;
					if (ArgsObj->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr->IsValid())
					{
						FString OutputJson;
						TSharedRef<TJsonWriter<>> RealArgsWriter = TJsonWriterFactory<>::Create(&OutputJson);
						FJsonSerializer::Serialize((*ArgsPtr).ToSharedRef(), RealArgsWriter);
						RealArgsJson = OutputJson;
					}

					if (!ToolsetName.IsEmpty() && !RealToolName.IsEmpty())
					{
						FString FullToolName = ToolsetName + TEXT(".") + RealToolName;
						UE_LOG(LogMCPToolbox, Log, TEXT("[DAG] call_tool -> MCP tool: %s"), *FullToolName);

						MCPServerClient.ExecuteTool(FullToolName, RealArgsJson,
							[this, TaskId, Pending, StartTime, AllResults, ExecuteNextBatch](bool bOk, const FString& Result)
							{
								FTaskExecutionResult Res;
								Res.TaskId = TaskId;
								Res.bSuccess = bOk;
								Res.ResultJson = Result;
								Res.LatencyMs = FDateTime::Now().ToUnixTimestamp() * 1000.0 - StartTime;
								Res.Attempts = 1;
								AllResults->Add(TaskId, Res);

								(*Pending)--;
								if (*Pending <= 0)
								{
									(*ExecuteNextBatch)();
								}
							});
						continue;
					}
				}

				FTaskExecutionResult Res;
				Res.TaskId = TaskId;
				Res.bSuccess = false;
				Res.ResultJson = TEXT("{\"error\":\"Invalid call_tool parameters\"}");
				Res.LatencyMs = 0;
				Res.Attempts = 1;
				AllResults->Add(TaskId, Res);
				(*Pending)--;
				if (*Pending <= 0) (*ExecuteNextBatch)();
				continue;
			}

			// 判断工具类型
			bool bIsMCP = MCPServerClient.IsConnected() && MCPServerClient.IsMCPTool(ToolId);

			if (bIsMCP)
			{
				// IdleSpec: speculative execution for DAG mode
				LaunchIdleSpec(ToolId);

				MCPServerClient.ExecuteTool(ToolId, ArgsJson,
					[this, TaskId, Pending, StartTime, AllResults, ExecuteNextBatch](bool bOk, const FString& Result)
					{
						FTaskExecutionResult Res;
						Res.TaskId = TaskId;
						Res.bSuccess = bOk;
						Res.ResultJson = Result;
						Res.LatencyMs = FDateTime::Now().ToUnixTimestamp() * 1000.0 - StartTime;
						Res.Attempts = 1;
						AllResults->Add(TaskId, Res);

						(*Pending)--;
						if (*Pending <= 0)
						{
							(*ExecuteNextBatch)();
						}
					});
			}
			else
			{
				// 本地工具：放到后台线程执行
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
					[this, TaskId, ToolId, ArgsJson, Pending, StartTime, AllResults, ExecuteNextBatch]()
					{
						FString Result;
						bool bSuccess = false;
						try
						{
							Result = ExecuteToolCall(ToolId, ArgsJson);
							bSuccess = true;
						}
						catch (...)
						{
							Result = TEXT("Exception during tool execution");
							bSuccess = false;
						}

						double Latency = FDateTime::Now().ToUnixTimestamp() * 1000.0 - StartTime;

						AsyncTask(ENamedThreads::GameThread,
							[TaskId, bSuccess, Result, Latency, Pending, AllResults, ExecuteNextBatch]()
							{
								FTaskExecutionResult Res;
								Res.TaskId = TaskId;
								Res.bSuccess = bSuccess;
								Res.ResultJson = Result;
								Res.LatencyMs = Latency;
								Res.Attempts = 1;
								AllResults->Add(TaskId, Res);

								(*Pending)--;
								if (*Pending <= 0)
								{
									(*ExecuteNextBatch)();
								}
							});
					});
			}
		}
	};

	(*ExecuteNextBatchPtr)();
}

FString SMCPToolboxChatWidget::EncodeFileToDataURI(const FString& FilePath) const
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *FilePath)) return TEXT("");
	return FString::Printf(TEXT("data:%s;base64,%s"), *GetMimeType(FPaths::GetExtension(FilePath)), *FBase64::Encode(Data));
}

FString SMCPToolboxChatWidget::GetMimeType(const FString& Ext) const
{
	if (Ext == TEXT("png"))  return TEXT("image/png");
	if (Ext == TEXT("jpg") || Ext == TEXT("jpeg")) return TEXT("image/jpeg");
	if (Ext == TEXT("gif"))  return TEXT("image/gif");
	if (Ext == TEXT("bmp"))  return TEXT("image/bmp");
	return TEXT("application/octet-stream");
}

// ============================================================================
// Clean Markdown → readable text (inspired by PengyCPP's 4-phase pipeline)
// ============================================================================
FString SMCPToolboxChatWidget::CleanMarkdownToText(const FString& Markdown)
{
	FString Result;
	Result.Reserve(Markdown.Len() + 256);

	bool bInCodeBlock = false;
	FString CodeLang;
	TArray<FString> Lines;
	Markdown.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();

		// Phase 1: Code blocks (process first, like PengyCPP)
		if (Trimmed.StartsWith(TEXT("```")))
		{
			if (!bInCodeBlock)
			{
				bInCodeBlock = true;
				CodeLang = Trimmed.RightChop(3).TrimStartAndEnd();
				Result += CodeLang.IsEmpty() ? TEXT("[Code]\n") : FString::Printf(TEXT("[%s]\n"), *CodeLang);
			}
			else
			{
				bInCodeBlock = false;
				Result += TEXT("\n");
			}
			continue;
		}

		if (bInCodeBlock)
		{
			Result += TEXT("  ") + Line + TEXT("\n"); // indent code blocks
			continue;
		}

		// Phase 2: Headings
		FString Clean = Trimmed;
		if (Clean.StartsWith(TEXT("### ")))
			Clean = Clean.RightChop(4);
		else if (Clean.StartsWith(TEXT("## ")))
			Clean = Clean.RightChop(3);
		else if (Clean.StartsWith(TEXT("# ")))
			Clean = Clean.RightChop(2);

		// Phase 3: Inline formatting
		Clean = Clean.Replace(TEXT("**"), TEXT(""));   // bold
		Clean = Clean.Replace(TEXT("__"), TEXT(""));   // bold alt
		Clean = Clean.Replace(TEXT("*"), TEXT(""));    // italic (also list bullets, acceptable tradeoff)
		Clean = Clean.Replace(TEXT("`"), TEXT(""));    // inline code
		
		// Phase 4: Tables — convert to readable text
		if (Clean.Contains(TEXT("|")) && Clean.TrimStartAndEnd().StartsWith(TEXT("|")))
		{
			// Skip separator rows like |---|----|
			if (Clean.Contains(TEXT("---")))
				continue;
			// Clean up table cells
			Clean = Clean.Replace(TEXT("|"), TEXT(" "));
		}

		// Blockquotes
		if (Clean.StartsWith(TEXT("> ")))
			Clean = TEXT("  ") + Clean.RightChop(2);

		// Horizontal rules
		if (Clean == TEXT("---") || Clean == TEXT("***") || Clean == TEXT("___"))
		{
			Result += TEXT("────────────────\n");
			continue;
		}

		Result += Clean + TEXT("\n");
	}

	// Trim excess trailing newlines
	while (Result.EndsWith(TEXT("\n")))
		Result.RemoveFromEnd(TEXT("\n"));

	return Result;
}

// ============================================================================
// Session Management Implementation
// ============================================================================
void SMCPToolboxChatWidget::RefreshSessionList()
{
	SessionOptions.Empty();
	SessionIdToTitle.Empty();
	SessionIdToPreview.Empty();

	TArray<FMCPToolboxChatSession*> Sessions = FMCPToolboxChatSessionManager::Get().GetAllSessions();
	for (FMCPToolboxChatSession* Session : Sessions)
	{
		SessionOptions.Add(MakeShareable(new FString(Session->SessionId)));
		SessionIdToTitle.Add(Session->SessionId, Session->Title);
		SessionIdToPreview.Add(Session->SessionId, Session->GetPreviewText());
	}

	if (SessionListView.IsValid())
	{
		SessionListView->RequestListRefresh();
	}
}

FReply SMCPToolboxChatWidget::OnNewChat()
{
	FMCPToolboxChatSessionManager::Get().CreateNewSession(TEXT("新对话"));
	RefreshSessionList();
	
	Messages.Empty();
	FMCPToolboxChatMessage WelcomeMsg;
	WelcomeMsg.Role = EMCPToolboxMessageRole::Assistant;
	WelcomeMsg.Content = TEXT("**欢迎使用 MCP Toolbox！**\n\n"
		"- 先在 \"API密钥\" Tab 添加密钥（选择服务商→选模型→填密钥→添加）\n"
		"- 点击 \"启动MCP\" 启动MCP服务器\n"
		"- AI会自动调用 UE5 编辑器工具（截图、命令执行、Actor选择等）\n\n"
		"支持 22+ 国内外服务商。");
	Messages.Add(WelcomeMsg);
	UpdateCurrentSessionWithMessage(WelcomeMsg);
	
	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ClearChildren();
		RebuildChatDisplay();
	}

	if (SessionListView.IsValid())
	{
		SessionListView->RequestListRefresh();
	}

	return FReply::Handled();
}

void SMCPToolboxChatWidget::OnSessionSelected(TSharedPtr<FString> SessionId, ESelectInfo::Type SelectInfo)
{
	if (!SessionId.IsValid()) return;
	SwitchToSession(*SessionId);
}

void SMCPToolboxChatWidget::SwitchToSession(const FString& SessionId)
{
	// Guard against switching while a streaming response is in flight.
	// The streaming ticker holds a bare pointer (StreamPtr = &Messages.Last())
	// into the Messages array; replacing Messages here would free that memory
	// and the ticker would write to freed memory (use-after-free / crash).
	// Also the in-flight stream content has not been persisted yet, so switching
	// would silently discard the partial AI reply.
	if (bIsStreaming || bIsWaiting)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Ignored session switch while streaming/waiting"));
		return;
	}

	if (FMCPToolboxChatSessionManager::Get().SetCurrentSession(SessionId))
	{
		FMCPToolboxChatSession* Session = FMCPToolboxChatSessionManager::Get().GetCurrentSession();
		if (Session)
		{
			Messages = Session->Messages;
			RebuildChatDisplay();
		}
	}

	if (SessionListView.IsValid())
	{
		SessionListView->RequestListRefresh();
	}
}

void SMCPToolboxChatWidget::OnDeleteSession(const FString& SessionId)
{
	FMCPToolboxChatSession* CurrentSession = FMCPToolboxChatSessionManager::Get().GetCurrentSession();
	bool bWasCurrent = CurrentSession && CurrentSession->SessionId == SessionId;

	FMCPToolboxChatSessionManager::Get().DeleteSession(SessionId);
	RefreshSessionList();

	if (bWasCurrent)
	{
		FMCPToolboxChatSession* NewCurrent = FMCPToolboxChatSessionManager::Get().GetCurrentSession();
		if (NewCurrent)
		{
			Messages = NewCurrent->Messages;
		}
		else
		{
			Messages.Empty();
			FMCPToolboxChatMessage WelcomeMsg;
			WelcomeMsg.Role = EMCPToolboxMessageRole::Assistant;
			WelcomeMsg.Content = TEXT("**欢迎使用 MCP Toolbox！**\n\n"
				"- 先在 \"API密钥\" Tab 添加密钥（选择服务商→选模型→填密钥→添加）\n"
				"- 点击 \"启动MCP\" 启动MCP服务器\n"
				"- AI会自动调用 UE5 编辑器工具（截图、命令执行、Actor选择等）\n\n"
				"支持 22+ 国内外服务商。");
			Messages.Add(WelcomeMsg);
		}
		RebuildChatDisplay();
	}
}

void SMCPToolboxChatWidget::UpdateCurrentSessionWithMessage(const FMCPToolboxChatMessage& Message)
{
	FMCPToolboxChatSessionManager::Get().AddMessageToCurrentSession(Message);
	RefreshSessionList();
}

// ============================================================================
// Widget State Persistence — restores vision/sidebar/more-expanded across restarts
// ============================================================================
FString SMCPToolboxChatWidget::GetWidgetStatePath() const
{
	return FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".mcptoolbox"), TEXT("widget_state.json"));
}

void SMCPToolboxChatWidget::LoadWidgetState()
{
	FString Path = GetWidgetStatePath();
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return;

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	Json->TryGetBoolField(TEXT("vision_enabled"), bVisionModeEnabled);
	Json->TryGetBoolField(TEXT("sidebar_collapsed"), bSidebarCollapsed);
	Json->TryGetBoolField(TEXT("more_expanded"), bMoreExpanded);
	Json->TryGetBoolField(TEXT("summary_declined"), bSummaryDeclined);
	Json->TryGetBoolField(TEXT("auto_archive_enabled"), bAutoArchiveEnabled);
	Json->TryGetBoolField(TEXT("image_generation_enabled"), bImageGenerationMode);

	// Stage 6.3 + 阶段 A1: 读取 disabled_skills 数组,委托给 SkillService
	{
		TArray<FString> LoadedDisabled;
		const TArray<TSharedPtr<FJsonValue>>* DisabledArr = nullptr;
		if (Json->TryGetArrayField(TEXT("disabled_skills"), DisabledArr) && DisabledArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *DisabledArr)
			{
				FString S = V->AsString();
				if (!S.IsEmpty()) LoadedDisabled.AddUnique(S);
			}
		}
		SkillService.SetDisabledSkills(LoadedDisabled);
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Widget state loaded: vision=%d sidebar=%d more=%d summary_declined=%d auto_archive=%d disabled_skills=%d"),
		(int32)bVisionModeEnabled, (int32)bSidebarCollapsed, (int32)bMoreExpanded, (int32)bSummaryDeclined, (int32)bAutoArchiveEnabled, SkillService.GetDisabledSkills().Num());
}

void SMCPToolboxChatWidget::SaveWidgetState() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetBoolField(TEXT("vision_enabled"), bVisionModeEnabled);
	Json->SetBoolField(TEXT("sidebar_collapsed"), bSidebarCollapsed);
	Json->SetBoolField(TEXT("more_expanded"), bMoreExpanded);
	Json->SetBoolField(TEXT("summary_declined"), bSummaryDeclined);
	Json->SetBoolField(TEXT("auto_archive_enabled"), bAutoArchiveEnabled);
	Json->SetBoolField(TEXT("image_generation_enabled"), bImageGenerationMode);

	// Stage 6.3 + 阶段 A1: 持久化 disabled_skills(从 SkillService 读取)
	TArray<TSharedPtr<FJsonValue>> DisabledArr;
	for (const FString& S : SkillService.GetDisabledSkills())
	{
		DisabledArr.Add(MakeShared<FJsonValueString>(S));
	}
	Json->SetArrayField(TEXT("disabled_skills"), DisabledArr);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	FString Path = GetWidgetStatePath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Output, *Path);
}

// ============================================================================
// Skill Manager — enable/disable individual skills (Stage 6.3)
// IsSkillDisabled / ToggleSkillEnabled 已移至 FMCPToolboxSkillService (阶段 A1)
// widget 通过 SkillService 成员委托调用
// ============================================================================

FReply SMCPToolboxChatWidget::OnOpenSkillManager()
{
	// 防重复打开
	if (SkillManagerWindow.IsValid())
	{
		TSharedPtr<SWindow> Existing = SkillManagerWindow.Pin();
		Existing->BringToFront();
		return FReply::Handled();
	}

	// 扫描 .mcptoolbox/toolsets/*.md,提取 skill 列表(name + description + enabled)
	FString ToolsetsDir = FPaths::ProjectDir() / TEXT(".mcptoolbox") / TEXT("toolsets");
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*ToolsetsDir))
	{
		// 目录不存在,提示用户先刷新工具
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(TEXT("Skill 目录不存在。请先点击 \"获取工具\" 按钮生成 .mcptoolbox/toolsets/ 后再使用 Skill 管理。")));
		return FReply::Handled();
	}

	TArray<FString> MdFiles;
	PF.FindFiles(MdFiles, *ToolsetsDir, TEXT(".md"));
	MdFiles.Sort();

	if (MdFiles.Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(TEXT("没有发现任何 skill 文件。请先点击 \"获取工具\" 按钮发现并缓存 MCP 工具集。")));
		return FReply::Handled();
	}

	// 准备 skill 信息(name + description + enabled)用于 UI 显示
	struct FSkillItem
	{
		FString Name;
		FString Description;
		bool bEnabled;
	};
	TArray<FSkillItem> Items;
	Items.Reserve(MdFiles.Num());
	for (const FString& FilePath : MdFiles)
	{
		FSkillItem It;
		It.Name = FPaths::GetBaseFilename(FilePath);
		It.bEnabled = !SkillService.IsSkillDisabled(It.Name);

		FString Content;
		if (FFileHelper::LoadFileToString(Content, *FilePath))
		{
			FString Desc, Body;
			SkillService.ParseFrontmatter(Content, Desc, Body);
			It.Description = Desc;
		}
		Items.Add(MoveTemp(It));
	}

	// 创建独立窗口
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Skill 管理")))
		.ClientSize(FVector2D(640, 520))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.AutoCenter(EAutoCenter::PreferredWorkArea);

	// 窗口内容: 标题 + 列表 + 底部状态栏
	TSharedRef<SVerticalBox> ContentBox = SNew(SVerticalBox);

	// 顶部说明
	ContentBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(12, 10, 12, 6))
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("勾选启用 / 取消勾选禁用对应 skill。禁用后 LLM 在 list_skills 中看不到该 skill。")))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
		];

	// Skill 列表(每行一项: 复选框 + 名称 + description)
	TSharedRef<SScrollBox> ListScroll = SNew(SScrollBox);
	for (const FSkillItem& Item : Items)
	{
		TSharedRef<SWidget> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(12, 4, 8, 4))
			[
				SNew(SCheckBox)
				.IsChecked(Item.bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged_Lambda([this, Name = Item.Name](ECheckBoxState NewState)
				{
					const bool bWantsEnabled = (NewState == ECheckBoxState::Checked);
					const bool bCurrentlyEnabled = !SkillService.IsSkillDisabled(Name);
					if (bWantsEnabled != bCurrentlyEnabled)
					{
						SkillService.ToggleSkillEnabled(Name);
					}
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0, 4, 12, 4))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item.Name))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FLinearColor(0.92f, 0.92f, 0.95f))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0, 2, 0, 0))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item.Description))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
					.WrapTextAt(560.0f)
				]
			];

		ListScroll->AddSlot()
			[
				Row
			];
	}

	ContentBox->AddSlot()
		.FillHeight(1.0f)
		[
			ListScroll
		];

	// 底部状态栏:总览 + 关闭按钮
	const int32 EnabledCount = Items.FilterByPredicate([](const FSkillItem& I) { return I.bEnabled; }).Num();
	ContentBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(12, 8, 12, 10))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::Format(FText::FromString(TEXT("已启用 {0} / {1} 个 skill")), EnabledCount, Items.Num()))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FLinearColor(0.7f, 0.85f, 0.7f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.ButtonColorAndOpacity(FLinearColor(0.15f, 0.28f, 0.18f, 1.0f))
				.ContentPadding(FMargin(20, 4))
				.OnClicked_Lambda([WindowRef = TWeakPtr<SWindow>(Window)]() -> FReply
				{
					TSharedPtr<SWindow> W = WindowRef.Pin();
					if (W.IsValid()) W->RequestDestroyWindow();
					return FReply::Handled();
				})
				.Content()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("关闭")))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.ColorAndOpacity(FLinearColor(0.85f, 0.95f, 0.85f))
				]
			]
		];

	Window->SetContent(ContentBox);

	// 注册窗口销毁回调,清理 WeakPtr
	Window->GetOnWindowClosedEvent().AddLambda([this](const TSharedRef<SWindow>&)
	{
		SkillManagerWindow.Reset();
	});

	SkillManagerWindow = Window;

	// 添加到 Slate 应用(独立顶级窗口,跟随现有 OnArchiveSummary 的模式)
	FSlateApplication::Get().AddWindow(Window);

	return FReply::Handled();
}

// ============================================================================
// Conversation Summary (Archive) — one-click session summarization
// ============================================================================
FReply SMCPToolboxChatWidget::OnArchiveSummary()
{
	// Prevent duplicate dialog
	if (SummaryDialogWindow.IsValid())
	{
		SummaryDialogWindow.Pin()->BringToFront();
		return FReply::Handled();
	}

	if (bIsStreaming || bIsWaiting)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Cannot archive summary while streaming/waiting"));
		return FReply::Handled();
	}

	// Must have at least one user message
	bool bHasUserMsg = false;
	for (const FMCPToolboxChatMessage& M : Messages)
	{
		if (M.Role == EMCPToolboxMessageRole::User) { bHasUserMsg = true; break; }
	}
	if (!bHasUserMsg)
	{
		FMCPToolboxChatMessage Hint;
		Hint.Role = EMCPToolboxMessageRole::System;
		Hint.Content = TEXT("没有可总结的内容：请先与 AI 进行一轮对话后再点击'归档总结'。");
		AddMessage(Hint);
		return FReply::Handled();
	}

	ShowSummaryChoiceDialog();
	return FReply::Handled();
}

FString SMCPToolboxChatWidget::BuildSummaryPrompt(bool bForTools, bool bForMemory) const
{
	FString Prompt;

	if (bForTools)
	{
		Prompt += TEXT(
			"请总结以下对话中所有 MCP 工具的使用经验。\n\n"
			"输出格式（严格遵守）：\n"
			"### <toolset_name>.<tool_name>\n"
			"- **作用**: 一句话描述\n"
			"- **调用要点**: 参数如何填写，注意事项\n"
			"- **示例**: `call_tool(toolset_name=..., tool_name=..., arguments={...})`\n\n"
			"要求：\n"
			"1. 只提取实际调用过的工具，不要虚构\n"
			"2. 同一工具的不同用法合并为一个条目\n"
			"3. 包含失败案例（如有），说明失败原因和正确做法\n"
			"4. 用中文输出\n\n"
			"对话内容：\n\n"
		);
	}
	else if (bForMemory)
	{
		Prompt += TEXT(
			"请总结以下对话中的核心要点、决策、项目状态和用户偏好。\n\n"
			"输出格式（严格遵守）：\n"
			"### 项目状态\n"
			"- 项目名称、当前进展、未完成的工作\n\n"
			"### 关键决策\n"
			"- 重要的设计选择和理由\n\n"
			"### 用户偏好\n"
			"- 工作方式、代码风格、工具选择偏好\n\n"
			"### 已知问题\n"
			"- 待解决的 bug、已知限制\n\n"
			"要求：\n"
			"1. 只提取对话中实际出现的内容，不要虚构\n"
			"2. 简洁明了，每个要点一行\n"
			"3. 用中文输出\n\n"
			"对话内容：\n\n"
		);
	}

	// Append conversation transcript (limit to ~30K chars to avoid token overflow)
	const int32 MaxChars = 30000;
	FString Transcript;
	for (const FMCPToolboxChatMessage& M : Messages)
	{
		FString RoleStr;
		switch (M.Role)
		{
		case EMCPToolboxMessageRole::User:      RoleStr = TEXT("用户"); break;
		case EMCPToolboxMessageRole::Assistant: RoleStr = TEXT("AI");  break;
		case EMCPToolboxMessageRole::System:    RoleStr = TEXT("系统"); break;
		case EMCPToolboxMessageRole::Thinking:  RoleStr = TEXT("思考"); break;
		default:                                 RoleStr = TEXT("?");   break;
		}

		Transcript += FString::Printf(TEXT("[%s] %s\n\n"), *RoleStr, *M.Content);

		if (Transcript.Len() > MaxChars)
		{
			Transcript = Transcript.Left(MaxChars) + TEXT("\n...(对话被截断)...\n");
			break;
		}
	}

	Prompt += Transcript;
	return Prompt;
}

void SMCPToolboxChatWidget::OnSummaryGenerated(bool bSuccess, const FString& ToolsSummary, const FString& MemorySummary)
{
	if (!bSuccess && ToolsSummary.IsEmpty() && MemorySummary.IsEmpty())
	{
		FMCPToolboxChatMessage FailMsg;
		FailMsg.Role = EMCPToolboxMessageRole::System;
		FailMsg.Content = TEXT("**归档总结失败**\n\n可能原因：\n- 本地辅助模型未启动或异常\n- 主模型 API key 无效或网络超时\n\n可从工具栏'更多 → 归档总结'重新触发。");
		AddMessage(FailMsg);
		// Reset auto-archive guard on failure too, so retry is possible
		bIsAutoArchiving = false;
		return;
	}

	// Update in-memory caches (kept across the session so subsequent BuildSystemPrompt calls pick them up)
	if (!ToolsSummary.IsEmpty()) CachedToolsSummary = ToolsSummary;
	if (!MemorySummary.IsEmpty()) CachedMemorySummary = MemorySummary;
	bSystemPromptDirty = true;  // summary changed, rebuild system prompt

	// Persist to ~/.mcptoolbox/conversation_summary.md (overwrite) + summaries/*.md (archive)
	FMCPToolboxMemoryManager::Get().SaveConversationSummary(
		ToolsSummary.IsEmpty() ? CachedToolsSummary : ToolsSummary,
		MemorySummary.IsEmpty() ? CachedMemorySummary : MemorySummary);

	FString ToolsInfo = ToolsSummary.IsEmpty()
		? TEXT("未生成（已沿用上次缓存）")
		: FString::Printf(TEXT("%d 字符"), ToolsSummary.Len());
	FString MemInfo = MemorySummary.IsEmpty()
		? TEXT("未生成（已沿用上次缓存）")
		: FString::Printf(TEXT("%d 字符"), MemorySummary.Len());

	FMCPToolboxChatMessage DoneMsg;
	DoneMsg.Role = EMCPToolboxMessageRole::System;
	DoneMsg.Content = TEXT("**归档总结完成**\n\n");
	DoneMsg.Content += FString::Printf(TEXT("- 工具使用归档: %s\n"), *ToolsInfo);
	DoneMsg.Content += FString::Printf(TEXT("- 记忆归档: %s\n"), *MemInfo);
	DoneMsg.Content += TEXT("\n已保存到 `~/.mcptoolbox/conversation_summary.md`，并归档到 `~/.mcptoolbox/summaries/`。下次启动编辑器将自动加载到系统提示词，无需用户重复说明上下文。");
	AddMessage(DoneMsg);

	// Reset auto-archive guard so the next idle window can trigger again
	bIsAutoArchiving = false;
}

void SMCPToolboxChatWidget::GenerateSummary(ESummaryModelChoice ModelChoice, bool bArchiveTools, bool bArchiveMemory)
{
	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] GenerateSummary: model=%u tools=%d memory=%d"),
		(uint8)ModelChoice, (int32)bArchiveTools, (int32)bArchiveMemory);

	if (!bArchiveTools && !bArchiveMemory)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] GenerateSummary: no archive type selected"));
		return;
	}

	bool bLocalReady = FMCPToolboxAuxModelManager::Get().IsReady();
	bool bUseLocalForTools = false;
	bool bUseLocalForMemory = false;

	switch (ModelChoice)
	{
	case ESummaryModelChoice::LocalFirst:
		// Use local for both if available; otherwise fall back to main for both.
		bUseLocalForTools = bLocalReady;
		bUseLocalForMemory = bLocalReady;
		break;
	case ESummaryModelChoice::MainModel:
		bUseLocalForTools = false;
		bUseLocalForMemory = false;
		break;
	case ESummaryModelChoice::Hybrid:
		// Tools: local (large transcript, no quality requirement).
		// Memory: main (requires nuanced extraction).
		bUseLocalForTools = bLocalReady;
		bUseLocalForMemory = false;
		break;
	}

	if (bUseLocalForTools && !bLocalReady)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Local aux model not ready, falling back to main model for tools"));
		bUseLocalForTools = false;
	}
	if (bUseLocalForMemory && !bLocalReady)
	{
		bUseLocalForMemory = false;
	}

	// Shared state across concurrent requests — collected when both complete.
	struct FSummaryState
	{
		FThreadSafeCounter PendingCount;
		FThreadSafeCounter SuccessCount;
		FString ToolsSummary;
		FString MemorySummary;
		FCriticalSection Lock;
		TWeakPtr<SMCPToolboxChatWidget> WeakThis;
	};
	TSharedPtr<FSummaryState> State = MakeShareable(new FSummaryState);
	State->PendingCount.Set((bArchiveTools ? 1 : 0) + (bArchiveMemory ? 1 : 0));
	State->WeakThis = SharedThis(this);

	// Notify the user that summarization is in progress
	FMCPToolboxChatMessage StatusMsg;
	StatusMsg.Role = EMCPToolboxMessageRole::System;
	StatusMsg.Content = TEXT("**归档总结进行中...**\n\n");
	StatusMsg.Content += FString::Printf(TEXT("- 工具使用归档: %s\n"),
		bArchiveTools ? (bUseLocalForTools ? TEXT("本地辅助模型生成中") : TEXT("主模型生成中")) : TEXT("跳过"));
	StatusMsg.Content += FString::Printf(TEXT("- 记忆归档: %s"),
		bArchiveMemory ? (bUseLocalForMemory ? TEXT("本地辅助模型生成中") : TEXT("主模型生成中")) : TEXT("跳过"));
	AddMessage(StatusMsg);

	// Helper lambda: dispatch one summary request to either local or main model
	auto DispatchOne = [this, State](bool bForTools, bool bUseLocal)
	{
		FString Prompt = BuildSummaryPrompt(bForTools, !bForTools);

		if (bUseLocal)
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Summary via local aux model (%s)"),
				bForTools ? TEXT("tools") : TEXT("memory"));

			FMCPToolboxAuxModelManager::Get().InferAsync(Prompt, 2048,
				[State, bForTools](bool bSuccess, const FString& Output)
			{
				// AuxModelManager::InferAsync runs its callback on the game thread already,
				// but be defensive and hop back to game thread explicitly.
				AsyncTask(ENamedThreads::GameThread, [State, bForTools, bSuccess, Output]()
				{
					TSharedPtr<SMCPToolboxChatWidget> This = State->WeakThis.Pin();
					if (!This.IsValid()) return;

					if (bSuccess && !Output.IsEmpty())
					{
						FScopeLock ScopeLock(&State->Lock);
						if (bForTools) State->ToolsSummary = Output;
						else          State->MemorySummary = Output;
						State->SuccessCount.Increment();
					}
					else
					{
						UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Local summary (%s) failed"),
							bForTools ? TEXT("tools") : TEXT("memory"));
					}

					State->PendingCount.Decrement();
					if (State->PendingCount.GetValue() == 0)
					{
						This->OnSummaryGenerated(
							State->SuccessCount.GetValue() > 0,
							State->ToolsSummary,
							State->MemorySummary);
					}
				});
			});
		}
		else
		{
			// Main model: independent HTTP request (does not pollute the chat stream).
			const FMCPToolboxAPIKeyEntry* Entry = FMCPToolboxAPIManager::Get().GetActiveEntry();
			if (!Entry)
			{
				UE_LOG(LogMCPToolbox, Error, TEXT("[Chat] No active API entry for summary (%s)"),
					bForTools ? TEXT("tools") : TEXT("memory"));
				AsyncTask(ENamedThreads::GameThread, [State]()
				{
					State->PendingCount.Decrement();
					if (State->PendingCount.GetValue() == 0)
					{
						TSharedPtr<SMCPToolboxChatWidget> This = State->WeakThis.Pin();
						if (This.IsValid())
						{
							This->OnSummaryGenerated(State->SuccessCount.GetValue() > 0,
								State->ToolsSummary, State->MemorySummary);
						}
					}
				});
				return;
			}

			FString ApiKey;
			FBase64::Decode(Entry->EncryptedKey, ApiKey);
			FString ApiUrl = Entry->BaseURL;
			if (!ApiUrl.EndsWith(TEXT("/chat/completions")))
			{
				if (ApiUrl.EndsWith(TEXT("/"))) ApiUrl = ApiUrl.LeftChop(1);
				ApiUrl += TEXT("/chat/completions");
			}

			TSharedPtr<FJsonObject> ReqJson = MakeShareable(new FJsonObject);
			ReqJson->SetStringField(TEXT("model"), Entry->ModelId);
			ReqJson->SetNumberField(TEXT("max_tokens"), 2048);
			ReqJson->SetNumberField(TEXT("temperature"), 0.3);

			TArray<TSharedPtr<FJsonValue>> MsgsArr;
			TSharedPtr<FJsonObject> UserMsg = MakeShareable(new FJsonObject);
			UserMsg->SetStringField(TEXT("role"), TEXT("user"));
			UserMsg->SetStringField(TEXT("content"), Prompt);
			MsgsArr.Add(MakeShareable(new FJsonValueObject(UserMsg)));
			ReqJson->SetArrayField(TEXT("messages"), MsgsArr);

			FString ReqBody;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReqBody);
			FJsonSerializer::Serialize(ReqJson.ToSharedRef(), Writer);

			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
			Http->SetURL(ApiUrl);
			Http->SetVerb(TEXT("POST"));
			Http->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Http->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
			Http->SetContentAsString(ReqBody);

			Http->OnProcessRequestComplete().BindLambda(
				[State, bForTools](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOK)
			{
				FString Output;
				bool bSuccess = false;

				if (bOK && Resp.IsValid())
				{
					FString Body = Resp->GetContentAsString();
					TSharedPtr<FJsonObject> Json;
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
					if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* ChoicesPtr = nullptr;
						if (Json->TryGetArrayField(TEXT("choices"), ChoicesPtr) && ChoicesPtr && ChoicesPtr->Num() > 0)
						{
							TSharedPtr<FJsonObject> FirstChoice = (*ChoicesPtr)[0]->AsObject();
							if (FirstChoice.IsValid())
							{
								TSharedPtr<FJsonObject> Message = FirstChoice->GetObjectField(TEXT("message"));
								if (Message.IsValid())
								{
									Output = Message->GetStringField(TEXT("content"));
									bSuccess = !Output.IsEmpty();
								}
							}
						}
					}
				}

				if (!bSuccess)
				{
					UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Main model summary (%s) failed (ok=%d)"),
						bForTools ? TEXT("tools") : TEXT("memory"), (int32)bOK);
				}

				AsyncTask(ENamedThreads::GameThread, [State, bForTools, bSuccess, Output]()
				{
					TSharedPtr<SMCPToolboxChatWidget> This = State->WeakThis.Pin();
					if (!This.IsValid()) return;

					if (bSuccess)
					{
						FScopeLock ScopeLock(&State->Lock);
						if (bForTools) State->ToolsSummary = Output;
						else          State->MemorySummary = Output;
						State->SuccessCount.Increment();
					}

					State->PendingCount.Decrement();
					if (State->PendingCount.GetValue() == 0)
					{
						This->OnSummaryGenerated(
							State->SuccessCount.GetValue() > 0,
							State->ToolsSummary,
							State->MemorySummary);
					}
				});
			});

			Http->ProcessRequest();
		}
	};

	if (bArchiveTools)
	{
		DispatchOne(true, bUseLocalForTools);
	}
	if (bArchiveMemory)
	{
		DispatchOne(false, bUseLocalForMemory);
	}
}

// ============================================================================
// Auto Archive — fires when the aux model is idle and the chat stream just ended
// ============================================================================
void SMCPToolboxChatWidget::TryAutoArchiveWhenIdle()
{
	// User must have explicitly opted in via the summary dialog checkbox
	if (!bAutoArchiveEnabled) return;

	// Re-entrancy guard: don't trigger while a previous auto-archive is still pending
	if (bIsAutoArchiving) return;

	// Don't fire while the main chat is still busy (streaming/waiting)
	if (bIsStreaming || bIsWaiting) return;

	// Don't fire while the user is interacting with the summary dialog
	if (SummaryDialogWindow.IsValid()) return;

	// Aux model must be ready (the whole point is to use idle aux capacity)
	if (!FMCPToolboxAuxModelManager::Get().IsReady()) return;

	// Cooldown: 30 minutes between auto-archives to prevent loop idle-spin
	constexpr double CooldownSeconds = 1800.0;
	const double Now = FPlatformTime::Seconds();
	if (LastAutoArchiveTime > 0.0 && (Now - LastAutoArchiveTime) < CooldownSeconds)
	{
		return;
	}

	// Must have at least one user message worth summarizing
	bool bHasUserMsg = false;
	for (const FMCPToolboxChatMessage& M : Messages)
	{
		if (M.Role == EMCPToolboxMessageRole::User) { bHasUserMsg = true; break; }
	}
	if (!bHasUserMsg) return;

	// All conditions met — fire
	bIsAutoArchiving = true;
	LastAutoArchiveTime = Now;

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Auto-archive triggered (aux idle, cooldown elapsed)"));

	FMCPToolboxChatMessage Notice;
	Notice.Role = EMCPToolboxMessageRole::System;
	Notice.Content = TEXT("**辅助模型空闲，自动归档当前会话**\n\n");
	Notice.Content += TEXT("- 使用本地辅助模型生成（LocalFirst 策略）\n");
	Notice.Content += TEXT("- 工具使用归档 + 记忆归档双归档\n");
	Notice.Content += TEXT("- 旧索引文件已自动备份到 `summaries/backup_*.md`\n");
	Notice.Content += TEXT("- 冷却时间 30 分钟，防止循环空转");
	AddMessage(Notice);

	// LocalFirst: prefer local aux model for both (free, fast, no API quota)
	GenerateSummary(ESummaryModelChoice::LocalFirst, true, true);
}

void SMCPToolboxChatWidget::ShowSummaryChoiceDialog()
{
	bool bIsFirstTime = !bSummaryDeclined;

	// Shared mutable state captured by lambda — survives until window closes.
	TSharedPtr<ESummaryModelChoice> SelectedChoice = MakeShareable(new ESummaryModelChoice(ESummaryModelChoice::LocalFirst));
	TSharedPtr<bool> bArchiveTools = MakeShareable(new bool(true));
	TSharedPtr<bool> bArchiveMemory = MakeShareable(new bool(true));

	bool bLocalReady = FMCPToolboxAuxModelManager::Get().IsReady();

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("SummaryDialogTitle", "归档总结"))
		.ClientSize(FVector2D(560, 560))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.SizingRule(ESizingRule::UserSized)
		.AutoCenter(EAutoCenter::PreferredWorkArea);

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(16))
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SummaryTitle", "归档当前会话"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]

			// Advantages description (first-time shows more detail)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.Text(FText::FromString(bIsFirstTime
					? TEXT("归档总结功能：将当前对话的工具使用经验和核心要点提炼成 Markdown，存到 ~/.mcptoolbox/conversation_summary.md。\n\n"
						  "优势：\n"
						  "- 下次启动编辑器自动加载到系统提示词，无需重复说明上下文\n"
						  "- 工具归档让 AI 复用之前的调用经验，减少试错\n"
						  "- 记忆归档保留项目状态和决策，重启不丢\n"
						  "- 单文件索引（覆盖式更新）+ 多文件归档（可追溯历史）")
					: TEXT("继续生成会话归档。会覆盖上次的索引文件，旧归档保留在 summaries/ 目录。")))
				.AutoWrapText(true)
				.WrappingPolicy(ETextWrappingPolicy::DefaultWrapping)
			]

			// Separator
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SSeparator)
			]

			// Archive type section
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ArchiveTypeLabel", "归档类型："))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(20, 0, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([bArchiveTools]() -> ECheckBoxState { return *bArchiveTools ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([bArchiveTools](ECheckBoxState State) { *bArchiveTools = (State == ECheckBoxState::Checked); })
					.Content()
					[
						SNew(STextBlock).Text(LOCTEXT("ArchiveTools", " 工具使用归档（call_tool 调用经验）"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(20, 0, 0, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([bArchiveMemory]() -> ECheckBoxState { return *bArchiveMemory ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([bArchiveMemory](ECheckBoxState State) { *bArchiveMemory = (State == ECheckBoxState::Checked); })
					.Content()
					[
						SNew(STextBlock).Text(LOCTEXT("ArchiveMemory", " 记忆归档（项目状态/决策/偏好）"))
					]
				]
			]

			// Model selection section
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelLabel", "总结模型："))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(20, 0, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.ButtonColorAndOpacity_Lambda([SelectedChoice]() -> FLinearColor
					{
						return *SelectedChoice == ESummaryModelChoice::LocalFirst
							? FLinearColor(0.20f, 0.40f, 0.20f, 1.0f)
							: FLinearColor(0.15f, 0.15f, 0.18f, 1.0f);
					})
					.OnClicked_Lambda([SelectedChoice]() -> FReply
					{
						*SelectedChoice = ESummaryModelChoice::LocalFirst;
						return FReply::Handled();
					})
					.Content()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("LocalFirst", "本地优先（推荐）"))
						.ColorAndOpacity(FLinearColor(0.9f, 0.95f, 0.9f))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.ButtonColorAndOpacity_Lambda([SelectedChoice]() -> FLinearColor
					{
						return *SelectedChoice == ESummaryModelChoice::MainModel
							? FLinearColor(0.20f, 0.40f, 0.20f, 1.0f)
							: FLinearColor(0.15f, 0.15f, 0.18f, 1.0f);
					})
					.OnClicked_Lambda([SelectedChoice]() -> FReply
					{
						*SelectedChoice = ESummaryModelChoice::MainModel;
						return FReply::Handled();
					})
					.Content()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("MainModel", "主模型"))
						.ColorAndOpacity(FLinearColor(0.9f, 0.95f, 0.9f))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonColorAndOpacity_Lambda([SelectedChoice]() -> FLinearColor
					{
						return *SelectedChoice == ESummaryModelChoice::Hybrid
							? FLinearColor(0.20f, 0.40f, 0.20f, 1.0f)
							: FLinearColor(0.15f, 0.15f, 0.18f, 1.0f);
					})
					.OnClicked_Lambda([SelectedChoice]() -> FReply
					{
						*SelectedChoice = ESummaryModelChoice::Hybrid;
						return FReply::Handled();
					})
					.Content()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Hybrid", "混合"))
						.ColorAndOpacity(FLinearColor(0.9f, 0.95f, 0.9f))
					]
				]
			]

			// Local model status
			+ SVerticalBox::Slot().AutoHeight().Padding(20, 6, 0, 4)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("本地辅助模型状态: %s"),
					bLocalReady ? TEXT("已就绪") : TEXT("未就绪（自动回退主模型）"))))
				.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			]
			// Model trade-offs explanation
			+ SVerticalBox::Slot().AutoHeight().Padding(20, 0, 0, 12)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelTradeOffs",
					"本地优先: 免费、快、不消耗 API 额度；质量取决于本地模型\n"
					"主模型: 质量好（GPT-4/Claude 等），但消耗 API 额度\n"
					"混合: 工具归档用本地（量大），记忆归档用主模型（要求质量）"))
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
				.WrappingPolicy(ETextWrappingPolicy::DefaultWrapping)
			]

			// Separator
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SSeparator)
			]

			// Auto-archive toggle
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() -> ECheckBoxState
				{
					return bAutoArchiveEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
				{
					bAutoArchiveEnabled = (State == ECheckBoxState::Checked);
					SaveWidgetState();
					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Auto-archive %s"),
						bAutoArchiveEnabled ? TEXT("enabled") : TEXT("disabled"));
				})
				.Content()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AutoArchiveToggle",
							"启用辅助模型空闲时自动归档"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AutoArchiveHint",
							"启用后：主聊天流结束时，若辅助模型就绪且距上次自动归档超过 30 分钟，\n"
							"将自动用本地模型生成归档（覆盖索引文件，旧版本自动备份到 summaries/backup_*.md）。\n"
							"防止循环空转：30 分钟冷却 + bIsAutoArchiving 状态保护。"))
						.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.AutoWrapText(true)
						.WrappingPolicy(ETextWrappingPolicy::DefaultWrapping)
					]
				]
			]

			// Spacer
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SBox)
			]

			// Bottom buttons
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SBox)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelSummary", "取消（可从工具栏'更多 → 归档总结'再次触发）"))
					.OnClicked_Lambda([this, Window]() -> FReply
					{
						// First-time decline so subsequent opens skip the long advantages block
						if (!bSummaryDeclined)
						{
							bSummaryDeclined = true;
							SaveWidgetState();
						}
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("StartSummary", "开始总结"))
					.ButtonColorAndOpacity(FLinearColor(0.20f, 0.40f, 0.20f, 1.0f))
					.OnClicked_Lambda([this, Window, SelectedChoice, bArchiveTools, bArchiveMemory]() -> FReply
					{
						// First-time dialog counts as "seen" now — subsequent opens are concise
						if (!bSummaryDeclined)
						{
							bSummaryDeclined = true;
							SaveWidgetState();
						}

						ESummaryModelChoice Choice = *SelectedChoice;
						bool bTools = *bArchiveTools;
						bool bMemory = *bArchiveMemory;

						if (!bTools && !bMemory)
						{
							UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] No archive type selected"));
							Window->RequestDestroyWindow();
							return FReply::Handled();
						}

						Window->RequestDestroyWindow();

						// Run on the game thread (we're already on it, but be explicit)
						AsyncTask(ENamedThreads::GameThread, [this, Choice, bTools, bMemory]()
						{
							GenerateSummary(Choice, bTools, bMemory);
						});

						return FReply::Handled();
					})
				]
			]
		]
	);

	// Track window for the "don't open twice" check
	SummaryDialogWindow = Window;

	FSlateApplication::Get().AddWindow(Window);
}

// ============================================================================
// Auxiliary Model Integration — IdleSpec + SWE-Pruner
// ============================================================================

void SMCPToolboxChatWidget::LaunchIdleSpec(const FString& CurrentToolName)
{
	// ── Speculative execution DISABLED ──
	// The speculative prediction mechanism caused measurable slowdown and
	// correctness issues:
	//   1. Every MCP tool call triggered an extra local-model inference HTTP
	//      round-trip (LaunchSpeculation → InferAsync), adding latency.
	//   2. When the prediction hadn't returned by the time the tool finished,
	//      the conversation BLOCKED waiting for it (bPendingToolCompletion).
	//   3. TrySpeculativeExecution ran the predicted tool with empty arguments
	//      "{}", producing error results that polluted the LLM context with
	//      no rollback (PreSpeculationMessages was never wired up).
	// To re-enable, this early return must be removed and the issues above
	// addressed (parameter prediction, non-blocking semantics, rollback).
	return;

	FMCPToolboxAuxModelManager& AuxMgr = FMCPToolboxAuxModelManager::Get();
	if (!AuxMgr.IsReady()) return;

	// Build conversation context from Messages array
	FString ContextStr;
	for (const FMCPToolboxChatMessage& Msg : Messages)
	{
		FString RoleName;
		switch (Msg.Role)
		{
		case EMCPToolboxMessageRole::User:      RoleName = TEXT("user"); break;
		case EMCPToolboxMessageRole::Assistant:  RoleName = TEXT("assistant"); break;
		case EMCPToolboxMessageRole::System:     RoleName = TEXT("system"); break;
		case EMCPToolboxMessageRole::Thinking:   RoleName = TEXT("thinking"); break;
		}
		FString Truncated = Msg.Content.Len() > 300 ? Msg.Content.Left(300) + TEXT("...") : Msg.Content;
		ContextStr += FString::Printf(TEXT("<%s>: %s\n"), *RoleName, *Truncated);
	}

	bSpeculationPending = true;

	// Store the tool name so the deferred continuation can use it when speculation completes
	PendingSpecToolName = CurrentToolName;

	// Collect available tool names for better prediction
	TArray<FString> AvailableTools;
	if (ToolFunctionTable)
	{
		for (const auto& Pair : ToolFunctionTable->GetAllFunctions())
			AvailableTools.Add(FString(Pair.first.c_str()));
	}

	AuxMgr.LaunchSpeculation(ContextStr, CurrentToolName, AvailableTools,
		[this](const FSpeculativeResult& Result)
	{
		bSpeculationPending = false;
		LastSpeculation = Result;

		if (Result.IsValid())
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[IdleSpec] Predicted: %s → %s (%.0fms)"),
				*Result.PredictedToolName, *Result.PredictedReasoning, Result.InferenceTimeMs);
		}

		// If tool completion is waiting for the speculation, continue now
		if (bPendingToolCompletion)
		{
			bPendingToolCompletion = false;
			TrySpeculativeOrContinue(DeferredPendingMsgs);
		}
	});
}

void SMCPToolboxChatWidget::TrySpeculativeOrContinue(TSharedPtr<TArray<TSharedPtr<FJsonValue>>> PendingMsgs)
{
	// Speculative execution is disabled (see LaunchIdleSpec). Since LaunchIdleSpec
	// is a no-op, bSpeculationPending is never set and LastSpeculation is always
	// invalid, so we always take the direct-continue path here. The branches below
	// are retained for when speculation is properly reimplemented.
	if (bSpeculationPending)
	{
		// Still waiting — store for later
		bPendingToolCompletion = true;
		DeferredPendingMsgs = PendingMsgs;
		return;
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 工具调用完成(MCP), 继续对话"));
	SendAIRequest(*PendingMsgs);
}

/** Preprocess images in messages through local VL model (when vision mode is OFF but aux VL available) */
void SMCPToolboxChatWidget::PreprocessImagesLocally(
	const TArray<TSharedPtr<FJsonValue>>& Msgs,
	TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnDone)
{
	FMCPToolboxAuxModelManager& AuxMgr = FMCPToolboxAuxModelManager::Get();

	// ── Step 1: Collect ALL messages with image_url (not just the last one) ──
	struct FImageTarget { int32 Index; FString Base64; FString UserText; };
	TArray<FImageTarget> Targets;

	for (int32 i = 0; i < Msgs.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj = Msgs[i]->AsObject();
		FString Role;
		Obj->TryGetStringField(TEXT("role"), Role);
		if (Role != TEXT("user")) continue;

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Obj->TryGetArrayField(TEXT("content"), Arr)) continue;

		for (const auto& Part : *Arr)
		{
			TSharedPtr<FJsonObject> PartObj = Part->AsObject();
			if (!PartObj.IsValid()) continue;
			FString Type;
			if (PartObj->TryGetStringField(TEXT("type"), Type) && Type == TEXT("image_url"))
			{
				FImageTarget Target;
				Target.Index = i;
				const TSharedPtr<FJsonObject>* ImgUrlObj = nullptr;
				if (PartObj->TryGetObjectField(TEXT("image_url"), ImgUrlObj) && ImgUrlObj)
				{
					FString URL;
					(*ImgUrlObj)->TryGetStringField(TEXT("url"), URL);
					int32 Comma = URL.Find(TEXT("base64,"));
					Target.Base64 = (Comma != INDEX_NONE) ? URL.Mid(Comma + 7) : URL;
				}
				// Also capture text part
				for (const auto& TP : *Arr)
				{
					TSharedPtr<FJsonObject> TPObj = TP->AsObject();
					if (!TPObj.IsValid()) continue;
					FString TType;
					if (TPObj->TryGetStringField(TEXT("type"), TType) && TType == TEXT("text"))
						TPObj->TryGetStringField(TEXT("text"), Target.UserText);
				}
				Targets.Add(Target);
				break;
			}
		}
	}

	if (Targets.Num() == 0)
	{
		if (OnDone) OnDone(Msgs);
		return;
	}

	// ── Step 2: Process each image target sequentially ──
	TSharedPtr<TArray<TSharedPtr<FJsonValue>>> Modified = MakeShared<TArray<TSharedPtr<FJsonValue>>>(Msgs);
	TSharedPtr<int32> Remaining = MakeShared<int32>(Targets.Num());

	UE_LOG(LogMCPToolbox, Log, TEXT("[VL] Preprocessing %d images locally (vision OFF)..."), Targets.Num());

	for (const auto& Target : Targets)
	{
		if (Target.Base64.IsEmpty())
		{
			// No image data — just strip image_url, keep text only
			TSharedPtr<FJsonObject> NewObj = MakeShareable(new FJsonObject(*(*Modified)[Target.Index]->AsObject()));
			TArray<TSharedPtr<FJsonValue>> TextOnly;
			TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
			TextPart->SetStringField(TEXT("type"), TEXT("text"));
			TextPart->SetStringField(TEXT("text"), Target.UserText.IsEmpty() ? TEXT("[Image - no data]") : Target.UserText);
			TextOnly.Add(MakeShareable(new FJsonValueObject(TextPart)));
			NewObj->SetArrayField(TEXT("content"), TextOnly);
			(*Modified)[Target.Index] = MakeShareable(new FJsonValueObject(NewObj));
			(*Remaining)--;
			// Note: synchronous path must also check completion — otherwise if all
			// targets had empty base64, OnDone would never fire and the request stalls.
			if (*Remaining <= 0 && OnDone)
			{
				OnDone(*Modified);
			}
			continue;
		}

		double StartTime = FPlatformTime::Seconds();
		// Completion flag shared between the AnalyzeImage callback and the timeout
		// ticker below — whichever fires first wins, the other becomes a no-op.
		TSharedPtr<bool> bDone = MakeShared<bool>(false);

		// Timeout guard: if the local VL server crashes or the HTTP callback never
		// fires, Remaining would never decrement and OnDone would never be called,
		// permanently stalling the conversation. After 30s we give up and degrade.
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([bDone, Modified, Target, Remaining, OnDone, StartTime](float) -> bool
		{
			if (*bDone) return false; // already completed by the real callback
			if (FPlatformTime::Seconds() - StartTime < 30.0) return true; // keep waiting

			// Timeout — degrade to placeholder text
			*bDone = true;
			UE_LOG(LogMCPToolbox, Warning, TEXT("[VL] Image analysis timed out (30s), degrading"));

			TSharedPtr<FJsonObject> NewObj = MakeShareable(new FJsonObject(*(*Modified)[Target.Index]->AsObject()));
			TArray<TSharedPtr<FJsonValue>> NewContent;
			TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
			TextPart->SetStringField(TEXT("type"), TEXT("text"));
			TextPart->SetStringField(TEXT("text"),
				Target.UserText.IsEmpty() ? TEXT("[图片分析超时]") : Target.UserText);
			NewContent.Add(MakeShareable(new FJsonValueObject(TextPart)));
			NewObj->SetArrayField(TEXT("content"), NewContent);
			(*Modified)[Target.Index] = MakeShareable(new FJsonValueObject(NewObj));

			(*Remaining)--;
			if (*Remaining <= 0 && OnDone) OnDone(*Modified);
			return false; // stop ticking
		}), 1.0f);

		AuxMgr.AnalyzeImage(Target.Base64,
			TEXT("Describe what you actually see in this image in detail. Focus on visible objects, people, text, colors, layout. Be specific and objective. Reply in Chinese."),
			[this, bDone, Modified, Target, Remaining, OnDone, StartTime](const FString& Description)
		{
			if (*bDone) return; // already handled by timeout
			*bDone = true;

			double ElapsedSec = FPlatformTime::Seconds() - StartTime;
			TSharedPtr<FJsonObject> NewObj = MakeShareable(new FJsonObject(*(*Modified)[Target.Index]->AsObject()));
			TArray<TSharedPtr<FJsonValue>> NewContent;
			TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
			TextPart->SetStringField(TEXT("type"), TEXT("text"));

			FString TextContent;
			if (!Description.IsEmpty())
			{
				TextContent = FString::Printf(TEXT("[本地VL图像分析 (%.1fs)]\n%s"), ElapsedSec, *Description);
				UE_LOG(LogMCPToolbox, Log, TEXT("[VL] Image analyzed (%.1fs): %s"), ElapsedSec, *Description.Left(100));
			}
			else
			{
				TextContent = Target.UserText.IsEmpty() ? TEXT("[用户上传了图片]") : Target.UserText;
				UE_LOG(LogMCPToolbox, Warning, TEXT("[VL] Image analysis returned empty"));
			}
			TextPart->SetStringField(TEXT("text"), TextContent);
			NewContent.Add(MakeShareable(new FJsonValueObject(TextPart)));
			NewObj->SetArrayField(TEXT("content"), NewContent);
			(*Modified)[Target.Index] = MakeShareable(new FJsonValueObject(NewObj));

			(*Remaining)--;
			if (*Remaining <= 0 && OnDone)
			{
				OnDone(*Modified);
			}
		});
	}
}

/** Try to auto-execute the speculated tool call — skip LLM round entirely.
 *  This is the CPU branch-prediction equivalent for LLM agents.
 *  If the speculation is wrong, the tool execution just returns an error, harmless. */
bool SMCPToolboxChatWidget::TrySpeculativeExecution(TArray<TSharedPtr<FJsonValue>>& PendingMsgs)
{
	if (!LastSpeculation.IsValid() || bSpeculationPending) return false;

	FString PredictedTool = LastSpeculation.PredictedToolName;
	bSpeculationPending = false; // consumed

	// Verify predicted tool exists in our function table
	assistant::FunctionTable& FT = GetFunctionTable();
	bool bToolExists = false;
	for (const auto& Pair : FT.GetAllFunctions())
	{
		if (FString(Pair.first.c_str()).Equals(PredictedTool, ESearchCase::IgnoreCase))
		{
			bToolExists = true;
			break;
		}
	}

	if (!bToolExists)
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("[IdleSpec] Predicted '%s' not in function table, fallback to LLM"), *PredictedTool);
		return false;
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[IdleSpec] Speculative execute: %s (skip LLM round)"), *PredictedTool);

	// Build tool call object
	TSharedPtr<FJsonObject> TCObj = MakeShareable(new FJsonObject());
	FString SpecId = FString::Printf(TEXT("spec_%s_%d"), *PredictedTool, FMath::Rand());
	TCObj->SetStringField(TEXT("id"), SpecId);
	TCObj->SetStringField(TEXT("type"), TEXT("function"));

	TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject());
	FuncObj->SetStringField(TEXT("name"), PredictedTool);
	FuncObj->SetStringField(TEXT("arguments"), TEXT("{}"));
	TCObj->SetObjectField(TEXT("function"), FuncObj);

	// Inject fake assistant message with tool_calls
	TArray<TSharedPtr<FJsonValue>> SpecToolCallsArr;
	SpecToolCallsArr.Add(MakeShareable(new FJsonValueObject(TCObj)));

	TSharedPtr<FJsonObject> SpecMsg = MakeShareable(new FJsonObject());
	SpecMsg->SetStringField(TEXT("role"), TEXT("assistant"));
	SpecMsg->SetStringField(TEXT("content"), TEXT(""));
	SpecMsg->SetStringField(TEXT("reasoning_content"), TEXT("")); // Required by DeepSeek thinking mode
	SpecMsg->SetArrayField(TEXT("tool_calls"), SpecToolCallsArr);
	PendingMsgs.Add(MakeShareable(new FJsonValueObject(SpecMsg)));

	// Execute the tool: MCP or local
	bool bIsMCP = MCPServerClient.IsConnected() && MCPServerClient.IsMCPTool(PredictedTool);

	TSharedPtr<TArray<TSharedPtr<FJsonValue>>> SharedMsgs = MakeShared<TArray<TSharedPtr<FJsonValue>>>(MoveTemp(PendingMsgs));
	TSharedPtr<int32> Pending = MakeShared<int32>(0);

	if (bIsMCP)
	{
		(*Pending)++;
		MCPServerClient.ExecuteTool(PredictedTool, TEXT("{}"),
			[this, SharedMsgs, Pending, SpecId, PredictedTool](bool bOk, const FString& R)
		{
			TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
			ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
			ToolResultMsg->SetStringField(TEXT("tool_call_id"), SpecId);
			ToolResultMsg->SetStringField(TEXT("name"), PredictedTool);
			ToolResultMsg->SetStringField(TEXT("content"), R);
			SharedMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

			FMCPToolboxChatMessage ResultMsg;
			ResultMsg.Role = EMCPToolboxMessageRole::System;
			ResultMsg.Content = FString::Printf(TEXT("[IdleSpec] **%s** 结果:\n```\n%s\n```"), *PredictedTool, *R.Left(500));
			AddMessage(ResultMsg);

			(*Pending)--;
			if (*Pending <= 0)
			{
				UE_LOG(LogMCPToolbox, Log, TEXT("[IdleSpec] Speculative execution complete, continuing"));
				SendAIRequest(*SharedMsgs);
			}
		});
	}
	else
	{
		FString R = ExecuteToolCall(PredictedTool, TEXT("{}"));

		TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
		ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
		ToolResultMsg->SetStringField(TEXT("tool_call_id"), SpecId);
		ToolResultMsg->SetStringField(TEXT("name"), PredictedTool);
		ToolResultMsg->SetStringField(TEXT("content"), R);
		SharedMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

		FMCPToolboxChatMessage ResultMsg;
		ResultMsg.Role = EMCPToolboxMessageRole::System;
		ResultMsg.Content = FString::Printf(TEXT("[IdleSpec] **%s** 结果:\n```\n%s\n```"), *PredictedTool, *R.Left(500));
		AddMessage(ResultMsg);

		UE_LOG(LogMCPToolbox, Log, TEXT("[IdleSpec] Speculative execution complete, continuing"));
		SendAIRequest(*SharedMsgs);
	}

	return true;
}

/** Inject a speculation hint into PendingMsgs if IdleSpec made a valid prediction */
static void InjectSpeculationHint(
	TArray<TSharedPtr<FJsonValue>>& PendingMsgs,
	const FSpeculativeResult& Spec,
	bool& bSpeculationPending)
{
	if (!Spec.IsValid() || bSpeculationPending) return;

	// Add a system message with the speculation hint to guide the main model
	TSharedPtr<FJsonObject> HintMsg = MakeShareable(new FJsonObject());
	HintMsg->SetStringField(TEXT("role"), TEXT("system"));
	HintMsg->SetStringField(TEXT("content"),
		FString::Printf(TEXT("[Hint] The next step likely needs tool: %s. Reason: %s"),
			*Spec.PredictedToolName, *Spec.PredictedReasoning));
	PendingMsgs.Add(MakeShareable(new FJsonValueObject(HintMsg)));
}

void SMCPToolboxChatWidget::ApplyPruningBeforeSend(
	const TArray<TSharedPtr<FJsonValue>>& ApiMessages,
	TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnPruned)
{
	FMCPToolboxAuxModelManager& AuxMgr = FMCPToolboxAuxModelManager::Get();
	if (!AuxMgr.IsReady() || ApiMessages.Num() <= 15)
	{
		if (OnPruned) OnPruned(ApiMessages);
		return;
	}

	// Build numbered message list for the pruner (role + first 200 chars)
	FString NumberedMsgs;
	TArray<int32> MsgIndexToOriginal; // maps numbered index → ApiMessages index
	for (int32 i = 0; i < ApiMessages.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj = ApiMessages[i]->AsObject();
		if (!Obj.IsValid()) continue;

		FString Role;
		Obj->TryGetStringField(TEXT("role"), Role);
		FString Content;
		Obj->TryGetStringField(TEXT("content"), Content);

		// Handle array content (multimodal messages)
		if (Content.IsEmpty())
		{
			const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
			if (Obj->TryGetArrayField(TEXT("content"), ContentArr))
			{
				for (const auto& Part : *ContentArr)
				{
					TSharedPtr<FJsonObject> PartObj = Part->AsObject();
					if (PartObj.IsValid())
					{
						FString PartText;
						if (PartObj->TryGetStringField(TEXT("text"), PartText))
							Content += PartText + TEXT(" ");
					}
				}
			}
		}

		FString Truncated = Content.Len() > 200 ? Content.Left(200) + TEXT("...") : Content;
		Truncated.ReplaceInline(TEXT("\n"), TEXT(" "));
		Truncated.ReplaceInline(TEXT("\r"), TEXT(""));

		NumberedMsgs += FString::Printf(TEXT("[%d] %s: %s\n"),
			MsgIndexToOriginal.Num(), *Role, *Truncated);
		MsgIndexToOriginal.Add(i);
	}

	// Split into TArray<FString> for structured PruneContext
	TArray<FString> NumberedBlocks;
	NumberedMsgs.ParseIntoArray(NumberedBlocks, TEXT("\n"), false);
	NumberedBlocks.RemoveAll([](const FString& S) { return S.IsEmpty(); });

	// Extract task goal from last user message
	FString TaskGoal = TEXT("Continue the conversation and assist the user");
	for (int32 i = ApiMessages.Num() - 1; i >= 0; --i)
	{
		TSharedPtr<FJsonObject> Obj = ApiMessages[i]->AsObject();
		if (Obj.IsValid())
		{
			FString Role;
			if (Obj->TryGetStringField(TEXT("role"), Role) && Role == TEXT("user"))
			{
				Obj->TryGetStringField(TEXT("content"), TaskGoal);
				TaskGoal = TaskGoal.Left(200);
				break;
			}
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[SWE-Pruner] Pruning %d messages, goal=%s"), ApiMessages.Num(), *TaskGoal.Left(60));

	AuxMgr.PruneContext(NumberedBlocks, TaskGoal,
		[ApiMessages, MsgIndexToOriginal, OnPruned](const FPruningResult& Result)
	{
		// Use RemoveIndices directly from pruner (no fragile re-parsing needed)
		TSet<int32> KeptNumberedIndices;
		for (int32 i = 0; i < MsgIndexToOriginal.Num(); ++i)
			KeptNumberedIndices.Add(i);

		for (int32 RemoveIdx : Result.RemoveIndices)
			KeptNumberedIndices.Remove(RemoveIdx);

		if (Result.ReductionPercent() < 5.0 || KeptNumberedIndices.Num() == 0)
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("[SWE-Pruner] Skipped (%.1f%% reduction)"), Result.ReductionPercent());
			if (OnPruned) OnPruned(ApiMessages);
			return;
		}

		// Always keep system message and last 2 messages
		for (int32 i = 0; i < MsgIndexToOriginal.Num(); ++i)
		{
			int32 OrigIdx = MsgIndexToOriginal[i];
			TSharedPtr<FJsonObject> Obj = ApiMessages[OrigIdx]->AsObject();
			if (!Obj.IsValid()) continue;
			FString Role;
			Obj->TryGetStringField(TEXT("role"), Role);
			if (Role == TEXT("system"))
				KeptNumberedIndices.Add(i);
		}
		int32 LastIdx = MsgIndexToOriginal.Num() - 1;
		KeptNumberedIndices.Add(LastIdx);
		if (LastIdx > 0) KeptNumberedIndices.Add(LastIdx - 1);

		TArray<TSharedPtr<FJsonValue>> PrunedMessages;
		for (int32 i = 0; i < MsgIndexToOriginal.Num(); ++i)
		{
			if (KeptNumberedIndices.Contains(i))
			{
				int32 OrigIdx = MsgIndexToOriginal[i];
				if (OrigIdx >= 0 && OrigIdx < ApiMessages.Num())
					PrunedMessages.Add(ApiMessages[OrigIdx]);
			}
		}

		int32 OldCount = ApiMessages.Num();
		int32 NewCount = PrunedMessages.Num();
		UE_LOG(LogMCPToolbox, Log, TEXT("[SWE-Pruner] %d→%d msgs (%.1f%%)"),
			OldCount, NewCount, 100.0 * (1.0 - static_cast<double>(NewCount) / FMath::Max(1, OldCount)));

		if (PrunedMessages.Num() < 2)
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("[SWE-Pruner] Too aggressive, passing through"));
			if (OnPruned) OnPruned(ApiMessages);
			return;
		}

		if (OnPruned) OnPruned(PrunedMessages);
	});
}

void SMCPToolboxChatWidget::HandleStreamingTextCompletion(const FString& Content)
{
	if (bInterrupted) return;

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] HandleStreamingTextCompletion: %d chars"), Content.Len());

	FString DetectedToolName;

	bool bHasIntent = Content.Contains(TEXT("调用工具")) ||
	                  Content.Contains(TEXT("call_tool")) ||
	                  Content.Contains(TEXT("先查看")) ||
	                  Content.Contains(TEXT("先读取")) ||
	                  Content.Contains(TEXT("先调用")) ||
	                  Content.Contains(TEXT("接下来调用")) ||
	                  Content.Contains(TEXT("接下来查看")) ||
	                  Content.Contains(TEXT("生图工具")) ||
	                  Content.Contains(TEXT("generate_image")) ||
	                  Content.Contains(TEXT("生成图片")) ||
	                  Content.Contains(TEXT("画一张")) ||
	                  Content.Contains(TEXT("画个")) ||
	                  Content.Contains(TEXT("正在调用生图")) ||
	                  Content.Contains(TEXT("正在调用绘图")) ||
	                  Content.Contains(TEXT("让我为你生成")) ||
	                  Content.Contains(TEXT("让我来生成")) ||
	                  Content.Contains(TEXT("正在生成图片")) ||
	                  Content.Contains(TEXT("即将调用")) ||
	                  Content.Contains(TEXT("准备调用"));
	bool bHasPast = Content.Contains(TEXT("已调用")) ||
	                Content.Contains(TEXT("调用了")) ||
	                Content.Contains(TEXT("已完成")) ||
	                Content.Contains(TEXT("已生成")) ||
	                Content.Contains(TEXT("已画"));

	if (bHasIntent && !bHasPast)
	{
		if (Content.Contains(TEXT("generate_image")))
			DetectedToolName = TEXT("generate_image");
		else if (Content.Contains(TEXT("screenshot")))
			DetectedToolName = TEXT("screenshot");
		else if (Content.Contains(TEXT("call_tool")))
			DetectedToolName = TEXT("call_tool");
		else if (Content.Contains(TEXT("list_directory")))
			DetectedToolName = TEXT("list_directory");
		else if (Content.Contains(TEXT("search_codebase")))
			DetectedToolName = TEXT("search_codebase");
		else if (Content.Contains(TEXT("batch_read_files")))
			DetectedToolName = TEXT("batch_read_files");
		else if (Content.Contains(TEXT("glob_search")))
			DetectedToolName = TEXT("glob_search");
		else if (Content.Contains(TEXT("command")))
			DetectedToolName = TEXT("command");
	}

	if (DetectedToolName.IsEmpty())
	{
		FString UserMessage;
		for (int32 i = Messages.Num() - 1; i >= 0; i--)
		{
			if (Messages[i].Role == EMCPToolboxMessageRole::User)
			{
				UserMessage = Messages[i].Content;
				break;
			}
		}

		if (!UserMessage.IsEmpty())
		{
			bool bUserWantsImage = UserMessage.Contains(TEXT("画")) ||
			                       UserMessage.Contains(TEXT("生成图片")) ||
			                       UserMessage.Contains(TEXT("生图")) ||
			                       UserMessage.Contains(TEXT("绘图")) ||
			                       UserMessage.Contains(TEXT("image")) ||
			                       UserMessage.Contains(TEXT("picture")) ||
			                       UserMessage.Contains(TEXT("photo")) ||
			                       UserMessage.Contains(TEXT("帮我画")) ||
			                       UserMessage.Contains(TEXT("画一个")) ||
			                       UserMessage.Contains(TEXT("画一张")) ||
			                       UserMessage.Contains(TEXT("给我画"));

			if (bUserWantsImage)
			{
				DetectedToolName = TEXT("generate_image");
				UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Streaming text completion: detected user image intent, auto-triggering generate_image"));
			}
		}
	}

	if (!DetectedToolName.IsEmpty())
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Streaming text completion: extracted tool from content: %s"), *DetectedToolName);

		FString UserPrompt;
		for (int32 i = Messages.Num() - 1; i >= 0; i--)
		{
			if (Messages[i].Role == EMCPToolboxMessageRole::User)
			{
				UserPrompt = Messages[i].Content;
				break;
			}
		}

		FString FuncArgs;
		if (DetectedToolName == TEXT("generate_image"))
		{
			TSharedPtr<FJsonObject> ArgsObj = MakeShareable(new FJsonObject());
			ArgsObj->SetStringField(TEXT("prompt"), UserPrompt);
			ArgsObj->SetStringField(TEXT("save_path"), TEXT("saved:/GeneratedImages/"));
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&FuncArgs);
			FJsonSerializer::Serialize(ArgsObj.ToSharedRef(), Writer);
		}
		else
		{
			FuncArgs = TEXT("{}");
		}

		FMCPToolboxChatMessage ToolCallMsg;
		ToolCallMsg.Role = EMCPToolboxMessageRole::System;
		ToolCallMsg.Content = FString::Printf(TEXT("**调用工具: %s**"), *DetectedToolName);
		AddMessage(ToolCallMsg);

		if (DetectedToolName == TEXT("generate_image"))
		{
			FString NameCap = DetectedToolName;
			FString ContentCap = Content;
			FString FuncArgsCap = FuncArgs;

			TArray<TSharedPtr<FJsonValue>> HistoryMsgs;
			for (const auto& Msg : Messages)
			{
				TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject());
				FString RoleStr;
				switch (Msg.Role)
				{
				case EMCPToolboxMessageRole::User: RoleStr = TEXT("user"); break;
				case EMCPToolboxMessageRole::Assistant: RoleStr = TEXT("assistant"); break;
				case EMCPToolboxMessageRole::System: RoleStr = TEXT("system"); break;
				case EMCPToolboxMessageRole::Thinking: RoleStr = TEXT("assistant"); break;
				default: continue;
				}
				MsgObj->SetStringField(TEXT("role"), RoleStr);

				if (Msg.bHasImageAttachment && !Msg.ImageDataURIs.IsEmpty())
				{
					const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
					bool bVisionOK = ActiveEntry && (
						ActiveEntry->ModelId.Contains(TEXT("gpt-4o")) ||
						ActiveEntry->ModelId.Contains(TEXT("claude-3")) || ActiveEntry->ModelId.Contains(TEXT("claude-4")) ||
						ActiveEntry->ModelId.Contains(TEXT("gemini")) ||
						ActiveEntry->ModelId.Contains(TEXT("vl")) || ActiveEntry->ModelId.Contains(TEXT("vision")));

					if (bVisionOK)
					{
						TArray<TSharedPtr<FJsonValue>> ContentArray;

						TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
						TextPart->SetStringField(TEXT("type"), TEXT("text"));
						TextPart->SetStringField(TEXT("text"), Msg.Content);
						ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));

						for (const FString& ImageURI : Msg.ImageDataURIs)
						{
							TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject());
							ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));

							TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject());
							ImageUrlObj->SetStringField(TEXT("url"), ImageURI);
							ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);

							ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));
						}

						MsgObj->SetArrayField(TEXT("content"), ContentArray);
					}
					else
					{
						MsgObj->SetStringField(TEXT("content"), Msg.Content.IsEmpty() ? TEXT("图片已生成。") : Msg.Content);
					}
				}
				else
				{
					MsgObj->SetStringField(TEXT("content"), Msg.Content);
				}

				if (Msg.Role == EMCPToolboxMessageRole::Assistant && !Msg.ReasoningContent.IsEmpty())
				{
					MsgObj->SetStringField(TEXT("reasoning_content"), Msg.ReasoningContent);
				}

				HistoryMsgs.Add(MakeShareable(new FJsonValueObject(MsgObj)));
			}

			TWeakPtr<SMCPToolboxChatWidget> WeakSelf = SharedThis(this);

			Async(EAsyncExecution::Thread,
				[this, WeakSelf, NameCap, ContentCap, FuncArgsCap, HistoryMsgs]()
			{
				FString Result = ExecuteToolCall(NameCap, FuncArgsCap);

				AsyncTask(ENamedThreads::GameThread,
					[WeakSelf, NameCap, ContentCap, FuncArgsCap, Result, HistoryMsgs]()
					{
					TSharedPtr<SMCPToolboxChatWidget> Self = WeakSelf.Pin();
					if (!Self.IsValid()) return;

					FMCPToolboxChatMessage ResultMsg;
					ResultMsg.Role = EMCPToolboxMessageRole::System;
					ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *Result.Left(500));
					Self->AddMessage(ResultMsg);

					TArray<TSharedPtr<FJsonValue>> NewMsgs = HistoryMsgs;

					TSharedPtr<FJsonObject> AsstMsgWithToolCalls = MakeShareable(new FJsonObject());
					AsstMsgWithToolCalls->SetStringField(TEXT("role"), TEXT("assistant"));
					AsstMsgWithToolCalls->SetStringField(TEXT("content"), TEXT(""));

					TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
					TSharedPtr<FJsonObject> ToolCallObj = MakeShareable(new FJsonObject());
					ToolCallObj->SetStringField(TEXT("id"), TEXT("call_0"));

					TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject());
					FuncObj->SetStringField(TEXT("name"), NameCap);
					FuncObj->SetStringField(TEXT("arguments"), FuncArgsCap);
					ToolCallObj->SetObjectField(TEXT("function"), FuncObj);

					ToolCallsArray.Add(MakeShareable(new FJsonValueObject(ToolCallObj)));
					AsstMsgWithToolCalls->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
					NewMsgs.Add(MakeShareable(new FJsonValueObject(AsstMsgWithToolCalls)));

					TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
					ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
					ToolResultMsg->SetStringField(TEXT("tool_call_id"), TEXT("call_0"));
					ToolResultMsg->SetStringField(TEXT("name"), NameCap);
					ToolResultMsg->SetStringField(TEXT("content"), Result);
					NewMsgs.Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

					FString DisplayURL;
					FString ImageAnalysis;

					TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
					TSharedPtr<FJsonObject> ResultObj;
					if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
					{
						FString ImageURL, ImageData, Status;
						ResultObj->TryGetStringField(TEXT("status"), Status);
						ResultObj->TryGetStringField(TEXT("image_url"), ImageURL);
						ResultObj->TryGetStringField(TEXT("image_data"), ImageData);

						if (Status == TEXT("ok") && (!ImageURL.IsEmpty() || !ImageData.IsEmpty()))
						{
							DisplayURL = ImageURL.IsEmpty() ? ImageData : ImageURL;

							FMCPToolboxChatMessage ImageMsg;
							ImageMsg.Role = EMCPToolboxMessageRole::Assistant;
							ImageMsg.Content = TEXT("**生图成功！**");
							ImageMsg.bHasImageAttachment = true;
							ImageMsg.ImageDataURIs.Add(DisplayURL);
							ImageMsg.ImageFileNames.Add(TEXT("generated.png"));
							Self->AddMessage(ImageMsg);

							const FMCPToolboxAPIKeyEntry* ActiveEntry = FMCPToolboxAPIManager::Get().GetActiveEntry();
							bool bModelSupportsVision = false;
							if (ActiveEntry)
							{
								const FString& M = ActiveEntry->ModelId;
								bModelSupportsVision = M.Contains(TEXT("gpt-4o")) ||
									M.Contains(TEXT("gpt-4-turbo")) || M.Contains(TEXT("gpt-4-vision")) ||
									M.Contains(TEXT("o1")) || M.Contains(TEXT("o3")) ||
									M.Contains(TEXT("claude-3")) || M.Contains(TEXT("claude-4")) ||
									M.Contains(TEXT("gemini")) ||
									M.Contains(TEXT("vl")) || M.Contains(TEXT("vision")) ||
									M.Contains(TEXT("image")) ||
									M.Contains(TEXT("qwen-vl")) || M.Contains(TEXT("glm-4v")) ||
									M.Contains(TEXT("yi-vision")) || M.Contains(TEXT("step-1v")) ||
									M.Contains(TEXT("pixtral")) || M.Contains(TEXT("sonar"));
							}

							if (bModelSupportsVision)
							{
								TArray<TSharedPtr<FJsonValue>> ContentArray;

								TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject());
								TextPart->SetStringField(TEXT("type"), TEXT("text"));
								TextPart->SetStringField(TEXT("text"), TEXT("图片已生成，请分析。"));
								ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));

								TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject());
								ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));

								TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject());
								ImageUrlObj->SetStringField(TEXT("url"), DisplayURL);
								ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);

								ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));

								TSharedPtr<FJsonObject> ImageUserMsg = MakeShareable(new FJsonObject());
								ImageUserMsg->SetStringField(TEXT("role"), TEXT("user"));
								ImageUserMsg->SetArrayField(TEXT("content"), ContentArray);
								NewMsgs.Add(MakeShareable(new FJsonValueObject(ImageUserMsg)));
							}
							else
							{
								TSharedPtr<FJsonObject> TextOnlyMsg = MakeShareable(new FJsonObject());
								TextOnlyMsg->SetStringField(TEXT("role"), TEXT("user"));
								TextOnlyMsg->SetStringField(TEXT("content"), TEXT("图片已生成。图片内容分析：这是用户要求生成的图片。"));
								NewMsgs.Add(MakeShareable(new FJsonValueObject(TextOnlyMsg)));
							}
						}
					}

					Self->RebuildChatDisplay();

					Self->bIsWaiting = true;
					Self->SendAIRequest(NewMsgs);
				});
			});
		}
		else
		{
			FString Result = ExecuteToolCall(DetectedToolName, FuncArgs);

			FMCPToolboxChatMessage ResultMsg;
			ResultMsg.Role = EMCPToolboxMessageRole::System;
			ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *DetectedToolName, *Result.Left(500));
			AddMessage(ResultMsg);

			TArray<TSharedPtr<FJsonValue>> NewMsgs;

			TSharedPtr<FJsonObject> AsstMsg = MakeShareable(new FJsonObject());
			AsstMsg->SetStringField(TEXT("role"), TEXT("assistant"));
			AsstMsg->SetStringField(TEXT("content"), Content);
			NewMsgs.Add(MakeShareable(new FJsonValueObject(AsstMsg)));

			TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
			ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
			ToolResultMsg->SetStringField(TEXT("tool_call_id"), TEXT(""));
			ToolResultMsg->SetStringField(TEXT("name"), DetectedToolName);
			ToolResultMsg->SetStringField(TEXT("content"), Result);
			NewMsgs.Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

			bIsWaiting = true;
			SendAIRequest(NewMsgs);
		}
		return;
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Streaming text completion: no tool intent detected"));
}

#undef LOCTEXT_NAMESPACE
