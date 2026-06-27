#include "MCPToolboxAPIManager.h"
#include "MCPToolbox.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"

// ============================================================================
// 22+ 预设服务商
// ============================================================================
const TArray<FMCPToolboxProviderPreset>& FMCPToolboxProviderPreset::GetAll()
{
	static TArray<FMCPToolboxProviderPreset> Presets = {
		// ======== 国际服务商 ========
		{TEXT("openai"),       TEXT("OpenAI"),       TEXT("https://api.openai.com/v1"),       {TEXT("gpt-4o"), TEXT("gpt-4o-mini"), TEXT("gpt-4-turbo"), TEXT("gpt-4"), TEXT("gpt-3.5-turbo"), TEXT("o3-mini"), TEXT("o1"), TEXT("o1-mini")}, true},
		{TEXT("anthropic"),    TEXT("Anthropic Claude"), TEXT("https://api.anthropic.com/v1"), {TEXT("claude-sonnet-4-20250514"), TEXT("claude-opus-4-20250514"), TEXT("claude-3.5-sonnet"), TEXT("claude-3.5-haiku"), TEXT("claude-3-opus"), TEXT("claude-3-sonnet"), TEXT("claude-3-haiku")}, true},
		{TEXT("google"),       TEXT("Google Gemini"), TEXT("https://generativelanguage.googleapis.com/v1beta"), {TEXT("gemini-2.5-pro"), TEXT("gemini-2.5-flash"), TEXT("gemini-2.0-flash"), TEXT("gemini-1.5-pro"), TEXT("gemini-1.5-flash")}, true},
		{TEXT("meta"),         TEXT("Meta Llama"),    TEXT("https://api.llama-api.com/v1"),   {TEXT("llama-3.3-70b"), TEXT("llama-3.2-90b"), TEXT("llama-3.2-11b"), TEXT("llama-3.1-405b")}, false},
		{TEXT("mistral"),      TEXT("Mistral AI"),    TEXT("https://api.mistral.ai/v1"),      {TEXT("mistral-large"), TEXT("mistral-medium"), TEXT("mistral-small"), TEXT("codestral"), TEXT("pixtral-large")}, true},
		{TEXT("cohere"),       TEXT("Cohere"),        TEXT("https://api.cohere.com/v1"),      {TEXT("command-r-plus"), TEXT("command-r"), TEXT("command"), TEXT("command-light")}, false},
		{TEXT("groq"),         TEXT("Groq"),          TEXT("https://api.groq.com/openai/v1"), {TEXT("llama-3.3-70b-versatile"), TEXT("llama-3.2-90b-vision-preview"), TEXT("mixtral-8x7b-32768"), TEXT("gemma2-9b-it"), TEXT("qwen-2.5-32b")}, true},
		{TEXT("perplexity"),   TEXT("Perplexity"),    TEXT("https://api.perplexity.ai"),     {TEXT("sonar-pro"), TEXT("sonar"), TEXT("sonar-reasoning")}, false},
		{TEXT("together"),     TEXT("Together AI"),   TEXT("https://api.together.xyz/v1"),   {TEXT("together/llama-3.3-70b"), TEXT("together/mixtral-8x7b"), TEXT("together/qwen-2.5-72b")}, false},
		{TEXT("replicate"),    TEXT("Replicate"),     TEXT("https://api.replicate.com/v1"),  {}, false},

		// ======== 国内服务商 ========
		{TEXT("deepseek"),     TEXT("DeepSeek"),      TEXT("https://api.deepseek.com/v1"),   {TEXT("deepseek-v4-flash"), TEXT("deepseek-v4-pro"), TEXT("deepseek-chat"), TEXT("deepseek-reasoner")}, true},
		{TEXT("moonshot"),     TEXT("Moonshot (月之暗面 Kimi)"), TEXT("https://api.moonshot.cn/v1"), {TEXT("kimi-k2.7-code"), TEXT("kimi-k2-thinking"), TEXT("moonshot-v1-8k"), TEXT("moonshot-v1-32k"), TEXT("moonshot-v1-128k")}, true},
		{TEXT("minimax"),      TEXT("MiniMax (稀宇)"), TEXT("https://api.minimax.chat/v1"),  {TEXT("minimax-m3"), TEXT("abab7-chat"), TEXT("abab6.5g-chat"), TEXT("abab6.5s-chat")}, true},
		{TEXT("zhipu"),        TEXT("智谱 AI (GLM)"),  TEXT("https://open.bigmodel.cn/api/paas/v4"), {TEXT("glm-5.2"), TEXT("glm-4-plus"), TEXT("glm-4-flash"), TEXT("glm-4v"), TEXT("glm-4-air")}, true},
		{TEXT("qwen"),         TEXT("通义千问 (阿里云)"), TEXT("https://dashscope.aliyuncs.com/compatible-mode/v1"), {TEXT("qwen3.7-plus"), TEXT("qwen3-max"), TEXT("qwen-plus"), TEXT("qwen-turbo"), TEXT("qwen-vl-max")}, true},
		{TEXT("baidu"),        TEXT("百度文心 (ERNIE)"), TEXT("https://aip.baidubce.com/rpc/2.0/ai_custom/v1/wenxinworkshop/chat"), {TEXT("ernie-4.0-turbo-8k"), TEXT("ernie-4.0-8k"), TEXT("ernie-3.5-8k"), TEXT("ernie-speed-8k")}, false},
		{TEXT("bytedance"),    TEXT("字节跳动 (豆包)"), TEXT("https://ark.cn-beijing.volces.com/api/v3"), {TEXT("doubao-pro-32k"), TEXT("doubao-lite-32k"), TEXT("doubao-pro-128k")}, true},
		{TEXT("tencent"),      TEXT("腾讯混元 (Hunyuan)"), TEXT("https://api.hunyuan.cloud.tencent.com/v1"), {TEXT("hunyuan-pro"), TEXT("hunyuan-turbo"), TEXT("hunyuan-standard"), TEXT("hunyuan-lite")}, false},
		{TEXT("yi"),           TEXT("01.AI (Yi)"),     TEXT("https://api.lingyiwanwu.com/v1"), {TEXT("yi-large"), TEXT("yi-medium"), TEXT("yi-lightning"), TEXT("yi-vision")}, true},
		{TEXT("baichuan"),     TEXT("百川智能 (Baichuan)"), TEXT("https://api.baichuan-ai.com/v1"), {TEXT("baichuan4"), TEXT("baichuan3-turbo"), TEXT("baichuan3-turbo-128k")}, false},
		{TEXT("stepfun"),      TEXT("阶跃星辰 (StepFun)"), TEXT("https://api.stepfun.com/v1"), {TEXT("step-2-16k"), TEXT("step-1-8k"), TEXT("step-1v-8k"), TEXT("step-1-flash")}, true},
		{TEXT("sense"),         TEXT("商汤 SenseNova"), TEXT("https://api.sensenova.cn/v1"), {TEXT("sensechat-5"), TEXT("sensechat-vision")}, true},
		{TEXT("ollama"),       TEXT("Ollama (本地)"),  TEXT("http://127.0.0.1:11434/v1"),   {}, false},
		// ======== 本地 OpenAI 兼容推理服务 ========
		// llama.cpp llama-server: `./llama-server -m model.gguf --port 8088`
		// 两者均实现标准 /v1/chat/completions 与 /v1/models，无需 API key，走 openai 通用代码路径。
		{TEXT("llamacpp"),     TEXT("llama.cpp (本地)"), TEXT("http://127.0.0.1:8088/v1"),   {}, false},
		{TEXT("lmstudio"),     TEXT("LM Studio (本地)"), TEXT("http://127.0.0.1:1234/v1"),   {}, false},
		{TEXT("custom"),       TEXT("自定义 (OpenAI 兼容)"), TEXT(""),                         {}, false},
	};

	return Presets;
}

