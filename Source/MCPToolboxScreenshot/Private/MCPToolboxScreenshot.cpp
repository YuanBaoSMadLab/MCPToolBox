#include "MCPToolboxScreenshot.h"
#include "Async/Async.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMCPToolboxScreenshot, Log, All);

// ---------------------------------------------------------------------------
// Module singleton & lifecycle
// ---------------------------------------------------------------------------
static FMCPToolboxScreenshotModule* GScreenshotModule = nullptr;

FMCPToolboxScreenshotModule& FMCPToolboxScreenshotModule::Get() { return *GScreenshotModule; }
FName FMCPToolboxScreenshotModule::GetModuleName() { return TEXT("MCPToolboxScreenshot"); }

void FMCPToolboxScreenshotModule::StartupModule()
{
	GScreenshotModule = this;
	bModuleStarted = true;
	UE_LOG(LogMCPToolboxScreenshot, Log, TEXT("MCPToolboxScreenshot module started"));
}

void FMCPToolboxScreenshotModule::ShutdownModule()
{
	bModuleStarted = false;
	GScreenshotModule = nullptr;
	UE_LOG(LogMCPToolboxScreenshot, Log, TEXT("MCPToolboxScreenshot module shutdown"));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
FString FMCPToolboxScreenshotModule::CaptureScreenshot(bool bIncludeUI, bool bHighQuality, int32 TgtW, int32 TgtH)
{
#if PLATFORM_WINDOWS
	return CaptureWindowsScreenshot(bIncludeUI, bHighQuality, TgtW, TgtH);
#elif PLATFORM_LINUX
	return CaptureLinuxScreenshot(bIncludeUI, bHighQuality, TgtW, TgtH);
#elif PLATFORM_MAC
	return CaptureMacScreenshot(bIncludeUI, bHighQuality, TgtW, TgtH);
#else
	return FString();
#endif
}

void FMCPToolboxScreenshotModule::CaptureScreenshotAsync(bool bIncludeUI, bool bHighQuality)
{
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, bIncludeUI, bHighQuality]()
		{
			const FString Result = CaptureScreenshot(bIncludeUI, bHighQuality);
			AsyncTask(ENamedThreads::GameThread,
				[this, Result]()
				{
					if (!Result.IsEmpty())
						OnScreenshotCaptured.ExecuteIfBound(Result);
					else
						OnScreenshotError.ExecuteIfBound(TEXT("Screenshot capture failed"));
				});
		});
}

