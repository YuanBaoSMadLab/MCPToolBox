#include "MCPToolboxMemoryManager.h"
#include "MCPToolbox.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"

// ============================================================================
// FMCPToolboxMemoryManager
// ============================================================================
FMCPToolboxMemoryManager& FMCPToolboxMemoryManager::Get()
{
	static FMCPToolboxMemoryManager Instance;
	return Instance;
}

void FMCPToolboxMemoryManager::Initialize()
{
	FScopeLock ScopeLock(&Lock);

	// Memory root: ~/.mcptoolbox/memory/
	MemoryRoot = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".mcptoolbox"), TEXT("memory"));

	// Ensure directory exists
	IFileManager::Get().MakeDirectory(*MemoryRoot, true);

	// Ensure MEMORY.md exists (create from template if needed)
	FString IndexPath = FPaths::Combine(MemoryRoot, TEXT("MEMORY.md"));
	if (!FPaths::FileExists(IndexPath))
	{
		FString Template = TEXT("# Memory Index\n\n"
			"> Pointers only — actual content lives in linked files.\n"
			"> 软上限 150行/20KB, 硬上限 200行/25KB.\n\n"
			"## user\n"
			"<!-- user identity, role, preferences -->\n\n"
			"## feedback\n"
			"<!-- procedural memory: how the agent should behave -->\n\n"
			"## project\n"
			"<!-- current project state not in code/git -->\n\n"
			"## reference\n"
			"<!-- pointers to external resources, URLs, paths -->\n");
		FFileHelper::SaveStringToFile(Template, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8);
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Memory] 初始化记忆系统: %s"), *MemoryRoot);
	LoadIndex();
}

// ============================================================================
// Index Operations
// ============================================================================
void FMCPToolboxMemoryManager::LoadIndex()
{
	FScopeLock ScopeLock(&Lock);

	FString IndexPath = FPaths::Combine(MemoryRoot, TEXT("MEMORY.md"));
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *IndexPath))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Memory] 无法加载MEMORY.md"));
		return;
	}

	IndexLines.Empty();
	Content.ParseIntoArrayLines(IndexLines);

	LoadedNotes.Empty();

	// Parse all pointer lines
	for (const FString& Line : IndexLines)
	{
		FString Title, Slug, Hook;
		if (ParseIndexLine(Line, Title, Slug, Hook))
		{
			// Try to load the note
			FMCPToolboxMemoryNote Note;
			if (ReadNote(Slug, Note))
			{
				LoadedNotes.Add(Slug, MoveTemp(Note));
			}
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("[Memory] 加载了 %d 条记忆"), LoadedNotes.Num());

	// Check caps
	int32 LineCount = 0;
	int32 ByteCount = 0;
	FString DummyTitle, DummySlug, DummyHook;
	for (const FString& Line : IndexLines)
	{
		ByteCount += Line.Len() + 1; // +1 for newline
		if (ParseIndexLine(Line, DummyTitle, DummySlug, DummyHook)) LineCount++;
	}
	if (LineCount > WarnCapLines || ByteCount > WarnCapBytes)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("[Memory] 索引接近上限: %d行, %d字节"), LineCount, ByteCount);
	}
}

void FMCPToolboxMemoryManager::AddToIndex(const FMCPToolboxMemoryNote& Note)
{
	FScopeLock ScopeLock(&Lock);

	FString TypeHeading = GetTypeHeading(Note.Type);
	FString NewLine = FString::Printf(TEXT("- [%s](%s) -- %s"), *Note.Name, *Note.GetFilename(), *Note.Description);

	// Find the correct section to insert under
	int32 InsertIdx = INDEX_NONE;
	bool bFoundSection = false;
	for (int32 i = 0; i < IndexLines.Num(); ++i)
	{
		if (IndexLines[i].StartsWith(TEXT("## ")) && IndexLines[i].Contains(TypeHeading))
		{
			bFoundSection = true;
			// Find first blank line or next section after this heading
			for (int32 j = i + 1; j < IndexLines.Num(); ++j)
			{
				if (IndexLines[j].StartsWith(TEXT("## ")))
				{
					InsertIdx = j; // insert before next section
					break;
				}
			}
			if (InsertIdx == INDEX_NONE)
			{
				InsertIdx = IndexLines.Num(); // append at end
			}
			break;
		}
	}

	if (InsertIdx != INDEX_NONE)
	{
		IndexLines.Insert(NewLine, InsertIdx);
	}

	// Check if we need to remove old pointer for same slug
	for (int32 i = 0; i < IndexLines.Num(); ++i)
	{
		FString Title, Slug, Hook;
		if (ParseIndexLine(IndexLines[i], Title, Slug, Hook) && Slug == Note.Name)
		{
			if (i != InsertIdx) // remove duplicate at old position
			{
				IndexLines.RemoveAt(i);
				if (i < InsertIdx) InsertIdx--;
			}
		}
	}

	// Write back
	FString IndexPath = FPaths::Combine(MemoryRoot, TEXT("MEMORY.md"));
	FFileHelper::SaveStringToFile(FString::Join(IndexLines, TEXT("\n")) + TEXT("\n"), *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8);

	LoadedNotes.Add(Note.Name, Note);
}