const FMCPToolboxProviderPreset* FMCPToolboxProviderPreset::Find(const FString& ProviderId)
{
	for (const auto& P : GetAll())
	{
		if (P.Id == ProviderId) return &P;
	}
	return nullptr;
}

TArray<FString> FMCPToolboxProviderPreset::GetAllProviderIds()
{
	TArray<FString> Ids;
	for (const auto& P : GetAll()) Ids.Add(P.Id);
	return Ids;
}

// ============================================================================
// FMCPToolboxAPIKeyEntry
// ============================================================================
FString FMCPToolboxAPIKeyEntry::GetMaskedKey() const
{
	if (EncryptedKey.IsEmpty()) return TEXT("****");
	FString Decrypted;
	FBase64::Decode(EncryptedKey, Decrypted);
	if (Decrypted.Len() <= 8) return TEXT("****");
	return Decrypted.Left(4) + TEXT("****") + Decrypted.Right(4);
}

// ============================================================================
// FMCPToolboxAPIKeyManager
// ============================================================================
FMCPToolboxAPIKeyManager::FMCPToolboxAPIKeyManager() { Load(); }
FMCPToolboxAPIKeyManager::~FMCPToolboxAPIKeyManager() { Save(); }

FMCPToolboxAPIKeyManager& FMCPToolboxAPIKeyManager::Get()
{
	static FMCPToolboxAPIKeyManager Instance;
	return Instance;
}

