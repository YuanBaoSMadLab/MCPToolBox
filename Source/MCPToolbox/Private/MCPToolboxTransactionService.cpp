// ============================================================================
// MCPToolboxTransactionService — 编辑器 Undo/Redo 服务 (Stage 6.1)
// ============================================================================
// 移植自 VibeUE Private/PythonAPI/UTransactionService.cpp
// 改造: 返回 JSON 字符串,对接 MCPToolboxErrorCodes 结构化错误码
// ============================================================================

#include "MCPToolboxTransactionService.h"
#include "MCPToolboxErrorCodes.h"
#include "MCPToolboxServiceRegistry.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// 跟踪最近一次 BeginTransaction 返回的索引(CancelTransaction 用)
	int32 GActiveTransactionIndex = INDEX_NONE;
}

UTransactor* FMCPToolboxTransactionService::GetTransactor()
{
	return GEditor ? GEditor->Trans : nullptr;
}

// ── 辅助函数: 构建 JSON 响应 ──
static FString MakeSuccessJson(const TFunction<void(TSharedPtr<FJsonObject>&)>& FillFields)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	if (FillFields)
	{
		FillFields(Obj);
	}
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}

static FString MakeErrorJson(const FString& ErrorCode, const FString& Message)
{
	return MCPToolboxErrorFormat::FormatGenericError(ErrorCode, Message);
}

static FString MakeErrorJson(const FString& ErrorCode, const FString& ToolName, const FString& Message)
{
	return MCPToolboxErrorFormat::FormatToolError(ErrorCode, ToolName, Message);
}

// ============================================================================
// Undo / Redo
// ============================================================================
FString FMCPToolboxTransactionService::Undo()
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("undo"), TEXT("GEditor not available"));
	}
	bool bUndone = GEditor->UndoTransaction();
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetBoolField(TEXT("undone"), bUndone);
	});
}

FString FMCPToolboxTransactionService::Redo()
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("redo"), TEXT("GEditor not available"));
	}
	bool bRedone = GEditor->RedoTransaction();
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetBoolField(TEXT("redone"), bRedone);
	});
}

FString FMCPToolboxTransactionService::UndoMultiple(int32 Count)
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("undo_multiple"), TEXT("GEditor not available"));
	}
	if (Count <= 0)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::ParamInvalidValue, TEXT("undo_multiple"),
			FString::Printf(TEXT("Count must be positive, got %d"), Count));
	}
	int32 Done = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		if (!GEditor->UndoTransaction())
		{
			break;
		}
		++Done;
	}
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetNumberField(TEXT("undone_count"), Done);
	});
}

FString FMCPToolboxTransactionService::RedoMultiple(int32 Count)
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("redo_multiple"), TEXT("GEditor not available"));
	}
	if (Count <= 0)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::ParamInvalidValue, TEXT("redo_multiple"),
			FString::Printf(TEXT("Count must be positive, got %d"), Count));
	}
	int32 Done = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		if (!GEditor->RedoTransaction())
		{
			break;
		}
		++Done;
	}
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetNumberField(TEXT("redone_count"), Done);
	});
}

// ============================================================================
// Grouping
// ============================================================================
FString FMCPToolboxTransactionService::BeginTransaction(const FString& Description)
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("begin_transaction"), TEXT("GEditor not available"));
	}
	const FText Desc = Description.IsEmpty()
		? NSLOCTEXT("MCPToolbox", "TransactionService_Default", "MCPToolbox Transaction")
		: FText::FromString(Description);
	GActiveTransactionIndex = GEditor->BeginTransaction(Desc);
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetNumberField(TEXT("transaction_index"), GActiveTransactionIndex);
	});
}

FString FMCPToolboxTransactionService::EndTransaction()
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("end_transaction"), TEXT("GEditor not available"));
	}
	int32 Index = GEditor->EndTransaction();
	GActiveTransactionIndex = INDEX_NONE;
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetNumberField(TEXT("transaction_index"), Index);
	});
}

FString FMCPToolboxTransactionService::CancelTransaction()
{
	if (!GEditor)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("cancel_transaction"), TEXT("GEditor not available"));
	}
	// Trick: GEditor->CancelTransaction 只丢弃事务记录,不还原对象状态。
	// 为实现"回滚自 begin_transaction 以来的所有更改",先 End 再 Undo。
	// 被回滚的事务保留为 redo 候选。
	GEditor->EndTransaction();
	GActiveTransactionIndex = INDEX_NONE;
	bool bRolledBack = GEditor->UndoTransaction();
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetBoolField(TEXT("rolled_back"), bRolledBack);
	});
}

