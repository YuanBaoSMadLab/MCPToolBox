// ============================================================================
// MCPToolboxResult — Result 类型 (移植自 VibeUE Core/Result.h)
// ============================================================================
// 设计动机:
//   VibeUE 的 Python 服务体系使用 TResult<T> 进行类型安全的错误处理。
//   本文件提供独立的 Result 模板,不依赖 VibeUE 的任何其他模块。
//
// 使用模式:
//   TResult<FPythonExecutionResult> ExecuteCode(...);
//   if (Result.IsError()) { return ErrorJson(Result.GetErrorCode(), Result.GetErrorMessage()); }
//   auto& Value = Result.GetValue();
//
// 线程安全: 本模板为值类型,无共享状态,天然线程安全。
// ============================================================================
#pragma once

#include "CoreMinimal.h"

/**
 * Result 类型,用于可成功或失败的操作。
 * 提供类型安全的错误处理,避免运行时 JSON 解析。
 *
 * @tparam T 成功时返回的值类型
 */
template<typename T>
class MCPTOOLBOX_API TMCPToolboxResult
{
public:
	/** 创建成功结果(拷贝) */
	static TMCPToolboxResult Success(const T& Value)
	{
		return TMCPToolboxResult(true, Value, FString(), FString());
	}

	/** 创建成功结果(移动) */
	static TMCPToolboxResult Success(T&& Value)
	{
		return TMCPToolboxResult(true, MoveTemp(Value), FString(), FString());
	}

	/** 创建错误结果 */
	static TMCPToolboxResult Error(const FString& ErrorCode, const FString& ErrorMessage)
	{
		return TMCPToolboxResult(false, T(), ErrorCode, ErrorMessage);
	}

	bool IsSuccess() const { return bSuccess; }
	bool IsError() const { return !bSuccess; }

	const T& GetValue() const
	{
		check(bSuccess);
		return Value;
	}

	const FString& GetErrorCode() const { return ErrorCode; }
	const FString& GetErrorMessage() const { return ErrorMessage; }

	/** Map 操作:成功时转换值类型,Failure 时传递错误 */
	template<typename U>
	TMCPToolboxResult<U> Map(TFunction<U(const T&)> Fn) const
	{
		if (IsSuccess())
		{
			return TMCPToolboxResult<U>::Success(Fn(Value));
		}
		return TMCPToolboxResult<U>::Error(ErrorCode, ErrorMessage);
	}

private:
	TMCPToolboxResult(bool bInSuccess, const T& InValue, const FString& InErrorCode, const FString& InErrorMessage)
		: bSuccess(bInSuccess), Value(InValue), ErrorCode(InErrorCode), ErrorMessage(InErrorMessage)
	{}

	TMCPToolboxResult(bool bInSuccess, T&& InValue, const FString& InErrorCode, const FString& InErrorMessage)
		: bSuccess(bInSuccess), Value(MoveTemp(InValue)), ErrorCode(InErrorCode), ErrorMessage(InErrorMessage)
	{}

	bool bSuccess;
	T Value;
	FString ErrorCode;
	FString ErrorMessage;
};

/**
 * Result<void> 特化:用于无返回值但可失败的操作。
 */
template<>
class MCPTOOLBOX_API TMCPToolboxResult<void>
{
public:
	static TMCPToolboxResult Success()
	{
		return TMCPToolboxResult(true, FString(), FString());
	}

	static TMCPToolboxResult Error(const FString& ErrorCode, const FString& ErrorMessage)
	{
		return TMCPToolboxResult(false, ErrorCode, ErrorMessage);
	}

	bool IsSuccess() const { return bSuccess; }
	bool IsError() const { return !bSuccess; }

	const FString& GetErrorCode() const { return ErrorCode; }
	const FString& GetErrorMessage() const { return ErrorMessage; }

private:
	TMCPToolboxResult(bool bInSuccess, const FString& InErrorCode, const FString& InErrorMessage)
		: bSuccess(bInSuccess), ErrorCode(InErrorCode), ErrorMessage(InErrorMessage)
	{}

	bool bSuccess;
	FString ErrorCode;
	FString ErrorMessage;
};
