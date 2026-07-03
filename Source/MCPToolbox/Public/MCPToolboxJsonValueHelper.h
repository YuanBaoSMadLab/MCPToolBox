#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// FMCPToolboxJsonValueHelper — 容错 JSON 解析层 (Stage 5)
// ============================================================================
// 设计参考: VibeUE JsonValueHelper (CoerceValue/TryGetVector 容错解析层)
// 用途: 处理 LLM 发送 JSON 值时的常见问题:
//   - 嵌套 JSON 编码导致值变成字符串 (如 "[1.0, 2.0]" 而非 [1.0, 2.0])
//   - Vector 多形态 ([x,y,z] / {"X":x,"Y":y,"Z":z} / "1,2,3")
//   - 布尔值字符串 ("true"/"false"/"yes"/"no")
//   - 数字字符串 ("42" → 42)
//
// 在 ExecuteToolCall 入口对 arguments 应用 CoerceValue,自动升级字符串为强类型。
// ============================================================================

class MCPTOOLBOX_API FMCPToolboxJsonValueHelper
{
public:
	/**
	 * 尝试将字符串 JSON 值解析为其真实类型。
	 * 如果字符串包含 JSON 数组/对象/布尔/数字/null,返回对应类型的 FJsonValue。
	 * 解析失败则返回原始字符串值。
	 */
	static TSharedPtr<FJsonValue> ParseStringToValue(const FString& StringValue);

	/**
	 * 智能强制转换 JSON 值为其"真实"类型。
	 * 如果值是看起来像 JSON 的字符串,则解析它。否则返回原值。
	 * 这是 ExecuteToolCall 入口的核心容错方法。
	 */
	static TSharedPtr<FJsonValue> CoerceValue(const TSharedPtr<FJsonValue>& Value);

	/**
	 * 对 JSON 对象的所有字段应用 CoerceValue(递归)。
	 * 用于 ExecuteToolCall 入口:对 arguments 对象做整体容错升级。
	 */
	static TSharedPtr<FJsonObject> CoerceObject(const TSharedPtr<FJsonObject>& Object);

	/** 尝试从 JSON 值获取数组(处理字符串表示)。 */
	static bool TryGetArray(const TSharedPtr<FJsonValue>& Value, TArray<TSharedPtr<FJsonValue>>& OutArray);

	/** 尝试从 JSON 值获取对象(处理字符串表示)。 */
	static bool TryGetObject(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonObject>& OutObject);

	/** 尝试从 JSON 值获取数字数组(如 [1.0, 2.0])。 */
	static bool TryGetNumberArray(const TSharedPtr<FJsonValue>& Value, TArray<double>& OutNumbers);

	/** 尝试从 JSON 值获取 Vector。接受 [x,y,z], {"X":x,"Y":y,"Z":z}, 或字符串版本。 */
	static bool TryGetVector(const TSharedPtr<FJsonValue>& Value, FVector& OutVector);

	/** 尝试从 JSON 对象字段获取 Vector。 */
	static bool TryGetVectorField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FVector& OutVector);

	/** 尝试从 JSON 值获取 Rotator。接受 [p,y,r], {"Pitch":p,"Yaw":y,"Roll":r}, 或字符串版本。 */
	static bool TryGetRotator(const TSharedPtr<FJsonValue>& Value, FRotator& OutRotator);

	/** 尝试从 JSON 值获取布尔值。处理 true/false, "true"/"false", 1/0, "yes"/"no"。 */
	static bool TryGetBool(const TSharedPtr<FJsonValue>& Value, bool& OutBool);

	/** 尝试从 JSON 值获取数字。处理 42, 3.14, "42", "3.14"。 */
	static bool TryGetNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber);

	/** 尝试从 JSON 值获取字符串。数字和布尔会转为字符串。 */
	static bool TryGetString(const TSharedPtr<FJsonValue>& Value, FString& OutString);

private:
	/** 检查字符串是否看起来像 JSON 值(以 [ 或 { 开头)。 */
	static bool LooksLikeJson(const FString& Str);
};
