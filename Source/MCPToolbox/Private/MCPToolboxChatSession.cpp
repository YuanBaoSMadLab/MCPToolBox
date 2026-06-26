#include "MCPToolboxChatSession.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

FString FMCPToolboxChatSessionManager::GetProjectName() const
{
	FString ProjectDir = FPaths::ProjectDir();
	FString ProjectName = FPaths::GetBaseFilename(ProjectDir);
	if (ProjectName.IsEmpty())
		ProjectName = TEXT("DefaultProject");
	return ProjectName;
}

FString FMCPToolboxChatSessionManager::GetMCPToolboxRootDir() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCPToolbox"));
}

FString FMCPToolboxChatSessionManager::GetSkillsDirectory() const
{
	return FPaths::Combine(GetMCPToolboxRootDir(), TEXT("skills"));
}

FString FMCPToolboxChatSession::GetPreviewText() const
{
	if (Messages.Num() == 0)
		return TEXT("新对话");
	
	for (const auto& Msg : Messages)
	{
		if (Msg.Role == EMCPToolboxMessageRole::User || Msg.Role == EMCPToolboxMessageRole::Assistant)
		{
			FString Preview = Msg.Content.Left(50);
			if (Msg.Content.Len() > 50)
				Preview += TEXT("...");
			return Preview;
		}
	}
	return TEXT("新对话");
}

bool FMCPToolboxChatSession::Serialize(FJsonObject& OutJson) const
{
	OutJson.SetStringField(TEXT("session_id"), SessionId);
	OutJson.SetStringField(TEXT("title"), Title);
	OutJson.SetStringField(TEXT("created_at"), CreatedAt.ToString());
	OutJson.SetStringField(TEXT("updated_at"), UpdatedAt.ToString());
	OutJson.SetStringField(TEXT("active_model_id"), ActiveModelId);

	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	for (const auto& Msg : Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShareable(new FJsonObject());
		MsgObj->SetNumberField(TEXT("role"), static_cast<int32>(Msg.Role));
		MsgObj->SetStringField(TEXT("content"), Msg.Content);
		MsgObj->SetStringField(TEXT("timestamp"), Msg.Timestamp.ToString());
		MsgObj->SetBoolField(TEXT("is_streaming"), Msg.bIsStreaming);
		MsgObj->SetBoolField(TEXT("has_image_attachment"), Msg.bHasImageAttachment);
		MsgObj->SetStringField(TEXT("model_name"), Msg.ModelName);
		
		TArray<TSharedPtr<FJsonValue>> ImageURIs;
		for (const auto& Uri : Msg.ImageDataURIs)
			ImageURIs.Add(MakeShareable(new FJsonValueString(Uri)));
		MsgObj->SetArrayField(TEXT("image_data_uris"), ImageURIs);
		
		TArray<TSharedPtr<FJsonValue>> Attachments;
		for (const auto& Att : Msg.FileAttachments)
			Attachments.Add(MakeShareable(new FJsonValueString(Att)));
		MsgObj->SetArrayField(TEXT("file_attachments"), Attachments);
		
		MessagesArray.Add(MakeShareable(new FJsonValueObject(MsgObj)));
	}
	OutJson.SetArrayField(TEXT("messages"), MessagesArray);
	return true;
}

bool FMCPToolboxChatSession::Deserialize(const FJsonObject& Json)
{
	Json.TryGetStringField(TEXT("session_id"), SessionId);
	Json.TryGetStringField(TEXT("title"), Title);
	
	FString CreatedStr, UpdatedStr;
	if (Json.TryGetStringField(TEXT("created_at"), CreatedStr))
		FDateTime::ParseIso8601(*CreatedStr, CreatedAt);
	if (Json.TryGetStringField(TEXT("updated_at"), UpdatedStr))
		FDateTime::ParseIso8601(*UpdatedStr, UpdatedAt);
	
	Json.TryGetStringField(TEXT("active_model_id"), ActiveModelId);

	const TArray<TSharedPtr<FJsonValue>>* MessagesArray;
	if (Json.TryGetArrayField(TEXT("messages"), MessagesArray))
	{
		for (const auto& MsgVal : *MessagesArray)
		{
			TSharedPtr<FJsonObject> MsgObj = MsgVal->AsObject();
			if (!MsgObj.IsValid()) continue;
			
			FMCPToolboxChatMessage Msg;
			int32 RoleInt = 0;
			MsgObj->TryGetNumberField(TEXT("role"), RoleInt);
			Msg.Role = static_cast<EMCPToolboxMessageRole>(RoleInt);
			MsgObj->TryGetStringField(TEXT("content"), Msg.Content);
			
			FString TimeStr;
			if (MsgObj->TryGetStringField(TEXT("timestamp"), TimeStr))
				FDateTime::ParseIso8601(*TimeStr, Msg.Timestamp);
			
			MsgObj->TryGetBoolField(TEXT("is_streaming"), Msg.bIsStreaming);
			MsgObj->TryGetBoolField(TEXT("has_image_attachment"), Msg.bHasImageAttachment);
			MsgObj->TryGetStringField(TEXT("model_name"), Msg.ModelName);
			
			const TArray<TSharedPtr<FJsonValue>>* ImageURIs;
			if (MsgObj->TryGetArrayField(TEXT("image_data_uris"), ImageURIs))
			{
				for (const auto& Uri : *ImageURIs)
					Msg.ImageDataURIs.Add(Uri->AsString());
			}
			
			const TArray<TSharedPtr<FJsonValue>>* Attachments;
			if (MsgObj->TryGetArrayField(TEXT("file_attachments"), Attachments))
			{
				for (const auto& Att : *Attachments)
					Msg.FileAttachments.Add(Att->AsString());
			}
			
			Messages.Add(Msg);
		}
	}
	return true;
}

