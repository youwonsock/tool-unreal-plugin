#include "CubeShotImageUtils.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "ImageUtils.h"

#define LOCTEXT_NAMESPACE "CubeShotImageUtils"

namespace
{
    constexpr float AlphaEpsilon = 1.0e-6f;
    constexpr int32 MaximumDilationDistance = 32;
    // SCS_SceneColorHDR is captured with manual exposure and physical camera exposure disabled.
    // Keeping the remaining CubeShot exposure here makes preview and asset generation identical.
    constexpr float FixedExposureMultiplier = 1.0f;

    bool IsValidImage(const FImage& Image)
    {
        return Image.IsImageInfoValid()
            && Image.SizeX > 0
            && Image.SizeY > 0
            && Image.NumSlices > 0
            && Image.RawData.Num() == Image.GetImageSizeBytes();
    }

    float SanitizeChannel(const float Value)
    {
        return FMath::IsFinite(Value) ? FMath::Max(Value, 0.0f) : 0.0f;
    }

    FLinearColor SanitizePremultiplied(const FLinearColor& Color)
    {
        return FLinearColor(
            SanitizeChannel(Color.R),
            SanitizeChannel(Color.G),
            SanitizeChannel(Color.B),
            FMath::IsFinite(Color.A) ? FMath::Clamp(Color.A, 0.0f, 1.0f) : 0.0f);
    }

    bool LoadBackgroundSourceImage(UTexture2D* Texture, FImage& OutSourceImage, FText& OutError)
    {
        if (!IsValid(Texture))
        {
            OutError = LOCTEXT("MissingBackgroundTexture", "The selected background texture is unavailable.");
            return false;
        }

        if (!FImageUtils::GetTexture2DSourceImage(Texture, OutSourceImage) || !IsValidImage(OutSourceImage))
        {
            OutError = FText::Format(
                LOCTEXT("UnreadableBackgroundTexture", "CubeShot could not read source pixels from {0}."),
                FText::FromString(Texture->GetPathName()));
            return false;
        }

        return true;
    }

    FLinearColor SampleBackgroundBilinear(const FImage& Image, const FVector2D& UV)
    {
        const double PixelX = FMath::Clamp(UV.X, 0.0, 1.0) * static_cast<double>(FMath::Max(Image.SizeX - 1, 0));
        const double PixelY = FMath::Clamp(UV.Y, 0.0, 1.0) * static_cast<double>(FMath::Max(Image.SizeY - 1, 0));
        const int32 X0 = FMath::FloorToInt(PixelX);
        const int32 Y0 = FMath::FloorToInt(PixelY);
        const int32 X1 = FMath::Min(X0 + 1, Image.SizeX - 1);
        const int32 Y1 = FMath::Min(Y0 + 1, Image.SizeY - 1);
        const float FractionX = static_cast<float>(PixelX - static_cast<double>(X0));
        const float FractionY = static_cast<float>(PixelY - static_cast<double>(Y0));

        const FLinearColor Top = FMath::Lerp(
            Image.GetOnePixelLinear(X0, Y0),
            Image.GetOnePixelLinear(X1, Y0),
            FractionX);
        const FLinearColor Bottom = FMath::Lerp(
            Image.GetOnePixelLinear(X0, Y1),
            Image.GetOnePixelLinear(X1, Y1),
            FractionX);
        return FMath::Lerp(Top, Bottom, FractionY);
    }
}

bool CubeShot::Image::ConvertInverseOpacityImage(
    const FImage& SourceImage,
    FCubeShotRawFrame& OutFrame,
    FText& OutError)
{
    OutFrame.Reset();
    OutError = FText::GetEmpty();

    if (!IsValidImage(SourceImage) || SourceImage.NumSlices != 1)
    {
        OutError = LOCTEXT("InvalidCapturedImage", "The render target returned an invalid image.");
        return false;
    }

    FImage ConvertedHalfImage;
    const FImage* LinearHalfImage = &SourceImage;
    if (SourceImage.Format != ERawImageFormat::RGBA16F || SourceImage.GammaSpace != EGammaSpace::Linear)
    {
        SourceImage.CopyTo(ConvertedHalfImage, ERawImageFormat::RGBA16F, EGammaSpace::Linear);
        LinearHalfImage = &ConvertedHalfImage;
    }
    if (!IsValidImage(*LinearHalfImage))
    {
        OutError = LOCTEXT("CapturedImageConversionFailed", "CubeShot could not convert the captured image to linear RGBA16F.");
        return false;
    }

    OutFrame.Width = LinearHalfImage->SizeX;
    OutFrame.Height = LinearHalfImage->SizeY;
    OutFrame.PremultipliedLinearPixels.SetNumUninitialized(OutFrame.Width * OutFrame.Height);

    const TArrayView64<const FFloat16Color> SourcePixels = LinearHalfImage->AsRGBA16F();
    for (int64 Index = 0; Index < SourcePixels.Num(); ++Index)
    {
        FLinearColor Pixel = SourcePixels[Index].GetFloats();
        Pixel.R = SanitizeChannel(Pixel.R);
        Pixel.G = SanitizeChannel(Pixel.G);
        Pixel.B = SanitizeChannel(Pixel.B);
        Pixel.A = FMath::IsFinite(Pixel.A) ? 1.0f - FMath::Clamp(Pixel.A, 0.0f, 1.0f) : 0.0f;
        OutFrame.PremultipliedLinearPixels[static_cast<int32>(Index)] = FFloat16Color(Pixel);
    }

    return true;
}

