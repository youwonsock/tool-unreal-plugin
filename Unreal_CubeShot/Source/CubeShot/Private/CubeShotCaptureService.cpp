#include "CubeShotCaptureService.h"

#include "CubeShotImageUtils.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Math/OrthoMatrix.h"
#include "Math/RotationMatrix.h"
#include "PreviewScene.h"
#include "RHIGlobals.h"
#include "StaticMeshCompiler.h"
#include "TextureCompiler.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "CubeShotCaptureService"

namespace
{
    constexpr double MinimumNearPlane = CubeShot::MinimumNearPlane;
    constexpr double MinimumClipMargin = 1.0;

    TArray<FVector> GetBoundsCorners(const FBoxSphereBounds& Bounds)
    {
        TArray<FVector> Corners;
        Corners.Reserve(8);
        for (int32 XSign = -1; XSign <= 1; XSign += 2)
        {
            for (int32 YSign = -1; YSign <= 1; YSign += 2)
            {
                for (int32 ZSign = -1; ZSign <= 1; ZSign += 2)
                {
                    Corners.Add(Bounds.Origin + FVector(
                        Bounds.BoxExtent.X * XSign,
                        Bounds.BoxExtent.Y * YSign,
                        Bounds.BoxExtent.Z * ZSign));
                }
            }
        }
        return Corners;
    }

    void GetCameraBasis(
        const FCubeShotDirectionInfo& DirectionInfo,
        FVector& OutForward,
        FVector& OutRight,
        FVector& OutUp)
    {
        OutForward = DirectionInfo.ViewDirection.GetSafeNormal();
        OutUp = DirectionInfo.UpVector.GetSafeNormal();
        OutRight = FVector::CrossProduct(OutUp, OutForward).GetSafeNormal();
        OutUp = FVector::CrossProduct(OutForward, OutRight).GetSafeNormal();
    }


}

FCubeShotCaptureService::FCubeShotCaptureService() = default;

FCubeShotCaptureService::~FCubeShotCaptureService()
{
    Shutdown();
}

bool FCubeShotCaptureService::Initialize(const FCubeShotOptions& Options, FText& OutError)
{
    if (!CubeShot::CanExecute(OutError)) return false;
    if (!PreviewScene.IsValid() && !CreatePreviewScene(OutError))
    {
        return false;
    }

    bInitialized = true;
    return UpdateOptions(Options, OutError);
}

