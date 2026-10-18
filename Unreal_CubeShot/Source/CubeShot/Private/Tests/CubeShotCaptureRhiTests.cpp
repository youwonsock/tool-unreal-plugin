#include "CubeShotCaptureService.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Async/Async.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/Paths.h"
#include "UnrealToolsRuntime.h"
#include "UObject/StrongObjectPtr.h"
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

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    struct FOutputSweepImage { FCubeShotImage Image; FString Error; bool bSucceeded=false; };
    class FCubeShotOutputSweepCommand final : public IAutomationLatentCommand
    {
        FAutomationTestBase* Test;
        TStrongObjectPtr<UStaticMesh> Mesh;
        TStrongObjectPtr<UTexture2D> Background;
        TUniquePtr<FCubeShotCaptureService> Capture;
        TFuture<FOutputSweepImage> Pending;
        int32 Index=0;
        uint64 MemoryBefore=0;
        TSharedPtr<FJsonObject> Evidence=UnrealToolsRuntime::Ok();
        TArray<TSharedPtr<FJsonValue>> Cases;
        FString CaseName()const{return Index==0?TEXT("npot257_transparent"):Index==1?TEXT("256_solid"):Index==2?TEXT("256_texture"):TEXT("4096_transparent");}
    public:
        explicit FCubeShotOutputSweepCommand(FAutomationTestBase* InTest):Test(InTest){}
        virtual ~FCubeShotOutputSweepCommand()override{if(Pending.IsValid())Pending.Wait();if(Capture)Capture->Shutdown();}
        virtual bool Update()override
        {
            if(!Capture)
            {
                FText Policy;if(!CubeShot::CanExecute(Policy)){Test->AddWarning(TEXT("RHI output sweep needs rendering Editor: ")+Policy.ToString());return true;}
                Mesh.Reset(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
                if(!Test->TestNotNull(TEXT("Sweep Engine Cube"),Mesh.Get()))return true;
                Background.Reset(NewObject<UTexture2D>());FColor Pixels[4]={FColor::Green,FColor::Green,FColor::Green,FColor::Green};
                Background->Source.Init(2,2,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(Pixels));Background->SRGB=true;
                Capture=MakeUnique<FCubeShotCaptureService>();MemoryBefore=FPlatformMemory::GetStats().UsedPhysical;
            }
            if(!Pending.IsValid())
            {
                FCubeShotOptions Options;Options.StaticMesh=Mesh.Get();Options.Resolution=Index==0?257:Index==3?4096:256;
                Options.BackgroundMode=Index==1?ECubeShotBackgroundMode::SolidColor:Index==2?ECubeShotBackgroundMode::Texture:ECubeShotBackgroundMode::Transparent;
                Options.SolidColor=FLinearColor(.05f,.2f,.4f,1);Options.BackgroundTexture=Background.Get();
                FCubeShotPixelWork Work;FText Error;
                if(!Capture->PrepareCaptureWork(Options,ECubeShotDirection::Front,Options.Resolution,Work,Error)){Test->AddError(CaseName()+TEXT(": ")+Error.ToString());return true;}
                Pending=Async(EAsyncExecution::ThreadPool,[Work=MoveTemp(Work)]()mutable{FOutputSweepImage Output;FText Error;Output.bSucceeded=Work(Output.Image,Error);Output.Error=Error.ToString();return Output;});
                return false;
            }
            if(!Pending.IsReady())return false;
            FOutputSweepImage Output=Pending.Get();Pending=TFuture<FOutputSweepImage>();
            if(!Test->TestTrue(CaseName()+TEXT(" real GPU/worker composite"),Output.bSucceeded)){Test->AddError(Output.Error);return true;}
            const int32 Resolution=Index==0?257:Index==3?4096:256;
            Test->TestEqual(CaseName()+TEXT(" exact width"),Output.Image.Width,Resolution);Test->TestEqual(CaseName()+TEXT(" exact height"),Output.Image.Height,Resolution);
            int64 Visible=0,Transparent=0;for(const FColor& Pixel:Output.Image.Pixels){Visible+=Pixel.A>0;Transparent+=Pixel.A==0;}
            Test->TestTrue(CaseName()+TEXT(" visible pixels"),Visible>0);
            if(Index==0||Index==3)Test->TestTrue(CaseName()+TEXT(" transparent margin"),Transparent>0);else Test->TestEqual(CaseName()+TEXT(" opaque background"),Transparent,static_cast<int64>(0));
            FImage Image;Image.Init(Resolution,Resolution,ERawImageFormat::BGRA8,EGammaSpace::sRGB);FMemory::Memcpy(Image.RawData.GetData(),Output.Image.Pixels.GetData(),Output.Image.Pixels.Num()*sizeof(FColor));Output.Image.Reset();
            const FString Directory=FPaths::ProjectSavedDir()/TEXT("UnrealTools/V12/Images");IFileManager::Get().MakeDirectory(*Directory,true);const FString Filename=Directory/(TEXT("CubeShot_")+CaseName()+TEXT(".png"));
            Test->TestTrue(CaseName()+TEXT(" QA PNG"),FImageUtils::SaveImageByExtension(*Filename,Image,100));
            auto Record=MakeShared<FJsonObject>();Record->SetStringField(TEXT("case"),CaseName());Record->SetNumberField(TEXT("resolution"),Resolution);Record->SetNumberField(TEXT("visible_pixels"),Visible);Record->SetNumberField(TEXT("transparent_pixels"),Transparent);Record->SetStringField(TEXT("png_path"),Filename);Record->SetNumberField(TEXT("process_used_physical_bytes"),static_cast<double>(FPlatformMemory::GetStats().UsedPhysical));Cases.Add(MakeShared<FJsonValueObject>(Record));
            ++Index;if(Index<4)return false;
            Evidence->SetArrayField(TEXT("cases"),Cases);Evidence->SetNumberField(TEXT("process_used_physical_before_bytes"),static_cast<double>(MemoryBefore));Evidence->SetNumberField(TEXT("process_peak_used_physical_bytes"),static_cast<double>(FPlatformMemory::GetStats().PeakUsedPhysical));FString Error;
            Test->TestTrue(TEXT("RHI sweep evidence saved"),UnrealToolsRuntime::AtomicJson(Directory/TEXT("CubeShot_output_sweep.json"),Evidence,Error));if(!Error.IsEmpty())Test->AddError(Error);
            Capture->Shutdown();Capture.Reset();return true;
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotOutputSweepTest,"CubeShot.Editor.Capture.RHI.OutputSweep",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotOutputSweepTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FCubeShotOutputSweepCommand(this));return true;}
#endif