void FMCPToolboxMemoryManager::RemoveFromIndex(const FString& Slug)
{
	FScopeLock ScopeLock(&Lock);

	for (int32 i = IndexLines.Num() - 1; i >= 0; --i)
	{
		FString Title, LineSlug, Hook;
		if (ParseIndexLine(IndexLines[i], Title, LineSlug, Hook) && LineSlug == Slug)
		{
			IndexLines.RemoveAt(i);
		}
	}

	FString IndexPath = FPaths::Combine(MemoryRoot, TEXT("MEMORY.md"));
	FFileHelper::SaveStringToFile(FString::Join(IndexLines, TEXT("\n")) + TEXT("\n"), *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8);

	LoadedNotes.Remove(Slug);
}

FString FMCPToolboxMemoryManager::GetIndexAsContext() const
{
	FScopeLock ScopeLock(&Lock);
	FString Result;
	for (const FString& Line : IndexLines)
	{
		// Only include non-empty non-comment lines
		FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("#")) && !Trimmed.StartsWith(TEXT(">")))
		{
			Result += Line + TEXT("\n");
		}
	}
	// Apply hard cap
	if (Result.Len() > HardCapBytes)
	{
		Result = Result.Left(HardCapBytes);
	}
	return Result;
}

// ============================================================================
// Note CRUD
// ============================================================================
bool FMCPToolboxMemoryManager::WriteNote(const FMCPToolboxMemoryNote& Note)
{
	FScopeLock ScopeLock(&Lock);

	FString FilePath = FPaths::Combine(MemoryRoot, Note.GetFilename());

	// Build content with frontmatter
	FString TypeStr;
	switch (Note.Type)
	{
	case EMCPToolboxMemoryType::User:      TypeStr = TEXT("user");      break;
	case EMCPToolboxMemoryType::Feedback:  TypeStr = TEXT("feedback");   break;
	case EMCPToolboxMemoryType::Project:   TypeStr = TEXT("project");    break;
	case EMCPToolboxMemoryType::Reference: TypeStr = TEXT("reference");   break;
	}

	FString NowStr = FDateTime::Now().ToString(TEXT("%Y-%m-%d"));

	FString Content = FString::Printf(
		TEXT("---\n"
		     "name: %s\n"
		     "description: %s\n"
		     "type: %s\n"
		     "created: %s\n"
		     "updated: %s\n"
		     "---\n\n"
		     "%s\n"),
		*Note.Name,
		*Note.Description,
		*TypeStr,
		Note.Created.IsEmpty() ? *NowStr : *Note.Created,
		*NowStr,
		*Note.Body
	);

	bool bSuccess = FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8);
	if (bSuccess)
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("[Memory] 写入记忆: %s"), *Note.Name);
	}
	else
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("[Memory] 写入失败: %s"), *Note.Name);
	}
	return bSuccess;
}

bool FMCPToolboxMemoryManager::ReadNote(const FString& Slug, FMCPToolboxMemoryNote& OutNote)
{
	FString FilePath = FPaths::Combine(MemoryRoot, Slug + TEXT(".md"));
	if (!FPaths::FileExists(FilePath))
	{
		return false;
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		return false;
	}

	return ParseFrontmatter(Content, OutNote);
}

bool FMCPToolboxMemoryManager::DeleteNote(const FString& Slug)
{
	FString FilePath = FPaths::Combine(MemoryRoot, Slug + TEXT(".md"));
	if (IFileManager::Get().Delete(*FilePath))
	{
		RemoveFromIndex(Slug);
		UE_LOG(LogMCPToolbox, Log, TEXT("[Memory] 删除记忆: %s"), *Slug);
		return true;
	}
	return false;
}

