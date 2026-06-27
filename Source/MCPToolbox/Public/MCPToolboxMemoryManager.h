#pragma once

#include "CoreMinimal.h"
#include "MCPToolboxMemoryManager.generated.h"

// ============================================================================
// Types
// ============================================================================
UENUM(BlueprintType)
enum class EMCPToolboxMemoryType : uint8
{
	User,
	Feedback,
	Project,
	Reference
};

// ============================================================================
// FMCPToolboxMemoryNote
// ============================================================================
USTRUCT(BlueprintType)
struct MCPTOOLBOX_API FMCPToolboxMemoryNote
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;        // kebab-case slug

	UPROPERTY()
	FString Description; // one-line hook

	UPROPERTY()
	EMCPToolboxMemoryType Type = EMCPToolboxMemoryType::Reference;

	UPROPERTY()
	FString Created;

	UPROPERTY()
	FString Updated;

	UPROPERTY()
	FString Body;        // the fact content (after frontmatter)

	FString GetFilename() const { return Name + TEXT(".md"); }
};

// ============================================================================
// FMCPToolboxMemoryManager - C++ rewrite of engramory
// ============================================================================
class MCPTOOLBOX_API FMCPToolboxMemoryManager
{
public:
	static FMCPToolboxMemoryManager& Get();

	/** Initialize with the memory root path (~/.mcptoolbox/memory/) */
	void Initialize();

	/** Get the memory root path */
	FString GetMemoryRoot() const { return MemoryRoot; }

	// ---- Index Operations ----

	/** Load the index (MEMORY.md) and parse all pointers */
	void LoadIndex();

	/** Add a pointer line to the index under the correct type section */
	void AddToIndex(const FMCPToolboxMemoryNote& Note);

	/** Remove a pointer line from the index */
	void RemoveFromIndex(const FString& Slug);

	/** Get all pointers as a compact string for AI context */
	FString GetIndexAsContext() const;

	// ---- Note CRUD ----

	/** Write a new memory note file */
	bool WriteNote(const FMCPToolboxMemoryNote& Note);

	/** Read a memory note file */
	bool ReadNote(const FString& Slug, FMCPToolboxMemoryNote& OutNote);

	/** Delete a memory note */
	bool DeleteNote(const FString& Slug);

	/** Check if a note already covers the same ground (dedup) */
	const FMCPToolboxMemoryNote* FindSimilar(const FString& Description) const;

	// ---- Smart Memory Extraction ----

	/**
	 * Extract important facts from an AI response and save as memories.
	 * Looks for patterns like:
	 * - "记住：..." / "重要：..." / "偏好：..."
	 * - Explicit fact statements
	 * Returns number of new memories created.
	 */
	int32 ExtractMemoriesFromResponse(const FString& AIResponse);

	/**
	 * Build a system prompt prefix with relevant memories.
	 * Returns a compact context string for the AI.
	 */
	FString BuildMemoryContext() const;

	// ---- Conversation Summary (Archive) ----

	/**
	 * Path to the single-index summary file (~/.mcptoolbox/conversation_summary.md).
	 * This file is overwritten on each summary and is what BuildSystemPrompt loads.
	 */
	FString GetConversationSummaryIndexPath() const;

	/**
	 * Path to the multi-file archive directory (~/.mcptoolbox/summaries/).
	 * Each archive call writes tools_<ts>.md and/or memory_<ts>.md here.
	 */
	FString GetConversationSummaryArchiveDir() const;

	/**
	 * Save a conversation summary.
	 * - Writes ~/.mcptoolbox/conversation_summary.md (overwrite) as the live index
	 *   containing both ToolsSummary and MemorySummary sections.
	 * - Writes timestamped archive copies to ~/.mcptoolbox/summaries/.
	 * Empty strings are skipped (only the non-empty type is saved).
	 */
	bool SaveConversationSummary(const FString& ToolsSummary, const FString& MemorySummary);

	/**
	 * Load the live conversation summary index file.
	 * OutToolsSummary / OutMemorySummary receive the parsed sections.
	 * Returns true if the index file existed and was read.
	 */
	bool LoadConversationSummary(FString& OutToolsSummary, FString& OutMemorySummary) const;

private:
	FMCPToolboxMemoryManager() = default;
	~FMCPToolboxMemoryManager() = default;
	FMCPToolboxMemoryManager(const FMCPToolboxMemoryManager&) = delete;
	FMCPToolboxMemoryManager& operator=(const FMCPToolboxMemoryManager&) = delete;

	/** Parse YAML-like frontmatter (restricted key: value format) */
	bool ParseFrontmatter(const FString& Content, FMCPToolboxMemoryNote& OutNote);

	/** Get the type section heading in MEMORY.md */
	FString GetTypeHeading(EMCPToolboxMemoryType Type) const;

	/** Parse index line: "- [title](slug.md) -- hook" */
	bool ParseIndexLine(const FString& Line, FString& OutTitle, FString& OutSlug, FString& OutHook);

	// ---- Data ----
	FString MemoryRoot;
	TMap<FString, FMCPToolboxMemoryNote> LoadedNotes; // slug -> note
	TArray<FString> IndexLines; // all lines from MEMORY.md
	mutable FCriticalSection Lock;

	static constexpr int32 HardCapLines = 200;
	static constexpr int32 HardCapBytes = 25600;
	static constexpr int32 WarnCapLines = 150;
	static constexpr int32 WarnCapBytes = 20480;
};