bool FCubeShotCaptureService::UpdateOptions(const FCubeShotOptions& Options, FText& OutError)
{
    OutError = FText::GetEmpty();
    if (!bInitialized || !PreviewScene.IsValid() || MeshComponent == nullptr || CaptureComponent == nullptr)
    {
        OutError = LOCTEXT("CaptureNotInitialized", "The CubeShot preview scene is not initialized.");
        return false;
    }

    // Wait only for the selected mesh. A global compilation flush would stall unrelated editor work.
    if (IsValid(Options.StaticMesh))
    {
        FStaticMeshCompilingManager::Get().FinishCompilation({Options.StaticMesh});
    }

    FText Warning;
    if (!CubeShot::ValidateOptions(Options, OutError, Warning))
    {
        return false;
    }

    FCubeShotAutoFitResult AutoFit;
    if (!CalculateAutoFit(Options, AutoFit, OutError))
    {
        return false;
    }

    CurrentOptions = Options;
    CurrentAutoFit = AutoFit;
    if (CurrentOptions.bAutoFit)
    {
        CurrentOptions.CameraDistance = AutoFit.CameraDistance;
        CurrentOptions.OrthoWidth = AutoFit.OrthoWidth;
    }
    else
    {
        const double DepthExtent = Options.StaticMesh->GetBounds().BoxExtent.GetMax();
        const double ClipMargin = FMath::Max(Options.StaticMesh->GetBounds().SphereRadius * 0.1, MinimumClipMargin);
        CurrentAutoFit.NearPlane = FMath::Max(MinimumNearPlane, CurrentOptions.CameraDistance - DepthExtent - ClipMargin);
        CurrentAutoFit.FarPlane = FMath::Max(
            CurrentAutoFit.NearPlane + MinimumClipMargin,
            CurrentOptions.CameraDistance + DepthExtent + ClipMargin);
    }

    if (MeshComponent->GetStaticMesh() != Options.StaticMesh)
    {
        // FPreviewScene registered this component before it had a mesh, so its initial force-resident
        // pass had no used textures. Balance the previous mesh explicitly, then apply residency only
        // after the new mesh and its materials are assigned.
        MeshComponent->SetTextureForceResidentFlag(false);
        MeshComponent->SetStaticMesh(Options.StaticMesh);
        MeshComponent->SetForcedLodModel(1);
        MeshComponent->SetRelativeTransform(FTransform::Identity);
        MeshComponent->MarkRenderStateDirty();
        MeshComponent->SetTextureForceResidentFlag(true);

        // The panel normally gives streaming several ticks before Generate. MCP
        // can capture on the first load, so resolve only this mesh's resources.
        // Otherwise the saved icon contains the default material or tiny mips.
        TArray<UTexture*> UsedTextures;
        for (const FStaticMaterial& Slot : Options.StaticMesh->GetStaticMaterials())
        {
            if (UMaterialInterface* Material = Slot.MaterialInterface)
            {
                Material->EnsureIsComplete();
                TArray<UTexture*> MaterialTextures;
                Material->GetUsedTextures(MaterialTextures);
                for (UTexture* Texture : MaterialTextures) UsedTextures.AddUnique(Texture);
            }
        }
        FTextureCompilingManager::Get().FinishCompilation(UsedTextures);
        for (UTexture* Texture : UsedTextures)
        {
            if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
            {
                Texture2D->WaitForStreaming();
            }
        }
    }

    CaptureComponent->ProjectionType = CurrentOptions.ProjectionMode == ECubeShotProjectionMode::Perspective
        ? ECameraProjectionMode::Perspective
        : ECameraProjectionMode::Orthographic;
    CaptureComponent->FOVAngle = static_cast<float>(CurrentOptions.FieldOfView);
    CaptureComponent->OrthoWidth = static_cast<float>(CurrentOptions.OrthoWidth);
    CaptureComponent->bOverride_CustomNearClippingPlane = CurrentOptions.ProjectionMode == ECubeShotProjectionMode::Perspective;
    CaptureComponent->CustomNearClippingPlane = static_cast<float>(CurrentAutoFit.NearPlane);
    CaptureComponent->bAutoCalculateOrthoPlanes = true;
    CaptureComponent->bUpdateOrthoPlanes = true;
    CaptureComponent->bUseCameraHeightAsViewTarget = false;

    if (CurrentOptions.ProjectionMode == ECubeShotProjectionMode::Orthographic)
    {
        const double HalfOrthoWidth = CurrentOptions.OrthoWidth * 0.5;
        const double ZScale = 1.0 / (CurrentAutoFit.FarPlane - CurrentAutoFit.NearPlane);
        const double ZOffset = -CurrentAutoFit.NearPlane;
        CaptureComponent->bUseCustomProjectionMatrix = true;
        CaptureComponent->CustomProjectionMatrix = FReversedZOrthoMatrix(
            HalfOrthoWidth,
            HalfOrthoWidth,
            ZScale,
            ZOffset);
        CaptureComponent->MaxViewDistanceOverride = 0.0f;
        CaptureComponent->bFiniteFarPlane = false;
    }
    else
    {
        CaptureComponent->bUseCustomProjectionMatrix = false;
        CaptureComponent->MaxViewDistanceOverride = static_cast<float>(CurrentAutoFit.FarPlane);
        CaptureComponent->bFiniteFarPlane = true;
    }

    CaptureComponent->ClearShowOnlyComponents();
    CaptureComponent->ShowOnlyComponent(MeshComponent);
    return true;
}

bool FCubeShotCaptureService::CalculateAutoFit(
    const FCubeShotOptions& Options,
    FCubeShotAutoFitResult& OutAutoFit,
    FText& OutError) const
{
    if (!IsValid(Options.StaticMesh))
    {
        OutError = LOCTEXT("AutoFitMissingMesh", "Select a Static Mesh before calculating Auto Fit.");
        return false;
    }

    return CalculateAutoFitForBounds(
        Options.StaticMesh->GetBounds(),
        Options.FieldOfView,
        CubeShot::FrameFill,
        OutAutoFit,
        OutError);
}

