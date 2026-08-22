#include "CubeShotCaptureService.h"

#include "Engine/StaticMesh.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
    FCubeShotBasicShapeCaptureCommand,
    FAutomationTestBase*, Test);

bool FCubeShotBasicShapeCaptureCommand::Update()
{
    if (!FApp::CanEverRender())
    {
        Test->AddWarning(TEXT("CubeShot RHI capture test skipped because rendering is unavailable."));
        return true;
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Test->TestNotNull(TEXT("Engine cube mesh loads"), Cube))
    {
        return true;
    }

    FCubeShotOptions Options;
    Options.StaticMesh = Cube;
    Options.Resolution = 256;
    Options.BackgroundMode = ECubeShotBackgroundMode::Transparent;

    FCubeShotCaptureService CaptureService;
    FText Error;
    if (!Test->TestTrue(TEXT("Capture service initializes"), CaptureService.Initialize(Options, Error)))
    {
        Test->AddError(Error.ToString());
        return true;
    }

    FCubeShotRawFrame FirstFrame;
    FCubeShotRawFrame SecondFrame;
    if (!Test->TestTrue(TEXT("First cube frame captures"), CaptureService.CaptureRaw(ECubeShotDirection::Front, 64, FirstFrame, Error))
        || !Test->TestTrue(TEXT("Repeated cube frame captures"), CaptureService.CaptureRaw(ECubeShotDirection::Front, 64, SecondFrame, Error)))
    {
        Test->AddError(Error.ToString());
        return true;
    }

    Test->TestEqual(TEXT("64 target uses a 128 capture"), FirstFrame.Width, 128);
    Test->TestEqual(TEXT("Repeated capture width is stable"), SecondFrame.Width, FirstFrame.Width);
    Test->TestEqual(TEXT("Repeated capture height is stable"), SecondFrame.Height, FirstFrame.Height);

    int32 VisiblePixelCount = 0;
    int32 NonBlackPixelCount = 0;
    for (const FFloat16Color& HalfPixel : FirstFrame.PremultipliedLinearPixels)
    {
        const FLinearColor Pixel = HalfPixel.GetFloats();
        VisiblePixelCount += Pixel.A > 0.01f ? 1 : 0;
        NonBlackPixelCount += FMath::Max3(Pixel.R, Pixel.G, Pixel.B) > 0.001f ? 1 : 0;
    }
    Test->TestTrue(TEXT("Captured alpha is not empty"), VisiblePixelCount > 0);
    Test->TestTrue(TEXT("Captured RGB is not empty"), NonBlackPixelCount > 0);

    int32 RepeatedVisiblePixelCount = 0;
    for (const FFloat16Color& HalfPixel : SecondFrame.PremultipliedLinearPixels)
    {
        RepeatedVisiblePixelCount += HalfPixel.GetFloats().A > 0.01f ? 1 : 0;
    }
    Test->TestTrue(
        TEXT("Repeated capture coverage remains stable"),
        FMath::Abs(RepeatedVisiblePixelCount - VisiblePixelCount) <= FMath::Max(4, VisiblePixelCount / 100));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotBasicShapeCaptureTest,
    "CubeShot.Editor.Capture.RHI.BasicShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotBasicShapeCaptureTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FCubeShotBasicShapeCaptureCommand(this));
    return true;
}
