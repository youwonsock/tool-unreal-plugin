#pragma once

#include "CoreMinimal.h"
#include "CubeShotTypes.h"

struct FImage;
class UTexture2D;

struct FCubeShotPngExportInfo
{
    int32 SourceWidth = 0;
    int32 SourceHeight = 0;
    int32 Width = 0;
    int32 Height = 0;
    int64 VisiblePixels = 0;
    int64 TransparentPixels = 0;
    bool bSubjectTouchesEdge = false;
    bool bExistingFileObserved = false;
};

namespace CubeShot::Image
{
    bool SavePngAtomic(const FString& Filename, const FImage& Image, bool bOverwrite, FText& OutError,
        bool* ExistingFileObserved = nullptr, TFunction<void()> BeforeRename = {});
    /** Pure CPU/file operation. Source ownership must already be detached from UObjects. */
    bool ExportOwnedImage(const FImage& Source, const FString& Filename, bool bTrim, int32 Padding,
        int32 QuarterTurns, bool bOverwrite, FCubeShotPngExportInfo& Info, FText& Error);
    FString GetPngFilename(const FString& TexturePath, bool bTrim, int32 Padding, int32 QuarterTurns);
    /** Trims transparent margins and rotates in 90-degree steps without resampling captured pixels. */
    bool PrepareIconExport(const FImage& Source, bool bTrimTransparent, int32 PaddingPixels,
        int32 ClockwiseQuarterTurns, FImage& OutImage, FText& OutError);

    /** Converts SCS_SceneColorHDR's inverse-opacity alpha to ordinary opacity without touching RGB. */
    bool ConvertInverseOpacityImage(const FImage& SourceImage, FCubeShotRawFrame& OutFrame, FText& OutError);

    /** Box-filters premultiplied linear RGBA so color and coverage share the same reconstruction filter. */
    bool DownsamplePremultiplied(
        const FCubeShotRawFrame& SourceFrame,
        int32 TargetWidth,
        int32 TargetHeight,
        FCubeShotRawFrame& OutFrame,
        FText& OutError);

    /** Fixed ACES-fitted display curve used by both previews and generated textures. */
    FLinearColor ApplyAcesFitted(const FLinearColor& LinearColor);

    /** Returns source UVs for a centered, aspect-preserving cover/crop. */
    FVector2D ComputeCoverUV(const FVector2D& DestinationUV, const FIntPoint& SourceSize, const FIntPoint& DestinationSize);

    /** Propagates the nearest visible foreground RGB into zero-alpha texels without changing alpha. */
    void DilateTransparentRGB(FCubeShotImage& Image);

    /** Reads and center-cover resamples a texture once for reuse across a six-face capture revision. */
    bool PrepareBackgroundImage(
        UTexture2D* Texture,
        int32 TargetResolution,
        FImage& OutLinearImage,
        FText& OutError);

    /** Tone maps and composites a cached raw frame into a BGRA8/sRGB output image. */
    bool Composite(
        const FCubeShotRawFrame& RawFrame,
        const FCubeShotOptions& Options,
        int32 TargetResolution,
        FCubeShotImage& OutImage,
        FText& OutError,
        const FImage* PreparedBackgroundImage = nullptr);
}
