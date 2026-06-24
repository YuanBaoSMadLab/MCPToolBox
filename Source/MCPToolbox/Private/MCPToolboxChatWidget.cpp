#include "MCPToolboxChatWidget.h"
#include "MCPToolbox.h"
#include "MCPToolboxAPIManager.h"
#include "MCPToolboxMemoryManager.h"

// Assistant library — only FunctionBuilder/FunctionTable (no HTTP layer)
#include "assistant/function.hpp"

#include "SlateCore.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Base64.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "EngineUtils.h"
#include "Engine/Selection.h"
#include "HttpModule.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "MCPToolboxChat"

// ============================================================================
// Construction
// ============================================================================
void SMCPToolboxChatWidget::Construct(const FArguments& InArgs)
{
	Messages.Empty();
	bVisionModeEnabled = false;
	bIsStreaming = false;
	bIsWaiting = false;

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

	TSharedRef<SWidget> CollapseBtn = SNew(SButton)
		.Text(LOCTEXT("CollapseSidebar", ">>"))
		.OnClicked(this, &SMCPToolboxChatWidget::ToggleSidebar)
		.ButtonColorAndOpacity(FLinearColor(0.15f, 0.15f, 0.18f));

	return SNew(SBox)
		.WidthOverride(220.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(4))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.0)
				[
					NewChatBtn
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
				[
					CollapseBtn
				]
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
	FLinearColor BgColor = GetMessageColor(Message.Role);
	FString RoleLabel;
	bool bAlignRight = false;

	switch (Message.Role)
	{
	case EMCPToolboxMessageRole::User:      RoleLabel = TEXT("你"); bAlignRight = true; break;
	case EMCPToolboxMessageRole::Assistant:  RoleLabel = TEXT("AI助手"); break;
	case EMCPToolboxMessageRole::System:     RoleLabel = TEXT("系统"); break;
	case EMCPToolboxMessageRole::Thinking:   RoleLabel = TEXT("⚙ 思考中"); break;
	}

	// Clean markdown for display
	FString DisplayContent = CleanMarkdownToText(Message.Content);

	// --- Tool execution card (Thinking or System with result) ---
	if (Message.Role == EMCPToolboxMessageRole::Thinking || 
		(Message.Role == EMCPToolboxMessageRole::System && Message.Content.Contains(TEXT("结果:"))))
	{
		bool bIsResult = Message.Role == EMCPToolboxMessageRole::System;
		if (DisplayContent.Len() > 500) DisplayContent = DisplayContent.Left(500) + TEXT("...");

		return SNew(SBox)
			.MaxDesiredWidth(700)
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
							.Text(FText::FromString(bIsResult ? TEXT("✓") : TEXT("⚙")))
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
						SNew(SMultiLineEditableTextBox)
						.Text(FText::FromString(DisplayContent))
						.IsReadOnly(true)
						.BackgroundColor(FLinearColor::Transparent)
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.AutoWrapText(true)
					]
				]
			];
	}

	return SNew(SBox)
		.HAlign(bAlignRight ? HAlign_Right : HAlign_Left)
		.MaxDesiredWidth(700)
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
					SNew(SMultiLineEditableTextBox)
					.Text(FText::FromString(DisplayContent))
					.IsReadOnly(true)
					.BackgroundColor(FLinearColor::Transparent)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.AutoWrapText(true)
				]
			]
		];
}