const FMCPToolboxMemoryNote* FMCPToolboxMemoryManager::FindSimilar(const FString& Description) const
{
	for (const auto& Pair : LoadedNotes)
	{
		// Simple similarity: check if descriptions share significant words
		if (Pair.Value.Description.Contains(Description.Left(10)))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

// ============================================================================
// Smart Memory Extraction
// ============================================================================
int32 FMCPToolboxMemoryManager::ExtractMemoriesFromResponse(const FString& AIResponse)
{
	FScopeLock ScopeLock(&Lock);
	int32 Count = 0;

	// Patterns to detect: "记住：xxx", "重要：xxx", "偏好：xxx", "MEMORY:xxx"
	TArray<FString> Lines;
	AIResponse.ParseIntoArrayLines(Lines);

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty()) continue;

		bool bIsMemory = false;
		FString Fact;
		EMCPToolboxMemoryType MemType = EMCPToolboxMemoryType::Reference;

		if (Line.StartsWith(TEXT("记住：")) || Line.StartsWith(TEXT("记住:")))
		{
			Line.RightChopInline(3);
			bIsMemory = true;
			MemType = EMCPToolboxMemoryType::Feedback;
		}
		else if (Line.StartsWith(TEXT("重要：")) || Line.StartsWith(TEXT("重要:")))
		{
			Line.RightChopInline(3);
			bIsMemory = true;
			MemType = EMCPToolboxMemoryType::Project;
		}
		else if (Line.StartsWith(TEXT("偏好：")) || Line.StartsWith(TEXT("偏好:")))
		{
			Line.RightChopInline(3);
			bIsMemory = true;
			MemType = EMCPToolboxMemoryType::User;
		}
		else if (Line.StartsWith(TEXT("MEMORY:")) || Line.StartsWith(TEXT("MEMORY：")))
		{
			Line.RightChopInline(7);
			bIsMemory = true;
		}

		if (bIsMemory && !Line.IsEmpty())
		{
			// Create slug from content
			FString Slug = Line.Left(30);
			Slug.ReplaceInline(TEXT(" "), TEXT("-"));
			Slug.ReplaceInline(TEXT("/"), TEXT("-"));
			Slug.ReplaceInline(TEXT("\\"), TEXT("-"));
			Slug = Slug.ToLower();

			// Check for duplicates
			if (FindSimilar(Slug)) continue;

			FMCPToolboxMemoryNote Note;
			Note.Name = Slug;
			Note.Description = Line.Left(100);
			Note.Type = MemType;
			Note.Body = Line;

			if (WriteNote(Note))
			{
				AddToIndex(Note);
				Count++;
			}
		}
	}

	if (Count > 0)
	{
		UE_LOG(LogMCPToolbox, Log, TEXT("[Memory] 从回复中提取了 %d 条新记忆"), Count);
	}

	return Count;
}

FString FMCPToolboxMemoryManager::BuildMemoryContext() const
{
	FScopeLock ScopeLock(&Lock);

	if (LoadedNotes.Num() == 0) return TEXT("");

	FString Context;
	Context += TEXT("## 用户记忆\n\n");

	// Group by type
	TMap<EMCPToolboxMemoryType, TArray<const FMCPToolboxMemoryNote*>> Grouped;
	for (const auto& Pair : LoadedNotes)
	{
		Grouped.FindOrAdd(Pair.Value.Type).Add(&Pair.Value);
	}

	// Output each group
	auto AppendGroup = [&](EMCPToolboxMemoryType Type, const TCHAR* Label)
	{
		const TArray<const FMCPToolboxMemoryNote*>* Notes = Grouped.Find(Type);
		if (!Notes || Notes->Num() == 0) return;

		Context += FString::Printf(TEXT("### %s\n\n"), Label);
		for (const FMCPToolboxMemoryNote* Note : *Notes)
		{
			Context += FString::Printf(TEXT("- **%s**: %s\n"), *Note->Description, *Note->Body.Left(200));
		}
		Context += TEXT("\n");
	};

	AppendGroup(EMCPToolboxMemoryType::User,     TEXT("用户偏好"));
	AppendGroup(EMCPToolboxMemoryType::Feedback, TEXT("工作方式约定"));
	AppendGroup(EMCPToolboxMemoryType::Project,  TEXT("项目信息"));
	AppendGroup(EMCPToolboxMemoryType::Reference,TEXT("参考资料"));

	return Context;
}