bool FCubeShotCaptureService::CaptureRaw(
    const ECubeShotDirection Direction,
    const int32 TargetResolution,
    FCubeShotRawFrame& OutFrame,
    FText& OutError)
{
    OutFrame.Reset();
    OutError = FText::GetEmpty();
    if (!bInitialized || !PreviewScene.IsValid() || !IsValid(CurrentOptions.StaticMesh))
    {
        OutError = LOCTEXT("CaptureUnavailable", "CubeShot is not ready to capture a mesh.");
        return false;
    }

    if (Direction < ECubeShotDirection::Front || Direction >= ECubeShotDirection::Count)
    {
        OutError = LOCTEXT("InvalidDirection", "CubeShot received an invalid capture direction.");
        return false;
    }

    if (TargetResolution <= 0 || TargetResolution > CubeShot::MaxResolution)
    {
        OutError = FText::Format(
            LOCTEXT("CaptureResolutionRange", "Capture resolution must be between 1 and {0}."),
            CubeShot::MaxResolution);
        return false;
    }

    const int32 CaptureResolution = GetCaptureResolution(TargetResolution);
    if (!EnsureRenderTarget(CaptureResolution, OutError))
    {
        return false;
    }

    ApplyCamera(Direction);
    ApplyCameraRelativeLighting(Direction);

    if (UWorld* World = PreviewScene->GetWorld())
    {
        constexpr double FixedWorldTime = 1.0;
        World->TimeSeconds = FixedWorldTime;
        World->UnpausedTimeSeconds = FixedWorldTime;
        World->RealTimeSeconds = FixedWorldTime;
        World->AudioTimeSeconds = FixedWorldTime;
        World->DeltaTimeSeconds = 0.0f;
        World->DeltaRealTimeSeconds = 0.0f;
    }

    CaptureComponent->bCameraCutThisFrame = true;
    CaptureComponent->CaptureScene();

    FImage CapturedImage;
    if (!FImageUtils::GetRenderTargetImage(RenderTarget.Get(), CapturedImage))
    {
        OutError = LOCTEXT("RenderTargetReadbackFailed", "CubeShot could not read pixels from the render target.");
        return false;
    }

    return CubeShot::Image::ConvertInverseOpacityImage(CapturedImage, OutFrame, OutError);
}

bool FCubeShotCaptureService::ResolveBackground(const FCubeShotOptions& Options, int32 TargetResolution,
    TSharedPtr<const FImage>& OutImage, FText& OutError)
{
    OutImage.Reset();
    if (Options.BackgroundMode == ECubeShotBackgroundMode::Texture)
    {
        if (!IsValid(Options.BackgroundTexture))
        {
            OutError = LOCTEXT("MissingCompositeBackground", "The selected background texture is unavailable.");
            return false;
        }

        const FGuid SourceId = Options.BackgroundTexture->Source.GetId();
        const bool bCacheMatches = CachedBackgroundImage.IsValid()
            && CachedBackgroundTexture.Get() == Options.BackgroundTexture
            && CachedBackgroundSourceId == SourceId
            && CachedBackgroundResolution == TargetResolution;
        if (!bCacheMatches)
        {
            TSharedRef<FImage> NewBackground = MakeShared<FImage>();
            if (!CubeShot::Image::PrepareBackgroundImage(
                Options.BackgroundTexture,
                TargetResolution,
                NewBackground.Get(),
                OutError))
            {
                CachedBackgroundImage.Reset();
                CachedBackgroundTexture.Reset();
                CachedBackgroundSourceId.Invalidate();
                CachedBackgroundResolution = 0;
                return false;
            }

            CachedBackgroundImage = NewBackground;
            CachedBackgroundTexture = Options.BackgroundTexture;
            CachedBackgroundSourceId = SourceId;
            CachedBackgroundResolution = TargetResolution;
        }
        OutImage = CachedBackgroundImage;
    }
    else
    {
        CachedBackgroundImage.Reset();
        CachedBackgroundTexture.Reset();
        CachedBackgroundSourceId.Invalidate();
        CachedBackgroundResolution = 0;
    }

    return true;
}