bool CubeShot::Image::DownsamplePremultiplied(
    const FCubeShotRawFrame& SourceFrame,
    const int32 TargetWidth,
    const int32 TargetHeight,
    FCubeShotRawFrame& OutFrame,
    FText& OutError)
{
    OutFrame.Reset();
    OutError = FText::GetEmpty();

    if (!SourceFrame.IsValid() || TargetWidth <= 0 || TargetHeight <= 0
        || TargetWidth > SourceFrame.Width || TargetHeight > SourceFrame.Height)
    {
        OutError = LOCTEXT("InvalidDownsampleInput", "The captured frame cannot be downsampled to the requested size.");
        return false;
    }

    if (TargetWidth == SourceFrame.Width && TargetHeight == SourceFrame.Height)
    {
        OutFrame = SourceFrame;
        return true;
    }

    OutFrame.Width = TargetWidth;
    OutFrame.Height = TargetHeight;
    OutFrame.PremultipliedLinearPixels.SetNumUninitialized(TargetWidth * TargetHeight);

    for (int32 TargetY = 0; TargetY < TargetHeight; ++TargetY)
    {
        const int32 SourceY0 = FMath::FloorToInt(static_cast<double>(TargetY) * SourceFrame.Height / TargetHeight);
        const int32 SourceY1 = FMath::Max(
            SourceY0 + 1,
            FMath::CeilToInt(static_cast<double>(TargetY + 1) * SourceFrame.Height / TargetHeight));

        for (int32 TargetX = 0; TargetX < TargetWidth; ++TargetX)
        {
            const int32 SourceX0 = FMath::FloorToInt(static_cast<double>(TargetX) * SourceFrame.Width / TargetWidth);
            const int32 SourceX1 = FMath::Max(
                SourceX0 + 1,
                FMath::CeilToInt(static_cast<double>(TargetX + 1) * SourceFrame.Width / TargetWidth));

            FLinearColor Sum = FLinearColor::Transparent;
            int32 SampleCount = 0;
            for (int32 SourceY = SourceY0; SourceY < FMath::Min(SourceY1, SourceFrame.Height); ++SourceY)
            {
                for (int32 SourceX = SourceX0; SourceX < FMath::Min(SourceX1, SourceFrame.Width); ++SourceX)
                {
                    Sum += SanitizePremultiplied(
                        SourceFrame.PremultipliedLinearPixels[SourceY * SourceFrame.Width + SourceX].GetFloats());
                    ++SampleCount;
                }
            }

            const FLinearColor Average = SampleCount > 0 ? Sum / static_cast<float>(SampleCount) : FLinearColor::Transparent;
            OutFrame.PremultipliedLinearPixels[TargetY * TargetWidth + TargetX] = FFloat16Color(Average);
        }
    }

    return true;
}

FLinearColor CubeShot::Image::ApplyAcesFitted(const FLinearColor& LinearColor)
{
    const auto FitChannel = [](const float Value)
    {
        const float X = SanitizeChannel(Value);
        return FMath::Clamp((X * (2.51f * X + 0.03f)) / (X * (2.43f * X + 0.59f) + 0.14f), 0.0f, 1.0f);
    };

    return FLinearColor(FitChannel(LinearColor.R), FitChannel(LinearColor.G), FitChannel(LinearColor.B), LinearColor.A);
}

FVector2D CubeShot::Image::ComputeCoverUV(
    const FVector2D& DestinationUV,
    const FIntPoint& SourceSize,
    const FIntPoint& DestinationSize)
{
    if (SourceSize.X <= 0 || SourceSize.Y <= 0 || DestinationSize.X <= 0 || DestinationSize.Y <= 0)
    {
        return FVector2D(0.5, 0.5);
    }

    const double SourceAspect = static_cast<double>(SourceSize.X) / SourceSize.Y;
    const double DestinationAspect = static_cast<double>(DestinationSize.X) / DestinationSize.Y;
    FVector2D Result = DestinationUV;

    if (SourceAspect > DestinationAspect)
    {
        const double VisibleWidth = DestinationAspect / SourceAspect;
        Result.X = 0.5 + (DestinationUV.X - 0.5) * VisibleWidth;
    }
    else if (SourceAspect < DestinationAspect)
    {
        const double VisibleHeight = SourceAspect / DestinationAspect;
        Result.Y = 0.5 + (DestinationUV.Y - 0.5) * VisibleHeight;
    }

    Result.X = FMath::Clamp(Result.X, 0.0, 1.0);
    Result.Y = FMath::Clamp(Result.Y, 0.0, 1.0);
    return Result;
}

