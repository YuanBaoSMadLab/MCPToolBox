#pragma once

#include <memory>
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "MCPToolboxAPIManager.h"
#include "Http.h"
#include "Interfaces/IHttpRequest.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "IDesktopPlatform.h"
#include "DesktopPlatformModule.h"

// Assistant FunctionTable (needs full definition for TUniquePtr)
#include "assistant/function.hpp"

// MCP Server Client
#include "MCPToolboxMCPServerClient.h"
#include "MCPToolboxChatMessage.h"
#include "MCPToolboxChatSession.h"

// ---- Forward declarations ----
class FMCPToolboxMCPClient;
struct FMCPToolboxChatSession;

// ============================================================================
// SMCPToolboxChatWidget - Chat UI Slate Widget
// ============================================================================

/**
 * Main chat window widget for MCPToolbox.
 * Provides a full chat interface with provider/model selection,
 * message display, file upload, and streaming response support.
 */
class MCPTOOLBOX_API SMCPToolboxChatWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPToolboxChatWidget) {}
	SLATE_END_ARGS()

	/** Construct the widget */
	void Construct(const FArguments& InArgs);

	/** Add a message to the chat display */
	void AddMessage(const FMCPToolboxChatMessage& Message);

	/** Clear all messages from the chat */
	void ClearMessages();

	/** Set the current provider by ID */
	void SetCurrentProvider(const FString& ProviderId);

	/** Get the currently selected provider ID */
	FString GetCurrentProviderId() const;

	/** Get the currently selected model ID */
	FString GetCurrentModelId() const;

private:
	// ---- UI Construction Methods ----

	/** Build the toolbar (provider selector, model selector, buttons) */
	TSharedRef<SWidget> BuildToolbar();

	/** Build the session sidebar */
	TSharedRef<SWidget> BuildSessionSidebar();

	/** Build the chat message display area */
	TSharedRef<SScrollBox> BuildChatArea();

	/** Rebuild the chat display from Messages array */
	void RebuildChatDisplay();

	/** Build the message input area */
	TSharedRef<SWidget> BuildInputArea();

	/** Create a message bubble widget for the given message */
	TSharedRef<SWidget> CreateMessageBubble(const FMCPToolboxChatMessage& Message);

	/** Generate a row for the session list view */
	TSharedRef<ITableRow> GenerateSessionRow(TSharedPtr<FString> SessionId, const TSharedRef<STableViewBase>& OwnerTable);

	// ---- Session Management ----

	/** Create a new chat session */
	FReply OnNewChat();

	/** Refresh the session list */
	void RefreshSessionList();

	/** Handle session selection */
	void OnSessionSelected(TSharedPtr<FString> SessionId, ESelectInfo::Type SelectInfo);

	/** Handle session deletion */
	void OnDeleteSession(const FString& SessionId);

	/** Switch to a specific session */
	void SwitchToSession(const FString& SessionId);

	/** Update the current session with a new message */
	void UpdateCurrentSessionWithMessage(const FMCPToolboxChatMessage& Message);

	// ---- User Actions ----

	/** Send the current input message */
	FReply OnSendMessage();

	/** Handle Ctrl+V for image paste detection */
	void OnInputTextPasted(const FText& PastedText);

	/** Open file dialog for image/file upload */
	FReply OnUploadFile();

	/** Clear all messages */
	FReply OnClearChat();

	/** Toggle vision/image mode */
	FReply OnToggleVisionMode();

	/** Handle typing in the input box (Ctrl+Enter detection) */
	FReply OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);

	/** Interrupt the current AI request */
	FReply OnInterrupt();

	// ---- Provider/Model Selection ----

	/** Rebuild entry list from APIManager saved entries */
	void RefreshEntryList();

	/** Called when an entry is selected */
	void OnEntrySelected(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);

	/** Generate row widget for entry combo */
	TSharedRef<SWidget> GenerateEntryRow(TSharedPtr<FString> InItem);

	/** Rebuild provider list from APIManager */
	void RefreshProviderList();

	/** Called when a provider is selected */
	void OnProviderSelected(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);

	/** Rebuild the model list for the current provider */
	void RefreshModelList();

	/** Called when a model is selected */
	void OnModelSelected(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);

	/** Generate row widget for provider combo */
	TSharedRef<SWidget> GenerateProviderRow(TSharedPtr<FString> InItem);

	/** Generate row widget for model combo */
	TSharedRef<SWidget> GenerateModelRow(TSharedPtr<FString> InItem);

	// ---- Communication ----

	/** Send an AI request with the given messages array */
	void SendAIRequest(const TArray<TSharedPtr<FJsonValue>>& ApiMessages);

	/** Handle the AI response and tool call loop */
	void HandleAIResponse(FHttpResponsePtr Resp, const TArray<TSharedPtr<FJsonValue>>& SentMessages);

	/** Execute a tool call and return result text */
	FString ExecuteToolCall(const FString& ToolName, const FString& ArgsJson);

	// ---- Clipboard Handling ----

	/** Check if clipboard contains image data */
	bool HasImageInClipboard() const;

	/** Paste image from clipboard as base64 data URI */
	FString GetClipboardImageAsDataURI() const;

	/** Send tool result back to AI for analysis */
	void SendToolResultToAI(const FString& ToolName, const FString& ToolResult);

	/** Handle MCP tool call extracted from AI response */
	void HandleMCPToolCall(const FString& ToolCallJson);

	/** Build system prompt with memory context */
	FString BuildSystemPrompt(const FString& MemoryContext);

	/** Register MCP tools in the local FunctionTable */
	void RegisterMCPTools();

	/** Get the local FunctionTable for building requests */
	assistant::FunctionTable& GetFunctionTable();