bool FCubeShotCaptureService::Composite(const FCubeShotRawFrame& RawFrame, const FCubeShotOptions& Options,
    int32 TargetResolution, FCubeShotImage& OutImage, FText& OutError)
{
    TSharedPtr<const FImage> Background;
    if (!ResolveBackground(Options, TargetResolution, Background, OutError)) return false;
    return CubeShot::Image::Composite(RawFrame, Options, TargetResolution, OutImage, OutError, Background.Get());
}

bool FCubeShotCaptureService::PrepareCompositeWork(FCubeShotRawFrame RawFrame, const FCubeShotOptions& Options,
    int32 TargetResolution, FCubeShotPixelWork& OutWork, FText& OutError)
{
    TSharedPtr<const FImage> Background;
    if (!ResolveBackground(Options, TargetResolution, Background, OutError)) return false;
    FCubeShotOptions PixelOptions = Options;
    PixelOptions.StaticMesh = nullptr; PixelOptions.BackgroundTexture = nullptr;
    OutWork = [Frame = MoveTemp(RawFrame), PixelOptions, TargetResolution, Background](FCubeShotImage& Image, FText& Error)
    {
        return CubeShot::Image::Composite(Frame, PixelOptions, TargetResolution, Image, Error, Background.Get());
    };
    return true;
}

bool FCubeShotCaptureService::PrepareCaptureWork(const FCubeShotOptions& Options, ECubeShotDirection Direction,
    int32 TargetResolution, FCubeShotPixelWork& OutWork, FText& OutError)
{
    if (!(bInitialized ? UpdateOptions(Options, OutError) : Initialize(Options, OutError))) return false;
    FCubeShotRawFrame RawFrame;
    if (!CaptureRaw(Direction, TargetResolution, RawFrame, OutError)) return false;
    return PrepareCompositeWork(MoveTemp(RawFrame), Options, TargetResolution, OutWork, OutError);
}

bool FCubeShotCaptureService::CaptureAndComposite(const FCubeShotOptions& Options, ECubeShotDirection Direction,
    int32 TargetResolution, FCubeShotImage& OutImage, FText& OutError)
{
    OutImage.Reset(); FCubeShotPixelWork Work;
    if (!PrepareCaptureWork(Options, Direction, TargetResolution, Work, OutError)) return false;
    return Work(OutImage, OutError);
}

void FCubeShotCaptureService::Shutdown()
{
    bInitialized = false;
    CurrentOptions = FCubeShotOptions();
    CurrentAutoFit = FCubeShotAutoFitResult();
    CachedBackgroundImage.Reset();
    CachedBackgroundTexture.Reset();
    CachedBackgroundSourceId.Invalidate();
    CachedBackgroundResolution = 0;

    if (CaptureComponent != nullptr)
    {
        CaptureComponent->ClearShowOnlyComponents();
        CaptureComponent->TextureTarget = nullptr;
    }
    if (MeshComponent != nullptr)
    {
        MeshComponent->SetTextureForceResidentFlag(false);
    }
    RenderTarget.Reset();

    if (PreviewScene.IsValid())
    {
        if (CaptureComponent != nullptr)
        {
            PreviewScene->RemoveComponent(CaptureComponent);
        }
        if (MeshComponent != nullptr)
        {
            PreviewScene->RemoveComponent(MeshComponent);
        }
        if (FillLight != nullptr)
        {
            PreviewScene->RemoveComponent(FillLight);
        }
        if (RimLight != nullptr)
        {
            PreviewScene->RemoveComponent(RimLight);
        }
    }

    CaptureComponent = nullptr;
    MeshComponent = nullptr;
    FillLight = nullptr;
    RimLight = nullptr;
    PreviewScene.Reset();
}

