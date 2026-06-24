#include "MCPToolboxCommands.h"
#include "MCPToolboxStyle.h"

#define LOCTEXT_NAMESPACE "FMCPToolboxCommands"

FMCPToolboxCommands::FMCPToolboxCommands()
    : TCommands<FMCPToolboxCommands>(
        TEXT("MCPToolbox"),
        LOCTEXT("MCPToolbox", "MCP Toolbox"),
        NAME_None,
        FMCPToolboxStyle::GetStyleSetName())
{
}

void FMCPToolboxCommands::RegisterCommands()
{
    UI_COMMAND(OpenMCPToolboxWindow, "MCP Toolbox", "打开MCP Toolbox AI助手窗口", EUserInterfaceActionType::ToggleButton, FInputChord());
    UI_COMMAND(SendMessage, "发送消息", "发送当前消息到AI助手", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::Enter));
    UI_COMMAND(CaptureScreenshot, "截屏", "捕获当前编辑器视口截图", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::S));
    UI_COMMAND(ToggleVisionMode, "切换视觉模式", "开启/关闭视觉分析模式", EUserInterfaceActionType::ToggleButton, FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::V));
    UI_COMMAND(ClearChat, "清空对话", "清空当前对话历史", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::L));
    UI_COMMAND(OpenHelp, "帮助", "打开MCP Toolbox帮助文档", EUserInterfaceActionType::Button, FInputChord(EModifierKey::None, EKeys::F1));
}

#undef LOCTEXT_NAMESPACE