// ============================================================================
// Input Area
// ============================================================================
TSharedRef<SWidget> SMCPToolboxChatWidget::BuildInputArea()
{
	return SNew(SBorder).Padding(FMargin(4))
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
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
			[
				SNew(SButton)
				.Text(LOCTEXT("UploadBtn", "上传"))
				.OnClicked(this, &SMCPToolboxChatWidget::OnUploadFile)
				.IsEnabled_Lambda([this]() { return bVisionModeEnabled; })
				.ToolTipText_Lambda([this]() {
					return bVisionModeEnabled ? LOCTEXT("UploadTooltip", "上传图片") : LOCTEXT("UploadDisabledTooltip", "请先开启视觉模式以使用图片上传功能");
				})
				.ButtonColorAndOpacity_Lambda([this]() {
					return bVisionModeEnabled ? FLinearColor::White : FLinearColor(0.3f, 0.3f, 0.3f);
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4, 0, 0, 0))
			[
				SNew(SButton).Text(LOCTEXT("ClearBtn", "清空"))
				.OnClicked(this, &SMCPToolboxChatWidget::OnClearChat)
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
	if (ApiKey.IsEmpty())
	{
		FMCPToolboxChatMessage Err;
		Err.Role = EMCPToolboxMessageRole::System;
		Err.Content = TEXT("API密钥为空，请重新添加密钥。");
		AddMessage(Err);
		return FReply::Handled();
	}

	// Add user message to chat
	FMCPToolboxChatMessage UserMsg;
	UserMsg.Role = EMCPToolboxMessageRole::User;
	UserMsg.Content = UserText;
	AddMessage(UserMsg);
	InputTextBox->SetText(FText::GetEmpty());

	bIsWaiting = true;
	bInterrupted = false;
	ToolCallIteration = 0; // Reset iteration counter for new message
	ToolCallHistory.Empty();
	ConsecutiveSameToolCount = 0;

	// Build messages array for API — PRESERVE conversation history!
	TArray<TSharedPtr<FJsonValue>> Msgs;

	// Always fresh system prompt (memory may have been updated)
	FString MemoryCtx = FMCPToolboxMemoryManager::Get().BuildMemoryContext();
	FString SystemPrompt = BuildSystemPrompt(MemoryCtx);
	TSharedPtr<FJsonObject> SysMsg = MakeShareable(new FJsonObject());
	SysMsg->SetStringField(TEXT("role"), TEXT("system"));
	SysMsg->SetStringField(TEXT("content"), SystemPrompt);
	Msgs.Add(MakeShareable(new FJsonValueObject(SysMsg)));

	// Append conversation history from local Messages array
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
		default: RoleStr = TEXT("user");
		}
		MsgObj->SetStringField(TEXT("role"), RoleStr);
		MsgObj->SetStringField(TEXT("content"), Msg.Content);
		Msgs.Add(MakeShareable(new FJsonValueObject(MsgObj)));
	}

	// Add new user message (the one just entered)
	TSharedPtr<FJsonObject> UsrMsg = MakeShareable(new FJsonObject());
	UsrMsg->SetStringField(TEXT("role"), TEXT("user"));
	UsrMsg->SetStringField(TEXT("content"), UserText);
	Msgs.Add(MakeShareable(new FJsonValueObject(UsrMsg)));

	SendAIRequest(Msgs);
	return FReply::Handled();
}