bool FCubeShotCaptureService::CalculateAutoFitForBounds(
    const FBoxSphereBounds& Bounds,
    const double FieldOfViewDegrees,
    const double FrameFill,
    FCubeShotAutoFitResult& OutAutoFit,
    FText& OutError)
{
    OutAutoFit = FCubeShotAutoFitResult();
    OutError = FText::GetEmpty();

    if (!CubeShot::ValidateBounds(Bounds,OutError)) return false;
    if (!FMath::IsFinite(FieldOfViewDegrees) || FieldOfViewDegrees < 1.0 || FieldOfViewDegrees > 170.0)
    {
        OutError = LOCTEXT("InvalidAutoFitFov", "Auto Fit requires an FOV between 1 and 170 degrees.");
        return false;
    }
    if (!FMath::IsFinite(FrameFill) || FrameFill <= 0.0 || FrameFill > 1.0)
    {
        OutError = LOCTEXT("InvalidFrameFill", "Auto Fit frame fill must be greater than zero and no more than one.");
        return false;
    }

    const double Tangent = FMath::Tan(FMath::DegreesToRadians(FieldOfViewDegrees * 0.5));
    const TArray<FVector> Corners = GetBoundsCorners(Bounds);
    double RequiredDistance = 0.0;
    double MaximumScreenExtent = 0.0;
    double MaximumDepthExtent = 0.0;

    for (const FCubeShotDirectionInfo& DirectionInfo : CubeShot::GetDirectionInfos())
    {
        FVector Forward;
        FVector Right;
        FVector Up;
        GetCameraBasis(DirectionInfo, Forward, Right, Up);

        for (const FVector& Corner : Corners)
        {
            const FVector CenteredCorner = Corner - Bounds.Origin;
            const double ScreenExtent = FMath::Max(
                FMath::Abs(FVector::DotProduct(CenteredCorner, Right)),
                FMath::Abs(FVector::DotProduct(CenteredCorner, Up)));
            const double ForwardOffset = FVector::DotProduct(CenteredCorner, Forward);
            RequiredDistance = FMath::Max(
                RequiredDistance,
                ScreenExtent / (FrameFill * Tangent) - ForwardOffset);
            MaximumScreenExtent = FMath::Max(MaximumScreenExtent, ScreenExtent);
            MaximumDepthExtent = FMath::Max(MaximumDepthExtent, FMath::Abs(ForwardOffset));
        }
    }

    const double ClipMargin = FMath::Max(Bounds.SphereRadius * 0.1, MinimumClipMargin);
    RequiredDistance = FMath::Max(RequiredDistance, MaximumDepthExtent + ClipMargin + MinimumNearPlane);

    OutAutoFit.CameraDistance = RequiredDistance;
    OutAutoFit.OrthoWidth = FMath::Max(2.0 * MaximumScreenExtent / FrameFill, UE_SMALL_NUMBER);
    OutAutoFit.NearPlane = FMath::Max(MinimumNearPlane, RequiredDistance - MaximumDepthExtent - ClipMargin);
    OutAutoFit.FarPlane = FMath::Max(
        OutAutoFit.NearPlane + MinimumClipMargin,
        RequiredDistance + MaximumDepthExtent + ClipMargin);
    return true;
}

FTransform FCubeShotCaptureService::MakeCameraTransform(
    const FBoxSphereBounds& Bounds,
    const ECubeShotDirection Direction,
    const double CameraDistance)
{
    const FCubeShotDirectionInfo& DirectionInfo = CubeShot::GetDirectionInfo(Direction);
    const FVector Location = Bounds.Origin + DirectionInfo.CameraAxis.GetSafeNormal() * CameraDistance;
    const FRotator Rotation = FRotationMatrix::MakeFromXZ(
        DirectionInfo.ViewDirection.GetSafeNormal(),
        DirectionInfo.UpVector.GetSafeNormal()).Rotator();
    return FTransform(Rotation, Location);
}

int32 FCubeShotCaptureService::GetCaptureResolution(const int32 TargetResolution)
{
    if (TargetResolution <= 0)
    {
        return 0;
    }

    const int32 SupersampleFactor = TargetResolution <= 2048 ? 2 : 1;
    const int64 RequestedResolution = static_cast<int64>(TargetResolution) * SupersampleFactor;
    return static_cast<int32>(FMath::Min<int64>(RequestedResolution, MAX_int32));
}

