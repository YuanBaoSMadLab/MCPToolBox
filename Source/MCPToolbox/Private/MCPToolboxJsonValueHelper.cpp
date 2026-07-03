// ============================================================================
// FMCPToolboxJsonValueHelper — 容错 JSON 解析层 (Stage 5)
// ============================================================================
// 移植自 VibeUE Source/VibeUE/Private/Core/JsonValueHelper.cpp
// 核心方法: ParseStringToValue / CoerceValue / TryGetVector / TryGetNumber
// ============================================================================

#include "MCPToolboxJsonValueHelper.h"

TSharedPtr<FJsonValue> FMCPToolboxJsonValueHelper::ParseStringToValue(const FString& StringValue)
{
	FString Trimmed = StringValue.TrimStartAndEnd();

	// 空字符串保持为字符串
	if (Trimmed.IsEmpty())
	{
		return MakeShared<FJsonValueString>(StringValue);
	}

	// 如果看起来像 JSON,尝试解析
	if (LooksLikeJson(Trimmed))
	{
		TSharedPtr<FJsonValue> ParsedValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);

		if (FJsonSerializer::Deserialize(Reader, ParsedValue) && ParsedValue.IsValid())
		{
			return ParsedValue;
		}
	}

	// 检查布尔字符串
	FString LowerTrimmed = Trimmed.ToLower();
	if (LowerTrimmed == TEXT("true") || LowerTrimmed == TEXT("yes"))
	{
		return MakeShared<FJsonValueBoolean>(true);
	}
	if (LowerTrimmed == TEXT("false") || LowerTrimmed == TEXT("no"))
	{
		return MakeShared<FJsonValueBoolean>(false);
	}

	// 检查 null
	if (LowerTrimmed == TEXT("null"))
	{
		return MakeShared<FJsonValueNull>();
	}

	// 检查数字(整数或浮点)
	if (Trimmed.IsNumeric() ||
		(Trimmed.StartsWith(TEXT("-")) && Trimmed.Mid(1).IsNumeric()) ||
		Trimmed.Contains(TEXT(".")))
	{
		double NumValue;
		if (LexTryParseString(NumValue, *Trimmed))
		{
			return MakeShared<FJsonValueNumber>(NumValue);
		}
	}

	// 返回为字符串
	return MakeShared<FJsonValueString>(StringValue);
}

TSharedPtr<FJsonValue> FMCPToolboxJsonValueHelper::CoerceValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return Value;
	}

	// 只强制转换字符串值
	if (Value->Type != EJson::String)
	{
		return Value;
	}

	return ParseStringToValue(Value->AsString());
}

TSharedPtr<FJsonObject> FMCPToolboxJsonValueHelper::CoerceObject(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return Object;
	}

	// 对每个字段应用 CoerceValue
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const auto& Pair : Object->Values)
	{
		TSharedPtr<FJsonValue> Coerced = CoerceValue(Pair.Value);
		Result->Values.Add(Pair.Key, Coerced);
	}
	return Result;
}

bool FMCPToolboxJsonValueHelper::TryGetArray(const TSharedPtr<FJsonValue>& Value, TArray<TSharedPtr<FJsonValue>>& OutArray)
{
	if (!Value.IsValid())
	{
		return false;
	}

	// 直接数组
	if (Value->Type == EJson::Array)
	{
		OutArray = Value->AsArray();
		return true;
	}

	// 可能是数组的字符串
	if (Value->Type == EJson::String)
	{
		TSharedPtr<FJsonValue> Parsed = ParseStringToValue(Value->AsString());
		if (Parsed.IsValid() && Parsed->Type == EJson::Array)
		{
			OutArray = Parsed->AsArray();
			return true;
		}
	}

	return false;
}