// ============================================================================
// Helpers
// ============================================================================
FString FMCPToolboxMemoryManager::GetTypeHeading(EMCPToolboxMemoryType Type) const
{
	switch (Type)
	{
	case EMCPToolboxMemoryType::User:      return TEXT("user");
	case EMCPToolboxMemoryType::Feedback:  return TEXT("feedback");
	case EMCPToolboxMemoryType::Project:   return TEXT("project");
	case EMCPToolboxMemoryType::Reference: return TEXT("reference");
	}
	return TEXT("");
}

bool FMCPToolboxMemoryManager::ParseFrontmatter(const FString& Content, FMCPToolboxMemoryNote& OutNote)
{
	// Find frontmatter boundaries (--- ... ---)
	int32 FirstFence = Content.Find(TEXT("---"));
	if (FirstFence == INDEX_NONE) return false;

	int32 SecondFence = Content.Find(TEXT("---"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstFence + 3);
	if (SecondFence == INDEX_NONE) return false;

	FString Frontmatter = Content.Mid(FirstFence + 3, SecondFence - FirstFence - 3);
	OutNote.Body = Content.Mid(SecondFence + 3).TrimStart();

	// Parse key: value lines
	TArray<FString> Lines;
	Frontmatter.ParseIntoArrayLines(Lines);

	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine.TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#"))) continue;

		int32 ColonIdx = Line.Find(TEXT(":"));
		if (ColonIdx == INDEX_NONE) continue;

		FString Key = Line.Left(ColonIdx).TrimStartAndEnd();
		FString Value = Line.Mid(ColonIdx + 1).TrimStartAndEnd();

		// Remove surrounding quotes if present
		if (Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}

		if (Key == TEXT("name"))        OutNote.Name = Value;
		else if (Key == TEXT("description")) OutNote.Description = Value;
		else if (Key == TEXT("type"))
		{
			if (Value == TEXT("user"))       OutNote.Type = EMCPToolboxMemoryType::User;
			else if (Value == TEXT("feedback"))  OutNote.Type = EMCPToolboxMemoryType::Feedback;
			else if (Value == TEXT("project"))   OutNote.Type = EMCPToolboxMemoryType::Project;
			else if (Value == TEXT("reference")) OutNote.Type = EMCPToolboxMemoryType::Reference;
		}
		else if (Key == TEXT("created"))  OutNote.Created = Value;
		else if (Key == TEXT("updated"))  OutNote.Updated = Value;
	}

	return !OutNote.Name.IsEmpty();
}

bool FMCPToolboxMemoryManager::ParseIndexLine(const FString& Line, FString& OutTitle, FString& OutSlug, FString& OutHook)
{
	// Pattern: "- [title](slug.md) -- hook"
	if (!Line.TrimStartAndEnd().StartsWith(TEXT("- ["))) return false;

	FString Trimmed = Line.TrimStartAndEnd();

	int32 TitleStart = Trimmed.Find(TEXT("["));
	int32 TitleEnd = Trimmed.Find(TEXT("]"));
	if (TitleStart == INDEX_NONE || TitleEnd == INDEX_NONE) return false;

	OutTitle = Trimmed.Mid(TitleStart + 1, TitleEnd - TitleStart - 1);

	int32 SlugStart = Trimmed.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, TitleEnd);
	int32 SlugEnd = Trimmed.Find(TEXT(".md)"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SlugStart > 0 ? SlugStart : TitleEnd);
	if (SlugStart == INDEX_NONE || SlugEnd == INDEX_NONE)
	{
		// Try just ")":
		SlugEnd = Trimmed.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SlugStart > 0 ? SlugStart : TitleEnd);
	}
	if (SlugStart == INDEX_NONE || SlugEnd == INDEX_NONE) return false;

	FString FullSlug = Trimmed.Mid(SlugStart + 1, SlugEnd - SlugStart - 1);
	// Strip .md extension
	if (FullSlug.EndsWith(TEXT(".md")))
	{
		OutSlug = FullSlug.Left(FullSlug.Len() - 3);
	}
	else
	{
		OutSlug = FullSlug;
	}

	// Hook: everything after " -- " (if present)
	int32 HookIdx = Trimmed.Find(TEXT(" -- "), ESearchCase::CaseSensitive, ESearchDir::FromStart, SlugEnd);
	if (HookIdx != INDEX_NONE)
	{
		OutHook = Trimmed.Mid(HookIdx + 4);
	}
	else
	{
		OutHook = TEXT("");
	}

	return true;
}
