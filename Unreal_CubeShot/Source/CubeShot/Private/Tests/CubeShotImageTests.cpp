#include "CubeShotImageUtils.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotPaddingRgbContractTest,
    "CubeShot.Editor.Image.TransparentPaddingPreservesRGBA",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotPaddingRgbContractTest::RunTest(const FString&)
{
    FImage Source(10,8,ERawImageFormat::BGRA8,EGammaSpace::sRGB);auto Pixels=Source.AsBGRA8();
    for(int32 Y=0;Y<8;++Y)for(int32 X=0;X<10;++X)Pixels[Y*10+X]=FColor(5+X*10,7+Y*20,137,0);
    Pixels[3*10+4].A=255;Pixels[4*10+5].A=128;
    FImage Output;FText Error;TestTrue(TEXT("Trim padded source"),CubeShot::Image::PrepareIconExport(Source,true,2,0,Output,Error));
    TestEqual(TEXT("Padded width"),Output.SizeX,6);TestEqual(TEXT("Padded height"),Output.SizeY,6);
    for(int32 Y=0;Y<6;++Y)for(int32 X=0;X<6;++X)
        if(Output.AsBGRA8()[Y*6+X]!=Pixels[(Y+1)*10+X+2]){AddError(TEXT("Trim must preserve every source RGBA pixel, including nonzero RGB at alpha zero."));return false;}
    FImage Rotated;TestTrue(TEXT("Rotate padded source"),CubeShot::Image::PrepareIconExport(Source,true,2,1,Rotated,Error));
    for(int32 Y=0;Y<6;++Y)for(int32 X=0;X<6;++X)
        if(Rotated.AsBGRA8()[X*6+5-Y]!=Output.AsBGRA8()[Y*6+X]){AddError(TEXT("Rotation changed a padded RGBA pixel."));return false;}
    Pixels[3*10+4].A=Pixels[4*10+5].A=0;Pixels[0].A=255;
    TestTrue(TEXT("Edge subject pads within original frame"),CubeShot::Image::PrepareIconExport(Source,true,3,0,Output,Error));
    TestEqual(TEXT("Padding clamps at source left edge"),Output.SizeX,4);TestEqual(TEXT("Padding clamps at source top edge"),Output.SizeY,4);
    for(int32 Y=0;Y<4;++Y)for(int32 X=0;X<4;++X)TestEqual(TEXT("Clamped padding preserves source RGBA"),Output.AsBGRA8()[Y*4+X],Pixels[Y*10+X]);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotIconExportTest,
    "CubeShot.Editor.Image.IconExportFraming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotIconExportTest::RunTest(const FString& Parameters)
{
    FImage Source(9, 7, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    for (FColor& Pixel : Source.AsBGRA8()) Pixel = FColor(19, 23, 31, 0);
    // An asymmetric subject includes a faint antialiased edge that must not be discarded.
    const FColor A(80, 20, 30, 255), B(20, 90, 40, 1), C(30, 40, 100, 127);
    Source.AsBGRA8()[2 * 9 + 3] = A;
    Source.AsBGRA8()[2 * 9 + 5] = B;
    Source.AsBGRA8()[3 * 9 + 4] = C;
    FText Error;
    FImage Output;
    TestTrue(TEXT("Default export succeeds"), CubeShot::Image::PrepareIconExport(Source, false, 8, 0, Output, Error));
    TestTrue(TEXT("Default export preserves all bytes"), Output.RawData == Source.RawData);
    TestTrue(TEXT("Trim with padding succeeds"), CubeShot::Image::PrepareIconExport(Source, true, 1, 0, Output, Error));
    TestEqual(TEXT("Trimmed width includes both padded edges"), Output.SizeX, 5);
    TestEqual(TEXT("Trimmed height includes both padded edges"), Output.SizeY, 4);
    TestEqual(TEXT("Faint alpha is preserved"), Output.AsBGRA8()[1 * 5 + 3], B);
    for (int32 Turn = 1; Turn <= 3; ++Turn)
    {
        FImage Rotated;
        TestTrue(TEXT("Quarter turn succeeds"), CubeShot::Image::PrepareIconExport(Output, false, 0, Turn, Rotated, Error));
        FImage Restored;
        TestTrue(TEXT("Inverse quarter turn succeeds"), CubeShot::Image::PrepareIconExport(Rotated, false, 0, 4 - Turn, Restored, Error));
        TestTrue(TEXT("Round trip preserves every color and alpha byte"), Restored.RawData == Output.RawData);
    }
    FImage Clockwise;
    CubeShot::Image::PrepareIconExport(Output, false, 0, 1, Clockwise, Error);
    TestEqual(TEXT("Clockwise export swaps width"), Clockwise.SizeX, 4);
    TestEqual(TEXT("Clockwise export swaps height"), Clockwise.SizeY, 5);
    TestEqual(TEXT("Clockwise mapping retains the subject orientation"), Clockwise.AsBGRA8()[1 * 4 + 2], A);
    TestFalse(TEXT("Fractional rotations are not part of this API"), CubeShot::Image::PrepareIconExport(Source, true, 1, 4, Output, Error));
    TestFalse(TEXT("Negative padding is rejected"), CubeShot::Image::PrepareIconExport(Source, true, -1, 0, Output, Error));
    for (FColor& Pixel : Source.AsBGRA8()) Pixel.A = 0;
    TestFalse(TEXT("An empty capture cannot be framed as a valid icon"), CubeShot::Image::PrepareIconExport(Source, true, 8, 0, Output, Error));
    return true;
}

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