void CubeShot::Image::DilateTransparentRGB(FCubeShotImage& Image)
{
    if (!Image.IsValid())
    {
        return;
    }

    TBitArray<> Visited(false, Image.Pixels.Num());
    TArray<int32> CurrentFrontier;
    TArray<int32> NextFrontier;

    const auto VisitTransparentNeighbors = [&Image, &Visited](
        const int32 SourceIndex,
        TArray<int32>& OutFrontier)
    {
        const int32 X = SourceIndex % Image.Width;
        const int32 Y = SourceIndex / Image.Width;
        const FColor SourceColor = Image.Pixels[SourceIndex];
        const int32 Neighbors[4] =
        {
            X > 0 ? SourceIndex - 1 : INDEX_NONE,
            X + 1 < Image.Width ? SourceIndex + 1 : INDEX_NONE,
            Y > 0 ? SourceIndex - Image.Width : INDEX_NONE,
            Y + 1 < Image.Height ? SourceIndex + Image.Width : INDEX_NONE
        };

        for (const int32 NeighborIndex : Neighbors)
        {
            if (NeighborIndex == INDEX_NONE || Visited[NeighborIndex])
            {
                continue;
            }

            Visited[NeighborIndex] = true;
            FColor& Neighbor = Image.Pixels[NeighborIndex];
            Neighbor.R = SourceColor.R;
            Neighbor.G = SourceColor.G;
            Neighbor.B = SourceColor.B;
            OutFrontier.Add(NeighborIndex);
        }
    };

    for (int32 Index = 0; Index < Image.Pixels.Num(); ++Index)
    {
        if (Image.Pixels[Index].A > 0)
        {
            Visited[Index] = true;
        }
    }

    // Seed only the first transparent ring instead of enqueuing every foreground
    // texel. This keeps the 4K working set proportional to the silhouette boundary.
    for (int32 Index = 0; Index < Image.Pixels.Num(); ++Index)
    {
        if (Image.Pixels[Index].A > 0)
        {
            VisitTransparentNeighbors(Index, CurrentFrontier);
        }
    }

    for (int32 Distance = 1; Distance < MaximumDilationDistance && !CurrentFrontier.IsEmpty(); ++Distance)
    {
        NextFrontier.Reset();
        for (const int32 SourceIndex : CurrentFrontier)
        {
            VisitTransparentNeighbors(SourceIndex, NextFrontier);
        }
        Swap(CurrentFrontier, NextFrontier);
    }
}

bool CubeShot::Image::PrepareBackgroundImage(
    UTexture2D* Texture,
    const int32 TargetResolution,
    FImage& OutLinearImage,
    FText& OutError)
{
    OutLinearImage.Reset();
    OutError = FText::GetEmpty();
    if (TargetResolution <= 0)
    {
        OutError = LOCTEXT("InvalidBackgroundResolution", "The background target resolution must be greater than zero.");
        return false;
    }

    FImage SourceImage;
    if (!LoadBackgroundSourceImage(Texture, SourceImage, OutError))
    {
        return false;
    }

    OutLinearImage = FImage(
        TargetResolution,
        TargetResolution,
        1,
        ERawImageFormat::RGBA16F,
        EGammaSpace::Linear);
    if (!IsValidImage(OutLinearImage))
    {
        OutError = LOCTEXT("BackgroundAllocationFailed", "CubeShot could not allocate the prepared background image.");
        return false;
    }

    TArrayView64<FFloat16Color> DestinationPixels = OutLinearImage.AsRGBA16F();
    const FIntPoint SourceSize(SourceImage.SizeX, SourceImage.SizeY);
    const FIntPoint DestinationSize(TargetResolution, TargetResolution);
    for (int32 Y = 0; Y < TargetResolution; ++Y)
    {
        for (int32 X = 0; X < TargetResolution; ++X)
        {
            const FVector2D DestinationUV(
                (static_cast<double>(X) + 0.5) / TargetResolution,
                (static_cast<double>(Y) + 0.5) / TargetResolution);
            const FVector2D SourceUV = ComputeCoverUV(DestinationUV, SourceSize, DestinationSize);
            DestinationPixels[Y * TargetResolution + X] = FFloat16Color(
                SampleBackgroundBilinear(SourceImage, SourceUV));
        }
    }

    // Release the potentially much larger source image before the six output faces
    // are composited. The cached target uses 8 bytes per pixel instead of RGBA32F's 16.
    SourceImage.Reset();
    return true;
}

