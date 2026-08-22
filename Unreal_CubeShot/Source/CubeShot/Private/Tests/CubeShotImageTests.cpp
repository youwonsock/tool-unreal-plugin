#include "CubeShotImageUtils.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotInverseOpacityTest,
    "CubeShot.Editor.Image.InverseOpacity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotInverseOpacityTest::RunTest(const FString& Parameters)
{
    FImage CapturedImage(1, 1, ERawImageFormat::RGBA16F, EGammaSpace::Linear);
    CapturedImage.AsRGBA16F()[0] = FFloat16Color(FLinearColor(0.25f, 0.5f, 0.75f, 0.8f));

    FCubeShotRawFrame RawFrame;
    FText Error;
    TestTrue(TEXT("Inverse-opacity image converts"), CubeShot::Image::ConvertInverseOpacityImage(CapturedImage, RawFrame, Error));
    TestTrue(TEXT("Converted frame is valid"), RawFrame.IsValid());
    if (RawFrame.IsValid())
    {
        const FLinearColor Pixel = RawFrame.PremultipliedLinearPixels[0].GetFloats();
        TestEqual(TEXT("Premultiplied R is unchanged"), Pixel.R, 0.25f);
        TestEqual(TEXT("Premultiplied G is unchanged"), Pixel.G, 0.5f);
        TestTrue(TEXT("Ordinary alpha is one minus capture alpha"), FMath::IsNearlyEqual(Pixel.A, 0.2f, 0.001f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotPremultipliedDownsampleTest,
    "CubeShot.Editor.Image.PremultipliedDownsample",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotPremultipliedDownsampleTest::RunTest(const FString& Parameters)
{
    FCubeShotRawFrame Source;
    Source.Width = 2;
    Source.Height = 2;
    Source.PremultipliedLinearPixels =
    {
        FFloat16Color(FLinearColor(1.0f, 0.0f, 0.0f, 1.0f)),
        FFloat16Color(FLinearColor::Transparent),
        FFloat16Color(FLinearColor::Transparent),
        FFloat16Color(FLinearColor::Transparent)
    };

    FCubeShotRawFrame Downsampled;
    FText Error;
    TestTrue(
        TEXT("Premultiplied frame downsamples"),
        CubeShot::Image::DownsamplePremultiplied(Source, 1, 1, Downsampled, Error));
    if (Downsampled.IsValid())
    {
        const FLinearColor Pixel = Downsampled.PremultipliedLinearPixels[0].GetFloats();
        TestTrue(TEXT("Coverage is box filtered"), FMath::IsNearlyEqual(Pixel.A, 0.25f, 0.001f));
        TestTrue(TEXT("Premultiplied red is filtered with coverage"), FMath::IsNearlyEqual(Pixel.R, 0.25f, 0.001f));
    }

    FCubeShotOptions Options;
    Options.BackgroundMode = ECubeShotBackgroundMode::Transparent;
    FCubeShotImage Output;
    TestTrue(TEXT("Filtered frame composites"), CubeShot::Image::Composite(Source, Options, 1, Output, Error));
    if (Output.IsValid())
    {
        TestTrue(TEXT("Unpremultiply preserves foreground red"), Output.Pixels[0].R > 200);
        TestTrue(TEXT("Output alpha preserves coverage"), FMath::Abs(static_cast<int32>(Output.Pixels[0].A) - 64) <= 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotAcesAndCompositionTest,
    "CubeShot.Editor.Image.AcesAndComposition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotAcesAndCompositionTest::RunTest(const FString& Parameters)
{
    const FLinearColor Black = CubeShot::Image::ApplyAcesFitted(FLinearColor::Black);
    const FLinearColor Middle = CubeShot::Image::ApplyAcesFitted(FLinearColor::White);
    const FLinearColor Highlight = CubeShot::Image::ApplyAcesFitted(FLinearColor(16.0f, 16.0f, 16.0f));
    TestEqual(TEXT("ACES maps black to black"), Black.R, 0.0f);
    TestTrue(TEXT("ACES maps one below display white"), Middle.R > 0.8f && Middle.R < 0.81f);
    TestTrue(TEXT("ACES remains monotonic"), Highlight.R > Middle.R && Highlight.R <= 1.0f);

    FCubeShotRawFrame TransparentFrame;
    TransparentFrame.Width = 1;
    TransparentFrame.Height = 1;
    TransparentFrame.PremultipliedLinearPixels = {FFloat16Color(FLinearColor::Transparent)};

    FCubeShotOptions Options;
    Options.BackgroundMode = ECubeShotBackgroundMode::SolidColor;
    Options.SolidColor = FLinearColor(0.25f, 0.5f, 0.75f, 1.0f);
    FCubeShotImage Output;
    FText Error;
    TestTrue(TEXT("Solid background composites"), CubeShot::Image::Composite(TransparentFrame, Options, 1, Output, Error));
    if (Output.IsValid())
    {
        const FColor Expected = Options.SolidColor.ToFColorSRGB();
        TestEqual(TEXT("Solid background R is linear-composited then encoded"), Output.Pixels[0].R, Expected.R);
        TestEqual(TEXT("Solid output is opaque"), Output.Pixels[0].A, static_cast<uint8>(255));
    }

    UTexture2D* BackgroundTexture = NewObject<UTexture2D>();
    const TArray<FColor> WideSourcePixels =
    {
        FColor::Red, FColor::Green, FColor::Green, FColor::Blue,
        FColor::Red, FColor::Green, FColor::Green, FColor::Blue
    };
    BackgroundTexture->Source.Init(
        4,
        2,
        1,
        1,
        TSF_BGRA8,
        reinterpret_cast<const uint8*>(WideSourcePixels.GetData()));

    FImage PreparedBackground;
    TestTrue(
        TEXT("Texture background source is prepared"),
        CubeShot::Image::PrepareBackgroundImage(BackgroundTexture, 2, PreparedBackground, Error));

    FCubeShotRawFrame TextureFrame;
    TextureFrame.Width = 2;
    TextureFrame.Height = 2;
    TextureFrame.PremultipliedLinearPixels.Init(FFloat16Color(FLinearColor::Transparent), 4);
    Options.BackgroundMode = ECubeShotBackgroundMode::Texture;
    Options.BackgroundTexture = BackgroundTexture;
    TestTrue(
        TEXT("Prepared texture background composites"),
        CubeShot::Image::Composite(TextureFrame, Options, 2, Output, Error, &PreparedBackground));
    if (Output.IsValid())
    {
        TestTrue(
            TEXT("Centered cover crop selects the green middle of a wide texture"),
            Output.Pixels[0].G > Output.Pixels[0].R && Output.Pixels[0].G > Output.Pixels[0].B);
        TestEqual(TEXT("Texture background output is opaque"), Output.Pixels[0].A, static_cast<uint8>(255));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotCoverCropAndDilationTest,
    "CubeShot.Editor.Image.CoverCropAndDilation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotCoverCropAndDilationTest::RunTest(const FString& Parameters)
{
    const FVector2D WideLeft = CubeShot::Image::ComputeCoverUV(
        FVector2D(0.0, 0.5), FIntPoint(400, 200), FIntPoint(100, 100));
    const FVector2D WideRight = CubeShot::Image::ComputeCoverUV(
        FVector2D(1.0, 0.5), FIntPoint(400, 200), FIntPoint(100, 100));
    TestTrue(TEXT("Wide texture crops equally from the left"), FMath::IsNearlyEqual(WideLeft.X, 0.25, 0.0001));
    TestTrue(TEXT("Wide texture crops equally from the right"), FMath::IsNearlyEqual(WideRight.X, 0.75, 0.0001));

    const FVector2D TallTop = CubeShot::Image::ComputeCoverUV(
        FVector2D(0.5, 0.0), FIntPoint(200, 400), FIntPoint(100, 100));
    const FVector2D TallBottom = CubeShot::Image::ComputeCoverUV(
        FVector2D(0.5, 1.0), FIntPoint(200, 400), FIntPoint(100, 100));
    TestTrue(TEXT("Tall texture crops equally from the top"), FMath::IsNearlyEqual(TallTop.Y, 0.25, 0.0001));
    TestTrue(TEXT("Tall texture crops equally from the bottom"), FMath::IsNearlyEqual(TallBottom.Y, 0.75, 0.0001));

    FCubeShotImage Image;
    Image.Width = 3;
    Image.Height = 1;
    Image.bHasTransparency = true;
    Image.Pixels = {FColor(0, 0, 0, 0), FColor(200, 30, 10, 255), FColor(0, 0, 0, 0)};
    CubeShot::Image::DilateTransparentRGB(Image);
    TestEqual(TEXT("Dilation fills left transparent RGB"), Image.Pixels[0].R, static_cast<uint8>(200));
    TestEqual(TEXT("Dilation fills right transparent RGB"), Image.Pixels[2].R, static_cast<uint8>(200));
    TestEqual(TEXT("Dilation does not change alpha"), Image.Pixels[0].A, static_cast<uint8>(0));
    return true;
}
