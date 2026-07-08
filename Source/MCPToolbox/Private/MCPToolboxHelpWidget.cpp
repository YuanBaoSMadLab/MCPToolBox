// Copyright MCPToolbox. All Rights Reserved.

#include "MCPToolboxHelpWidget.h"
#include "SlateCore.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"

#define LOCTEXT_NAMESPACE "SMCPToolboxHelpWidget"

// ============================================================================
// SMCPToolboxHelpWidget::Construct
// ============================================================================

void SMCPToolboxHelpWidget::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		// Title bar
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			CreateTitleBar()
		]

		// Tab buttons row
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("MCPToolboxHelp_TabQuickStart", "快速入门"))
				.OnClicked(this, &SMCPToolboxHelpWidget::OnTabClicked, 0)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("MCPToolboxHelp_TabImageGen", "图片生成"))
				.OnClicked(this, &SMCPToolboxHelpWidget::OnTabClicked, 1)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("MCPToolboxHelp_TabFAQ", "常见问题"))
				.OnClicked(this, &SMCPToolboxHelpWidget::OnTabClicked, 2)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("MCPToolboxHelp_TabCommands", "命令参考"))
				.OnClicked(this, &SMCPToolboxHelpWidget::OnTabClicked, 3)
			]
		]

		// Scrollable content area
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ContentScrollBox, SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(ContentContainer, SVerticalBox)
			]
		]
	];

	SetHelpContent(0);
}

// ============================================================================
// Window Management
// ============================================================================

void SMCPToolboxHelpWidget::OpenHelpWindow()
{
	if (HelpWindow.IsValid())
	{
		HelpWindow->BringToFront();
		return;
	}

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("MCPToolboxHelp_WindowTitle", "MCP Toolbox - 帮助"))
		.ClientSize(FVector2D(DefaultWindowWidth, DefaultWindowHeight))
		.SupportsMaximize(true)
		.SupportsMinimize(true)
		.SizingRule(ESizingRule::UserSized)
		.MinWidth(MinWindowWidth)
		.MinHeight(MinWindowHeight)
		.MaxWidth(MaxWindowWidth)
		.MaxHeight(MaxWindowHeight);

	Window->SetContent(AsShared());

	FSlateApplication& SlateApp = FSlateApplication::Get();
	SlateApp.AddWindow(Window);

	FVector2D ScreenSize = SlateApp.GetPreferredWorkArea().GetSize();
	FVector2D WindowSize(DefaultWindowWidth, DefaultWindowHeight);
	Window->MoveWindowTo((ScreenSize - WindowSize) * 0.5f);

	HelpWindow = Window;
}

void SMCPToolboxHelpWidget::CloseHelpWindow()
{
	if (HelpWindow.IsValid())
	{
		HelpWindow->RequestDestroyWindow();
		HelpWindow.Reset();
	}
}

// ============================================================================
// Content Switching
// ============================================================================

void SMCPToolboxHelpWidget::SetHelpContent(int32 TabIndex)
{
	ActiveTabIndex = TabIndex;
	if (!ContentContainer.IsValid()) { return; }

	ContentContainer->ClearChildren();

	switch (TabIndex)
	{
	case 0:
		ContentContainer->AddSlot().AutoHeight().Padding(16.0f)[ CreateQuickStartSection() ];
		break;
	case 1:
		ContentContainer->AddSlot().AutoHeight().Padding(16.0f)[ CreateImageGenerationSection() ];
		break;
	case 2:
		ContentContainer->AddSlot().AutoHeight().Padding(16.0f)[ CreateFAQSection() ];
		break;
	case 3:
		ContentContainer->AddSlot().AutoHeight().Padding(16.0f)[ CreateCommandReferenceSection() ];
		break;
	}

	if (ContentScrollBox.IsValid())
	{
		ContentScrollBox->ScrollToStart();
	}
}