// ============================================================================
// Inspection
// ============================================================================
FString FMCPToolboxTransactionService::GetState()
{
	UTransactor* Trans = GetTransactor();
	if (!Trans)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("get_transaction_state"),
			TEXT("Transactor not available (GEditor->Trans is null)"));
	}

	const int32 QueueLength = Trans->GetQueueLength();
	const int32 RedoCount = Trans->GetUndoCount();  // GetUndoCount 返回已撤销条目数(即 redo 候选)
	const int32 UndoCount = FMath::Max(0, QueueLength - RedoCount);

	FText UndoText;
	bool bCanUndo = Trans->CanUndo(&UndoText);
	FText RedoText;
	bool bCanRedo = Trans->CanRedo(&RedoText);

	FString NextUndoTitle = Trans->GetUndoContext().Title.ToString();
	FString NextRedoTitle = Trans->GetRedoContext().Title.ToString();

	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		Obj->SetBoolField(TEXT("can_undo"), bCanUndo);
		Obj->SetBoolField(TEXT("can_redo"), bCanRedo);
		Obj->SetNumberField(TEXT("undo_count"), UndoCount);
		Obj->SetNumberField(TEXT("redo_count"), RedoCount);
		Obj->SetStringField(TEXT("next_undo_title"), NextUndoTitle);
		Obj->SetStringField(TEXT("next_redo_title"), NextRedoTitle);
	});
}

FString FMCPToolboxTransactionService::GetHistory(int32 MaxEntries)
{
	UTransactor* Trans = GetTransactor();
	if (!Trans)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("get_transaction_history"),
			TEXT("Transactor not available (GEditor->Trans is null)"));
	}

	const int32 QueueLength = Trans->GetQueueLength();
	const int32 RedoCount = Trans->GetUndoCount();
	const int32 FirstUndoneIndex = QueueLength - RedoCount;  // [FirstUndoneIndex, QueueLength) 是已撤销条目

	const int32 Start = (MaxEntries > 0) ? FMath::Max(0, QueueLength - MaxEntries) : 0;

	// 构建 JSON 数组
	TArray<TSharedPtr<FJsonValue>> HistoryArray;
	for (int32 i = Start; i < QueueLength; ++i)
	{
		const FTransaction* Transaction = Trans->GetTransaction(i);
		if (!Transaction)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("title"), Transaction->GetContext().Title.ToString());
		Entry->SetNumberField(TEXT("queue_index"), i);
		Entry->SetBoolField(TEXT("is_undone"), (i >= FirstUndoneIndex));
		HistoryArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// 包装为响应对象
	TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
	ResponseObj->SetBoolField(TEXT("success"), true);
	ResponseObj->SetNumberField(TEXT("total_count"), QueueLength);
	ResponseObj->SetNumberField(TEXT("returned_count"), HistoryArray.Num());
	ResponseObj->SetArrayField(TEXT("history"), HistoryArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(ResponseObj.ToSharedRef(), Writer);
	return Output;
}

// ============================================================================
// Buffer
// ============================================================================
FString FMCPToolboxTransactionService::ResetBuffer(const FString& Reason)
{
	UTransactor* Trans = GetTransactor();
	if (!Trans)
	{
		return MakeErrorJson(EMCPToolboxErrorCode::InternalError, TEXT("reset_transaction_buffer"),
			TEXT("Transactor not available (GEditor->Trans is null)"));
	}
	const FText ResetReason = Reason.IsEmpty()
		? NSLOCTEXT("MCPToolbox", "TransactionService_Reset", "MCPToolbox reset transaction buffer")
		: FText::FromString(Reason);
	Trans->Reset(ResetReason);
	GActiveTransactionIndex = INDEX_NONE;
	return MakeSuccessJson([&](TSharedPtr<FJsonObject>& Obj) {
		// 仅清历史,不改变世界状态
	});
}

// ============================================================================
// Service Metadata (Stage 6.4)
// ============================================================================
FMCPToolboxServiceInfo FMCPToolboxTransactionService::GetServiceInfo()
{
	FMCPToolboxServiceInfo Info;
	Info.Name = TEXT("transaction");
	Info.Description = TEXT("Editor undo/redo service — drive GEditor->Trans for undo/redo, group edits into named transactions, inspect history, reset buffer.");
	Info.Source = TEXT("VibeUE");
	Info.ToolNames = {
		TEXT("undo"), TEXT("redo"),
		TEXT("undo_multiple"), TEXT("redo_multiple"),
		TEXT("begin_transaction"), TEXT("end_transaction"), TEXT("cancel_transaction"),
		TEXT("get_transaction_state"), TEXT("get_transaction_history"),
		TEXT("reset_transaction_buffer")
	};
	return Info;
}
