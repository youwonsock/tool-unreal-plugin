#pragma once

#include "CoreMinimal.h"
#include "CubeShotTypes.h"
#include "UObject/StrongObjectPtr.h"

class FPreviewScene;
struct FImage;
class UDirectionalLightComponent;
class USceneCaptureComponent2D;
class UStaticMeshComponent;
class UTexture2D;
class UTextureRenderTarget2D;

/** Owns the isolated preview world used for deterministic one-pass CubeShot captures. */
class FCubeShotCaptureService final
{
public:
    FCubeShotCaptureService();
    ~FCubeShotCaptureService();

    FCubeShotCaptureService(const FCubeShotCaptureService&) = delete;
    FCubeShotCaptureService& operator=(const FCubeShotCaptureService&) = delete;

    bool Initialize(const FCubeShotOptions& Options, FText& OutError);
    bool UpdateOptions(const FCubeShotOptions& Options, FText& OutError);

    bool CalculateAutoFit(
        const FCubeShotOptions& Options,
        FCubeShotAutoFitResult& OutAutoFit,
        FText& OutError) const;

    bool CaptureRaw(
        ECubeShotDirection Direction,
        int32 TargetResolution,
        FCubeShotRawFrame& OutFrame,
        FText& OutError);

    bool Composite(
        const FCubeShotRawFrame& RawFrame,
        const FCubeShotOptions& Options,
        int32 TargetResolution,
        FCubeShotImage& OutImage,
        FText& OutError);

    bool CaptureAndComposite(
        const FCubeShotOptions& Options,
        ECubeShotDirection Direction,
        int32 TargetResolution,
        FCubeShotImage& OutImage,
        FText& OutError);
    bool PrepareCompositeWork(FCubeShotRawFrame RawFrame, const FCubeShotOptions& Options,
        int32 TargetResolution, FCubeShotPixelWork& OutWork, FText& OutError);
    bool PrepareCaptureWork(const FCubeShotOptions& Options, ECubeShotDirection Direction,
        int32 TargetResolution, FCubeShotPixelWork& OutWork, FText& OutError);

    void Shutdown();

    static bool CalculateAutoFitForBounds(
        const FBoxSphereBounds& Bounds,
        double FieldOfViewDegrees,
        double FrameFill,
        FCubeShotAutoFitResult& OutAutoFit,
        FText& OutError);

    static FTransform MakeCameraTransform(
        const FBoxSphereBounds& Bounds,
        ECubeShotDirection Direction,
        double CameraDistance);

    static int32 GetCaptureResolution(int32 TargetResolution);

private:
    bool CreatePreviewScene(FText& OutError);
    bool EnsureRenderTarget(int32 CaptureResolution, FText& OutError);
    void ConfigureCaptureComponent();
    void ApplyCamera(ECubeShotDirection Direction);
    void ApplyCameraRelativeLighting(ECubeShotDirection Direction);
    bool ResolveBackground(const FCubeShotOptions& Options, int32 TargetResolution,
        TSharedPtr<const FImage>& OutImage, FText& OutError);

private:
    TUniquePtr<FPreviewScene> PreviewScene;
    UStaticMeshComponent* MeshComponent = nullptr;
    USceneCaptureComponent2D* CaptureComponent = nullptr;
    TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget;
    TSharedPtr<FImage> CachedBackgroundImage;
    TWeakObjectPtr<UTexture2D> CachedBackgroundTexture;
    FGuid CachedBackgroundSourceId;
    int32 CachedBackgroundResolution = 0;
    UDirectionalLightComponent* FillLight = nullptr;
    UDirectionalLightComponent* RimLight = nullptr;
    FCubeShotOptions CurrentOptions;
    FCubeShotAutoFitResult CurrentAutoFit;
    bool bInitialized = false;
};
