// Copyright MCPToolbox Team. All Rights Reserved.
#include "MCPToolboxStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Brushes/SlateImageBrush.h"

TSharedPtr<FSlateStyleSet> FMCPToolboxStyle::StyleInstance = nullptr;

void FMCPToolboxStyle::Initialize()
{
    if (!StyleInstance.IsValid())
    {
        StyleInstance = Create();
        FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
    }
}

void FMCPToolboxStyle::Shutdown()
{
    FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
    ensure(StyleInstance.IsUnique());
    StyleInstance.Reset();
}

void FMCPToolboxStyle::ReloadTextures()
{
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
    }
}

const ISlateStyle& FMCPToolboxStyle::Get()
{
    return *StyleInstance;
}

FName FMCPToolboxStyle::GetStyleSetName()
{
    static FName StyleSetName(TEXT("MCPToolboxStyle"));
    return StyleSetName;
}

const FSlateBrush* FMCPToolboxStyle::GetBrush(FName PropertyName, const ANSICHAR* Specifier)
{
    return StyleInstance->GetBrush(PropertyName, Specifier);
}

TSharedRef<FSlateStyleSet> FMCPToolboxStyle::Create()
{
    TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(GetStyleSetName()));
    Style->SetContentRoot(IPluginManager::Get().FindPlugin("MCPToolbox")->GetBaseDir() / TEXT("Resources"));

    // Toolbar icon
    Style->Set("MCPToolbox.OpenMCPToolboxWindow", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(40.0f, 40.0f)));

    Style->Set("MCPToolbox.OpenMCPToolboxWindow.Small", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(20.0f, 20.0f)));

    // Command icons
    Style->Set("MCPToolbox.SendMessage", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(20.0f, 20.0f)));

    Style->Set("MCPToolbox.CaptureScreenshot", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(20.0f, 20.0f)));

    Style->Set("MCPToolbox.ToggleVisionMode", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(20.0f, 20.0f)));

    Style->Set("MCPToolbox.ClearChat", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(20.0f, 20.0f)));

    Style->Set("MCPToolbox.OpenHelp", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(20.0f, 20.0f)));

    // Tab icons
    Style->Set("MCPToolbox.Tab.Chat", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(16.0f, 16.0f)));

    Style->Set("MCPToolbox.Tab.Settings", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(16.0f, 16.0f)));

    Style->Set("MCPToolbox.Tab.MCPServer", new FSlateImageBrush(
        Style->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
        FVector2D(16.0f, 16.0f)));

    return Style;
}