FMCPToolboxChatSessionManager::FMCPToolboxChatSessionManager()
{
	LoadAllSessions();
	if (Sessions.Num() == 0)
	{
		CreateNewSession(TEXT("新对话"));
	}
	else
	{
		CurrentSessionId = Sessions[0]->SessionId;
	}
	RefreshSkills();
}

FMCPToolboxChatSessionManager::~FMCPToolboxChatSessionManager()
{
}

FMCPToolboxChatSessionManager& FMCPToolboxChatSessionManager::Get()
{
	static FMCPToolboxChatSessionManager Instance;
	return Instance;
}

FMCPToolboxChatSession* FMCPToolboxChatSessionManager::CreateNewSession(const FString& Title)
{
	TUniquePtr<FMCPToolboxChatSession> NewSession = MakeUnique<FMCPToolboxChatSession>(Title.IsEmpty() ? TEXT("新对话") : Title);
	FMCPToolboxChatSession* Ptr = NewSession.Get();
	Sessions.Insert(MoveTemp(NewSession), 0);
	CurrentSessionId = Ptr->SessionId;
	SaveSession(*Ptr);
	return Ptr;
}

FMCPToolboxChatSession* FMCPToolboxChatSessionManager::GetCurrentSession() const
{
	return GetSessionById(CurrentSessionId);
}

bool FMCPToolboxChatSessionManager::SetCurrentSession(const FString& SessionId)
{
	if (GetSessionById(SessionId))
	{
		CurrentSessionId = SessionId;
		return true;
	}
	return false;
}

void FMCPToolboxChatSessionManager::DeleteSession(const FString& SessionId)
{
	for (int32 i = 0; i < Sessions.Num(); ++i)
	{
		if (Sessions[i]->SessionId == SessionId)
		{
			FString FilePath = GetSessionFilePath(SessionId);
			IFileManager::Get().Delete(*FilePath);
			Sessions.RemoveAt(i);
			
			if (CurrentSessionId == SessionId)
			{
				if (Sessions.Num() > 0)
					CurrentSessionId = Sessions[0]->SessionId;
				else
					CurrentSessionId = TEXT("");
			}
			break;
		}
	}
}

TArray<FMCPToolboxChatSession*> FMCPToolboxChatSessionManager::GetAllSessions() const
{
	TArray<FMCPToolboxChatSession*> Result;
	for (const auto& Session : Sessions)
		Result.Add(Session.Get());
	return Result;
}

FMCPToolboxChatSession* FMCPToolboxChatSessionManager::GetSessionById(const FString& SessionId) const
{
	for (const auto& Session : Sessions)
	{
		if (Session->SessionId == SessionId)
			return Session.Get();
	}
	return nullptr;
}

FString FMCPToolboxChatSessionManager::GetSessionsDirectory() const
{
	return FPaths::Combine(GetMCPToolboxRootDir(), TEXT("projects"), GetProjectName(), TEXT("Sessions"));
}

FString FMCPToolboxChatSessionManager::GetSessionFilePath(const FString& SessionId) const
{
	return FPaths::Combine(GetSessionsDirectory(), SessionId + TEXT(".json"));
}

void FMCPToolboxChatSessionManager::SaveSession(FMCPToolboxChatSession& Session)
{
	FString Dir = GetSessionsDirectory();
	IFileManager::Get().MakeDirectory(*Dir, true);
	
	FString FilePath = GetSessionFilePath(Session.SessionId);
	
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Session.Serialize(*Json);
	
	FString JsonStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	
	FFileHelper::SaveStringToFile(JsonStr, *FilePath);
}

void FMCPToolboxChatSessionManager::SaveCurrentSession()
{
	FMCPToolboxChatSession* Session = GetCurrentSession();
	if (Session)
	{
		Session->UpdatedAt = FDateTime::Now();
		SaveSession(*Session);
	}
}

void FMCPToolboxChatSessionManager::LoadAllSessions()
{
	Sessions.Empty();
	
	FString Dir = GetSessionsDirectory();
	if (!IFileManager::Get().DirectoryExists(*Dir))
		return;
	
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), false, false);
	
	for (const FString& File : Files)
	{
		LoadSessionFromFile(Dir / File);
	}
	
	Sessions.Sort([](const TUniquePtr<FMCPToolboxChatSession>& A, const TUniquePtr<FMCPToolboxChatSession>& B) {
		return A->UpdatedAt > B->UpdatedAt;
	});
}

