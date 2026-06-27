#include "MCPToolboxChatWidget.h"
#include "MCPToolbox.h"
#include "MCPToolboxAPIManager.h"
#include "MCPToolboxMemoryManager.h"
#include "MCPToolboxScreenshot.h"
#include "MCPToolboxAuxModelManager.h"

// Assistant library — only FunctionBuilder/FunctionTable (no HTTP layer)
#include "assistant/function.hpp"

#include "SlateCore.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
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
#include "HttpModule.h"
#include "Containers/Ticker.h"
#include "Async/Async.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
	return SNew(SBorder).Padding(FMargin(4))
		[
			SNew(SVerticalBox)
			// Row 1: input box + send
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0).Padding(FMargin(0, 0, 4, 0))
				[
					SAssignNew(InputTextBox, SMultiLineEditableTextBox)
					.HintText(LOCTEXT("InputHint", "输入消息... 回车发送, Shift+回车换行"))
					.OnKeyDownHandler(this, &SMCPToolboxChatWidget::OnInputKeyDown)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text_Lambda([this]() { return bIsWaiting ? LOCTEXT("StopBtn", "停止") : LOCTEXT("SendBtn", "发送"); })
					.OnClicked_Lambda([this]() { return bIsWaiting ? OnInterrupt() : OnSendMessage(); })
					.ButtonColorAndOpacity_Lambda([this]() { return bIsWaiting ? FLinearColor(0.8f, 0.2f, 0.2f) : FLinearColor::White; })
				]
			]
			// Row 2: action buttons (optimize / undo / upload / clear)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 2, 0, 0))
			[
				SNew(SHorizontalBox)
				// ── Prompt Optimization ──
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 4, 0))
				[
					SNew(SButton)
					.Text_Lambda([this]() {
						if (bOptimizingPrompt) return LOCTEXT("OptimizingBtn", "优化中...");
						return LOCTEXT("OptimizeBtn", "优化");
					})
					.OnClicked(this, &SMCPToolboxChatWidget::OnOptimizePrompt)
					.IsEnabled_Lambda([this]() {
						return !bIsWaiting && !bOptimizingPrompt && FMCPToolboxAuxModelManager::Get().IsReady();
					})
					.ToolTipText_Lambda([this]() {
						if (!FMCPToolboxAuxModelManager::Get().IsReady())
							return LOCTEXT("OptimizeDisabledTooltip", "需要本地辅助模型运行");
						return LOCTEXT("OptimizeTooltipFirst", "用本地模型提炼提示词核心意图");
					})
					.ButtonColorAndOpacity_Lambda([this]() {
						if (bOptimizingPrompt) return FLinearColor(0.4f, 0.5f, 0.9f);
						return (FMCPToolboxAuxModelManager::Get().IsReady())
							? FLinearColor(0.3f, 0.15f, 0.5f) : FLinearColor(0.3f, 0.3f, 0.3f);
					})
				]
				// ── Undo Optimization ──
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 4, 0))
				[
					SNew(SButton)
					.Text(LOCTEXT("UndoBtn", "退回"))
					.OnClicked(this, &SMCPToolboxChatWidget::OnUndoOptimization)
					.IsEnabled_Lambda([this]() { return bPromptOptimized && !bOptimizingPrompt; })
					.ToolTipText(LOCTEXT("UndoTooltip", "撤销优化，恢复原始提示词"))
					.Visibility_Lambda([this]() { return bPromptOptimized ? EVisibility::Visible : EVisibility::Collapsed; })
					.ButtonColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.2f))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 4, 0))
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
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton).Text(LOCTEXT("ClearBtn", "清空"))
					.OnClicked(this, &SMCPToolboxChatWidget::OnClearChat)
				]
			]
		];
}