bool FCubeShotCaptureService::CreatePreviewScene(FText& OutError)
{
    OutError = FText::GetEmpty();
    Shutdown();

    FPreviewScene::ConstructionValues ConstructionValues;
    ConstructionValues
        .SetCreateDefaultLighting(true)
        .SetCreatePhysicsScene(false)
        .ShouldSimulatePhysics(false)
        .SetTransactional(false)
        .SetEditor(true)
        .SetSkyBrightness(0.35f)
        .SetLightBrightness(3.0f);
    PreviewScene = MakeUnique<FPreviewScene>(ConstructionValues);
    if (!PreviewScene.IsValid() || !PreviewScene->IsInitialized())
    {
        OutError = LOCTEXT("PreviewSceneCreationFailed", "CubeShot could not create its isolated preview scene.");
        return false;
    }

    MeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    CaptureComponent = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
    FillLight = NewObject<UDirectionalLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    RimLight = NewObject<UDirectionalLightComponent>(GetTransientPackage(), NAME_None, RF_Transient);
    RenderTarget.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));

    if (MeshComponent == nullptr || CaptureComponent == nullptr || FillLight == nullptr || RimLight == nullptr || !RenderTarget.IsValid())
    {
        OutError = LOCTEXT("PreviewComponentCreationFailed", "CubeShot could not create one or more preview components.");
        Shutdown();
        return false;
    }

    MeshComponent->SetMobility(EComponentMobility::Movable);
    MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    MeshComponent->SetCastShadow(true);
    MeshComponent->bDisallowNanite = true;
    MeshComponent->SetForcedLodModel(1);

    FillLight->SetMobility(EComponentMobility::Movable);
    FillLight->SetIntensity(1.2f);
    FillLight->SetLightColor(FLinearColor::White, false);
    FillLight->SetCastShadows(false);
    FillLight->SetVolumetricScatteringIntensity(0.0f);

    RimLight->SetMobility(EComponentMobility::Movable);
    RimLight->SetIntensity(1.6f);
    RimLight->SetLightColor(FLinearColor::White, false);
    RimLight->SetCastShadows(false);
    RimLight->SetVolumetricScatteringIntensity(0.0f);

    PreviewScene->AddComponent(MeshComponent, FTransform::Identity);
    PreviewScene->AddComponent(CaptureComponent, FTransform::Identity);
    PreviewScene->AddComponent(FillLight, FTransform::Identity);
    PreviewScene->AddComponent(RimLight, FTransform::Identity);
    ConfigureCaptureComponent();
    return true;
}

bool FCubeShotCaptureService::EnsureRenderTarget(const int32 CaptureResolution, FText& OutError)
{
    if (!RenderTarget.IsValid() || CaptureResolution <= 0)
    {
        OutError = LOCTEXT("InvalidRenderTarget", "CubeShot cannot allocate the requested render target.");
        return false;
    }

    const int32 MaximumDimension = static_cast<int32>(GetMax2DTextureDimension());
    if (CaptureResolution > MaximumDimension)
    {
        OutError = FText::Format(
            LOCTEXT("RenderTargetTooLarge", "The requested capture size exceeds this RHI's {0}-pixel texture limit."),
            MaximumDimension);
        return false;
    }

    if (RenderTarget->SizeX != CaptureResolution || RenderTarget->SizeY != CaptureResolution
        || RenderTarget->GetFormat() != PF_FloatRGBA)
    {
        RenderTarget->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
        RenderTarget->RenderTargetFormat = RTF_RGBA16f;
        RenderTarget->bAutoGenerateMips = false;
        RenderTarget->InitCustomFormat(CaptureResolution, CaptureResolution, PF_FloatRGBA, true);
        RenderTarget->UpdateResourceImmediate(true);
    }

    CaptureComponent->TextureTarget = RenderTarget.Get();
    return true;
}