// ============================================================================
// Title Bar
// ============================================================================

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateTitleBar()
{
	return SNew(SBorder)
		.Padding(FMargin(8.0f, 4.0f))
		.OnMouseButtonDown_Lambda([this](const FGeometry& Geometry, const FPointerEvent& MouseEvent) -> FReply
		{
			return OnTitleBarMouseButtonDown(Geometry, MouseEvent);
		})
		.OnMouseMove_Lambda([this](const FGeometry& Geometry, const FPointerEvent& MouseEvent) -> FReply
		{
			return OnTitleBarMouseMove(Geometry, MouseEvent);
		})
		.OnMouseButtonUp_Lambda([this](const FGeometry& Geometry, const FPointerEvent& MouseEvent) -> FReply
		{
			return OnTitleBarMouseButtonUp(Geometry, MouseEvent);
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MCPToolboxHelp_TitleBar", "MCP Toolbox 帮助"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("MCPToolboxHelp_Close", "X"))
				.OnClicked(this, &SMCPToolboxHelpWidget::OnCloseClicked)
			]
		];
}

// ============================================================================
// Callbacks
// ============================================================================

FReply SMCPToolboxHelpWidget::OnCloseClicked()
{
	CloseHelpWindow();
	return FReply::Handled();
}

FReply SMCPToolboxHelpWidget::OnTabClicked(int32 TabIndex)
{
	SetHelpContent(TabIndex);
	return FReply::Handled();
}

FReply SMCPToolboxHelpWidget::OnCopyClicked(FString TextToCopy)
{
	FPlatformApplicationMisc::ClipboardCopy(*TextToCopy);
	return FReply::Handled();
}

FReply SMCPToolboxHelpWidget::OnTitleBarMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && HelpWindow.IsValid())
	{
		bIsDragging = true;
		DragOffset = MouseEvent.GetScreenSpacePosition() - HelpWindow->GetPositionInScreen();
		return FReply::Handled().CaptureMouse(AsShared());
	}
	return FReply::Unhandled();
}