// ============================================================================
// Send AI Request — reusable, called from OnSendMessage and tool loops
// ============================================================================
void SMCPToolboxChatWidget::SendAIRequest(const TArray<TSharedPtr<FJsonValue>>& ApiMessages)
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

	// Build request body
	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject());
	Body->SetStringField(TEXT("model"), ActiveEntry->ModelId);
	Body->SetBoolField(TEXT("stream"), true); // Enable SSE streaming
	Body->SetArrayField(TEXT("messages"), ApiMessages);

	// Add tools
	if (ToolFunctionTable.IsValid() && ToolFunctionTable->GetFunctionsCount() > 0)
	{
		assistant::json ToolsJson = ToolFunctionTable->ToJSON(assistant::EndpointKind::ollama, assistant::CachePolicy::kNone);
		TArray<TSharedPtr<FJsonValue>> ToolsArray;
		for (const auto& tool : ToolsJson)
		{
			FString ToolStr = UTF8_TO_TCHAR(tool.dump().c_str());
			TSharedPtr<FJsonObject> ToolObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolStr);
			if (FJsonSerializer::Deserialize(Reader, ToolObj) && ToolObj.IsValid())
				ToolsArray.Add(MakeShareable(new FJsonValueObject(ToolObj)));
		}
		if (ToolsArray.Num() > 0)
			Body->SetArrayField(TEXT("tools"), ToolsArray);
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
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
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

		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] SSE: content=%d chars(%d chunks), tool_calls=%d (deduped from map), finish=%s"),
			AccumulatedContent.Len(), TextChunks.Num(), AccumulatedToolCalls.Num(), *FinishReason);

		// Progressive display for text-only responses (no tool_calls)
		if (AccumulatedToolCalls.Num() == 0 && TextChunks.Num() > 0)
		{
			// Create streaming placeholder
			FMCPToolboxChatMessage StreamMsg;
			StreamMsg.Role = EMCPToolboxMessageRole::Assistant;
			StreamMsg.Content = TEXT("");
			StreamMsg.bIsStreaming = true;
			AddMessage(StreamMsg);
			FMCPToolboxChatMessage* StreamPtr = &Messages.Last();

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
						UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Stream done, %d chars"), DisplayBuffer->Len());
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

					// Only rebuild every batch (~150ms between updates)
					RebuildChatDisplay();

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
		ToolMsg.Content = FString::Printf(TEXT("⚙ 调用工具: %s"), *ToolNames);
		AddMessage(ToolMsg);

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
			bool bIsMCP = false;
			if (MCPServerClient.IsConnected())
			{
				for (const auto& T : MCPServerClient.GetTools())
				{
					FString N;
					if (T->TryGetStringField(TEXT("name"), N) && N == FuncName) { bIsMCP = true; break; }
				}
			}

			if (bIsMCP)
			{
				// MCP tool: execute asynchronously
				(*PendingMCP)++;
				FString NameCap = FuncName;
				FString IdCap = TCId;

				MCPServerClient.ExecuteTool(FuncName, FuncArgs,
					[this, PendingMCP, PendingMsgs, NameCap, IdCap](bool bOk, const FString& R)
					{
						// Build tool result message
						TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject());
						ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
						ToolResultMsg->SetStringField(TEXT("tool_call_id"), IdCap);
						ToolResultMsg->SetStringField(TEXT("name"), NameCap);
						ToolResultMsg->SetStringField(TEXT("content"), R);
						PendingMsgs->Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

						// Show result
						FMCPToolboxChatMessage ResultMsg;
						ResultMsg.Role = EMCPToolboxMessageRole::System;
						ResultMsg.Content = FString::Printf(TEXT("**%s** 结果:\n```\n%s\n```"), *NameCap, *R.Left(500));
						AddMessage(ResultMsg);

						(*PendingMCP)--;

						// All MCP calls done → continue conversation
						if (*PendingMCP <= 0)
						{
							UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] 工具调用完成(MCP), 继续对话"));
							SendAIRequest(*PendingMsgs);
						}
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
			}

			MergeMCPTools();
		});
}