FString FMCPToolboxAPIKeyManager::GetStoragePath() const
{
	return FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".mcptoolbox"), TEXT("keystore.dat"));
}

FString FMCPToolboxAPIKeyManager::DeriveKey() const
{
	// Simple key derivation from seed + login ID
	FString LoginId = FPlatformMisc::GetLoginId();
	FString Combined = FString(Seed) + LoginId;

	// Use a simple repeat-key for XOR encryption derived from the combined string
	// This is sufficient for local at-rest API key protection
	return FBase64::Encode(Combined);
}

FString FMCPToolboxAPIKeyManager::Encrypt(const FString& PlainText, const FString& Key)
{
	// XOR-based encryption with key cycling
	FString Result = PlainText;
	TArray<uint8> KeyBytes;
	FTCHARToUTF8 KeyConv(*Key);
	KeyBytes.Append((const uint8*)KeyConv.Get(), KeyConv.Length());

	for (int32 i = 0; i < Result.Len(); ++i)
	{
		Result[i] = Result[i] ^ (TCHAR)KeyBytes[i % KeyBytes.Num()];
	}
	return FBase64::Encode(Result);
}

FString FMCPToolboxAPIKeyManager::Decrypt(const FString& CipherText, const FString& Key)
{
	FString Decoded;
	FBase64::Decode(CipherText, Decoded);
	return Encrypt(Decoded, Key); // XOR twice = original
}

void FMCPToolboxAPIKeyManager::Load()
{
	FScopeLock ScopeLock(&Lock);
	bLoaded = true;
	FString Path = GetStoragePath();
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return;

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	for (const auto& Pair : Json->Values)
	{
		Store.Add(FString(Pair.Key), Pair.Value->AsString());
	}
}

void FMCPToolboxAPIKeyManager::Save()
{
	FScopeLock ScopeLock(&Lock);
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	for (const auto& Pair : Store)
	{
		Json->SetStringField(Pair.Key, Pair.Value);
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	FString Dir = FPaths::GetPath(GetStoragePath());
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Output, *GetStoragePath());
}

bool FMCPToolboxAPIKeyManager::StoreKey(const FString& KeyId, const FString& ApiKey)
{
	FScopeLock ScopeLock(&Lock);
	FString Enc = Encrypt(ApiKey, DeriveKey());
	Store.Add(KeyId, Enc);
	Save();
	return true;
}

bool FMCPToolboxAPIKeyManager::RetrieveKey(const FString& KeyId, FString& OutApiKey)
{
	FScopeLock ScopeLock(&Lock);
	const FString* Found = Store.Find(KeyId);
	if (!Found) return false;
	OutApiKey = Decrypt(*Found, DeriveKey());
	return true;
}

bool FMCPToolboxAPIKeyManager::DeleteKey(const FString& KeyId)
{
	FScopeLock ScopeLock(&Lock);
	if (Store.Remove(KeyId) > 0) { Save(); return true; }
	return false;
}

bool FMCPToolboxAPIKeyManager::KeyExists(const FString& KeyId) const
{
	FScopeLock ScopeLock(&Lock);
	return Store.Contains(KeyId);
}

// ============================================================================
// FMCPToolboxAPIManager
// ============================================================================
FMCPToolboxAPIManager::FMCPToolboxAPIManager() { LoadEntries(); }
FMCPToolboxAPIManager::~FMCPToolboxAPIManager()
{
	for (auto& Req : PendingVisionRequests)
		if (Req.IsValid()) Req->CancelRequest();
	PendingVisionRequests.Empty();
}

FMCPToolboxAPIManager& FMCPToolboxAPIManager::Get()
{
	static FMCPToolboxAPIManager Instance;
	return Instance;
}

FString FMCPToolboxAPIManager::GetEntriesPath() const
{
	return FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".mcptoolbox"), TEXT("settings.json"));
}