bool FMCPToolboxJsonValueHelper::TryGetObject(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonObject>& OutObject)
{
	if (!Value.IsValid())
	{
		return false;
	}

	// 直接对象
	if (Value->Type == EJson::Object)
	{
		OutObject = Value->AsObject();
		return true;
	}

	// 可能是对象的字符串
	if (Value->Type == EJson::String)
	{
		TSharedPtr<FJsonValue> Parsed = ParseStringToValue(Value->AsString());
		if (Parsed.IsValid() && Parsed->Type == EJson::Object)
		{
			OutObject = Parsed->AsObject();
			return true;
		}
	}

	return false;
}

bool FMCPToolboxJsonValueHelper::TryGetNumberArray(const TSharedPtr<FJsonValue>& Value, TArray<double>& OutNumbers)
{
	TArray<TSharedPtr<FJsonValue>> ArrayValues;
	if (!TryGetArray(Value, ArrayValues))
	{
		return false;
	}

	OutNumbers.Empty();
	for (const auto& ArrayValue : ArrayValues)
	{
		double Num;
		if (TryGetNumber(ArrayValue, Num))
		{
			OutNumbers.Add(Num);
		}
		else
		{
			return false;
		}
	}

	return true;
}

bool FMCPToolboxJsonValueHelper::TryGetVector(const TSharedPtr<FJsonValue>& Value, FVector& OutVector)
{
	if (!Value.IsValid())
	{
		return false;
	}

	// 强制转换字符串为实际值
	TSharedPtr<FJsonValue> CoercedValue = CoerceValue(Value);

	// 尝试数组 [x, y, z]
	if (CoercedValue->Type == EJson::Array)
	{
		TArray<double> Numbers;
		if (TryGetNumberArray(CoercedValue, Numbers) && Numbers.Num() >= 3)
		{
			OutVector.X = Numbers[0];
			OutVector.Y = Numbers[1];
			OutVector.Z = Numbers[2];
			return true;
		}
	}

	// 尝试对象 {X: x, Y: y, Z: z} (大小写不敏感)
	if (CoercedValue->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>& Obj = CoercedValue->AsObject();
		double X = 0, Y = 0, Z = 0;
		bool bHasX = false, bHasY = false, bHasZ = false;

		if (Obj->TryGetNumberField(TEXT("X"), X) || Obj->TryGetNumberField(TEXT("x"), X))
		{
			bHasX = true;
		}
		if (Obj->TryGetNumberField(TEXT("Y"), Y) || Obj->TryGetNumberField(TEXT("y"), Y))
		{
			bHasY = true;
		}
		if (Obj->TryGetNumberField(TEXT("Z"), Z) || Obj->TryGetNumberField(TEXT("z"), Z))
		{
			bHasZ = true;
		}

		if (bHasX && bHasY && bHasZ)
		{
			OutVector.X = X;
			OutVector.Y = Y;
			OutVector.Z = Z;
			return true;
		}
	}

	return false;
}

bool FMCPToolboxJsonValueHelper::TryGetVectorField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FVector& OutVector)
{
	if (!Object.IsValid() || !Object->HasField(FieldName))
	{
		return false;
	}

	TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
	return TryGetVector(Value, OutVector);
}

