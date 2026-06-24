// Copyright MCPToolbox Team. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

class MCPTOOLBOX_API FMCPToolboxStyle
{
public:
    static void Initialize();
    static void Shutdown();
    static void ReloadTextures();
    static const ISlateStyle& Get();
    static FName GetStyleSetName();
    static const FSlateBrush* GetBrush(FName PropertyName, const ANSICHAR* Specifier = nullptr);
private:
    static TSharedRef<FSlateStyleSet> Create();
    static TSharedPtr<FSlateStyleSet> StyleInstance;
};