void FCubeShotCaptureService::ConfigureCaptureComponent()
{
    CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
    CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    CaptureComponent->bCaptureEveryFrame = false;
    CaptureComponent->bCaptureOnMovement = false;
    CaptureComponent->bAlwaysPersistRenderingState = false;
    CaptureComponent->bExcludeFromSceneTextureExtents = true;
    CaptureComponent->bConsiderUnrenderedOpaquePixelAsFullyTranslucent = true;
    CaptureComponent->PostProcessBlendWeight = 1.0f;

    FPostProcessSettings& PostProcess = CaptureComponent->PostProcessSettings;
    PostProcess.bOverride_AutoExposureMethod = true;
    PostProcess.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    PostProcess.bOverride_AutoExposureBias = true;
    PostProcess.AutoExposureBias = 0.0f;
    PostProcess.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
    PostProcess.AutoExposureApplyPhysicalCameraExposure = false;
    PostProcess.bOverride_BloomIntensity = true;
    PostProcess.BloomIntensity = 0.0f;
    PostProcess.bOverride_VignetteIntensity = true;
    PostProcess.VignetteIntensity = 0.0f;
    PostProcess.bOverride_FilmGrainIntensity = true;
    PostProcess.FilmGrainIntensity = 0.0f;
    PostProcess.bOverride_MotionBlurAmount = true;
    PostProcess.MotionBlurAmount = 0.0f;
    PostProcess.bOverride_DepthOfFieldEnabled = true;
    PostProcess.DepthOfFieldEnabled = false;
    CaptureComponent->ShowFlags.SetAntiAliasing(false);
    CaptureComponent->ShowFlags.SetTemporalAA(false);
    CaptureComponent->ShowFlags.SetFog(false);
    CaptureComponent->ShowFlags.SetVolumetricFog(false);
    CaptureComponent->ShowFlags.SetAtmosphere(false);
    CaptureComponent->ShowFlags.SetCloud(false);
    CaptureComponent->ShowFlags.SetBloom(false);
    CaptureComponent->ShowFlags.SetLocalExposure(false);
    CaptureComponent->ShowFlags.SetVignette(false);
    CaptureComponent->ShowFlags.SetGrain(false);
    CaptureComponent->ShowFlags.SetDepthOfField(false);
    CaptureComponent->ShowFlags.SetMotionBlur(false);
    CaptureComponent->ShowFlags.SetSelectionOutline(false);
}

void FCubeShotCaptureService::ApplyCamera(const ECubeShotDirection Direction)
{
    const FBoxSphereBounds Bounds = CurrentOptions.StaticMesh->GetBounds();
    const FTransform CameraTransform = MakeCameraTransform(Bounds, Direction, CurrentOptions.CameraDistance);
    CaptureComponent->SetWorldTransform(CameraTransform);
    CaptureComponent->FOVAngle = static_cast<float>(CurrentOptions.FieldOfView);
    CaptureComponent->OrthoWidth = static_cast<float>(CurrentOptions.OrthoWidth);
}

void FCubeShotCaptureService::ApplyCameraRelativeLighting(const ECubeShotDirection Direction)
{
    const FCubeShotDirectionInfo& DirectionInfo = CubeShot::GetDirectionInfo(Direction);
    FVector Forward;
    FVector Right;
    FVector Up;
    GetCameraBasis(DirectionInfo, Forward, Right, Up);

    const FVector KeyRayDirection = (Forward + Right * 0.45 - Up * 0.65).GetSafeNormal();
    const FVector FillRayDirection = (Forward - Right * 0.8 - Up * 0.15).GetSafeNormal();
    const FVector RimRayDirection = (-Forward + Right * 0.25 - Up * 0.5).GetSafeNormal();

    PreviewScene->SetLightDirection(FRotationMatrix::MakeFromX(KeyRayDirection).Rotator());
    PreviewScene->SetLightBrightness(3.0f);
    PreviewScene->SetLightColor(FColor::White);
    FillLight->SetWorldRotation(FRotationMatrix::MakeFromX(FillRayDirection).Rotator());
    RimLight->SetWorldRotation(FRotationMatrix::MakeFromX(RimRayDirection).Rotator());
}

#undef LOCTEXT_NAMESPACE