// ============================================================================
// Send Message — builds messages, then delegates to SendAIRequest
// ============================================================================
FReply SMCPToolboxChatWidget::OnSendMessage()
{
	if (!InputTextBox.IsValid() || bIsWaiting) return FReply::Handled();

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
		PendingUploadURIs.Empty();
	}
	AddMessage(UserMsg);
	InputTextBox->SetText(FText::GetEmpty());

	bIsWaiting = true;
	bInterrupted = false;
	ToolCallIteration = 0; // Reset iteration counter for new message
	ToolCallHistory.Empty();
	ConsecutiveSameToolCount = 0;

	// Build messages array for API — PRESERVE conversation history!
	// NOTE: UserMsg has already been pushed into Messages via AddMessage() above,
	// so iterating Messages naturally includes it. Do NOT append it again.
	TArray<TSharedPtr<FJsonValue>> Msgs;

	// Always fresh system prompt (memory may have been updated)
	FString MemoryCtx = FMCPToolboxMemoryManager::Get().BuildMemoryContext();
	FString SystemPrompt = BuildSystemPrompt(MemoryCtx);
	TSharedPtr<FJsonObject> SysMsg = MakeShareable(new FJsonObject());
	SysMsg->SetStringField(TEXT("role"), TEXT("system"));
	SysMsg->SetStringField(TEXT("content"), SystemPrompt);
	Msgs.Add(MakeShareable(new FJsonValueObject(SysMsg)));

	// Append conversation history (includes the user message just added above)
	for (const auto& Msg : Messages)
	{
		// Skip the welcome message — it's UI-only, not for LLM context
		if (Msg.Role == EMCPToolboxMessageRole::Assistant &&
			Msg.Content.StartsWith(TEXT("**欢迎使用 MCP Toolbox")))
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

		if (Msg.bHasImageAttachment && Msg.ImageDataURIs.Num() > 0)
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
// Send AI Request — reusable, called from OnSendMessage and tool loops
// ============================================================================
void SMCPToolboxChatWidget::SendAIRequest(const TArray<TSharedPtr<FJsonValue>>& ApiMessages)
{
	if (bInterrupted) { bIsWaiting = false; return; }

	FMCPToolboxAuxModelManager& AuxMgr = FMCPToolboxAuxModelManager::Get();

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

	// Auto-fallback: vision ON but model doesn't support images → preprocess locally
	bool bNeedLocalVL = (bVisionModeEnabled && !bModelSupportsVision) || (!bVisionModeEnabled && AuxMgr.IsReady());
	bool bHasImages = false;

	for (const auto& V : ApiMessages)
	{
		TSharedPtr<FJsonObject> Obj = V->AsObject();
		if (!Obj.IsValid()) continue;
		const TArray<TSharedPtr<FJsonValue>>* Content;
		if (Obj->TryGetArrayField(TEXT("content"), Content))
		{
			for (const auto& C : *Content)
			{
				TSharedPtr<FJsonObject> CObj = C->AsObject();
				if (CObj.IsValid())
				{
					FString Type;
					if (CObj->TryGetStringField(TEXT("type"), Type) && Type == TEXT("image_url"))
						{ bHasImages = true; break; }
				}
			}
		}
		if (bHasImages) break;
	}

	if (bNeedLocalVL && bHasImages)
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Auto-preprocessing images: vision=%s, model_supports_vision=%s"),
			bVisionModeEnabled ? TEXT("ON") : TEXT("OFF"),
			bModelSupportsVision ? TEXT("yes") : TEXT("no"));
		PreprocessImagesLocally(ApiMessages,
			[this](const TArray<TSharedPtr<FJsonValue>>& Processed) { SendAIRequestInternal(Processed); });
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

			FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, StreamPtr, ChunkIndex, DisplayBuffer, Chunks](float DeltaTime) -> bool
			{
				if (bInterrupted || *ChunkIndex >= Chunks->Num())
				{
					StreamPtr->bIsStreaming = false;
					StreamPtr->Content = *DisplayBuffer;
					FMCPToolboxMemoryManager::Get().ExtractMemoriesFromResponse(*DisplayBuffer);
					bIsWaiting = false;
					// Final rebuild to show cleaned markdown
					RebuildChatDisplay();
					// Persist the completed streaming message to the session.
					// Previously this only called SaveCurrentSession(), which serializes
					// Session->Messages — but the streaming message was added to the
					// *widget's* Messages array via Messages.Add() at line 930 and never
					// transferred into Session->Messages. As a result, every streaming AI
					// reply was lost on editor restart or session switch.
					// UpdateCurrentSessionWithMessage adds to Session->Messages AND saves.
					UpdateCurrentSessionWithMessage(*StreamPtr);
					RefreshSessionList();
					StreamingMessageBox.Reset();
					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Stream done, %d chars"), DisplayBuffer->Len());

					// Auto-archive opportunity: defer to next game-thread tick to avoid
					// nesting HTTP startup inside the ticker callback. Use weak pointer
					// because the widget may be destroyed between ticks.
					TWeakPtr<SMCPToolboxChatWidget> WeakSelf = SharedThis(this);
					AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
					{
						if (TSharedPtr<SMCPToolboxChatWidget> Self = WeakSelf.Pin())
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
		// Loop detection: abort if same tool name repeats more than 10 times
		++ToolCallIteration;
		FString CurrentToolNames;
		for (const auto& TC : *ToolCalls)
		{
			TSharedPtr<FJsonObject> TCObj = TC->AsObject();
			if (TCObj.IsValid())
			{
				const TSharedPtr<FJsonObject>* Func;
				if (TCObj->TryGetObjectField(TEXT("function"), Func) && Func->IsValid())
				{
					FString Name;
					(*Func)->TryGetStringField(TEXT("name"), Name);
					if (!CurrentToolNames.IsEmpty()) CurrentToolNames += TEXT(",");
					CurrentToolNames += Name;
				}
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
				const TSharedPtr<FJsonObject>* Func;
				if (TCObj->TryGetObjectField(TEXT("function"), Func) && Func->IsValid())
				{
					FString Name, Args;
					(*Func)->TryGetStringField(TEXT("name"), Name);
					(*Func)->TryGetStringField(TEXT("arguments"), Args);
					if (!ToolNames.IsEmpty()) ToolNames += TEXT(", ");
					ToolNames += Name;
					ToolArgsList.Add(Args);
				}
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
			const TSharedPtr<FJsonObject>* Func;
			if (!TCObj->TryGetObjectField(TEXT("function"), Func) || !Func->IsValid()) continue;
			FString FuncName, FuncArgs;
			(*Func)->TryGetStringField(TEXT("name"), FuncName);
			(*Func)->TryGetStringField(TEXT("arguments"), FuncArgs);

			// Check if this is an MCP tool that needs async execution
			bool bIsMCP = MCPServerClient.IsConnected() && MCPServerClient.IsMCPTool(FuncName);

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
							ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *R.Left(500));
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
				// Local tool: execute synchronously
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

				// If this is a screenshot tool result with image data, add a user message with the image
				if (FuncName == TEXT("screenshot"))
				{
					TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result);
					TSharedPtr<FJsonObject> ResultObj;
					if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid())
					{
						FString DataURI;
						if (ResultObj->TryGetStringField(TEXT("data_uri"), DataURI) && !DataURI.IsEmpty())
						{
							// Build content array with text + image
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

							// Also add a visible message to the chat
							FMCPToolboxChatMessage ScreenshotMsg;
							ScreenshotMsg.Role = EMCPToolboxMessageRole::User;
							ScreenshotMsg.Content = TEXT("（截图已捕获，正在分析...）");
							ScreenshotMsg.bHasImageAttachment = true;
							ScreenshotMsg.ImageDataURIs.Add(DataURI);
							AddMessage(ScreenshotMsg);
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
		FMCPToolboxChatMessage Reply;
		Reply.Role = EMCPToolboxMessageRole::Assistant;
		Reply.Content = Content;
		AddMessage(Reply);
		FMCPToolboxMemoryManager::Get().ExtractMemoriesFromResponse(Content);
		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 响应完成, %d chars, finish=%s"), Content.Len(), *FinishReason);

		// If finish_reason is "length", AI was cut off — ask it to continue
		if (FinishReason == TEXT("length"))
		{
			TArray<TSharedPtr<FJsonValue>> ContinueMsgs = SentMessages;
			TSharedPtr<FJsonObject> AsstMsg = MakeShareable(new FJsonObject());
			AsstMsg->SetStringField(TEXT("role"), TEXT("assistant"));
			AsstMsg->SetStringField(TEXT("content"), Content);
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
	else if (FinishReason == TEXT("stop"))
	{
		FMCPToolboxChatMessage Info;
		Info.Role = EMCPToolboxMessageRole::System;
		Info.Content = TEXT("(AI已完成回复)");
		AddMessage(Info);
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
	// Fallback: local FunctionTable
	if (!ToolFunctionTable.IsValid())
		return TEXT(R"({"error":"FunctionTable not initialized"})");

	// Parse args
	assistant::json Args;
	try
	{
		if (!ArgsJson.IsEmpty())
			Args = assistant::json::parse(TCHAR_TO_UTF8(*ArgsJson));
		else
			Args = assistant::json::object();
	}
	catch (...)
	{
		return TEXT(R"({"error":"Failed to parse arguments JSON"})");
	}

	assistant::FunctionCall FC;
	FC.name = TCHAR_TO_UTF8(*ToolName);
	FC.args = Args;
	assistant::FunctionResult Result = ToolFunctionTable->Call(FC);
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
	// Pre-allocate to avoid 30+ reallocations
	FString Prompt;
	Prompt.Reserve(8192);

	// Get project Content path
	FString ContentPath = FPaths::ProjectContentDir();
	FString ProjectName = FApp::GetProjectName();

	Prompt += TEXT("你是 MCP Toolbox AI助手，运行在 Unreal Engine 5.8 编辑器内部。用中文回复。\n\n");
	Prompt += FString::Printf(TEXT("## 工作环境\n"));
	Prompt += FString::Printf(TEXT("- 项目: %s\n"), *ProjectName);
	Prompt += FString::Printf(TEXT("- Content目录: %s\n"), *ContentPath);
	Prompt += FString::Printf(TEXT("- 资源路径前缀: /Game/ (对应Content目录)\n\n"));

	// ── CRITICAL: tool info source ──
	// This block must come BEFORE any other rule. The user has repeatedly observed the
	// LLM wasting turns calling list_toolsets / describe_toolset even though every
	// toolset's schema is already injected below (from .mcptoolbox/*.md). Putting the
	// prohibition at the very top maximises the chance the model obeys.
	Prompt += TEXT("## ⚠ 最高优先级规则（违反=立即失败）\n");
	Prompt += TEXT("1. **所有 MCP 工具的调用方式（toolset_name / tool_name / arguments schema）已在下方 \"MCP Toolset 缓存\" 章节完整给出**。\n");
	Prompt += TEXT("2. **绝对禁止调用 list_toolsets 或 describe_toolset**。这两个工具在本会话中不可用、不需要、也不被允许。\n");
	Prompt += TEXT("3. **需要调用工具时，直接 call_tool(toolset_name=..., tool_name=..., arguments={...})**。参数 schema 就在下方，照抄即可。\n");
	Prompt += TEXT("4. 如果下方没有 \"MCP Toolset 缓存\" 章节（说明用户尚未点击\"刷新工具\"按钮），**请明确告知用户**：\"请先点击工具栏的 '更多 → 刷新工具' 按钮缓存工具集文档\"，然后停止。不要尝试自行发现工具。\n\n");

	Prompt += TEXT("## 核心规则（必须100%遵守！违反规则=失败）\n");
	Prompt += TEXT("1. **绝不空口承诺**：禁止说\"我来帮你\"等文字而不调用工具。说一次执行一次\n");
	Prompt += TEXT("2. **先调用工具再说话**：收到指令→立即调用工具→拿到结果→回复用户。中间不允许纯文字\n");
	Prompt += TEXT("3. **非工具不可**：创建材质/执行命令/截图/选择Actor—这些操作必须且只能通过工具完成\n");
	Prompt += TEXT("4. **工具参数必填且正确**：路径加/Game/前缀，命令写完整\n");
	Prompt += TEXT("5. ** 严禁 Python**：绝对禁止用 command(cmd=\"py ...\") 创建资产、材质、蓝图、PCG等编辑器操作。这些操作容易出错且不稳定。必须通过MCP工具完成。违者=失败！\n");
	Prompt += TEXT("6. ** MCP工具已内置（禁止重复发现！）**：所有MCP工具已在下方'已发现MCP工具'完整列出。直接 call_tool 调用。list_toolsets/describe_toolset 不可用，也不要尝试调用。\n");
	Prompt += TEXT("7. **command仅限控制台命令**：command只用于 HighResShot、stat fps 等纯控制台命令，绝不用于Python脚本。\n");
	Prompt += TEXT("8. **主动记忆总结**：每完成一个有价值的任务或学到新经验后，在回复末尾**单独一行**输出以下任一格式以触发持久化记忆：\n");
	Prompt += TEXT("   - `记住：xxx`（保存工具使用经验）\n");
	Prompt += TEXT("   - `重要：xxx`（保存项目关键事实）\n");
	Prompt += TEXT("   - `偏好：xxx`（保存用户偏好）\n");
	Prompt += TEXT("   - `总结：xxx`（保存本次任务的核心经验）\n");
	Prompt += TEXT("   - `经验：xxx`（保存踩坑/最佳实践）\n");
	Prompt += TEXT("   示例：`记住：call_tool 创建材质时 toolset_name=unreal_editor_asset, tool_name=create_material`\n");
	Prompt += TEXT("9. **视觉模式**：用户上传的图片和screenshot工具返回的截图，你都可以直接看到并分析。视觉模式开启时screenshot才可用。\n\n");

	Prompt += TEXT("##  效率规则（必须严格遵守！目标：减少round-trip，提高速度）\n");
	Prompt += TEXT("1. **批量读取**：需要读取多个文件时，使用 batch_read_files 一次性读取，禁止逐个调用 read_file\n");
	Prompt += TEXT("2. **并行工具调用**：如果有多个独立的工具需要调用（如搜索+读取、截图+inspect），在同一次响应中并行调用多个tool_calls，不要分多次\n");
	Prompt += TEXT("3. **先搜索再读取**：需要找代码时，先用 search_codebase 搜索定位，再用 batch_read_files 批量读取，避免盲目读取大量文件\n");
	Prompt += TEXT("4. **一次思考完整方案**：不要每次只想一步。先在脑海中规划好完整方案，然后一次性调用所有需要的工具\n");
	Prompt += TEXT("5. **减少对话轮数**：目标是用最少的轮数完成任务。能一轮完成的绝不两轮，能两轮完成的绝不三轮\n");
	Prompt += TEXT("6. **善用 glob_search**：找文件用 glob_search，比逐个目录 list_directory 快得多\n");
	Prompt += TEXT("7. **工具调用是免费的**：不要因为怕调用工具而省着用。但要聪明地用——批量用、并行用、一次用对\n");
	Prompt += TEXT("8. ** DAG 依赖式并行（LLMCompiler）**：当工具调用有依赖关系时，使用 $tN.xxx 引用语法声明依赖，系统会自动构建DAG并按层并行执行！\n");
	Prompt += TEXT("   - 格式：每个工具调用可通过参数中的 $t1.result、$t2.data 等引用前面任务的结果\n");
	Prompt += TEXT("   - 示例：t1=search_codebase(\"bug\"), t2=read_file(path=$t1.result[0]), t3=analyze(data=$t2.content)\n");
	Prompt += TEXT("   - 执行方式：系统自动检测依赖，构建DAG，无依赖的任务并行执行，大幅提速\n");
	Prompt += TEXT("   - 任务ID：使用 t1, t2, t3... 作为任务标识，在参数中用 $tN.field 引用\n\n");

	Prompt += TEXT("## 工具优先级（从高到低，严格按此顺序）\n");
	Prompt += TEXT("1. **MCP call_tool（直接调用！）**：工具已在下方列出，直接用 call_tool 调用。多个独立调用时一次性批量发出（多个tool_calls）\n");
	Prompt += TEXT("2. **本地工具**：screenshot, select, inspect\n");
	Prompt += TEXT("3. **command**：仅限控制台命令(HighResShot, stat等)，禁止py脚本\n\n");

	Prompt += TEXT("## 工具列表\n");
	Prompt += TEXT("### MCP工具（直接调用，无需发现）\n");
	Prompt += TEXT("- **call_tool** — 调用MCP工具。参数: toolset_name (string), tool_name (string), arguments (object)。工具集和工具名见下方'已发现MCP工具'列表\n");
	Prompt += TEXT("### 本地高效工具（优先使用，节省时间）\n");
	Prompt += TEXT("- **batch_read_files** —  批量读取多个文件。参数: file_paths (array of strings)。**读取多个文件时必须用这个，禁止逐个读**\n");
	Prompt += TEXT("- **search_codebase** —  搜索整个代码库。参数: pattern (string), path (可选), file_pattern (可选,默认*.cpp,*.h), max_results (可选,默认50)。**找代码先用这个**\n");
	Prompt += TEXT("- **glob_search** —  按文件名模式搜索文件。参数: pattern (string, 如 *.cpp, **/*.h), path (可选)。**找文件用这个**\n");
	Prompt += TEXT("- **list_directory** — 列出目录内容。参数: path (string)\n");
	Prompt += TEXT("### 本地辅助工具\n");
	Prompt += TEXT("- **screenshot** — 截取屏幕图片，你可以直接看到图片内容进行分析。**仅当视觉模式开启时可用**。返回 data:image/jpeg;base64 格式的图片数据\n");
	Prompt += TEXT("- **select** — 选择Actor。参数: name (string)\n");
	Prompt += TEXT("- **inspect** — 检查选中Actor属性\n");

	// Inject cached MCP tool descriptions (built once, avoids repeated list_toolsets queries)
	if (!CachedMCPToolDescriptionsMD.IsEmpty())
	{
		Prompt += TEXT("\n");
		Prompt += CachedMCPToolDescriptionsMD;
	}

	// ── 加载 .mcptoolbox/ 缓存（由 fetch-mcp-toolsets.ps1 预拉取生成）──
	// 这些 MD 文件包含所有 toolset 的精确 call_tool 调用方式，避免 LLM 反复试探参数
	{
		FString ToolboxDir = FPaths::ProjectDir() / TEXT(".mcptoolbox");
		IFileManager& FileMgr = IFileManager::Get();
		if (FileMgr.DirectoryExists(*ToolboxDir))
		{
			Prompt += TEXT("\n##  MCP Toolset 缓存（来自 .mcptoolbox/）\n");
			Prompt += TEXT("> 以下文档由 fetch-mcp-toolsets.ps1 预拉取生成，已包含所有 toolset 的精确调用方式（toolset_name/tool_name/arguments）。\n");
			Prompt += TEXT("> **不要调用 list_toolsets/describe_toolset** — 本章节已包含所有信息。\n");
			Prompt += TEXT("> **关键**：toolset_name 是完整 toolset 路径（不含 tool_name），tool_name 是 toolset 中的工具名。\n\n");

			int32 LoadedCount = 0;
			int32 TotalChars = 0;
			const int32 MaxTotalChars = 60000;  // ~15K tokens 上限

			// 先加载 index.md（总索引）
			FString IndexPath = ToolboxDir / TEXT("index.md");
			if (FileMgr.FileExists(*IndexPath))
			{
				FString IndexContent;
				if (FFileHelper::LoadFileToString(IndexContent, *IndexPath))
				{
					Prompt += TEXT("### 索引\n\n");
					Prompt += IndexContent;
					Prompt += TEXT("\n\n");
					TotalChars += IndexContent.Len();
					LoadedCount++;
				}
			}

			// 加载所有其他 .md（按字母序）
			TArray<FString> MdFiles;
			FileMgr.FindFiles(MdFiles, *ToolboxDir, TEXT(".md"));
			MdFiles.Sort();

			for (const FString& FileName : MdFiles)
			{
				if (FileName == TEXT("index.md")) continue;

				FString FilePath = ToolboxDir / FileName;
				FString FileContent;
				if (FFileHelper::LoadFileToString(FileContent, *FilePath))
				{
					int32 Remaining = MaxTotalChars - TotalChars;
					if (Remaining <= 0)
					{
						Prompt += FString::Printf(TEXT("<!-- 已达上限，剩余文件未加载: %s -->\n"), *FileName);
						break;
					}
					if (FileContent.Len() > Remaining)
					{
						FileContent = FileContent.Left(Remaining) + TEXT("\n... (已截断)\n");
					}
					Prompt += FileContent;
					Prompt += TEXT("\n");
					TotalChars += FileContent.Len();
					LoadedCount++;
				}
			}

			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Loaded %d toolset docs from .mcptoolbox/ (%d chars)"), LoadedCount, TotalChars);
		}
		else
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] .mcptoolbox/ not found at %s — run fetch-mcp-toolsets.ps1 to pre-fetch toolset docs"), *ToolboxDir);
		}
	}
	Prompt += TEXT("### 禁止使用（除非MCP完全不可用）\n");
	Prompt += TEXT("- **command** —  禁止用于py脚本。仅限纯控制台命令。\n\n");

	Prompt += TEXT("## 行为示例（正确 vs 错误）\n");
	Prompt += TEXT("用户: 帮我创建自发光白色材质\n");
	Prompt += TEXT(" 错误: 直接用 command(cmd=\"py ...\") (绕开MCP)\n");
	Prompt += TEXT(" 正确: 从下方'已发现MCP工具'中找到材质工具 → call_tool 创建材质\n");
	Prompt += TEXT(" 正确(MCP不可用时): command(cmd=\"py ...创建自发光材质...\")\n\n");

	Prompt += TEXT("##  效率示例（快 vs 慢）\n");
	Prompt += TEXT("用户: 帮我看看MCPToolboxChatWidget.h和MCPToolboxChatWidget.cpp里的工具调用相关代码\n");
	Prompt += TEXT(" 慢: 先list_directory找文件 → read_file读.h → read_file读.cpp → 读了5轮\n");
	Prompt += TEXT(" 快: search_codebase(\"ToolCall\") → 定位到相关行 → batch_read_files([两个文件]) → 1-2轮搞定\n\n");

	Prompt += TEXT("用户: 帮我修改5个文件\n");
	Prompt += TEXT(" 慢: 读文件1 → 改文件1 → 读文件2 → 改文件2 → ... 10轮\n");
	Prompt += TEXT(" 快: batch_read_files(5个文件) → 一次性看完 → 并行调用5个call_tool改文件 → 2-3轮搞定\n\n");

	Prompt += TEXT("用户: 搜索bug→读文件→分析→生成修复方案\n");
	Prompt += TEXT(" 慢: search → read → analyze → report → 4轮\n");
	Prompt += TEXT(" 快(DAG并行): t1=search, t2=search2(另一个关键词), t3=read(path=$t1.result[0]), t4=analyze(data=$t3.content) → 1轮(t1/t2并行, t3等t1, t4等t3)\n\n");

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

	Prompt += TEXT("## 记忆规则\n");
	Prompt += TEXT("- 每次对话后必须用\"记住：xxx\"保存用户偏好、MCP工具使用经验\n");
	Prompt += TEXT("- MCP工具调用成功的方法用\"记住：MCP工具 XXX 使用方法：...\"保存\n");
	Prompt += TEXT("- 项目信息用\"重要：xxx\"保存\n");
	Prompt += TEXT("- 用户偏好用\"偏好：xxx\"保存\n\n");

	// ── 科学辩证观 — AI 行为准则（八荣八耻） ──
	// 强制 AI 在每一轮决策前对照这套准则，避免瞎猜/浪费/破坏架构等典型问题。
	Prompt += TEXT("## AI 行为准则\n");
	Prompt += TEXT("> 每一轮决策前必须对照此准则自检；违反任意一条视为本轮失败。\n\n");
	Prompt += TEXT("- 以认真查询为荣，以瞎猜接口为耻\n");
	Prompt += TEXT("- 以寻求 MD 为荣，以浪费时间为耻\n");
	Prompt += TEXT("- 以人类确认为荣，以模糊执行为耻\n");
	Prompt += TEXT("- 以复用现有为荣，以创造求证为耻\n");
	Prompt += TEXT("- 以诚信无知为荣，以假装理解为耻\n");
	Prompt += TEXT("- 以遵循规范为荣，以破坏架构为耻\n");
	Prompt += TEXT("- 以谨慎重构为荣，以盲目脚本为耻\n");
	Prompt += TEXT("- 以备份数据为荣，以覆盖丢失为耻\n\n");
	Prompt += TEXT("具体落地：\n");
	Prompt += TEXT("1. 不确定的接口/路径/枚举值必须先 search/read 文件再使用，不得凭印象编写\n");
	Prompt += TEXT("2. 项目内已有 .mcptoolbox/*.md 缓存（工具集说明、归档总结）必须先读再行动\n");
	Prompt += TEXT("3. 涉及破坏性操作（删除/覆盖/重命名/批量修改）前必须先告知用户并获得确认\n");
	Prompt += TEXT("4. 优先复用现有函数/工具/抽象，禁止为了\"清洁\"而创造新接口\n");
	Prompt += TEXT("5. 不知道就说不知道，不得编造虚假 API 或参数\n");
	Prompt += TEXT("6. 严格遵守项目硬约束（见上下文中的 project_memory）\n");
	Prompt += TEXT("7. 重构必须最小化、谨慎进行，禁止\"顺手\"批量改架构\n");
	Prompt += TEXT("8. 覆盖任何持久化文件（MD/JSON/配置）前必须先备份原文件\n\n");

	Prompt += TEXT("## 格式\n");
	Prompt += TEXT("- 工具执行完后用中文Markdown回复结果\n");
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
		.SetDescription("Read multiple files at once. Returns contents of all files in a single response. Use this instead of reading files one by one to save time.")
		.AddRequiredParam("file_paths", "Array of absolute file paths to read", "array")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			if (!args.contains("file_paths") || !args["file_paths"].is_array())
			return {.isError = true, .text = R"RAW({"error":"Missing file_paths parameter (must be array)"})RAW"};

			std::string Result = R"({"status":"ok","files":[)";
			bool bFirst = true;
			
			for (const auto& PathItem : args["file_paths"])
			{
				if (!PathItem.is_string()) continue;
				std::string StdPath = PathItem.get<std::string>();
				FString FilePath = UTF8_TO_TCHAR(StdPath.c_str());
				
				FString Content;
				bool bSuccess = FFileHelper::LoadFileToString(Content, *FilePath);
				
				if (!bFirst) Result += ",";
				bFirst = false;
				
				Result += R"({"path":")" + StdPath + R"(",)";
				if (bSuccess)
				{
					std::string StdContent = TCHAR_TO_UTF8(*Content);
					Result += R"("success":true,"content":")" + StdContent + R"("})";
				}
				else
				{
					Result += R"("success":false,"error":"Failed to read file"})";
				}
			}
			Result += "]}";
			return {.isError = false, .text = Result};
		}).Build());

	// Search codebase tool
	ToolFunctionTable->Add(assistant::FunctionBuilder("search_codebase")
		.SetDescription("Search for code patterns across the entire codebase using regex. Returns matching files and line numbers.")
		.AddRequiredParam("pattern", "Regex pattern to search for", "string")
		.AddOptionalParam("path", "Directory to search in (defaults to project source directory)", "string")
		.AddOptionalParam("file_pattern", "File glob pattern, e.g. *.cpp, *.h (defaults to *.cpp,*.h)", "string")
		.AddOptionalParam("max_results", "Maximum number of results to return (default 50)", "number")
		.SetCallback([](const assistant::json& args) -> assistant::FunctionResult {
			std::string Pattern = args.value("pattern", "");
			if (Pattern.empty())
				return {.isError = true, .text = R"({"error":"Missing pattern parameter"})"};

			std::string SearchPath = args.value("path", "");
			FString BaseDir = SearchPath.empty() ? FPaths::ProjectDir() / TEXT("Source") : UTF8_TO_TCHAR(SearchPath.c_str());
			
			std::string FilePattern = args.value("file_pattern", "*.cpp,*.h");
			int32 MaxResults = args.value("max_results", 50);
			
			TArray<FString> FileTypes;
			FString FilePatternStr = UTF8_TO_TCHAR(FilePattern.c_str());
			FilePatternStr.ParseIntoArray(FileTypes, TEXT(","), true);
			
			TArray<FString> AllFiles;
			for (const FString& Ext : FileTypes)
			{
				TArray<FString> FoundFiles;
				IFileManager::Get().FindFilesRecursive(FoundFiles, *BaseDir, *Ext.TrimStartAndEnd(), true, false);
				AllFiles.Append(FoundFiles);
			}
			
			std::string Result = R"({"status":"ok","pattern":")" + Pattern + R"(","results":[)";
			int32 ResultCount = 0;
			bool bFirst = true;
			
			FString SearchPattern = UTF8_TO_TCHAR(Pattern.c_str());
			
			for (const FString& FilePath : AllFiles)
			{
				if (ResultCount >= MaxResults) break;
				
				FString Content;
				if (!FFileHelper::LoadFileToString(Content, *FilePath)) continue;
				
				TArray<FString> Lines;
				Content.ParseIntoArrayLines(Lines);
				
				for (int32 i = 0; i < Lines.Num() && ResultCount < MaxResults; ++i)
				{
					if (Lines[i].Contains(SearchPattern))
					{
						if (!bFirst) Result += ",";
						bFirst = false;
						
						std::string StdPath = TCHAR_TO_UTF8(*FilePath);
						std::string StdLine = TCHAR_TO_UTF8(*Lines[i].TrimStartAndEnd());
						
						Result += R"({"file":")" + StdPath + R"(",)"
							+ R"("line":)" + std::to_string(i + 1) + R"(,)"
							+ R"("content":")" + StdLine + R"("})";
						ResultCount++;
					}
				}
			}
			Result += R"(],"total":)" + std::to_string(ResultCount) + "}";
			return {.isError = false, .text = Result};
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

			std::string BasePath = args.value("path", "");
			FString BaseDir = BasePath.empty() ? FPaths::ProjectDir() : UTF8_TO_TCHAR(BasePath.c_str());
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
				IFileManager::Get().FindFiles(FoundFiles, *(BaseDir / SearchPattern), false, false);
			}
			
			std::string Result = R"({"status":"ok","pattern":")" + Pattern + R"(","files":[)";
			for (int32 i = 0; i < FoundFiles.Num(); ++i)
			{
				if (i > 0) Result += ",";
				FString FullPath = BaseDir / FoundFiles[i];
				Result += R"(")" + std::string(TCHAR_TO_UTF8(*FullPath)) + R"(")";
			}
			Result += R"(],"count":)" + std::to_string(FoundFiles.Num()) + "}";
			return {.isError = false, .text = Result};
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
			
			std::string Result = R"({"status":"ok","path":")" + Path + R"(","directories":[)";
			for (int32 i = 0; i < Dirs.Num(); ++i)
			{
				if (i > 0) Result += ",";
				Result += R"(")" + std::string(TCHAR_TO_UTF8(*Dirs[i])) + R"(")";
			}
			Result += R"(],"files":[)";
			for (int32 i = 0; i < Files.Num(); ++i)
			{
				if (i > 0) Result += ",";
				Result += R"(")" + std::string(TCHAR_TO_UTF8(*Files[i])) + R"(")";
			}
			Result += "]}";
			return {.isError = false, .text = Result};
		}).Build());

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 已注册 %d 个MCP工具"), ToolFunctionTable->GetFunctionsCount());
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
				PendingUploadURIs.Add(URI);
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
// Uses a simple extraction prompt (2B model can't handle complex optimization)
// + post-processing validation to reject garbage output
// ============================================================================

FReply SMCPToolboxChatWidget::OnOptimizePrompt()
{
	if (!InputTextBox.IsValid() || bOptimizingPrompt) return FReply::Handled();

	FString CurrentText = InputTextBox->GetText().ToString().TrimStartAndEnd();
	if (CurrentText.IsEmpty()) return FReply::Handled();

	// Save original to undo buffer
	UndoBuffer = CurrentText;
	bOptimizingPrompt = true;
	bPromptOptimized = false;

	// ── Simple extraction prompt for small model (2B params) ──
	// Instead of "optimize" (which 2B model can't do), ask it to extract core intent
	FString OptPrompt;
	OptPrompt += TEXT("从以下用户输入中提取核心需求,用1-3句简洁中文总结。不要添加任何额外内容,只输出总结:\n\n");
	OptPrompt += CurrentText;
	OptPrompt += TEXT("\n\n总结:");

	FMCPToolboxAuxModelManager& AuxMgr = FMCPToolboxAuxModelManager::Get();
	TWeakPtr<SMultiLineEditableTextBox> WeakInput = InputTextBox;
	FString OriginalText = CurrentText;

	AuxMgr.InferAsync(OptPrompt, 256,
		[this, WeakInput, OriginalText](bool bSuccess, const FString& Output)
		{
			bOptimizingPrompt = false;

			if (!bSuccess || Output.IsEmpty())
			{
				UndoBuffer.Empty();
				FNotificationInfo Info(FText::FromString(TEXT("优化失败：辅助模型未响应")));
				Info.ExpireDuration = 3.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				return;
			}

			FString Cleaned = Output.TrimStartAndEnd();

			// ── Garbage detection ──
			// Reject if output contains the original text repeated many times (model looping)
			int32 OrigCount = 0;
			int32 Pos = 0;
			FString CheckSub = OriginalText.Left(FMath::Min(20, OriginalText.Len()));
			while ((Pos = Cleaned.Find(CheckSub, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos)) != INDEX_NONE)
			{
				OrigCount++;
				Pos += CheckSub.Len();
			}

			// Reject if: repeated original >2x, or output >3x original length, or output
			// contains obvious meta-instructions (the model echoing the prompt template).
			// NOTE: "以下" was removed — it's a legitimate word in Chinese ("以下文件...").
			bool bIsGarbage = (OrigCount > 2) ||
				(Cleaned.Len() > OriginalText.Len() * 3) ||
				Cleaned.Contains(TEXT("优化规则")) ||
				Cleaned.Contains(TEXT("原始提示词")) ||
				Cleaned.Contains(TEXT("用户输入中提取")) ||
				Cleaned.Contains(TEXT("用1-3句"));

			if (bIsGarbage || !WeakInput.IsValid())
			{
				UndoBuffer.Empty();
				FNotificationInfo Info(FText::FromString(TEXT("优化结果不可用（模型输出无效），已自动恢复原文")));
				Info.ExpireDuration = 4.0f;
				FSlateNotificationManager::Get().AddNotification(Info);
				UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Prompt optimization rejected (garbage): orig=%d chars, out=%d chars, repeats=%d"),
					OriginalText.Len(), Cleaned.Len(), OrigCount);
				return;
			}

			// ── Acceptable output ──
			WeakInput.Pin()->SetText(FText::FromString(Cleaned));
			bPromptOptimized = true;

			FNotificationInfo Info(FText::FromString(TEXT("提示词已精简，不满意可点「退回」恢复原文")));
			Info.ExpireDuration = 4.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
			UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Prompt optimized (%d → %d chars)"), OriginalText.Len(), Cleaned.Len());
		});

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

	FNotificationInfo Info(FText::FromString(TEXT("正在感知项目 MCP 工具集...")));
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] OnRefreshToolCache: calling DiscoverRealTools..."));

	MCPServerClient.DiscoverRealTools(
		[this](const TArray<TSharedPtr<FJsonObject>>& Tools)
		{
			// Per project rules, MCP callbacks must execute on the game thread
			// to prevent cross-thread Slate/UObject access.
			AsyncTask(ENamedThreads::GameThread,
				[this, Tools]()
				{
					if (Tools.Num() == 0)
					{
						FNotificationInfo Err(FText::FromString(TEXT("未感知到任何 MCP 工具,请确认 UE5 MCP 服务器已启用")));
						Err.ExpireDuration = 4.0f;
						FSlateNotificationManager::Get().AddNotification(Err);
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
						TEXT("已缓存 %d 个 toolset / %d 个工具到 .mcptoolbox/"),
						Grouped.Num(), Tools.Num());
					FNotificationInfo Ok(FText::FromString(OkText));
					Ok.ExpireDuration = 4.0f;
					FSlateNotificationManager::Get().AddNotification(Ok);

					UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Toolset cache refreshed: %d toolsets, %d tools"),
						Grouped.Num(), Tools.Num());
				});
		});

	return FReply::Handled();
}

