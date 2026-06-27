#pragma once

#include "CoreMinimal.h"
#include "Http.h"
#include "MCPToolboxAPIManager.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnMCPToolboxEntriesChanged);
DECLARE_DELEGATE_TwoParams(FOnMCPToolboxVisionCheckResult, const FString& /*ProviderId*/, bool /*bSupportsVision*/);

// ============================================================================
// API密钥条目 —— 用户添加的每个密钥对应一条记录
// ============================================================================
USTRUCT(BlueprintType)
struct MCPTOOLBOX_API FMCPToolboxAPIKeyEntry
{
	GENERATED_BODY()

	/** 唯一ID (自动生成) */
	UPROPERTY()
	FString Id;

	/** 服务商标识 (如 "openai", "deepseek") */
	UPROPERTY()
	FString ProviderId;

	/** 服务商显示名 */
	UPROPERTY()
	FString ProviderName;

	/** API地址 (自动从预设填入) */
	UPROPERTY()
	FString BaseURL;

	/** 模型ID */
	UPROPERTY()
	FString ModelId;

	/** 加密后的API密钥 (Base64) */
	UPROPERTY()
	FString EncryptedKey;

	/** 是否为用户自定义服务商 */
	UPROPERTY()
	bool bIsCustom = false;

	/** 创建时间 */
	UPROPERTY()
	FDateTime CreatedAt;

	/** 获取脱敏显示的密钥 (前4位 + **** + 后4位) */
	FString GetMaskedKey() const;
};

// ============================================================================
// 服务商预设 —— 20+ 常见服务商
// ============================================================================
struct MCPTOOLBOX_API FMCPToolboxProviderPreset
{
	FString Id;
	FString Name;
	FString BaseURL;
	TArray<FString> Models;
	bool bSupportsVision = false;

	static const TArray<FMCPToolboxProviderPreset>& GetAll();
	static const FMCPToolboxProviderPreset* Find(const FString& ProviderId);
	static TArray<FString> GetAllProviderIds();
};

// ============================================================================
// FMCPToolboxAPIKeyManager —— 加密存储
// ============================================================================
class MCPTOOLBOX_API FMCPToolboxAPIKeyManager
{
public:
	static FMCPToolboxAPIKeyManager& Get();

	bool StoreKey(const FString& KeyId, const FString& ApiKey);
	bool RetrieveKey(const FString& KeyId, FString& OutApiKey);
	bool DeleteKey(const FString& KeyId);
	bool KeyExists(const FString& KeyId) const;

private:
	FMCPToolboxAPIKeyManager();
	~FMCPToolboxAPIKeyManager();
	FMCPToolboxAPIKeyManager(const FMCPToolboxAPIKeyManager&) = delete;
	FMCPToolboxAPIKeyManager& operator=(const FMCPToolboxAPIKeyManager&) = delete;

	FString GetStoragePath() const;
	FString DeriveKey() const;
	FString Encrypt(const FString& PlainText, const FString& Key);
	FString Decrypt(const FString& CipherText, const FString& Key);
	void Load();
	void Save();

	static constexpr const TCHAR* Seed = TEXT("MCPToolbox-UE5-SecureKey-v1.0");
	TMap<FString, FString> Store;
	mutable FCriticalSection Lock;
	bool bLoaded = false;
};

// ============================================================================
// FMCPToolboxAPIManager —— 主管理器
// ============================================================================
class MCPTOOLBOX_API FMCPToolboxAPIManager
{
public:
	static FMCPToolboxAPIManager& Get();

	// ---- 密钥条目管理 ----
	/**
	 * Add a new API key entry.
	 * @param BaseURLOverride  If non-empty, overrides the preset's BaseURL.
	 *                         This is essential for local providers (llama.cpp / LM Studio)
	 *                         where the user may run the server on a custom port.
	 *                         Previously the UI's BaseURL input was silently ignored.
	 */
	void AddEntry(const FString& ProviderId, const FString& ModelId, const FString& ApiKey,
	              const FString& BaseURLOverride = TEXT(""));
	void RemoveEntry(const FString& EntryId);
	const TArray<FMCPToolboxAPIKeyEntry>& GetEntries() const;
	const FMCPToolboxAPIKeyEntry* GetEntry(const FString& EntryId) const;

	// ---- 获取当前活动条目（最后添加的或用户选择的） ----
	const FMCPToolboxAPIKeyEntry* GetActiveEntry() const;
	void SetActiveEntry(const FString& EntryId);

	// ---- 视觉支持检测 ----
	void CheckVisionSupport(const FString& ProviderId);
	bool SupportsVision(const FString& ProviderId, const FString& ModelId) const;

	// ---- 导入/导出 ----
	bool ExportToJson(const FString& FilePath);
	bool ImportFromJson(const FString& FilePath);

	// ---- 委托 ----
	FOnMCPToolboxEntriesChanged OnEntriesChanged;
	FOnMCPToolboxVisionCheckResult OnVisionCheckResult;

private:
	FMCPToolboxAPIManager();
	~FMCPToolboxAPIManager();
	FMCPToolboxAPIManager(const FMCPToolboxAPIManager&) = delete;
	FMCPToolboxAPIManager& operator=(const FMCPToolboxAPIManager&) = delete;

	void OnVisionResponse(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk, FString ProviderId);
	void SaveEntries();
	void LoadEntries();
	FString GetEntriesPath() const;

	TArray<FMCPToolboxAPIKeyEntry> Entries;
	FString ActiveEntryId;
	TMap<FString, TMap<FString, bool>> VisionCache;
	mutable FCriticalSection Lock;

	/** Pending HTTP requests for vision checks */
	TArray<TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>> PendingVisionRequests;
};
