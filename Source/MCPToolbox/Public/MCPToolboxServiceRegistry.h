// ============================================================================
// MCPToolboxServiceRegistry — Service 层抽象基础 (Stage 6.4)
// ============================================================================
// 设计动机:
//   SMCPToolboxChatWidget 是 God Object(已知问题 #5),承担 7+ 职责。
//   完整的 DI 重构是长期任务,本阶段提供基础架构:
//   1. 服务元数据注册表(FServiceRegistry 单例)
//   2. 内省工具 list_services / get_service_info(让 LLM 查询当前已注册的服务)
//   3. 各 Service 通过 GetServiceInfo() 静态方法暴露元数据(不破坏现有静态方法)
//
// 设计选择:
//   - 使用 Service Registry 模式而非 FServiceBase 抽象基类,因为静态方法类
//     不能继承 virtual 方法
//   - 不强制改动现有 FunctionBuilder 调用,保持向后兼容
//   - 为未来真正的 DI 容器(FServiceContext)留接口
//
// 使用模式:
//   // 在 RegisterMCPTools 中,每个服务注册工具后调用:
//   FServiceRegistry::Get().Register(FMCPToolboxTransactionService::GetServiceInfo());
//   FServiceRegistry::Get().Register(FMCPToolboxPerformanceService::GetServiceInfo());
//
//   // LLM 通过工具调用查询:
//   //   list_services → 返回所有服务摘要
//   //   get_service_info(service="transaction") → 返回该服务的工具列表
// ============================================================================
#pragma once

#include "CoreMinimal.h"

/**
 * 服务元数据:描述一个 Service 的基本信息。
 * 各 Service 通过静态方法 GetServiceInfo() 返回此结构。
 */
struct FMCPToolboxServiceInfo
{
	/** 服务名(短标识符,如 "transaction", "performance") */
	FString Name;

	/** 人类可读的服务描述 */
	FString Description;

	/** 该服务包含的工具名列表 */
	TArray<FString> ToolNames;

	/** 服务来源(如 "VibeUE", "MCPToolbox 自研") */
	FString Source;
};

/**
 * Service Registry 单例:管理所有已注册 Service 的元数据。
 *
 * 这是 Service 层抽象的基础架构。完整 DI 重构(FServiceContext + 依赖注入容器)
 * 是长期任务,本阶段提供:
 *   - 服务元数据查询(ListServices / GetServiceInfo)
 *   - LLM 内省能力(通过 list_services / get_service_info 工具)
 *
 * 线程安全:仅在游戏线程(RegisterMCPTools)写入,读取通过工具调用也发生在
 * 游戏线程,无需加锁。
 */
class MCPTOOLBOX_API FMCPToolboxServiceRegistry
{
public:
	/** 获取单例 */
	static FMCPToolboxServiceRegistry& Get();

	/** 注册一个服务元数据。同名服务会被覆盖(后注册者胜)。 */
	void Register(const FMCPToolboxServiceInfo& Info);

	/** 查询所有已注册服务(返回 JSON 字符串,供 LLM 使用)。 */
	FString ListServices() const;

	/** 查询指定服务的详细信息(返回 JSON,失败时返回 error_code)。 */
	FString GetServiceInfo(const FString& ServiceName) const;

	/** 已注册服务数量 */
	int32 GetServiceCount() const { return Services.Num(); }

private:
	FMCPToolboxServiceRegistry() = default;
	FMCPToolboxServiceRegistry(const FMCPToolboxServiceRegistry&) = delete;
	FMCPToolboxServiceRegistry& operator=(const FMCPToolboxServiceRegistry&) = delete;

	TArray<FMCPToolboxServiceInfo> Services;
};