FString FMCPToolboxScreenshotModule::CaptureRegionScreenshot(int32 X, int32 Y, int32 W, int32 H)
{
#if PLATFORM_WINDOWS
	const HDC hScreenDC = GetDC(nullptr);
	if (!hScreenDC) return FString();

	const HDC hMemDC = CreateCompatibleDC(hScreenDC);
	const HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, W, H);
	const HGDIOBJ hOld = SelectObject(hMemDC, hBitmap);
	BitBlt(hMemDC, 0, 0, W, H, hScreenDC, X, Y, SRCCOPY);

	BITMAPINFOHEADER bi = {};
	bi.biSize = sizeof(bi);
	bi.biWidth = W;
	bi.biHeight = -H;
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;

	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(W * H * 4);
	GetDIBits(hMemDC, hBitmap, 0, H, Pixels.GetData(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

	SelectObject(hMemDC, hOld);
	DeleteObject(hBitmap);
	DeleteDC(hMemDC);
	ReleaseDC(nullptr, hScreenDC);

	return EncodeToJPEGBase64(Pixels, W, H);
#else
	return FString();
#endif
}

FString FMCPToolboxScreenshotModule::CaptureFullDesktop()
{
#if PLATFORM_WINDOWS
	return CaptureRegionScreenshot(
		GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
		GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
#else
	return FString();
#endif
}

// ---------------------------------------------------------------------------
// JPEG encoding: BGRA -> RGBA -> JPEG -> Base64
// ---------------------------------------------------------------------------
FString FMCPToolboxScreenshotModule::EncodeToJPEGBase64(TArray<uint8>& RawBGRA, int32 Width, int32 Height)
{
	TArray<uint8> RGBA;
	RGBA.SetNumUninitialized(RawBGRA.Num());
	for (int32 i = 0; i < RawBGRA.Num(); i += 4)
	{
		RGBA[i + 0] = RawBGRA[i + 2];
		RGBA[i + 1] = RawBGRA[i + 1];
		RGBA[i + 2] = RawBGRA[i + 0];
		RGBA[i + 3] = RawBGRA[i + 3];
	}

	IImageWrapperModule& ImageWrapperModule =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!Wrapper.IsValid() || !Wrapper->SetRaw(RGBA.GetData(), RGBA.Num(), Width, Height, ERGBFormat::RGBA, 8))
		return FString();

	const TArray64<uint8> Compressed64 = Wrapper->GetCompressed(JPEGQuality);
	if (Compressed64.Num() == 0) return FString();

	const TArray<uint8> Compressed(reinterpret_cast<const uint8*>(Compressed64.GetData()), Compressed64.Num());
	return FBase64::Encode(Compressed);
}

// ---------------------------------------------------------------------------
// Bilinear resize (BGRA)
// ---------------------------------------------------------------------------
bool FMCPToolboxScreenshotModule::ResizeImage(
	TArray<uint8>& SrcData, int32 SrcW, int32 SrcH,
	TArray<uint8>& DstData, int32 DstW, int32 DstH)
{
	if (SrcW <= 0 || SrcH <= 0 || DstW <= 0 || DstH <= 0 || SrcData.Num() < SrcW * SrcH * 4)
		return false;

	DstData.SetNumUninitialized(DstW * DstH * 4);
	const float XRatio = static_cast<float>(SrcW) / DstW;
	const float YRatio = static_cast<float>(SrcH) / DstH;

	for (int32 Y = 0; Y < DstH; ++Y)
	{
		const float SrcY = Y * YRatio;
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt(SrcY), 0, SrcH - 1);
		const int32 Y1 = FMath::Min(Y0 + 1, SrcH - 1);
		const float YFrac = SrcY - Y0;

		for (int32 X = 0; X < DstW; ++X)
		{
			const float SrcX = X * XRatio;
			const int32 X0 = FMath::Clamp(FMath::FloorToInt(SrcX), 0, SrcW - 1);
			const int32 X1 = FMath::Min(X0 + 1, SrcW - 1);
			const float XFrac = SrcX - X0;

			for (int32 C = 0; C < 4; ++C)
			{
				const float V00 = SrcData[(Y0 * SrcW + X0) * 4 + C];
				const float V10 = SrcData[(Y0 * SrcW + X1) * 4 + C];
				const float V01 = SrcData[(Y1 * SrcW + X0) * 4 + C];
				const float V11 = SrcData[(Y1 * SrcW + X1) * 4 + C];
				DstData[(Y * DstW + X) * 4 + C] = static_cast<uint8>(
					FMath::Lerp(FMath::Lerp(V00, V10, XFrac), FMath::Lerp(V01, V11, XFrac), YFrac));
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Platform-specific captures
// ---------------------------------------------------------------------------

#if PLATFORM_WINDOWS
FString FMCPToolboxScreenshotModule::CaptureWindowsScreenshot(
	bool bIncludeUI, bool bHighQuality, int32 TgtW, int32 TgtH)
{
	HDC hScreenDC = GetDC(nullptr);
	if (!hScreenDC) return FString();

	int32 SrcW = GetDeviceCaps(hScreenDC, HORZRES);
	int32 SrcH = GetDeviceCaps(hScreenDC, VERTRES);
	int32 SrcX = 0, SrcY = 0;

	// Scale / target resolution
	if (TgtW > 0) TgtW = FMath::Min(TgtW, MaxScreenshotWidth);
	else          TgtW = FMath::Min(FMath::RoundToInt(SrcW * ScreenshotScale), MaxScreenshotWidth);
	if (TgtH > 0) TgtH = FMath::Min(TgtH, MaxScreenshotHeight);
	else          TgtH = FMath::Min(FMath::RoundToInt(SrcH * ScreenshotScale), MaxScreenshotHeight);
	if (TgtW <= 0 || TgtH <= 0) { ReleaseDC(nullptr, hScreenDC); return FString(); }

	HDC hMemDC = CreateCompatibleDC(hScreenDC);
	HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, TgtW, TgtH);
	HGDIOBJ hOld = SelectObject(hMemDC, hBitmap);

	SetStretchBltMode(hMemDC, bHighQuality ? HALFTONE : COLORONCOLOR);
	if (bHighQuality) SetBrushOrgEx(hMemDC, 0, 0, nullptr);
	StretchBlt(hMemDC, 0, 0, TgtW, TgtH, hScreenDC, SrcX, SrcY, SrcW, SrcH, SRCCOPY);

	BITMAPINFOHEADER bi = {};
	bi.biSize = sizeof(bi);
	bi.biWidth = TgtW;
	bi.biHeight = -TgtH;
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;

	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(TgtW * TgtH * 4);
	GetDIBits(hMemDC, hBitmap, 0, TgtH, Pixels.GetData(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

	SelectObject(hMemDC, hOld);
	DeleteObject(hBitmap);
	DeleteDC(hMemDC);
	ReleaseDC(nullptr, hScreenDC);

	return EncodeToJPEGBase64(Pixels, TgtW, TgtH);
}
#endif // PLATFORM_WINDOWS

FString FMCPToolboxScreenshotModule::CaptureLinuxScreenshot(bool, bool, int32, int32) { return FString(); }
FString FMCPToolboxScreenshotModule::CaptureMacScreenshot(bool, bool, int32, int32) { return FString(); }

IMPLEMENT_MODULE(FMCPToolboxScreenshotModule, MCPToolboxScreenshot);