bool CubeShot::Image::Composite(
    const FCubeShotRawFrame& RawFrame,
    const FCubeShotOptions& Options,
    const int32 TargetResolution,
    FCubeShotImage& OutImage,
    FText& OutError,
    const FImage* PreparedBackgroundImage)
{
    OutImage.Reset();
    OutError = FText::GetEmpty();

    if (!RawFrame.IsValid() || TargetResolution <= 0)
    {
        OutError = LOCTEXT("InvalidCompositeInput", "CubeShot cannot composite an invalid captured frame.");
        return false;
    }

    FCubeShotRawFrame DownsampledFrame;
    const FCubeShotRawFrame* CompositingFrame = &RawFrame;
    if ((RawFrame.Width != TargetResolution || RawFrame.Height != TargetResolution)
        && !DownsamplePremultiplied(RawFrame, TargetResolution, TargetResolution, DownsampledFrame, OutError))
    {
        return false;
    }
    if (DownsampledFrame.IsValid())
    {
        CompositingFrame = &DownsampledFrame;
    }

    FImage LocalBackgroundImage;
    const FImage* BackgroundImage = PreparedBackgroundImage;
    if (Options.BackgroundMode == ECubeShotBackgroundMode::Texture && BackgroundImage == nullptr)
    {
        if (!PrepareBackgroundImage(Options.BackgroundTexture, TargetResolution, LocalBackgroundImage, OutError))
        {
            return false;
        }
        BackgroundImage = &LocalBackgroundImage;
    }
    if (Options.BackgroundMode == ECubeShotBackgroundMode::Texture
        && (BackgroundImage == nullptr
            || !IsValidImage(*BackgroundImage)
            || BackgroundImage->SizeX != TargetResolution
            || BackgroundImage->SizeY != TargetResolution))
    {
        OutError = LOCTEXT("InvalidPreparedBackground", "The prepared texture background does not match the output resolution.");
        return false;
    }

    OutImage.Width = TargetResolution;
    OutImage.Height = TargetResolution;
    OutImage.bHasTransparency = Options.BackgroundMode == ECubeShotBackgroundMode::Transparent;
    OutImage.Pixels.SetNumUninitialized(TargetResolution * TargetResolution);

    for (int32 Y = 0; Y < TargetResolution; ++Y)
    {
        for (int32 X = 0; X < TargetResolution; ++X)
        {
            const int32 Index = Y * TargetResolution + X;
            const FLinearColor Premultiplied = SanitizePremultiplied(
                CompositingFrame->PremultipliedLinearPixels[Index].GetFloats());
            const float Alpha = Premultiplied.A;
            const FLinearColor StraightLinear = Alpha > AlphaEpsilon
                ? FLinearColor(
                    Premultiplied.R * FixedExposureMultiplier / Alpha,
                    Premultiplied.G * FixedExposureMultiplier / Alpha,
                    Premultiplied.B * FixedExposureMultiplier / Alpha,
                    Alpha)
                : FLinearColor::Transparent;
            const FLinearColor DisplayForeground = ApplyAcesFitted(StraightLinear);

            if (Options.BackgroundMode == ECubeShotBackgroundMode::Transparent)
            {
                FColor Pixel = DisplayForeground.ToFColorSRGB();
                Pixel.A = static_cast<uint8>(FMath::RoundToInt(Alpha * 255.0f));
                OutImage.Pixels[Index] = Pixel;
                continue;
            }

            FLinearColor Background = Options.SolidColor;
            if (Options.BackgroundMode == ECubeShotBackgroundMode::Texture)
            {
                Background = BackgroundImage->GetOnePixelLinear(X, Y);
            }

            Background.R = SanitizeChannel(Background.R);
            Background.G = SanitizeChannel(Background.G);
            Background.B = SanitizeChannel(Background.B);
            const FLinearColor Composited(
                DisplayForeground.R * Alpha + Background.R * (1.0f - Alpha),
                DisplayForeground.G * Alpha + Background.G * (1.0f - Alpha),
                DisplayForeground.B * Alpha + Background.B * (1.0f - Alpha),
                1.0f);
            OutImage.Pixels[Index] = Composited.ToFColorSRGB();
            OutImage.Pixels[Index].A = 255;
        }
    }

    if (OutImage.bHasTransparency)
    {
        DilateTransparentRGB(OutImage);
    }

    return true;
}

#undef LOCTEXT_NAMESPACE