void SMCPToolboxChatWidget::MergeMCPTools()
{
	if (!ToolFunctionTable.IsValid()) return;

	const TArray<TSharedPtr<FJsonObject>>& McpTools = MCPServerClient.GetTools();
	for (const auto& ToolDef : McpTools)
	{
		FString ToolName;
		ToolDef->TryGetStringField(TEXT("name"), ToolName);
		if (ToolName.IsEmpty()) continue;

		FString ToolDesc;
		ToolDef->TryGetStringField(TEXT("description"), ToolDesc);

		// MCP tools are now dispatched directly in HandleAIResponse (fully async).
		// This FunctionTable callback provides schema info only.
		ToolFunctionTable->Add(assistant::FunctionBuilder(TCHAR_TO_UTF8(*ToolName))
			.SetDescription(TCHAR_TO_UTF8(*ToolDesc))
			.SetCallback([](const assistant::json& Args) -> assistant::FunctionResult
			{
				return assistant::FunctionResult{
					.isError = false,
					.text = R"({"status":"dispatched"})"
				};
			}).Build());

		UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Registered MCP tool: %s"), *ToolName);
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Total tools after MCP merge: %d"), ToolFunctionTable->GetFunctionsCount());
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
	// Get project Content path
	FString ContentPath = FPaths::ProjectContentDir();
	FString ProjectName = FApp::GetProjectName();

	FString Prompt;
	Prompt += TEXT("你是 MCP Toolbox AI助手，运行在 Unreal Engine 5.8 编辑器内部。必须用中文回复。\n\n");
	Prompt += FString::Printf(TEXT("## 工作环境\n"));
	Prompt += FString::Printf(TEXT("- 项目: %s\n"), *ProjectName);
	Prompt += FString::Printf(TEXT("- Content目录: %s\n"), *ContentPath);
	Prompt += FString::Printf(TEXT("- 资源路径前缀: /Game/ (对应Content目录)\n\n"));

	Prompt += TEXT("## 核心规则（必须100%遵守！违反规则=失败）\n");
	Prompt += TEXT("1. **绝不空口承诺**：禁止说\"我来帮你\"\"让我看看\"等文字而不调用工具。说一次执行一次\n");
	Prompt += TEXT("2. **先调用工具再说话**：收到指令→立即调用工具→拿到结果→回复用户。中间不允许纯文字\n");
	Prompt += TEXT("3. **非工具不可**：创建材质/执行命令/截图/选择Actor—这些操作必须且只能通过工具完成\n");
	Prompt += TEXT("4. **工具参数必填且正确**：路径加/Game/前缀，命令写完整\n");
	Prompt += TEXT("5. **🚫 严禁 Python**：绝对禁止用 command(cmd=\"py ...\") 创建资产、材质、蓝图、PCG等编辑器操作。这些操作容易出错且不稳定。必须通过MCP工具完成。违者=失败！\n");
	Prompt += TEXT("6. **MCP是唯一正途**：本插件的核心功能就是MCP集成。所有UE编辑器操作必须通过 list_toolsets → describe_toolset → call_tool 链路完成。\n");
	Prompt += TEXT("7. **command仅限控制台命令**：command只用于 HighResShot、stat fps 等纯控制台命令，绝不用于Python脚本。\n");
	Prompt += TEXT("8. **主动记忆**：每次对话后，用\"记住：xxx\"保存MCP工具使用经验和用户偏好。\n\n");

	Prompt += TEXT("## 工具优先级（从高到低，严格按此顺序）\n");
	Prompt += TEXT("1. **MCP工具**：list_toolsets → describe_toolset → call_tool（唯一正途！）\n");
	Prompt += TEXT("2. **本地工具**：screenshot, select, inspect\n");
	Prompt += TEXT("3. **command**：仅限控制台命令(HighResShot, stat等)，🚫禁止py脚本\n\n");

	Prompt += TEXT("## 工具列表\n");
	Prompt += TEXT("### MCP工具（必须优先使用）\n");
	Prompt += TEXT("- **list_toolsets** — 列出所有可用工具集。MCP连接后第一时间调用。\n");
	Prompt += TEXT("- **describe_toolset** — 查看工具集的工具列表和参数。参数: toolset_name (string)\n");
	Prompt += TEXT("- **call_tool** — 调用MCP工具。参数: toolset_name (string), tool_name (string), arguments (object)\n");
	Prompt += TEXT("### 本地辅助工具\n");
	Prompt += TEXT("- **screenshot** — 截图\n");
	Prompt += TEXT("- **select** — 选择Actor。参数: name (string)\n");
	Prompt += TEXT("- **inspect** — 检查选中Actor属性\n");
	Prompt += TEXT("### 禁止使用（除非MCP完全不可用）\n");
	Prompt += TEXT("- **command** — 🚫 禁止用于py脚本。仅限纯控制台命令。\n\n");

	Prompt += TEXT("## 行为示例（正确 vs 错误）\n");
	Prompt += TEXT("用户: 帮我创建自发光白色材质\n");
	Prompt += TEXT("❌ 错误: 直接用 command(cmd=\"py ...\") (绕开MCP)\n");
	Prompt += TEXT("✅ 正确: 先调 list_toolsets → 找到材质相关工具集 → describe_toolset → call_tool 创建材质\n");
	Prompt += TEXT("✅ 正确(MCP不可用时): command(cmd=\"py ...创建自发光材质...\")\n\n");

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
		.SetDescription("Capture a screenshot of the entire Unreal Engine editor window. Requires vision mode to be enabled.")
		.AddOptionalParam("mode", "Capture mode: 'viewport' for viewport only, 'editor' for full editor window", "string")
		.SetCallback([this](const assistant::json& args) -> assistant::FunctionResult {
			if (!bVisionModeEnabled)
			{
				return {.isError = true, .text = R"({"error":"Vision mode is not enabled. Please enable vision mode first."})"};
			}
			
			std::string Mode = args.value("mode", "editor");
			if (GEditor)
			{
				if (Mode == "viewport")
				{
					GEditor->Exec(nullptr, TEXT("HighResShot 1920x1080"));
				}
				else
				{
					GEditor->Exec(nullptr, TEXT("HighResShot"));
				}
				return {.isError = false, .text = R"({"status":"ok","message":"Screenshot saved to Saved/Screenshots/"})"};
			}
			return {.isError = true, .text = R"({"error":"Editor not available"})"};
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
		FMCPToolboxChatMessage FileMsg;
		FileMsg.Role = EMCPToolboxMessageRole::User;
		FileMsg.Content = TEXT("上传了文件：\n");
		for (const FString& F : Files)
		{
			FileMsg.Content += FPaths::GetCleanFilename(F) + TEXT("\n");
			FString URI = EncodeFileToDataURI(F);
			if (!URI.IsEmpty()) { FileMsg.ImageDataURIs.Add(URI); FileMsg.bHasImageAttachment = true; }
		}
		AddMessage(FileMsg);
		OnSendMessage(); // trigger AI
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
// Stubs & helpers
// ============================================================================
FReply SMCPToolboxChatWidget::OnToggleVisionMode()
{
	bVisionModeEnabled = !bVisionModeEnabled;
	
	FNotificationInfo Info(bVisionModeEnabled ? LOCTEXT("VisionModeEnabled", "视觉模式已开启 - 截图和图片上传功能可用") : LOCTEXT("VisionModeDisabled", "视觉模式已关闭"));
	Info.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	
	UE_LOG(LogMCPToolbox, Log, TEXT("[Chat] Vision mode %s"), bVisionModeEnabled ? TEXT("enabled") : TEXT("disabled"));
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
							return LOCTEXT("MCPReady", "MCP: 已连接");
						if (FMCPToolboxModule::IsMCPServerStarted())
							return LOCTEXT("MCPStarting", "MCP: 连接中...");
						return LOCTEXT("MCPOffline", "MCP: 未启动");
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
				// AI status
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						if (bIsWaiting)
							return LOCTEXT("AIThinking", "AI: 思考中...");
						return LOCTEXT("AIIdle", "AI: 就绪");
					})
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						return bIsWaiting ? FLinearColor(1.0f, 0.8f, 0.2f) : FLinearColor(0.5f, 0.5f, 0.5f);
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12, 0, 0, 0))
				[
					SNew(SButton)
					.ButtonColorAndOpacity_Lambda([this]() -> FLinearColor
					{
						return bVisionModeEnabled ? FLinearColor(0.2f, 0.4f, 0.6f) : FLinearColor(0.15f, 0.15f, 0.18f);
					})
					.OnClicked(this, &SMCPToolboxChatWidget::OnToggleVisionMode)
					.Content()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							return bVisionModeEnabled ? LOCTEXT("VisionEnabled", "👁 视觉模式") : LOCTEXT("VisionDisabled", "视觉: 关闭");
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
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

FReply SMCPToolboxChatWidget::ToggleSidebar()
{
	bSidebarCollapsed = !bSidebarCollapsed;
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