void FMCPToolboxAPIManager::LoadEntries()
{
	FScopeLock ScopeLock(&Lock);
	FString Path = GetEntriesPath();
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return;

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Json->TryGetArrayField(TEXT("entries"), Arr)) return;

	Entries.Empty();
	for (const auto& Val : *Arr)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Val->TryGetObject(Obj)) continue;

		FMCPToolboxAPIKeyEntry Entry;
		(*Obj)->TryGetStringField(TEXT("id"), Entry.Id);
		(*Obj)->TryGetStringField(TEXT("provider_id"), Entry.ProviderId);
		(*Obj)->TryGetStringField(TEXT("provider_name"), Entry.ProviderName);
		(*Obj)->TryGetStringField(TEXT("base_url"), Entry.BaseURL);
		(*Obj)->TryGetStringField(TEXT("model_id"), Entry.ModelId);
		(*Obj)->TryGetStringField(TEXT("encrypted_key"), Entry.EncryptedKey);
		(*Obj)->TryGetBoolField(TEXT("is_custom"), Entry.bIsCustom);

		FString TimeStr;
		if ((*Obj)->TryGetStringField(TEXT("created_at"), TimeStr))
			FDateTime::ParseIso8601(*TimeStr, Entry.CreatedAt);

		if (!Entry.Id.IsEmpty()) Entries.Add(Entry);
	}

	Json->TryGetStringField(TEXT("active_entry"), ActiveEntryId);
}

void FMCPToolboxAPIManager::SaveEntries()
{
	FScopeLock ScopeLock(&Lock);
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> Arr;

	for (const auto& Entry : Entries)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("id"), Entry.Id);
		Obj->SetStringField(TEXT("provider_id"), Entry.ProviderId);
		Obj->SetStringField(TEXT("provider_name"), Entry.ProviderName);
		Obj->SetStringField(TEXT("base_url"), Entry.BaseURL);
		Obj->SetStringField(TEXT("model_id"), Entry.ModelId);
		Obj->SetStringField(TEXT("encrypted_key"), Entry.EncryptedKey);
		Obj->SetBoolField(TEXT("is_custom"), Entry.bIsCustom);
		Obj->SetStringField(TEXT("created_at"), Entry.CreatedAt.ToIso8601());
		Arr.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	Json->SetArrayField(TEXT("entries"), Arr);
	Json->SetStringField(TEXT("active_entry"), ActiveEntryId);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	FString Dir = FPaths::GetPath(GetEntriesPath());
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Output, *GetEntriesPath());
}

void FMCPToolboxAPIManager::AddEntry(const FString& ProviderId, const FString& ModelId, const FString& ApiKey,
                                     const FString& BaseURLOverride)
{
	// 查找预设
	const FMCPToolboxProviderPreset* Preset = FMCPToolboxProviderPreset::Find(ProviderId);

	FMCPToolboxAPIKeyEntry Entry;
	Entry.Id = FGuid::NewGuid().ToString();
	Entry.ProviderId = ProviderId;
	Entry.ProviderName = Preset ? Preset->Name : ProviderId;
	// BaseURL priority: explicit override > preset > empty.
	// The override is essential for local providers (llama.cpp/LM Studio) where the
	// user may run the server on a non-default port, and for the "custom" preset which
	// has no default URL. Previously the UI's BaseURL input was silently discarded here.
	Entry.BaseURL = !BaseURLOverride.IsEmpty() ? BaseURLOverride :
	                (Preset ? Preset->BaseURL : TEXT(""));
	Entry.ModelId = ModelId;
	Entry.bIsCustom = (Preset == nullptr);
	Entry.CreatedAt = FDateTime::Now();

	// 本地Base64编码存储密钥（无需强加密，仅防窥视）
	Entry.EncryptedKey = FBase64::Encode(ApiKey);

	UE_LOG(LogMCPToolbox, Log, TEXT("[APIManager] 添加API密钥: Provider=%s, Model=%s, BaseURL=%s, EntryId=%s"),
		*ProviderId, *ModelId, *Entry.BaseURL, *Entry.Id);

	{
		FScopeLock ScopeLock(&Lock);
		Entries.Add(Entry);
		ActiveEntryId = Entry.Id;
		SaveEntries();
	}

	OnEntriesChanged.Broadcast();
}