bool FMCPToolboxJsonValueHelper::TryGetRotator(const TSharedPtr<FJsonValue>& Value, FRotator& OutRotator)
{
	if (!Value.IsValid())
	{
		return false;
	}

	TSharedPtr<FJsonValue> CoercedValue = CoerceValue(Value);

	// 尝试数组 [pitch, yaw, roll]
	if (CoercedValue->Type == EJson::Array)
	{
		TArray<double> Numbers;
		if (TryGetNumberArray(CoercedValue, Numbers) && Numbers.Num() >= 3)
		{
			OutRotator.Pitch = Numbers[0];
			OutRotator.Yaw = Numbers[1];
			OutRotator.Roll = Numbers[2];
			return true;
		}
	}

	// 尝试对象 {Pitch: p, Yaw: y, Roll: r}
	if (CoercedValue->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>& Obj = CoercedValue->AsObject();
		double Pitch = 0, Yaw = 0, Roll = 0;
		bool bHasP = false, bHasY = false, bHasR = false;

		if (Obj->TryGetNumberField(TEXT("Pitch"), Pitch) || Obj->TryGetNumberField(TEXT("pitch"), Pitch) || Obj->TryGetNumberField(TEXT("P"), Pitch))
		{
			bHasP = true;
		}
		if (Obj->TryGetNumberField(TEXT("Yaw"), Yaw) || Obj->TryGetNumberField(TEXT("yaw"), Yaw) || Obj->TryGetNumberField(TEXT("Y"), Yaw))
		{
			bHasY = true;
		}
		if (Obj->TryGetNumberField(TEXT("Roll"), Roll) || Obj->TryGetNumberField(TEXT("roll"), Roll) || Obj->TryGetNumberField(TEXT("R"), Roll))
		{
			bHasR = true;
		}

		if (bHasP && bHasY && bHasR)
		{
			OutRotator.Pitch = Pitch;
			OutRotator.Yaw = Yaw;
			OutRotator.Roll = Roll;
			return true;
		}
	}

	return false;
}

bool FMCPToolboxJsonValueHelper::TryGetBool(const TSharedPtr<FJsonValue>& Value, bool& OutBool)
{
	if (!Value.IsValid())
	{
		return false;
	}

	// 直接布尔
	if (Value->Type == EJson::Boolean)
	{
		OutBool = Value->AsBool();
		return true;
	}

	// 数字 (1/0)
	if (Value->Type == EJson::Number)
	{
		OutBool = (Value->AsNumber() != 0.0);
		return true;
	}

	// 字符串
	if (Value->Type == EJson::String)
	{
		FString Str = Value->AsString().TrimStartAndEnd().ToLower();
		if (Str == TEXT("true") || Str == TEXT("yes") || Str == TEXT("1"))
		{
			OutBool = true;
			return true;
		}
		if (Str == TEXT("false") || Str == TEXT("no") || Str == TEXT("0"))
		{
			OutBool = false;
			return true;
		}
	}

	return false;
}

bool FMCPToolboxJsonValueHelper::TryGetNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
{
	if (!Value.IsValid())
	{
		return false;
	}

	// 直接数字
	if (Value->Type == EJson::Number)
	{
		OutNumber = Value->AsNumber();
		return true;
	}

	// 字符串
	if (Value->Type == EJson::String)
	{
		FString Str = Value->AsString().TrimStartAndEnd();
		double Num;
		if (LexTryParseString(Num, *Str))
		{
			OutNumber = Num;
			return true;
		}
	}

	// 布尔转数字
	if (Value->Type == EJson::Boolean)
	{
		OutNumber = Value->AsBool() ? 1.0 : 0.0;
		return true;
	}

	return false;
}

bool FMCPToolboxJsonValueHelper::TryGetString(const TSharedPtr<FJsonValue>& Value, FString& OutString)
{
	if (!Value.IsValid())
	{
		return false;
	}

	switch (Value->Type)
	{
	case EJson::String:
		OutString = Value->AsString();
		return true;
	case EJson::Number:
		OutString = FString::SanitizeFloat(Value->AsNumber());
		return true;
	case EJson::Boolean:
		OutString = Value->AsBool() ? TEXT("true") : TEXT("false");
		return true;
	case EJson::Null:
		OutString = TEXT("");
		return true;
	default:
	{
		// 数组/对象序列化为 JSON 字符串
		FString JsonStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
		if (Value->Type == EJson::Array)
		{
			FJsonSerializer::Serialize(Value->AsArray(), Writer);
		}
		else if (Value->Type == EJson::Object)
		{
			FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
		}
		Writer->Close();
		OutString = JsonStr;
		return true;
	}
	}
}

bool FMCPToolboxJsonValueHelper::LooksLikeJson(const FString& Str)
{
	return Str.StartsWith(TEXT("[")) || Str.StartsWith(TEXT("{"));
}
