#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"

class SWindow;

// ============================================================================
// SMCPToolboxHelpWidget - Help manual modal window
// ============================================================================

/**
 * A draggable, resizable modal help window for MCP Toolbox.
 * Contains three tabs: Quick Start, FAQ, and Command Reference.
 */
class MCPTOOLBOX_API SMCPToolboxHelpWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPToolboxHelpWidget) {}
	SLATE_END_ARGS()

	/** Construct the widget */
	void Construct(const FArguments& InArgs);

	/** Open the help window (with 300ms scale-in animation) */
	void OpenHelpWindow();

	/** Close the help window (with 300ms scale-out animation) */
	void CloseHelpWindow();

	/** Dynamically switch the displayed help content */
	void SetHelpContent(int32 TabIndex);

private:
	// ---- UI Construction ----

	/** Build the draggable title bar with close button */
	TSharedRef<SWidget> CreateTitleBar();

	/** Build the three-tab help content area inside a scroll box */
	TSharedRef<SWidget> CreateHelpContent();

	/** Build the Quick Start guide section */
	TSharedRef<SWidget> CreateQuickStartSection();

	/** Build the Image Generation section */
	TSharedRef<SWidget> CreateImageGenerationSection();

	/** Build the FAQ section with 12+ entries */
	TSharedRef<SWidget> CreateFAQSection();

	/** Build the Command Reference section */
	TSharedRef<SWidget> CreateCommandReferenceSection();

	/** Create a copyable code block with a "Copy" button */
	TSharedRef<SWidget> CreateCopyButton(const FString& TextToCopy);

	// ---- Helpers ----

	/** Create a styled section header (H2) */
	TSharedRef<SWidget> CreateSectionHeader(const FText& Title);

	/** Create a subsection header (H3) */
	TSharedRef<SWidget> CreateSubSectionHeader(const FText& Title);

	/** Create a numbered step line */
	TSharedRef<SWidget> CreateStep(int32 StepNumber, const FText& Text);

	/** Create a warning/important tip box (yellow background + warning icon) */
	TSharedRef<SWidget> CreateWarningBox(const FText& Text);

	/** Create a dark-background code block with monospace font */
	TSharedRef<SWidget> CreateCodeBlock(const FString& CodeText);

	/** Create a table row with two columns */
	TSharedRef<SWidget> CreateTableRow(const FText& Col1, const FText& Col2, const FText& Col3 = FText::GetEmpty());

	/** Create a bold text block */
	TSharedRef<SWidget> CreateBoldText(const FText& Text);

	/** Create a regular paragraph text block */
	TSharedRef<SWidget> CreateParagraph(const FText& Text);

	// ---- Callbacks ----

	/** Close button clicked */
	FReply OnCloseClicked();

	/** Tab button clicked */
	FReply OnTabClicked(int32 TabIndex);

	/** Copy button interaction */
	FReply OnCopyClicked(FString TextToCopy);

	/** Mouse button down on title bar for drag start */
	FReply OnTitleBarMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	/** Mouse move for window dragging */
	FReply OnTitleBarMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	/** Mouse button up to end drag */
	FReply OnTitleBarMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	/** Handle key down (ESC to close) */
	FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	// ---- Member Variables ----

	/** The hosting window for this help widget */
	TSharedPtr<SWindow> HelpWindow;

	/** Scroll box for the content area */
	TSharedPtr<SScrollBox> ContentScrollBox;

	/** Content container that holds the current tab content */
	TSharedPtr<SVerticalBox> ContentContainer;

	/** Currently active tab index (0=QuickStart, 1=FAQ, 2=CommandRef) */
	int32 ActiveTabIndex = 0;

	/** Whether we are currently dragging the window */
	bool bIsDragging = false;

	/** Mouse position offset when dragging started */
	FVector2D DragOffset = FVector2D::ZeroVector;

	/** Copy button text states (track per-code-block) */
	TMap<TSharedPtr<SButton>, FString> CopyButtonTextMap;

	/** Default window size */
	static constexpr float DefaultWindowWidth = 1000.0f;
	static constexpr float DefaultWindowHeight = 700.0f;
	static constexpr float MinWindowWidth = 800.0f;
	static constexpr float MinWindowHeight = 500.0f;
	static constexpr float MaxWindowWidth = 1920.0f;
	static constexpr float MaxWindowHeight = 1080.0f;

	/** Animation duration in seconds */
	static constexpr float AnimationDuration = 0.3f;
};