void FMCPToolboxAPIManager::RemoveEntry(const FString& EntryId)
{
	{
		FScopeLock ScopeLock(&Lock);
		Entries.RemoveAll([&](const FMCPToolboxAPIKeyEntry& E) { return E.Id == EntryId; });
		if (ActiveEntryId == EntryId)
		{
			ActiveEntryId = Entries.Num() > 0 ? Entries.Last().Id : TEXT("");
		}
		SaveEntries();
	}

	OnEntriesChanged.Broadcast();
}

const TArray<FMCPToolboxAPIKeyEntry>& FMCPToolboxAPIManager::GetEntries() const
{
	return Entries;
}

const FMCPToolboxAPIKeyEntry* FMCPToolboxAPIManager::GetEntry(const FString& EntryId) const
{
	for (const auto& E : Entries)
		if (E.Id == EntryId) return &E;
	return nullptr;
}

const FMCPToolboxAPIKeyEntry* FMCPToolboxAPIManager::GetActiveEntry() const
{
	if (!ActiveEntryId.IsEmpty())
	{
		const FMCPToolboxAPIKeyEntry* Found = GetEntry(ActiveEntryId);
		if (Found) return Found;
	}
	return Entries.Num() > 0 ? &Entries.Last() : nullptr;
}

void FMCPToolboxAPIManager::SetActiveEntry(const FString& EntryId)
{
	ActiveEntryId = EntryId;
	SaveEntries();
	OnEntriesChanged.Broadcast();
}

// ---- 视觉支持检测 ----
void FMCPToolboxAPIManager::CheckVisionSupport(const FString& ProviderId)
{
	const FMCPToolboxProviderPreset* Preset = FMCPToolboxProviderPreset::Find(ProviderId);
	if (!Preset || Preset->BaseURL.IsEmpty()) return;

	FString URL = Preset->BaseURL + TEXT("/models");
	auto Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(URL);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(10.0f);

	FString ProviderIdCopy = ProviderId;
	Request->OnProcessRequestComplete().BindRaw(this, &FMCPToolboxAPIManager::OnVisionResponse, ProviderIdCopy);

	PendingVisionRequests.Add(Request);
	Request->ProcessRequest();
}

void FMCPToolboxAPIManager::OnVisionResponse(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk, FString ProviderId)
{
	PendingVisionRequests.Remove(Req);

	bool bVision = false;
	if (bOk && Resp.IsValid())
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
			if (Json->TryGetArrayField(TEXT("data"), Data) || Json->TryGetArrayField(TEXT("models"), Data))
			{
				for (const auto& ModelVal : *Data)
				{
					const TSharedPtr<FJsonObject>* ModelObj = nullptr;
					if (!ModelVal->TryGetObject(ModelObj)) continue;
					FString ModelId;
					(*ModelObj)->TryGetStringField(TEXT("id"), ModelId);

					const TSharedPtr<FJsonObject>* Cap = nullptr;
					bool SupportsImages = false;
					if ((*ModelObj)->TryGetObjectField(TEXT("capabilities"), Cap))
						(*Cap)->TryGetBoolField(TEXT("vision"), SupportsImages);
					if (!SupportsImages && (*ModelObj)->HasField(TEXT("vision")))
						(*ModelObj)->TryGetBoolField(TEXT("vision"), SupportsImages);

					VisionCache.FindOrAdd(ProviderId).Add(ModelId, SupportsImages);
					if (SupportsImages) bVision = true;
				}
			}
		}
	}

	OnVisionCheckResult.ExecuteIfBound(ProviderId, bVision);
}

bool FMCPToolboxAPIManager::SupportsVision(const FString& ProviderId, const FString& ModelId) const
{
	const TMap<FString, bool>* ProviderCache = VisionCache.Find(ProviderId);
	if (!ProviderCache) return false;
	const bool* Result = ProviderCache->Find(ModelId);
	return Result ? *Result : false;
}

// ---- 导入/导出 ----
bool FMCPToolboxAPIManager::ExportToJson(const FString& FilePath)
{
	FScopeLock ScopeLock(&Lock);
	return SaveEntries(), true; // SaveEntries writes to default path; for custom path, copy logic
}

bool FMCPToolboxAPIManager::ImportFromJson(const FString& FilePath)
{
	// Simple import: copy file to default path then reload
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath)) return false;
	FFileHelper::SaveStringToFile(Content, *GetEntriesPath());
	LoadEntries();
	OnEntriesChanged.Broadcast();
	return true;
}
