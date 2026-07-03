// ============================================================================
// MCPToolboxPerformanceService — 帧率诊断与 Unreal Insights trace 服务 (Stage 6.2)
// ============================================================================
// 移植自 VibeUE UPerformanceService (Public/PythonAPI/UPerformanceService.h)
// 改造点:
//   - 不继承 UToolsetDefinition(MCPToolbox 不走 Epic ToolsetRegistry 反射)
//   - 改为纯静态方法类,返回 JSON 字符串(便于通过 FunctionBuilder 传递给 LLM)
//   - 对接 MCPToolboxErrorCodes.h 结构化错误码
//
// 功能: 帧率诊断与 Unreal Insights trace 捕获/分析。
//   - FrameTiming() — 报告 Game/Render/RHI/GPU 线程毫秒 + CPU/GPU bound 判定 + 优化 hint
//   - StartTrace/StopTrace/GetTraceStatus — trace 捕获控制
//   - Bookmark/RegionStart/RegionEnd — trace 标记
//   - Analyse(trace|logs|both) — 分析 trace/log 文件,返回性能摘要
//   - StartStandalone/StopStandalone/GetStandaloneStatus — 独立进程带 trace
//
// 推荐流程: FrameTiming() 第一步(判断 CPU/GPU bound) → StartTrace → 复现场景 → StopTrace → Analyse
//
// 依赖模块: RenderCore (G*ThreadTime), RHI (RHIGetGPUFrameCycles),
//          TraceServices (IAnalysisService), TraceLog (FTraceAuxiliary)
// ============================================================================
#pragma once

#include "CoreMinimal.h"

class MCPTOOLBOX_API FMCPToolboxPerformanceService
{
public:
	// ── Frame Timing ──
	/**
	 * 报告最近渲染帧的 Game/Render/RHI/GPU 线程毫秒数 + CPU-vs-GPU bound 判定 + 优化 hint。
	 * 帧率调查第一步:优化 GPU 在 CPU/Render bound 时无意义。
	 * 返回 JSON: {game_thread_ms, render_thread_ms, rhi_thread_ms, gpu_ms,
	 *             frame_ms, fps, bound, hint, pie_running, note}
	 */
	static FString FrameTiming();

	// ── Trace Capture ──
	/**
	 * 启动 Unreal Insights trace 到文件。
	 * @param Name      trace 文件名(不带扩展名,空则用 "mcp_capture")
	 * @param Channels  逗号分隔的 trace 通道;空用默认集(frame,cpu,gpu,log,...)
	 * 返回 JSON: {status:"tracing", trace_file, channels, hint}
	 */
	static FString StartTrace(const FString& Name = TEXT("mcp_capture"), const FString& Channels = TEXT(""));

	/** 停止活动 trace,返回 trace 文件路径和大小。 */
	static FString StopTrace();

	/** 报告 trace 是否活动以及启用的通道。返回 JSON: {tracing, destination, active_channels, last_trace_file} */
	static FString GetTraceStatus();

	// ── Trace Markers ──
	/** 在活动 trace 中放置命名书签。 */
	static FString Bookmark(const FString& Name);

	/** 开始命名区域(配对 RegionEnd)。 */
	static FString RegionStart(const FString& Name);

	/** 结束命名区域。 */
	static FString RegionEnd(const FString& Name);

	// ── Analysis ──
	/**
	 * 读取 trace 和/或 log,返回性能摘要(帧统计、最差帧、值得注意的日志行)。
	 * @param Source "trace" / "logs" / "both"(默认)
	 * @param File   可选路径覆盖;空用最近一次 trace
	 */
	static FString Analyse(const FString& Source = TEXT("both"), const FString& File = TEXT(""));

	// ── Standalone Profiling ──
	/**
	 * 以独立进程启动游戏并附加 trace(编辑器视口给不了代表性读数)。
	 * 连回编辑器的 Unreal Trace Server。
	 */
	static FString StartStandalone(const FString& Name = TEXT("standalone_capture"), const FString& Channels = TEXT(""));

	/** 停止独立进程并最终化其 trace/log。 */
	static FString StopStandalone();

	/** 报告独立会话是否运行及其 trace/log 路径。 */
	static FString GetStandaloneStatus();

	// ── Service Metadata (Stage 6.4) ──
	/** 返回服务元数据,供 FServiceRegistry 注册 */
	static struct FMCPToolboxServiceInfo GetServiceInfo();
};
