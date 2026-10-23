#include "CubeShotAssetWriter.h"
#include "CubeShotImageUtils.h"
#include "CubeShotJobService.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "UObject/UnrealType.h"
#include "StaticMeshCompiler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

// ObjectTools can introduce the Win32 LoadImage alias after ImageUtils was
// parsed. Keep the engine method name intact without leaking macro changes to
// other source files in a unity build.
#if PLATFORM_WINDOWS && defined(LoadImage)
#pragma push_macro("LoadImage")
#undef LoadImage
#define CUBESHOT_RESTORE_WIN32_LOADIMAGE
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotStagedWriterTest,"CubeShot.Editor.Writer.StagedCancellationAndCleanup",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotStagedWriterTest::RunTest(const FString&)
{
    const FString Id=FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FCubeShotAssetWriterConfig Config;Config.OutputRoot=TEXT("/Game/CubeShotTests/Staged_")+Id;Config.StagingRoot=FPaths::ProjectSavedDir()/TEXT("CubeShot/Automation/Staged_")+Id;Config.bPromptForCheckout=false;
    Config.ShouldFailOperation=[](const FString& Operation,const FString&){return Operation==TEXT("stage_cleanup");};
    FCubeShotAssetWriter Writer(Config);FCubeShotOptions Options;Options.StaticMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));Options.Resolution=256;
    FCubeShotPreflightResult Plan;if(!TestTrue(TEXT("Preflight"),Writer.BuildPreflight(Options,Plan)))return false;
    int32 Captures=0;bool Cancel=false;
    auto Execution=Writer.BeginExecution(Options,[&](const FCubeShotOptions&,ECubeShotDirection,int32 Resolution,FCubeShotImage& Image,FText&){++Captures;Image.Width=Image.Height=Resolution;Image.Pixels.Init(FColor(20,40,60,255),Resolution*Resolution);return true;},Plan,false,{},[&](){return Cancel;});
    TestFalse(TEXT("Validation is a separate step"),Execution->Step());TestEqual(TEXT("Validation captures no face"),Captures,0);
    TestFalse(TEXT("First capture yields"),Execution->Step());TestEqual(TEXT("One step captures one face"),Captures,1);
    Cancel=true;TestTrue(TEXT("Cancellation terminates at safe boundary"),Execution->Step());const auto& Result=Execution->GetResult();
    TestTrue(TEXT("Cancelled result"),Result.bCancelled);TestTrue(TEXT("No packages changed"),Result.SavedObjectPaths.IsEmpty()&&Result.DirtyPackages.IsEmpty());
    TestFalse(TEXT("Cleanup failure keeps recovery path"),Result.StagingDirectory.IsEmpty());TestTrue(TEXT("Recovery staging still exists"),IFileManager::Get().DirectoryExists(*Result.StagingDirectory));TestFalse(TEXT("Cleanup warning recorded"),Result.Warnings.IsEmpty());
    IFileManager::Get().DeleteDirectory(*Config.StagingRoot,false,true);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotPendingWorkerCancelTest,"CubeShot.Editor.Writer.CancelPendingPixelWorker",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotPendingWorkerCancelTest::RunTest(const FString&)
{
    const FString Id=FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FCubeShotAssetWriterConfig Config;Config.OutputRoot=TEXT("/Game/CubeShotTests/Pending_")+Id;Config.StagingRoot=FPaths::ProjectSavedDir()/TEXT("CubeShot/Automation/Pending_")+Id;Config.bPromptForCheckout=false;
    FCubeShotAssetWriter Writer(Config);FCubeShotOptions Options;Options.StaticMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));Options.Resolution=256;
    FCubeShotPreflightResult Plan;if(!TestTrue(TEXT("Preflight"),Writer.BuildPreflight(Options,Plan)))return false;
    FEvent* Gate=FPlatformProcess::GetSynchEventFromPool(false);auto RanOnWorker=MakeShared<TAtomic<bool>>(false);bool Cancel=false;
    auto Execution=Writer.BeginPreparedExecution(Options,[Gate,RanOnWorker](const FCubeShotOptions&,ECubeShotDirection,int32 Resolution,FCubeShotPixelWork& Work,FText&)
    {
        Work=[Gate,RanOnWorker,Resolution](FCubeShotImage& Image,FText&)
        {RanOnWorker->Store(!IsInGameThread());Gate->Wait(5000);Image.Width=Image.Height=Resolution;Image.Pixels.Init(FColor(30,40,50,255),Resolution*Resolution);return true;};return true;
    },Plan,false,{},[&](){return Cancel;});
    TestFalse(TEXT("Validation yields"),Execution->Step(false));TestFalse(TEXT("Capture launches independent worker"),Execution->Step(false));
    Cancel=true;TestFalse(TEXT("Pending worker is not abandoned or deleted underneath"),Execution->Step(false));TestTrue(TEXT("Staging exists while worker owns it"),IFileManager::Get().DirectoryExists(*Execution->GetResult().StagingDirectory));
    Gate->Trigger();const double Deadline=FPlatformTime::Seconds()+5;bool Finished=false;while(!Finished&&FPlatformTime::Seconds()<Deadline){Finished=Execution->Step(false);if(!Finished)FPlatformProcess::Sleep(.001f);}
    TestTrue(TEXT("Worker requires no game-thread callback"),Finished);TestTrue(TEXT("CPU pixel work runs off game thread"),RanOnWorker->Load());TestTrue(TEXT("Cancellation applied after worker completes"),Execution->GetResult().bCancelled);TestTrue(TEXT("No output package changed"),Execution->GetResult().SavedObjectPaths.IsEmpty());TestTrue(TEXT("Completed worker staging cleaned"),Execution->GetResult().StagingDirectory.IsEmpty());
    Execution.Reset();FPlatformProcess::ReturnSynchEventToPool(Gate);IFileManager::Get().DeleteDirectory(*Config.StagingRoot,false,true);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotPartialSaveTest,"CubeShot.Editor.Writer.PartialSaveRecovery",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotPartialSaveTest::RunTest(const FString&)
{
    const FString Id=FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FCubeShotAssetWriterConfig Config;Config.OutputRoot=TEXT("/Game/CubeShotTests/Partial_")+Id;Config.StagingRoot=FPaths::ProjectSavedDir()/TEXT("CubeShot/Automation/Partial_")+Id;Config.bPromptForCheckout=false;
    Config.ShouldFailOperation=[](const FString& Operation,const FString& Path){return Operation==TEXT("package_save")&&Path.Contains(TEXT("_Front.uasset"));};
    FCubeShotAssetWriter Writer(Config);FCubeShotOptions Options;Options.StaticMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));Options.Resolution=256;
    FCubeShotPreflightResult Plan;if(!TestTrue(TEXT("Preflight"),Writer.BuildPreflight(Options,Plan)))return false;
    FCubeShotGenerationResult Result;Writer.ExecuteWithCaptureCallback(Options,[](const FCubeShotOptions&,ECubeShotDirection,int32 Resolution,FCubeShotImage& Image,FText&){Image.Width=Image.Height=Resolution;Image.Pixels.Init(FColor(30,40,50,255),Resolution*Resolution);return true;},Plan,false,{},{},Result);
    TestFalse(TEXT("Partial save never succeeds"),Result.bSucceeded);TestEqual(TEXT("Five saved"),Result.SavedObjectPaths.Num(),5);TestEqual(TEXT("One failed"),Result.FailedObjectPaths.Num(),1);TestEqual(TEXT("One dirty package"),Result.DirtyPackages.Num(),1);TestEqual(TEXT("Structured error"),Result.ErrorCode,FString(TEXT("partial_save")));TestFalse(TEXT("Failure detail kept"),Result.Errors.IsEmpty());TestTrue(TEXT("Staging retained"),IFileManager::Get().DirectoryExists(*Result.StagingDirectory));
    TArray<UObject*> Assets;for(const auto& Output:Plan.Outputs)if(UObject* Asset=FindObject<UObject>(nullptr,*Output.GetObjectPath()))Assets.Add(Asset);ObjectTools::DeleteObjectsUnchecked(Assets);
    FString OutputPath;FPackageName::TryConvertLongPackageNameToFilename(Config.OutputRoot,OutputPath);IFileManager::Get().DeleteDirectory(*OutputPath,false,true);IFileManager::Get().DeleteDirectory(*Config.StagingRoot,false,true);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotPngAtomicTest,"CubeShot.Editor.Image.AtomicPngPreservesExisting",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotPngAtomicTest::RunTest(const FString&)
{
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("CubeShot/Automation/Atomic_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);const FString Filename=Directory/TEXT("sample.png");
    FImage Image;Image.Init(2,2,ERawImageFormat::BGRA8,EGammaSpace::sRGB);FColor* Pixels=reinterpret_cast<FColor*>(Image.RawData.GetData());for(int32 Index=0;Index<4;++Index)Pixels[Index]=FColor(12,34,56,78);
    FText Error;TestTrue(TEXT("Initial PNG saved"),CubeShot::Image::SavePngAtomic(Filename,Image,false,Error));TArray<uint8> Before;FFileHelper::LoadFileToArray(Before,*Filename);
    Pixels[0]=FColor::Red;TestFalse(TEXT("No overwrite approval rejects replacement"),CubeShot::Image::SavePngAtomic(Filename,Image,false,Error));TArray<uint8> After;FFileHelper::LoadFileToArray(After,*Filename);TestTrue(TEXT("Existing PNG byte-for-byte preserved"),Before==After);
    TestTrue(TEXT("Explicitly approved replacement works"),CubeShot::Image::SavePngAtomic(Filename,Image,true,Error));FImage Loaded;TestTrue(TEXT("Replacement decodes"),FImageUtils::LoadImage(*Filename,Loaded));FImage Converted;Loaded.CopyTo(Converted,ERawImageFormat::BGRA8,EGammaSpace::sRGB);TestEqual(TEXT("Pixels preserved"),reinterpret_cast<const FColor*>(Converted.RawData.GetData())[0],FColor::Red);
    IFileManager::Get().DeleteDirectory(*Directory,false,true);return true;
}

namespace
{
    class FCubeShotSourceChangeCommand final : public IAutomationLatentCommand
    {
        FAutomationTestBase* Test;
        UPackage* Package=nullptr;
        UStaticMesh* Mesh=nullptr;
        UMaterialInterface* Material=nullptr;
        UMaterialFunction* Function=nullptr;
        UPackage* FunctionPackage=nullptr;
        bool ExternalFunction=false;
        TUniquePtr<UnrealToolsRuntime::IItemRunner> Runner;
        bool Modified=false;
        int32 Steps=0;
    public:
        explicit FCubeShotSourceChangeCommand(FAutomationTestBase* InTest,bool bExternal=false):Test(InTest),ExternalFunction(bExternal){}
        virtual ~FCubeShotSourceChangeCommand()override
        {
            if(Runner){Runner->Shutdown();Runner.Reset();}
            if(Package){Package->SetDirtyFlag(false);if(FunctionPackage)FunctionPackage->SetDirtyFlag(false);TArray<UObject*> Assets={Mesh,Material};if(Function)Assets.Add(Function);ObjectTools::DeleteObjectsUnchecked(Assets);}
        }
        virtual bool Update()override
        {
            if(!Package)
            {
                FText Policy;if(!CubeShot::CanExecute(Policy)){Test->AddWarning(TEXT("Source-change runner requires rendering Editor: ")+Policy.ToString());return true;}
                UStaticMesh* EngineCube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));if(!Test->TestNotNull(TEXT("Fixture Engine Cube"),EngineCube))return true;
                const FString PackageName=TEXT("/Game/UnrealToolsQA/SourceChange_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
                Package=CreatePackage(*PackageName);Mesh=DuplicateObject<UStaticMesh>(EngineCube,Package,TEXT("SM_SourceChange"));
                if(!ExternalFunction)
                {
                    auto Instance=NewObject<UMaterialInstanceConstant>(Package,TEXT("MI_SourceChange"),RF_Public|RF_Standalone);
                    Instance->SetParentEditorOnly(EngineCube->GetMaterial(0));Material=Instance;
                }
                else
                {
                    FunctionPackage=CreatePackage(*(PackageName+TEXT("_External")));Function=NewObject<UMaterialFunction>(FunctionPackage,TEXT("MF_External"),RF_Public|RF_Standalone);
                    auto Color=NewObject<UMaterialExpressionConstant3Vector>(Function);Color->Function=Function;Color->Constant=FLinearColor(.2f,.5f,.7f);
                    auto Out=NewObject<UMaterialExpressionFunctionOutput>(Function);Out->Function=Function;Out->OutputName=TEXT("Color");Out->A.Connect(0,Color);
                    Function->GetExpressionCollection().AddExpression(Color);Function->GetExpressionCollection().AddExpression(Out);Function->PostEditChange();
                    auto Base=NewObject<UMaterial>(Package,TEXT("M_FunctionSource"),RF_Public|RF_Standalone);
                    auto Call=NewObject<UMaterialExpressionMaterialFunctionCall>(Base);Call->Material=Base;Base->GetExpressionCollection().AddExpression(Call);Call->SetMaterialFunction(Function);Base->GetEditorOnlyData()->BaseColor.Connect(0,Call);Base->PostEditChange();Material=Base;
                    TArray<UMaterialFunctionInterface*> Dependencies;Material->GetDependentFunctions(Dependencies);Test->TestTrue(TEXT("Fixture uses an external-package function"),Dependencies.Contains(Function)&&FunctionPackage!=Package);FAssetRegistryModule::AssetCreated(Function);
                }
                Mesh->SetMaterial(0,Material);FAssetRegistryModule::AssetCreated(Mesh);FAssetRegistryModule::AssetCreated(Material);
                FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
                UnrealToolsRuntime::FItemSpec Spec;Spec.MeshPath=Mesh->GetPathName();Spec.Options=CubeShot::Service::DefaultOptions();Spec.Options->SetNumberField(TEXT("resolution"),256);
                Runner=CubeShot::Service::CreateRunnerForAutomation(Spec);
                // The automation scheduler advances one domain step per Editor frame; no nested Editor ticker.
                return false;
            }
            const auto State=Runner->Step(false,true);++Steps;
            if(!Modified&&Runner->GetStage()==TEXT("capture_Back"))
            {
                if(ExternalFunction){Function->Description=TEXT("Changed during capture");FPropertyChangedEvent Event(FindFProperty<FProperty>(UMaterialFunction::StaticClass(),TEXT("Description")));FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Function,Event);}
                else Material->Modify();Modified=true;
            }
            if(State==UnrealToolsRuntime::EStepResult::Continue && Steps<24)return false;
            Test->TestTrue(TEXT("Tracked fixture modified between capture stages"),Modified);Test->TestTrue(TEXT("Source change fails before commit"),State==UnrealToolsRuntime::EStepResult::Failed);
            const auto Result=Runner->GetResult();Test->TestEqual(TEXT("Structured source-change error"),UnrealToolsRuntime::GetString(Result,TEXT("error_code")),FString(TEXT("source_changed")));
            Test->TestTrue(TEXT("No output package saved"),UnrealToolsRuntime::GetArray(Result,TEXT("saved")).IsEmpty());Test->TestTrue(TEXT("No output left dirty"),UnrealToolsRuntime::GetArray(Result,TEXT("dirty_packages")).IsEmpty());
            return true;
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotSourceChangeTest,"CubeShot.Editor.MCP.SourceChangeBeforeCommit",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotSourceChangeTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FCubeShotSourceChangeCommand(this));return true;}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotFunctionSourceChangeTest,"CubeShot.Editor.MCP.ExternalFunctionPropertyChangeBeforeCommit",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotFunctionSourceChangeTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FCubeShotSourceChangeCommand(this,true));return true;}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotExecutionPolicyTest,"CubeShot.Editor.MCP.ExecutionPolicy",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotExecutionPolicyTest::RunTest(const FString&)
{
    FText Error;TestTrue(TEXT("Rendering Editor allowed"),CubeShot::EvaluateExecutionPolicy(true,false,false,true,false,true,Error));
    TestFalse(TEXT("PIE blocked without changing Editor"),CubeShot::EvaluateExecutionPolicy(true,true,false,true,false,true,Error));
    TestFalse(TEXT("SIE blocked without changing Editor"),CubeShot::EvaluateExecutionPolicy(true,false,true,true,false,true,Error));
    TestFalse(TEXT("NullRHI blocked even when RHI pointer exists"),CubeShot::EvaluateExecutionPolicy(true,false,false,true,true,true,Error));
    TestFalse(TEXT("Nonrendering process blocked"),CubeShot::EvaluateExecutionPolicy(true,false,false,false,false,true,Error));
    TestFalse(TEXT("Missing RHI blocked"),CubeShot::EvaluateExecutionPolicy(true,false,false,true,false,false,Error));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotOptionsParityTest,"CubeShot.Editor.MCP.OptionsParity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotOptionsParityTest::RunTest(const FString&)
{
    auto Options=CubeShot::Service::DefaultOptions();FString Error;
    TestTrue(TEXT("API defaults valid"),CubeShot::Service::Normalize(Options,Error));TestEqual(TEXT("API default resolution"),Options->GetIntegerField(TEXT("resolution")),512);TestEqual(TEXT("API default projection"),Options->GetStringField(TEXT("projection")),FString(TEXT("orthographic")));
    Options->SetNumberField(TEXT("resolution"),257);Options->SetStringField(TEXT("background"),TEXT("solid"));TestTrue(TEXT("NPOT and solid accepted"),CubeShot::Service::Normalize(Options,Error));
    Options->SetNumberField(TEXT("field_of_view"),171);TestFalse(TEXT("Invalid FOV rejected"),CubeShot::Service::Normalize(Options,Error));Options->SetNumberField(TEXT("field_of_view"),45);Options->SetBoolField(TEXT("overwrite"),true);TestFalse(TEXT("Overwrite excluded from stored options"),CubeShot::Service::Normalize(Options,Error));return true;
}
#endif

#if defined(CUBESHOT_RESTORE_WIN32_LOADIMAGE)
#pragma pop_macro("LoadImage")
#undef CUBESHOT_RESTORE_WIN32_LOADIMAGE
#endif