void SMCPToolboxChatWidget::WriteToolsetCacheToDisk(const TArray<TSharedPtr<FJsonObject>>& Tools)
{
	FString ToolboxDir = FPaths::ProjectDir() / TEXT(".mcptoolbox");

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.CreateDirectoryTree(*ToolboxDir))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] WriteToolsetCacheToDisk: failed to create dir %s"), *ToolboxDir);
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

	// Write index.md
	FString Index;
	Index += TEXT("# MCP Toolset 索引(运行时感知)\n\n");
	Index += FString::Printf(TEXT("生成时间: %s\n"), *FDateTime::Now().ToString());
	Index += FString::Printf(TEXT("工具集总数: %d, 工具总数: %d\n\n"), ToolsetOrder.Num(), Tools.Num());
	Index += TEXT("## 工具集列表\n\n");
	for (const FString& Ts : ToolsetOrder)
	{
		const TArray<TSharedPtr<FJsonObject>>& TsTools = Grouped[Ts];
		Index += FString::Printf(TEXT("- **%s** (%d 个工具) — 详见 `%s.md`\n"),
			*Ts, TsTools.Num(), *SafeFileName(Ts));
	}
	Index += TEXT("\n## 调用约定\n\n");
	Index += TEXT("使用 `call_tool` 工具调用,参数:\n");
	Index += TEXT("- `toolset_name`: toolset 完整路径(如 `editor_toolset.toolsets.material.MaterialTools`),**不含** tool_name\n");
	Index += TEXT("- `tool_name`: 工具名(如 `CreateMaterial`)\n");
	Index += TEXT("- `arguments`: 工具参数对象,具体 schema 见各 toolset 的 MD 文件\n\n");
	Index += TEXT("**禁止**调用 `list_toolsets` 或 `describe_toolset` — 该缓存已包含全部可用工具的 schema。\n");

	FString IndexPath = ToolboxDir / TEXT("index.md");
	if (!FFileHelper::SaveStringToFile(Index, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Failed to write %s"), *IndexPath);
	}

	// Write one MD per toolset
	int32 WrittenFiles = 1; // index.md already written
	for (const FString& Ts : ToolsetOrder)
	{
		const TArray<TSharedPtr<FJsonObject>>& TsTools = Grouped[Ts];
		FString Md;
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
		FString FilePath = ToolboxDir / FileName;
		if (FFileHelper::SaveStringToFile(Md, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			WrittenFiles++;
		}
		else
		{
			UE_LOG(LogMCPToolbox, Warning, TEXT("[Chat] Failed to write %s"), *FilePath);
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] WriteToolsetCacheToDisk: wrote %d files to %s"),
		WrittenFiles, *ToolboxDir);
}

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
	RefreshEntryList();

	return SNew(SBorder)
		.Padding(FMargin(2))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0, 0, 6, 0))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ModelLabel", "模型:"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]
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
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0, 2, 0, 0))
			[
				SNew(SHorizontalBox)
				// MCP status — static display, no auto-retry
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 12, 0))
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						if (MCPServerClient.IsConnected())
							return LOCTEXT("MCPReady", "MCP就绪");
						if (FMCPToolboxModule::IsMCPServerStarted())
							return LOCTEXT("MCPStarting", "MCP连接中…");
						return LOCTEXT("MCPOffline", "MCP未连接");
					})
					.ToolTipText_Lambda([this]() -> FText
					{
						if (MCPServerClient.IsConnected())
							return LOCTEXT("MCPReadyTip", "MCP 服务器已连接，可调用工具");
						if (FMCPToolboxModule::IsMCPServerStarted())
							return LOCTEXT("MCPStartingTip", "MCP 服务器启动中，正在建立连接");
						return LOCTEXT("MCPOfflineTip", "MCP 服务器未启动，点击'启动MCP'按钮");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						if (MCPServerClient.IsConnected())
							return FLinearColor(0.3f, 0.85f, 0.4f);
						if (FMCPToolboxModule::IsMCPServerStarted())
							return FLinearColor(1.0f, 0.8f, 0.2f);
						return FLinearColor(0.5f, 0.5f, 0.5f);
					})
				]
				// Aux Model status
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(10, 0, 0, 0))
				[
					SNew(STextBlock)
					.Text_Lambda([]() -> FText
					{
						return FMCPToolboxAuxModelManager::Get().IsReady()
							? LOCTEXT("AuxReady", "Aux就绪")
							: LOCTEXT("AuxOffline", "Aux未就绪");
					})
					.ToolTipText_Lambda([]() -> FText
					{
						return FMCPToolboxAuxModelManager::Get().IsReady()
							? LOCTEXT("AuxReadyTip", "本地辅助模型就绪（如 llama-server），可用于归档总结/视觉预处理")
							: LOCTEXT("AuxOfflineTip", "本地辅助模型未就绪，归档总结将回退到主模型");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity_Lambda([]() -> FLinearColor
					{
						return FMCPToolboxAuxModelManager::Get().IsReady()
							? FLinearColor(0.3f, 0.85f, 0.4f)
							: FLinearColor(0.4f, 0.4f, 0.4f);
					})
				]
				// AI status
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						if (bIsWaiting)
							return LOCTEXT("AIThinking", "AI…");
						return LOCTEXT("AIIdle", "AI");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						return bIsWaiting ? FLinearColor(1.0f, 0.8f, 0.2f) : FLinearColor(0.5f, 0.5f, 0.5f);
					})
				]
				// Spacer pushes action buttons to the right edge
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SBox)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						return bVisionModeEnabled ? FLinearColor(0.2f, 0.45f, 0.65f, 1.0f) : FLinearColor(0.13f, 0.13f, 0.15f, 1.0f);
					})
					.OnClicked(this, &SMCPToolboxChatWidget::OnToggleVisionMode)
				.ContentPadding(FMargin(6, 2))
				.ToolTipText(LOCTEXT("VisionTooltip", "切换视觉模式: 启用后可处理图片输入"))
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
				// ── "更多" toggle: expands/collapses the secondary action buttons (Sidebar/RefreshTool) ──
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(6, 0, 0, 0))
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
				// ── Sidebar toggle (visible only when "更多" is expanded) ──
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(6, 0, 0, 0))
				[
					SNew(SButton)
					.Visibility_Lambda([this]() -> EVisibility
					{
						return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ButtonColorAndOpacity(FLinearColor(0.13f, 0.13f, 0.15f, 1.0f))
					.ContentPadding(FMargin(6, 2))
					.OnClicked(this, &SMCPToolboxChatWidget::OnToggleSidebar)
					.ToolTipText(LOCTEXT("SidebarTooltip", "显示/隐藏会话历史侧边栏"))
					.Content()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							return bSidebarCollapsed ? LOCTEXT("SidebarShow", "显示侧栏") : LOCTEXT("SidebarHide", "隐藏侧栏");
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
					]
				]
				// ── Refresh MCP Toolset Cache: discovers all toolsets and writes them to <ProjectDir>/.mcptoolbox/*.md ──
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
				[
					SNew(SButton)
					.Visibility_Lambda([this]() -> EVisibility
					{
						return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ButtonColorAndOpacity(FLinearColor(0.15f, 0.28f, 0.18f, 1.0f))
					.ContentPadding(FMargin(6, 2))
					.OnClicked(this, &SMCPToolboxChatWidget::OnRefreshToolCache)
					.ToolTipText(LOCTEXT("RefreshToolCacheTooltip", "感知当前项目启用的所有 MCP 工具集，缓存为 .mcptoolbox/*.md 供 AI 直接调用，省去询问 list_toolsets/describe_toolset 的时间"))
					.Content()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("RefreshToolCacheBtn", "刷新工具"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FLinearColor(0.85f, 0.95f, 0.85f))
					]
				]
				// ── Archive Summary: generate a conversation summary and store it to ~/.mcptoolbox/ ──
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
				[
					SNew(SButton)
					.Visibility_Lambda([this]() -> EVisibility
					{
						return bMoreExpanded ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ButtonColorAndOpacity(FLinearColor(0.25f, 0.20f, 0.35f, 1.0f))
					.ContentPadding(FMargin(6, 2))
					.OnClicked(this, &SMCPToolboxChatWidget::OnArchiveSummary)
					.ToolTipText(LOCTEXT("ArchiveSummaryTooltip", "总结当前会话并归档到 ~/.mcptoolbox/。下次启动自动加载到系统提示词，避免每次重复说明上下文"))
					.Content()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ArchiveSummaryBtn", "归档总结"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FLinearColor(0.90f, 0.85f, 0.95f))
					]
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

			// 解析参数依赖
			TSharedPtr<FJsonObject> ResolvedParams = MakeShareable(new FJsonObject());
			if (Task.Parameters.IsValid())
			{
				for (const auto& ParamPair : Task.Parameters->Values)
				{
					const FString Key(ParamPair.Key);
					const TSharedPtr<FJsonValue>& Val = ParamPair.Value;

					if (Val->Type == EJson::String)
					{
						FString StrVal = Val->AsString();
						if (StrVal.StartsWith(TEXT("$")))
						{
							FString Ref = StrVal.Mid(1);
							FString RefTaskId = Ref;
							int32 DotIdx;
							if (Ref.FindChar(TEXT('.'), DotIdx))
								RefTaskId = Ref.Left(DotIdx);

							const FTaskExecutionResult* DepResult = AllResults->Find(RefTaskId);
							if (DepResult)
							{
								ResolvedParams->SetStringField(Key, DepResult->ResultJson);
								continue;
							}
						}
					}
					ResolvedParams->SetField(Key, Val);
				}
			}

			FString ArgsJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
			FJsonSerializer::Serialize(ResolvedParams.ToSharedRef(), Writer);

			double StartTime = FDateTime::Now().ToUnixTimestamp() * 1000.0;

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
	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Widget state loaded: vision=%d sidebar=%d more=%d summary_declined=%d auto_archive=%d"),
		(int32)bVisionModeEnabled, (int32)bSidebarCollapsed, (int32)bMoreExpanded, (int32)bSummaryDeclined, (int32)bAutoArchiveEnabled);
}

void SMCPToolboxChatWidget::SaveWidgetState() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetBoolField(TEXT("vision_enabled"), bVisionModeEnabled);
	Json->SetBoolField(TEXT("sidebar_collapsed"), bSidebarCollapsed);
	Json->SetBoolField(TEXT("more_expanded"), bMoreExpanded);
	Json->SetBoolField(TEXT("summary_declined"), bSummaryDeclined);
	Json->SetBoolField(TEXT("auto_archive_enabled"), bAutoArchiveEnabled);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	FString Path = GetWidgetStatePath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Output, *Path);
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

#undef LOCTEXT_NAMESPACE
