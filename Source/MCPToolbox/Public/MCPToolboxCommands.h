#pragma once
#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "EditorStyleSet.h"

class MCPTOOLBOX_API FMCPToolboxCommands : public TCommands<FMCPToolboxCommands>
{
public:
    FMCPToolboxCommands();
    virtual void RegisterCommands() override;
    
    TSharedPtr<FUICommandInfo> OpenMCPToolboxWindow;
    TSharedPtr<FUICommandInfo> SendMessage;
    TSharedPtr<FUICommandInfo> CaptureScreenshot;
    TSharedPtr<FUICommandInfo> ToggleVisionMode;
    TSharedPtr<FUICommandInfo> ClearChat;
    TSharedPtr<FUICommandInfo> OpenHelp;
};
