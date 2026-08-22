#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"

class UStaticMesh;
class UTexture2D;

enum class ECubeShotDirection : uint8
{
    Front,
    Back,
    Left,
    Right,
    Top,
    Bottom,
    Count
};

enum class ECubeShotBackgroundMode : uint8
{
    Transparent,
    SolidColor,
    Texture
};

enum class ECubeShotProjectionMode : uint8
{
    Perspective,
    Orthographic
};

struct FCubeShotDirectionInfo
{
    ECubeShotDirection Direction = ECubeShotDirection::Front;
    const TCHAR* Label = TEXT("Front");
    const TCHAR* AssetSuffix = TEXT("Front");
    FVector CameraAxis = FVector::XAxisVector;
    FVector ViewDirection = -FVector::XAxisVector;
    FVector UpVector = FVector::ZAxisVector;
};

struct FCubeShotOptions
{
    UStaticMesh* StaticMesh = nullptr;
    UTexture2D* BackgroundTexture = nullptr;
    ECubeShotBackgroundMode BackgroundMode = ECubeShotBackgroundMode::Transparent;
    ECubeShotProjectionMode ProjectionMode = ECubeShotProjectionMode::Perspective;
    FLinearColor SolidColor = FLinearColor::FromSRGBColor(FColor(32, 32, 32));
    double CameraDistance = 100.0;
    double FieldOfView = 45.0;
    double OrthoWidth = 100.0;
    int32 Resolution = 1024;
    bool bAutoFit = true;
};

struct FCubeShotAutoFitResult
{
    double CameraDistance = 100.0;
    double OrthoWidth = 100.0;
    double NearPlane = 1.0;
    double FarPlane = 1000.0;
};

/** Linear scene color is premultiplied by opacity. Alpha stores ordinary opacity, not inverse opacity. */
struct FCubeShotRawFrame
{
    int32 Width = 0;
    int32 Height = 0;
    TArray<FFloat16Color> PremultipliedLinearPixels;

    bool IsValid() const
    {
        return Width > 0 && Height > 0 && PremultipliedLinearPixels.Num() == Width * Height;
    }

    void Reset()
    {
        Width = 0;
        Height = 0;
        PremultipliedLinearPixels.Reset();
    }
};

/** BGRA8/sRGB pixels ready for a transient preview texture, staging PNG, or Texture2D source. */
struct FCubeShotImage
{
    int32 Width = 0;
    int32 Height = 0;
    TArray<FColor> Pixels;
    bool bHasTransparency = false;

    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Pixels.Num() == Width * Height;
    }

    void Reset()
    {
        Width = 0;
        Height = 0;
        Pixels.Reset();
        bHasTransparency = false;
    }
};

struct FCubeShotOutputDescriptor
{
    ECubeShotDirection Direction = ECubeShotDirection::Front;
    FString LongPackageName;
    FString AssetName;

    FString GetObjectPath() const
    {
        return LongPackageName + TEXT(".") + AssetName;
    }
};

struct FCubeShotPreflightResult
{
    bool bCanProceed = false;
    bool bHasExistingTextures = false;
    TArray<FCubeShotOutputDescriptor> Outputs;
    FText Error;
    FText Warning;
};

struct FCubeShotGenerationResult
{
    bool bSucceeded = false;
    bool bCancelled = false;
    FString StagingDirectory;
    TArray<FString> SavedObjectPaths;
    TArray<FString> FailedObjectPaths;
    FText Message;
};

namespace CubeShot
{
    constexpr int32 MinResolution = 256;
    constexpr int32 MaxResolution = 4096;
    constexpr int32 PreviewResolution = 512;
    constexpr double FrameFill = 0.90;

    const TArray<FCubeShotDirectionInfo>& GetDirectionInfos();
    const FCubeShotDirectionInfo& GetDirectionInfo(ECubeShotDirection Direction);
    bool IsPowerOfTwoResolution(int32 Resolution);
    /**
     * Validates editor source art without converting it to RGBA. Passing bProbeMip0=true
     * briefly locks mip 0 to prove its payload is readable; callers should cache that result.
     */
    bool ValidateBackgroundTextureSource(
        UTexture2D* Texture,
        bool bProbeMip0,
        FText& OutError);
    bool ValidateOptions(const FCubeShotOptions& Options, FText& OutError, FText& OutWarning);
}
