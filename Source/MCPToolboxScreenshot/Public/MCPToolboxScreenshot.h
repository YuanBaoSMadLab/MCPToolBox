#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_DELEGATE_OneParam(FOnMCPToolboxScreenshotCaptured, const FString& /*Base64JPEG*/);
DECLARE_DELEGATE_OneParam(FOnScreenshotError, const FString& /*ErrorMessage*/);

class MCPTOOLBOXSCREENSHOT_API FMCPToolboxScreenshotModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FMCPToolboxScreenshotModule& Get();
	static FName GetModuleName();

	// Screenshot capture
	FString CaptureScreenshot(bool bIncludeUI, bool bHighQuality, int32 TgtW = 0, int32 TgtH = 0);
	void CaptureScreenshotAsync(bool bIncludeUI, bool bHighQuality);
	FString CaptureRegionScreenshot(int32 X, int32 Y, int32 W, int32 H);
	FString CaptureFullDesktop();

	// Configuration
	void SetScreenshotScale(float Scale) { ScreenshotScale = FMath::Clamp(Scale, 0.1f, 4.0f); }
	float GetScreenshotScale() const { return ScreenshotScale; }
	void SetJPEGQuality(int32 Quality) { JPEGQuality = FMath::Clamp(Quality, 1, 100); }
	int32 GetJPEGQuality() const { return JPEGQuality; }
	void SetMaxResolution(int32 Width, int32 Height)
	{
		MaxScreenshotWidth = FMath::Max(1, Width);
		MaxScreenshotHeight = FMath::Max(1, Height);
	}

	// Delegates
	FOnMCPToolboxScreenshotCaptured OnScreenshotCaptured;
	FOnScreenshotError OnScreenshotError;

private:
	FString EncodeToJPEGBase64(TArray<uint8>& RawBGRA, int32 Width, int32 Height);
	bool ResizeImage(TArray<uint8>& SrcData, int32 SrcW, int32 SrcH, TArray<uint8>& DstData, int32 DstW, int32 DstH);

	// Platform-specific capture
	FString CaptureWindowsScreenshot(bool bIncludeUI, bool bHighQuality, int32 TgtW, int32 TgtH);
	FString CaptureLinuxScreenshot(bool bIncludeUI, bool bHighQuality, int32 TgtW, int32 TgtH);
	FString CaptureMacScreenshot(bool bIncludeUI, bool bHighQuality, int32 TgtW, int32 TgtH);

	// Configuration
	float ScreenshotScale = 1.0f;
	int32 JPEGQuality = 85;
	int32 MaxScreenshotWidth = 1920;
	int32 MaxScreenshotHeight = 1080;

	bool bModuleStarted = false;
};