public:
	/** Try connecting to MCP server and discovering tools */
	void ConnectToMCPServer();

	/** Refresh tool list from MCP server */
	void RefreshMCPTools();

private:

	// ---- Helpers ----

	/** Get the appropriate color for a message role */
	FLinearColor GetMessageColor(EMCPToolboxMessageRole Role) const;

	/** Encode a file to base64 data URI */
	FString EncodeFileToDataURI(const FString& FilePath) const;

	/** Get MIME type from file extension */
	FString GetMimeType(const FString& Extension) const;

	/** Convert markdown text to clean display text */
	static FString CleanMarkdownToText(const FString& Markdown);

	// ---- Data ----

	/** All chat messages */
	TArray<FMCPToolboxChatMessage> Messages;

	/** Scroll box for the chat area (to auto-scroll on new messages) */
	TSharedPtr<SScrollBox> ChatScrollBox;

	/** Input text box */
	TSharedPtr<SMultiLineEditableTextBox> InputTextBox;

	/** Entry selector combo (Provider - Model) */
	TSharedPtr<SComboBox<TSharedPtr<FString>>> EntryComboBox;

	/** Entry options for combo box */
	TArray<TSharedPtr<FString>> EntryOptions;

	/** Currently selected entry display string */
	TSharedPtr<FString> SelectedEntry;

	/** Map from display string → EntryId for quick lookup */
	TMap<FString, FString> EntryDisplayToId;

	/** Whether vision mode is enabled */
	bool bVisionModeEnabled = false;

	/** Whether we are currently receiving a streaming response */
	bool bIsStreaming = false;

	/** Whether we are waiting for any response */
	bool bIsWaiting = false;

	/** Whether the current request was interrupted */
	bool bInterrupted = false;

	/** Tool call iteration counter (reset per user message) */
	int32 ToolCallIteration = 0;

	/** History of tool call names for loop detection */
	TArray<FString> ToolCallHistory;

	/** Consecutive same tool call counter */
	int32 ConsecutiveSameToolCount = 0;

	/** Partial content accumulated during streaming */
	FString StreamingContent;

	/** Reference to the streaming message (updated in place) */
	FMCPToolboxChatMessage* CurrentStreamingMessage = nullptr;

	/** Local FunctionTable for tool registration */
	TUniquePtr<assistant::FunctionTable> ToolFunctionTable;

	/** Active HTTP request (for cancellation) */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveHttpRequest;

	/** MCP Server client — connects to UE's built-in MCP server */
	FMCPToolboxMCPServerClient MCPServerClient;

	/** Merge MCP-discovered tools into FunctionTable */
	void MergeMCPTools();

	// ---- Session Data ----

	/** Session list view */
	TSharedPtr<SListView<TSharedPtr<FString>>> SessionListView;

	/** Session options for the list view */
	TArray<TSharedPtr<FString>> SessionOptions;

	/** Map from session ID to display info */
	TMap<FString, FString> SessionIdToTitle;

	/** Map from session ID to preview text */
	TMap<FString, FString> SessionIdToPreview;

	/** Whether the sidebar is collapsed */
	bool bSidebarCollapsed = false;

	/** Toggle sidebar visibility */
	FReply ToggleSidebar();
};