void FMCPToolboxChatSessionManager::LoadSessionFromFile(const FString& FilePath)
{
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *FilePath))
		return;
	
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		return;
	
	TUniquePtr<FMCPToolboxChatSession> Session = MakeUnique<FMCPToolboxChatSession>();
	if (Session->Deserialize(*Json))
	{
		Sessions.Add(MoveTemp(Session));
	}
}

void FMCPToolboxChatSessionManager::AddMessageToCurrentSession(const FMCPToolboxChatMessage& Message)
{
	FMCPToolboxChatSession* Session = GetCurrentSession();
	if (Session)
	{
		Session->Messages.Add(Message);
		Session->UpdatedAt = FDateTime::Now();
		
		if (Session->Title == TEXT("新对话") || Session->Title.IsEmpty())
		{
			if (Message.Role == EMCPToolboxMessageRole::User)
			{
				Session->Title = Message.Content.Left(30);
				if (Message.Content.Len() > 30)
					Session->Title += TEXT("...");
			}
		}
		
		SaveSession(*Session);
	}
}

void FMCPToolboxChatSessionManager::ClearCurrentSessionMessages()
{
	FMCPToolboxChatSession* Session = GetCurrentSession();
	if (Session)
	{
		Session->Messages.Empty();
		Session->UpdatedAt = FDateTime::Now();
		SaveSession(*Session);
	}
}

void FMCPToolboxChatSessionManager::RefreshSkills()
{
	CachedSkills.Empty();
	bSkillsCached = true;

	FString SkillsDir = GetSkillsDirectory();
	if (!IFileManager::Get().DirectoryExists(*SkillsDir))
	{
		IFileManager::Get().MakeDirectory(*SkillsDir, true);
		return;
	}

	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *(SkillsDir / TEXT("*")), false, true);

	for (const FString& SubDir : SubDirs)
	{
		FString SkillDir = SkillsDir / SubDir;
		FString ManifestPath = SkillDir / TEXT("skill.json");
		if (IFileManager::Get().FileExists(*ManifestPath))
		{
			FMCPToolboxSkillInfo Info;
			if (LoadSkillManifest(ManifestPath, Info))
			{
				Info.DirectoryPath = SkillDir;
				Info.bIsInstalled = true;
				CachedSkills.Add(Info);
			}
		}
	}
}

TArray<FMCPToolboxSkillInfo> FMCPToolboxChatSessionManager::GetInstalledSkills() const
{
	if (!bSkillsCached)
	{
		const_cast<FMCPToolboxChatSessionManager*>(this)->RefreshSkills();
	}
	return CachedSkills;
}

bool FMCPToolboxChatSessionManager::LoadSkillManifest(const FString& ManifestPath, FMCPToolboxSkillInfo& OutInfo) const
{
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *ManifestPath))
		return false;

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		return false;

	Json->TryGetStringField(TEXT("id"), OutInfo.SkillId);
	Json->TryGetStringField(TEXT("name"), OutInfo.Name);
	Json->TryGetStringField(TEXT("description"), OutInfo.Description);
	Json->TryGetStringField(TEXT("version"), OutInfo.Version);
	Json->TryGetStringField(TEXT("author"), OutInfo.Author);

	if (OutInfo.SkillId.IsEmpty())
		return false;
	if (OutInfo.Name.IsEmpty())
		OutInfo.Name = OutInfo.SkillId;

	return true;
}

bool FMCPToolboxChatSessionManager::InstallSkillFromDirectory(const FString& SourceDir)
{
	if (!IFileManager::Get().DirectoryExists(*SourceDir))
		return false;

	FString ManifestPath = SourceDir / TEXT("skill.json");
	if (!IFileManager::Get().FileExists(*ManifestPath))
		return false;

	FMCPToolboxSkillInfo Info;
	if (!LoadSkillManifest(ManifestPath, Info))
		return false;

	FString DestDir = GetSkillsDirectory() / Info.SkillId;

	if (IFileManager::Get().DirectoryExists(*DestDir))
	{
		IFileManager::Get().DeleteDirectory(*DestDir, false, true);
	}

	IFileManager::Get().MakeDirectory(*DestDir, true);

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*.*"), true, false);

	for (const FString& File : Files)
	{
		FString RelPath = File;
		RelPath.RemoveFromStart(SourceDir);
		if (RelPath.StartsWith(TEXT("/")) || RelPath.StartsWith(TEXT("\\")))
			RelPath.RemoveAt(0);

		FString DestFile = DestDir / RelPath;
		FString DestFileDir = FPaths::GetPath(DestFile);
		IFileManager::Get().MakeDirectory(*DestFileDir, true);
		IFileManager::Get().Copy(*DestFile, *File);
	}

	bSkillsCached = false;
	return true;
}

bool FMCPToolboxChatSessionManager::UninstallSkill(const FString& SkillId)
{
	FString SkillDir = GetSkillsDirectory() / SkillId;
	if (!IFileManager::Get().DirectoryExists(*SkillDir))
		return false;

	IFileManager::Get().DeleteDirectory(*SkillDir, false, true);
	bSkillsCached = false;
	return true;
}
