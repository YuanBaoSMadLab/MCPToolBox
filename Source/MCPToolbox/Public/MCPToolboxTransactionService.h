// ============================================================================
// MCPToolboxTransactionService — 编辑器 Undo/Redo 服务 (Stage 6.1)
// ============================================================================
// 移植自 VibeUE UTransactionService (Public/PythonAPI/UTransactionService.h)
// 改造点:
//   - 不继承 UToolsetDefinition(MCPToolbox 不走 Epic ToolsetRegistry 反射)
//   - 改为纯静态方法类,返回 JSON 字符串(便于通过 FunctionBuilder 传递给 LLM)
//   - 对接 MCPToolboxErrorCodes.h 结构化错误码
//
// 功能: 封装 GEditor->Trans (UTransactor),让 agent 可以驱动 undo/redo,
//        将多个编辑组合为单个命名事务,检查历史,重置缓冲区。
//
// 核心方法:
//   - Undo() / Redo() / UndoMultiple(N) / RedoMultiple(N)
//   - BeginTransaction(Desc) / EndTransaction() / CancelTransaction()
//   - GetState() / GetHistory(MaxEntries) / ResetBuffer(Reason)
// ============================================================================
#pragma once

#include "CoreMinimal.h"

class MCPTOOLBOX_API FMCPToolboxTransactionService
{
public:
	// ── Undo / Redo ──
	/** 撤销最近一次事务。返回 JSON: {"success":bool,"undone":bool} */
	static FString Undo();

	/** 重做最近一次撤销的事务。返回 JSON: {"success":bool,"redone":bool} */
	static FString Redo();

	/** 撤销最近 N 次事务。返回 JSON: {"success":bool,"undone_count":int} */
	static FString UndoMultiple(int32 Count);

	/** 重做最近 N 次撤销的事务。返回 JSON: {"success":bool,"redone_count":int} */
	static FString RedoMultiple(int32 Count);

	// ── Grouping ──
	/**
	 * 开始命名事务,后续编辑将合并为单个 undo 步骤。
	 * 需配对 EndTransaction 或 CancelTransaction。
	 * 返回 JSON: {"success":bool,"transaction_index":int} (index>=0 成功, -1 失败)
	 */
	static FString BeginTransaction(const FString& Description);

	/** 结束由 BeginTransaction 开启的活动事务。返回 JSON: {"success":bool,"transaction_index":int} */
	static FString EndTransaction();

	/**
	 * 取消活动事务,回滚自 BeginTransaction 以来的所有更改。
	 * 实现: End + Undo trick(GEditor->CancelTransaction 只丢弃记录不还原状态)。
	 * 返回 JSON: {"success":bool,"rolled_back":bool}
	 */
	static FString CancelTransaction();

	// ── Inspection ──
	/**
	 * 获取 undo/redo 状态快照。
	 * 返回 JSON: {"success":bool,"can_undo":bool,"can_redo":bool,
	 *             "undo_count":int,"redo_count":int,
	 *             "next_undo_title":string,"next_redo_title":string}
	 */
	static FString GetState();

	/** 获取 undo 历史列表(最近 MaxEntries 条)。返回 JSON 数组。 */
	static FString GetHistory(int32 MaxEntries = 20);

	// ── Buffer ──
	/**
	 * 重置整个 undo/redo 缓冲区(仅清历史,不改变世界状态)。
	 * 返回 JSON: {"success":bool}
	 */
	static FString ResetBuffer(const FString& Reason);

	// ── Service Metadata (Stage 6.4) ──
	/** 返回服务元数据,供 FServiceRegistry 注册 */
	static struct FMCPToolboxServiceInfo GetServiceInfo();

private:
	/** 获取引擎事务处理器 */
	static class UTransactor* GetTransactor();
};
