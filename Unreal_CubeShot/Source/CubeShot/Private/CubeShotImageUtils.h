#pragma once

#include "CoreMinimal.h"
#include "CubeShotTypes.h"

struct FImage;
class UTexture2D;

namespace CubeShot::Image
{
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
