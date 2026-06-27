#pragma once

#include <memory>
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWindow.h"
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

// DAG Parallel Execution
#include "MCPToolboxDAGTypes.h"
#include "MCPToolboxExecutionPlanner.h"

// Auxiliary Model Manager (IdleSpec + SWE-Pruner)
#include "MCPToolboxAuxModelManager.h"

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
	
	/** Create a message bubble widget and optionally return the content text box for streaming updates */
	TSharedRef<SWidget> CreateMessageBubble(const FMCPToolboxChatMessage& Message, TSharedPtr<SMultiLineEditableTextBox>& OutTextBlock);

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

	/** Toggle sidebar visibility */
	FReply OnToggleSidebar();

	/** Handle typing in the input box (Ctrl+Enter detection) */
	FReply OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);

	/** Interrupt the current AI request */
	FReply OnInterrupt();

	/** Optimize the current prompt using auxiliary model (local LLM) */
	FReply OnOptimizePrompt();

	/** Revert optimized prompt back to original text */
	FReply OnUndoOptimization();

	/** Refresh MCP toolset cache: discovers all toolsets via list_toolsets + describe_toolset, writes them to <ProjectDir>/.mcptoolbox/*.md */
	FReply OnRefreshToolCache();

	/** Write discovered tools to .mcptoolbox/ directory as multiple MD files (one per toolset) */
	void WriteToolsetCacheToDisk(const TArray<TSharedPtr<FJsonObject>>& Tools);

	// ---- Conversation Summary (Archive) ----

	/** Summary model strategy for the OnArchiveSummary dialog */
	enum class ESummaryModelChoice : uint8
	{
		LocalFirst,   // 优先本地辅助模型（免费、快），失败回退主模型
		MainModel,    // 直接用当前选中的主模型（质量好，消耗 API 额度）
		Hybrid        // 工具归档用本地（量大），记忆归档用主模型（要求质量）
	};

	/** Button handler: open the summary choice dialog (first-time shows advantages) */
	FReply OnArchiveSummary();

	/** Show the summary choice dialog (advantages + model selection + archive type checkboxes) */
	void ShowSummaryChoiceDialog();

	/** Generate the summary using the chosen model strategy.
	 *  bArchiveTools / bArchiveMemory select which sections to produce. */
	void GenerateSummary(ESummaryModelChoice ModelChoice, bool bArchiveTools, bool bArchiveMemory);

	/** Called on the game thread when the summary is ready (or failed). */
	void OnSummaryGenerated(bool bSuccess, const FString& ToolsSummary, const FString& MemorySummary);

	/** Build a "give me a summary" prompt from the current Messages array. */
	FString BuildSummaryPrompt(bool bForTools, bool bForMemory) const;

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

	/** Internal: send AI request after pruning (or directly) */
	void SendAIRequestInternal(const TArray<TSharedPtr<FJsonValue>>& ApiMessages);

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

	// ---- DAG Parallel Execution ----

	/** Execute tool calls with DAG-based parallel scheduling */
	void ExecuteToolCallsDAG(
		const TArray<TSharedPtr<FJsonValue>>& ToolCalls,
		const TArray<TSharedPtr<FJsonValue>>& SentMessages,
		const TSharedPtr<FJsonObject>& AssistantMsg
	);

	/** Check if tool calls contain DAG dependencies (for LLMCompiler-style planning) */
	bool HasDAGDependencies(const TArray<TSharedPtr<FJsonValue>>& ToolCalls) const;

	/** Convert OpenAI-style tool_calls to DAG task format */
	void ConvertToolCallsToDAGFormat(
		const TArray<TSharedPtr<FJsonValue>>& ToolCalls,
		TArray<TSharedPtr<FJsonObject>>& OutDAGCalls
	) const;

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

	/** Cached JSON representations of Messages (built once, reused in OnSendMessage) */
	TArray<TSharedPtr<FJsonValue>> CachedMessagesJson;

	/** Cached MCP tool descriptions as Markdown — injected into system prompt to avoid repeated queries */
	FString CachedMCPToolDescriptionsMD;

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

	/** Multi-line editable text box for the streaming message (to update in place, supports copy) */
	TSharedPtr<SMultiLineEditableTextBox> StreamingTextBlock;

	/** Multi-line editable text box for the streaming message container (supports copy) */
	TSharedPtr<SMultiLineEditableTextBox> StreamingMessageBox;

	// ---- Prompt Optimization ----

	/** Undo buffer: stores original prompt text before optimization */
	FString UndoBuffer;

	/** Whether the current input text has been optimized (to show "退回" button) */
	bool bPromptOptimized = false;

	/** Whether optimization is in progress */
	bool bOptimizingPrompt = false;

	// ---- File Upload ----

	/** Pending image URIs from OnUploadFile — attached to the next user message and cleared after send.
	 *  This avoids the previous design where OnUploadFile called AddMessage(FileMsg) then OnSendMessage(),
	 *  which either sent a duplicate user message or (if input was empty) skipped sending entirely. */
	TArray<FString> PendingUploadURIs;

	/** Local FunctionTable for tool registration */
	TUniquePtr<assistant::FunctionTable> ToolFunctionTable;
	mutable TArray<TSharedPtr<FJsonValue>> CachedToolsArray; // Built once, reused per request

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

	/** Whether the "more actions" secondary toolbar row is expanded */
	bool bMoreExpanded = false;

	// ---- Conversation Summary State ----

	/** True if user previously declined the first-time summary dialog (persisted) */
	bool bSummaryDeclined = false;

	/** Cached conversation summary sections loaded at startup (injected into BuildSystemPrompt) */
	FString CachedToolsSummary;
	FString CachedMemorySummary;

	/** Weak reference to the summary dialog window (prevent duplicate open) */
	TWeakPtr<SWindow> SummaryDialogWindow;

	// ---- Widget State Persistence ----

	/** Get path to widget state file (~/.mcptoolbox/widget_state.json) */
	FString GetWidgetStatePath() const;

	/** Load bVisionModeEnabled / bSidebarCollapsed / bMoreExpanded from disk */
	void LoadWidgetState();

	/** Persist bVisionModeEnabled / bSidebarCollapsed / bMoreExpanded to disk */
	void SaveWidgetState() const;

	// ---- DAG Parallel Execution ----

	/** DAG execution planner for parallel tool calls */
	FExecutionPlanner ExecutionPlanner;

	// ---- Auxiliary Model Integration ----

	/** Current speculative execution result (IdleSpec) */
	FSpeculativeResult LastSpeculation;
	bool bPendingToolCompletion = false;
	TSharedPtr<TArray<TSharedPtr<FJsonValue>>> DeferredPendingMsgs;
	FString PendingSpecToolName;
	bool bSpeculationPending = false;

	/** Previous conversation context (kept for speculative validation rollback) */
	TArray<TSharedPtr<FJsonValue>> PreSpeculationMessages;

	/** Launch IdleSpec speculation for the given tool name */
	void LaunchIdleSpec(const FString& CurrentToolName);

	/** Try to auto-execute the speculated tool call, skipping an LLM round.
	 *  @return true if speculation was executed (caller skips SendAIRequest) */
	bool TrySpeculativeExecution(TArray<TSharedPtr<FJsonValue>>& PendingMsgs);

	/** Called when both tool and speculation are complete. Tries speculation, falls back to LLM. */
	void TrySpeculativeOrContinue(TSharedPtr<TArray<TSharedPtr<FJsonValue>>> PendingMsgs);

	/** Preprocess images in messages through local VL model (when vision mode is OFF but aux VL available) */
	void PreprocessImagesLocally(const TArray<TSharedPtr<FJsonValue>>& Msgs,
		TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnDone);

	/** Apply SWE-Pruner to messages before sending to AI */
	void ApplyPruningBeforeSend(
		const TArray<TSharedPtr<FJsonValue>>& ApiMessages,
		TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnPruned);
};