FReply SMCPToolboxHelpWidget::OnTitleBarMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bIsDragging && HelpWindow.IsValid())
	{
		HelpWindow->MoveWindowTo(MouseEvent.GetScreenSpacePosition() - DragOffset);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SMCPToolboxHelpWidget::OnTitleBarMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bIsDragging && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bIsDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply SMCPToolboxHelpWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		CloseHelpWindow();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

// ============================================================================
// Helper Methods
// ============================================================================

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateSectionHeader(const FText& Title)
{
	return SNew(STextBlock)
		.Text(Title)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
		.Margin(FMargin(0.0f, 12.0f, 0.0f, 6.0f));
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateSubSectionHeader(const FText& Title)
{
	return SNew(STextBlock)
		.Text(Title)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
		.Margin(FMargin(0.0f, 8.0f, 0.0f, 4.0f));
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateStep(int32 StepNumber, const FText& Text)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("%d."), StepNumber)))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			.ColorAndOpacity(FLinearColor(0.2f, 0.5f, 1.0f))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(Text)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateWarningBox(const FText& Text)
{
	return SNew(SBorder)
		.Padding(FMargin(12.0f))
		.BorderBackgroundColor(FLinearColor(0.8f, 0.7f, 0.1f, 0.3f))
		[
			SNew(STextBlock)
			.Text(Text)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateCodeBlock(const FString& CodeText)
{
	return SNew(SBorder)
		.Padding(FMargin(12.0f))
		.BorderBackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(CodeText))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
				.ColorAndOpacity(FLinearColor(0.3f, 1.0f, 0.3f))
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				CreateCopyButton(CodeText)
			]
		];
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateTableRow(const FText& Col1, const FText& Col2, const FText& Col3)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

	Row->AddSlot()
		.FillWidth(0.3f)
		.Padding(4.0f)
		[
			SNew(STextBlock).Text(Col1).AutoWrapText(true)
		];
	Row->AddSlot()
		.FillWidth(0.4f)
		.Padding(4.0f)
		[
			SNew(STextBlock).Text(Col2).AutoWrapText(true)
		];
	if (!Col3.IsEmpty())
	{
		Row->AddSlot()
			.FillWidth(0.3f)
			.Padding(4.0f)
			[
				SNew(STextBlock).Text(Col3).AutoWrapText(true)
			];
	}

	return Row;
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateBoldText(const FText& Text)
{
	return SNew(STextBlock)
		.Text(Text)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11));
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateParagraph(const FText& Text)
{
	return SNew(STextBlock)
		.Text(Text)
		.AutoWrapText(true)
		.Margin(FMargin(0.0f, 4.0f));
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateCopyButton(const FString& TextToCopy)
{
	return SNew(SButton)
		.Text(LOCTEXT("MCPToolboxHelp_Copy", "复制"))
		.OnClicked(this, &SMCPToolboxHelpWidget::OnCopyClicked, TextToCopy);
}

// ============================================================================
// Help Content Sections
// ============================================================================

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateQuickStartSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CreateSectionHeader(LOCTEXT("MCPToolboxHelp_QuickStartHeader", "快速入门指南")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(1, LOCTEXT("MCPToolboxHelp_Step1", "从窗口菜单或工具栏按钮打开 MCP Toolbox。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(2, LOCTEXT("MCPToolboxHelp_Step2", "从侧边栏选择工具，或使用搜索框查找工具。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(3, LOCTEXT("MCPToolboxHelp_Step3", "配置工具设置，包括 API 密钥和参数。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(4, LOCTEXT("MCPToolboxHelp_Step4", "点击运行或按 Ctrl+Enter 执行工具。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(5, LOCTEXT("MCPToolboxHelp_Step5", "在输出面板中查看结果。您可以复制或导出结果。")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateWarningBox(LOCTEXT("MCPToolboxHelp_WarningAPI", "重要提示：在运行任何工具之前，请确保在设置中配置了 API 密钥。没有有效的密钥，API 调用将失败。")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_TipsHeader", "小贴士")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_Tip1", "使用对话标签页通过自然语言与 AI 助手交互。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_Tip2", "使用 API 管理标签页管理多个 API 提供商。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_Tip3", "使用设置标签页配置主题、语言和默认参数。")) ];
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateImageGenerationSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CreateSectionHeader(LOCTEXT("MCPToolboxHelp_ImageGenHeader", "图片生成功能")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_ImageGenIntro", "MCP Toolbox 支持三种图片生成方式，您可以根据环境和需求选择合适的方案。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_ImageGenModes", "三种生图模式")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateBoldText(LOCTEXT("MCPToolboxHelp_ImageGenWebUI", "模式一：SD WebUI（本地）")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_ImageGenWebUIDesc", "使用本地部署的 Stable Diffusion WebUI 服务。适用于已有 SD WebUI 环境的用户。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateCodeBlock(TEXT("服务商: SD WebUI (本地)\n地址: http://127.0.0.1:7860\n模型: sd-1.5, sdxl, flux, 或手动输入")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_ImageGenWebUITips", "提示：服务端需要在设置中启用 API 支持（--api 参数）。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_ImageGenComfyUI", "模式二：ComfyUI（本地）")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_ImageGenComfyUIDesc", "使用本地部署的 ComfyUI 服务。支持复杂工作流，推荐高级用户使用。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateCodeBlock(TEXT("服务商: ComfyUI (本地)\n地址: http://127.0.0.1:8200\n模型: sdxl, flux, sd-1.5, 或工作流文件名")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_ImageGenComfyUITips", "提示：可将 ComfyUI 导出的工作流 JSON 文件放入 Saved/ComfyUIWorkflows/ 目录。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_ImageGenCloud", "模式三：云端多模态模型")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_ImageGenCloudDesc", "使用云端 API 服务。无需本地部署，配置简单。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateCodeBlock(TEXT("OpenAI DALL-E: https://api.openai.com/v1\nReplicate: https://api.replicate.com/v1\nPollinations AI: https://api.pollinations.ai")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_ImageGenUsage", "使用方法")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(1, LOCTEXT("MCPToolboxHelp_ImageGenStep1", "在 API 管理标签页添加生图模型配置。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(2, LOCTEXT("MCPToolboxHelp_ImageGenStep2", "在对话中直接要求 AI 生成图片，如：\"帮我画一只可爱的小猫\"。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(3, LOCTEXT("MCPToolboxHelp_ImageGenStep3", "AI 会自动调用 generate_image 工具，生成图片后显示在对话中。")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateStep(4, LOCTEXT("MCPToolboxHelp_ImageGenStep4", "也可以使用 /test_image 命令手动测试生图功能。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_ImageGenParams", "参数说明")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(LOCTEXT("MCPToolboxHelp_ParamPrompt", "prompt"), LOCTEXT("MCPToolboxHelp_ParamPromptDesc", "图片描述提示词"), LOCTEXT("MCPToolboxHelp_ParamRequired", "必填")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(LOCTEXT("MCPToolboxHelp_ParamNegPrompt", "negative_prompt"), LOCTEXT("MCPToolboxHelp_ParamNegPromptDesc", "负面提示词（排除内容）"), LOCTEXT("MCPToolboxHelp_ParamOptional", "可选")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(LOCTEXT("MCPToolboxHelp_ParamWidth", "width"), LOCTEXT("MCPToolboxHelp_ParamWidthDesc", "图片宽度（默认1024）"), LOCTEXT("MCPToolboxHelp_ParamOptional", "可选")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(LOCTEXT("MCPToolboxHelp_ParamHeight", "height"), LOCTEXT("MCPToolboxHelp_ParamHeightDesc", "图片高度（默认1024）"), LOCTEXT("MCPToolboxHelp_ParamOptional", "可选")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(LOCTEXT("MCPToolboxHelp_ParamSteps", "steps"), LOCTEXT("MCPToolboxHelp_ParamStepsDesc", "生成步数（默认20）"), LOCTEXT("MCPToolboxHelp_ParamOptional", "可选")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(LOCTEXT("MCPToolboxHelp_ParamCfg", "cfg_scale"), LOCTEXT("MCPToolboxHelp_ParamCfgDesc", "CFG比例（默认7.0）"), LOCTEXT("MCPToolboxHelp_ParamOptional", "可选")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateWarningBox(LOCTEXT("MCPToolboxHelp_ImageGenWarning", "注意：生图功能需要在 API 设置中正确配置生图模型。本地模型需要先启动对应服务（SD WebUI 或 ComfyUI）。")) ];
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateFAQSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CreateSectionHeader(LOCTEXT("MCPToolboxHelp_FAQHeader", "常见问题")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q1", "Q: 什么是 MCP Toolbox？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A1", "A: MCP Toolbox 是一个 Unreal Engine 编辑器插件，通过模型上下文协议 (MCP) 集成大语言模型 (LLM)。它让您可以在 Unreal Editor 中直接使用 AI 助手。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q2", "Q: 如何添加新的 API 提供商？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A2", "A: 前往 API 管理标签页，点击添加提供商。输入提供商名称、基础 URL 和您的 API 密钥。支持的提供商包括 OpenAI、Anthropic 以及任何兼容 OpenAI 的端点。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q3", "Q: 可以使用本地大模型吗？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A3", "A: 可以。添加自定义提供商，将基础 URL 指向您的本地 LLM 服务器（例如 LM Studio 使用 http://localhost:1234/v1）。确保它暴露了兼容 OpenAI 的 API。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q4", "Q: 如何切换浅色和深色主题？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A4", "A: 打开设置标签页，从主题下拉菜单中选择深色、浅色或自动。自动模式将跟随 Unreal Editor 的主题设置。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q5", "Q: 为什么我的 API 调用返回错误？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A5", "A: 常见原因包括：API 密钥无效、网络问题、速率限制或余额不足。请检查输出面板中的具体错误信息，并在设置中验证您的 API 密钥。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q6", "Q: 我的 API 密钥是否安全存储？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A6", "A: 是的。API 密钥存储在 Unreal Engine 配置系统中，仅用于经过身份验证的 API 请求。它们绝不会被传输给第三方。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q7", "Q: 如何更新插件？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A7", "A: 从市场或仓库下载最新版本。关闭编辑器，替换插件文件夹，然后重新启动。更新后您的设置将保留。")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateBoldText(LOCTEXT("MCPToolboxHelp_Q8", "Q: 生图时 UE 编辑器卡死怎么办？")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_A8", "A: 生图功能已改为异步执行，不会再阻塞主线程。如果仍出现卡死，请检查：1）本地 SD WebUI/ComfyUI 服务是否正常运行；2）API 地址端口是否正确（WebUI 默认 7860，ComfyUI 默认 8200）；3）网络连接是否正常。")) ];
}

TSharedRef<SWidget> SMCPToolboxHelpWidget::CreateCommandReferenceSection()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ CreateSectionHeader(LOCTEXT("MCPToolboxHelp_CmdRefHeader", "命令参考")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_CmdShortcuts", "键盘快捷键")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_SC_Open", "Ctrl+Shift+M"),
			LOCTEXT("MCPToolboxHelp_SC_OpenDesc", "打开 / 关闭 MCP Toolbox"),
			LOCTEXT("MCPToolboxHelp_SC_OpenCat", "窗口")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_SC_Send", "Ctrl+Enter"),
			LOCTEXT("MCPToolboxHelp_SC_SendDesc", "在对话中发送消息"),
			LOCTEXT("MCPToolboxHelp_SC_SendCat", "对话")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_SC_Help", "F1"),
			LOCTEXT("MCPToolboxHelp_SC_HelpDesc", "打开此帮助窗口"),
			LOCTEXT("MCPToolboxHelp_SC_HelpCat", "帮助")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_SC_Esc", "Esc"),
			LOCTEXT("MCPToolboxHelp_SC_EscDesc", "关闭帮助窗口 / 取消操作"),
			LOCTEXT("MCPToolboxHelp_SC_EscCat", "帮助")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_CmdAPI", "API 配置示例")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateParagraph(LOCTEXT("MCPToolboxHelp_CmdAPIDesc", "添加自定义提供商时，请使用以下 URL 格式：")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateCodeBlock(TEXT("https://api.openai.com/v1\nhttps://api.anthropic.com/v1\nhttp://localhost:1234/v1")) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateSubSectionHeader(LOCTEXT("MCPToolboxHelp_CmdMCP", "MCP 连接命令")) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_MCP_Connect", "连接"),
			LOCTEXT("MCPToolboxHelp_MCP_ConnectDesc", "连接到 MCP 服务器"),
			FText::GetEmpty()) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_MCP_Disconnect", "断开连接"),
			LOCTEXT("MCPToolboxHelp_MCP_DisconnectDesc", "从 MCP 服务器断开连接"),
			FText::GetEmpty()) ]
		+ SVerticalBox::Slot().AutoHeight()[ CreateTableRow(
			LOCTEXT("MCPToolboxHelp_MCP_Status", "状态"),
			LOCTEXT("MCPToolboxHelp_MCP_StatusDesc", "显示当前连接状态"),
			FText::GetEmpty()) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)[ CreateWarningBox(LOCTEXT("MCPToolboxHelp_CmdNote", "注意：默认 MCP 服务器端口为 8000。您可以在设置标签页的 MCP 服务器配置中进行更改。")) ];
}

#undef LOCTEXT_NAMESPACE
