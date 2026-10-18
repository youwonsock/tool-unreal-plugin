#include "CubeShotAssetWriter.h"
#include "CubeShotCaptureService.h"
#include "CubeShotImageUtils.h"
#include "CubeShotJobService.h"
#include "CubeShotViewModel.h"
#include "SCubeShotPanel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "ModelContextProtocolSession.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SavePackage.h"

#if PLATFORM_WINDOWS && defined(LoadImage)
#pragma push_macro("LoadImage")
#undef LoadImage
#define CUBESHOT_APPLICATION_RESTORE_LOADIMAGE
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    bool OwnedProject(FAutomationTestBase& Test)
    {
        FString Path=FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());Path.ReplaceInline(TEXT("\\"),TEXT("/"));
        if(Path.Contains(TEXT("/Saved/UnrealTools/V12/Projects/")))return true;
        Test.AddWarning(TEXT("CubeShot application mutation fixtures require an owned V12 QA project."));return false;
    }
    struct FOwnedFixture
    {
        UStaticMesh* Mesh=nullptr;UPackage* Package=nullptr;TArray<UObject*> ExtraAssets;
        FCubeShotOptions Options;TArray<FCubeShotOutputDescriptor> Outputs;
        bool Initialize()
        {
            auto Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));if(!Cube)return false;
            const FString Name=TEXT("/Game/UnrealToolsQA/V12Cube_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
            Package=CreatePackage(*Name);Mesh=DuplicateObject<UStaticMesh>(Cube,Package,TEXT("SM_Fixture"));
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});FAssetRegistryModule::AssetCreated(Mesh);
            Options.StaticMesh=Mesh;Options.Resolution=256;FText Error;
            return FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(Name,Mesh->GetName(),TEXT("/Game/CubeShot"),Outputs,Error);
        }
        bool WriteFlat(FCubeShotGenerationResult& Result,bool Retain=false)
        {
            FCubeShotAssetWriterConfig Config;Config.bPromptForCheckout=false;Config.bRetainSuccessfulStaging=Retain;
            FCubeShotAssetWriter Writer(Config);FCubeShotPreflightResult Plan;if(!Writer.BuildPreflight(Options,Plan))return false;
            Writer.ExecuteWithCaptureCallback(Options,[](const FCubeShotOptions&,ECubeShotDirection Direction,int32 Size,FCubeShotImage& Image,FText&)
            {Image.Width=Image.Height=Size;Image.Pixels.Init(FColor(17+static_cast<uint8>(Direction),93,151,255),Size*Size);return true;},Plan,false,{},{},Result);
            return Result.bSucceeded;
        }
        UnrealToolsRuntime::FObject Request()const
        {
            auto Input=MakeShared<FJsonObject>();Input->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
            Input->SetArrayField(TEXT("mesh_paths"),{MakeShared<FJsonValueString>(Mesh->GetPathName())});Input->SetObjectField(TEXT("options"),CubeShot::Service::DefaultOptions());return Input;
        }
        ~FOwnedFixture()
        {
            TArray<UObject*> Assets=ExtraAssets;if(Mesh)Assets.Add(Mesh);
            for(const auto& Output:Outputs)
            {
                const FString File=FPackageName::LongPackageNameToFilename(Output.LongPackageName,FPackageName::GetAssetPackageExtension());
                FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,false);
                if(auto Asset=FindObject<UObject>(nullptr,*Output.GetObjectPath())){Asset->GetOutermost()->SetDirtyFlag(false);Assets.Add(Asset);}
            }
            if(Package)Package->SetDirtyFlag(false);if(!Assets.IsEmpty())ObjectTools::DeleteObjectsUnchecked(Assets);
            if(!Outputs.IsEmpty())
            {
                FString Directory;FPackageName::TryConvertLongPackageNameToFilename(FPackageName::GetLongPackagePath(Outputs[0].LongPackageName),Directory);
                Directory=FPaths::ConvertRelativePathToFull(Directory);
                const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()/TEXT("CubeShot/UnrealToolsQA"));
                if(FPaths::IsUnderDirectory(Directory,Root))IFileManager::Get().DeleteDirectory(*Directory,false,true);
                const FString Export=FPaths::GetPath(CubeShot::Image::GetPngFilename(Outputs[0].GetObjectPath(),false,0,0));
                const FString ExportRoot=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("CubeShot/MCPExports/UnrealToolsQA"));
                if(FPaths::IsUnderDirectory(Export,ExportRoot))IFileManager::Get().DeleteDirectory(*Export,false,true);
            }
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotReadOnlyInspectionTest,"CubeShot.Editor.Application.ReadOnlyInspectionAndExecutionProbe",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotReadOnlyInspectionTest::RunTest(const FString&)
{
    FCubeShotAssetWriterConfig Config;Config.OutputRoot=TEXT("/Game/CubeShotTests/ReadOnly_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);Config.bPromptForCheckout=false;
    int32 Probes=0;Config.ShouldFailOperation=[&](const FString& Operation,const FString&){if(Operation==TEXT("write_probe")){++Probes;return true;}return false;};
    FCubeShotAssetWriter Writer(Config);FCubeShotOptions Options;Options.StaticMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));Options.Resolution=256;
    FCubeShotPreflightResult Plan;TestTrue(TEXT("Path-only preflight succeeds without permission mutation"),Writer.BuildPreflight(Options,Plan));TestEqual(TEXT("Inspection invokes no write probe"),Probes,0);
    int32 Captures=0;auto Execution=Writer.BeginExecution(Options,[&](const FCubeShotOptions&,ECubeShotDirection,int32,FCubeShotImage&,FText&){++Captures;return false;},Plan,false);
    TestTrue(TEXT("Execution rejects permission failure before capture/staging"),Execution->Step());TestEqual(TEXT("Execution performs exactly one permission probe"),Probes,1);
    TestEqual(TEXT("Permission failure has a structured code"),Execution->GetResult().ErrorCode,FString(TEXT("output_not_writable")));
    TestEqual(TEXT("No capture occurred"),Captures,0);TestTrue(TEXT("No staging directory created"),Execution->GetResult().StagingDirectory.IsEmpty());return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotBatchApplicationParityTest,"CubeShot.Editor.Application.BatchInspectionParity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotBatchApplicationParityTest::RunTest(const FString&)
{
    FText Policy;if(!CubeShot::CanExecute(Policy)){AddWarning(Policy.ToString());return true;}
    const FString Bad=TEXT("/Game/Bad..Bad"),Cube=TEXT("/Engine/BasicShapes/Cube.Cube"),Sphere=TEXT("/Engine/BasicShapes/Sphere.Sphere");
    auto Input=MakeShared<FJsonObject>();Input->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    const TArray<TSharedPtr<FJsonValue>> Paths={MakeShared<FJsonValueString>(Bad),MakeShared<FJsonValueString>(Cube),MakeShared<FJsonValueString>(Sphere)};
    Input->SetArrayField(TEXT("mesh_paths"),Paths);auto Options=MakeShared<FJsonObject>();Options->SetNumberField(TEXT("resolution"),257);Input->SetObjectField(TEXT("options"),Options);
    const auto Original=UnrealToolsRuntime::JsonText(Input);const auto Plan=CubeShot::Service::InspectBatch(Input);
    const auto Items=UnrealToolsRuntime::GetArray(Plan,TEXT("items"));TestEqual(TEXT("Full inspection retains every original row"),Items.Num(),3);
    if(Items.Num()==3)TestEqual(TEXT("A valid later target still receives six output paths"),UnrealToolsRuntime::GetArray(Items[2]->AsObject(),TEXT("outputs")).Num(),6);
    TestEqual(TEXT("Inspection preserves original request JSON"),UnrealToolsRuntime::JsonText(Input),Original);
    auto Mcp=MakeShared<FJsonObject>();Mcp->SetArrayField(TEXT("mesh_paths"),Paths);Mcp->SetNumberField(TEXT("resolution"),257);
    const auto Reply=IModelContextProtocolModule::GetChecked().FindTool(TEXT("cubeshot_preflight"))->Run(Mcp);
    const auto McpPlan=UnrealToolsRuntime::GetObject(Reply.JsonObject,TEXT("structuredContent"));
    TestEqual(TEXT("GUI service and MCP adapter share ordered item plans"),UnrealToolsRuntime::GetArray(McpPlan,TEXT("items")).Num(),Items.Num());
    if(Items.Num()==3&&UnrealToolsRuntime::GetArray(McpPlan,TEXT("items")).Num()==3)
        TestEqual(TEXT("Explicit options yield identical valid target plans"),UnrealToolsRuntime::JsonText(Items[2]->AsObject()),UnrealToolsRuntime::JsonText(UnrealToolsRuntime::GetArray(McpPlan,TEXT("items"))[2]->AsObject()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotInteractiveCheckoutTest,"CubeShot.Editor.Application.CheckoutCancelAndIncompleteRefuseAdmission",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotInteractiveCheckoutTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;FText Policy;if(!CubeShot::CanExecute(Policy)){AddWarning(Policy.ToString());return true;}
    FOwnedFixture Fixture;if(!TestTrue(TEXT("Owned source fixture"),Fixture.Initialize()))return false;FCubeShotGenerationResult Written;
    if(!TestTrue(TEXT("Existing output fixture"),Fixture.WriteFlat(Written)))return false;
    for(const auto& Output:Fixture.Outputs){const FString File=FPackageName::LongPackageNameToFilename(Output.LongPackageName,FPackageName::GetAssetPackageExtension());TestTrue(TEXT("Fixture can become read-only"),FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,true));}
    auto Request=Fixture.Request();const auto Interactive=CubeShot::Service::InspectBatch(Request,true),Strict=CubeShot::Service::InspectBatch(Request);
    TestTrue(TEXT("Interactive inspection reports required checkout"),UnrealToolsRuntime::GetBool(Interactive,TEXT("requires_checkout")));
    TestFalse(TEXT("Unattended inspection does not permit read-only output"),UnrealToolsRuntime::GetBool(Strict,TEXT("can_proceed")));
    auto Panel=SNew(SCubeShotPanel);Panel->ClearTargetsForAutomation();Panel->AddTargetForAutomation(Fixture.Mesh->GetPathName());
    const int32 Before=CubeShot::Service::Jobs().GetJobs().Num();int32 Dialogs=0;
    Panel->SetSubmissionDecisionsForAutomation([](){return true;},[&](const TArray<UPackage*>&,FText&){++Dialogs;return false;});Panel->StartBatchForAutomation();
    TestEqual(TEXT("Cancelled checkout refuses job admission"),CubeShot::Service::Jobs().GetJobs().Num(),Before);TestEqual(TEXT("Checkout happens before admission"),Dialogs,1);
    Panel->SetSubmissionDecisionsForAutomation([](){return true;},[&](const TArray<UPackage*>& Packages,FText&)
    {++Dialogs;if(!Packages.IsEmpty()){const FString File=FPackageName::LongPackageNameToFilename(Packages[0]->GetName(),FPackageName::GetAssetPackageExtension());FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File,false);}return true;});
    Panel->StartBatchForAutomation();TestEqual(TEXT("Incomplete checkout still refuses all admission"),CubeShot::Service::Jobs().GetJobs().Num(),Before);
    Panel->ClearTargetsForAutomation();Panel->Shutdown();return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotWarningsObserverTest,"CubeShot.Editor.Application.WarningTextAndHistoryCache",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotWarningsObserverTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;FOwnedFixture Fixture;if(!TestTrue(TEXT("Owned source fixture"),Fixture.Initialize()))return false;
    Fixture.Mesh->GetNaniteSettings().bEnabled=true;auto Material=NewObject<UMaterial>(Fixture.Package,TEXT("M_Warning"),RF_Public|RF_Standalone);Material->BlendMode=BLEND_Translucent;Fixture.ExtraAssets.Add(Material);Fixture.Mesh->SetMaterial(0,Material);
    auto Panel=SNew(SCubeShotPanel);Panel->ClearTargetsForAutomation();Panel->AddTargetForAutomation(Fixture.Mesh->GetPathName());Panel->SetResolutionForAutomation(257);
    const FString Status=Panel->StatusForAutomation().ToString();TestTrue(TEXT("Options area displays Nanite limitation"),Status.Contains(TEXT("Nanite")));
    TestTrue(TEXT("Options area displays NPOT warning"),Status.Contains(TEXT("Non-power-of-two")));TestTrue(TEXT("Options area displays material warning"),Status.Contains(TEXT("Translucent")));
    Panel->RefreshHistoryForAutomation();const uint64 Before=Panel->HistoryReadsForAutomation();for(int32 Index=0;Index<100;++Index)Panel->ResultsForAutomation();
    TestEqual(TEXT("Repeated Slate text reads do not fetch or clone history"),Panel->HistoryReadsForAutomation(),Before);
    Panel->ClearTargetsForAutomation();Panel->Shutdown();return true;
}

namespace
{
    class FFrozenQueuePngCommand final : public IAutomationLatentCommand
    {
        FAutomationTestBase* Test;FOwnedFixture Fixture;TUniquePtr<UnrealToolsRuntime::IItemRunner> Runner;FImage Expected;bool Edited=false;double Deadline=0;
    public:
        explicit FFrozenQueuePngCommand(FAutomationTestBase* InTest):Test(InTest){}
        virtual ~FFrozenQueuePngCommand()override{if(Runner){Runner->Shutdown();Runner.Reset();}}
        virtual bool Update()override
        {
            if(!Runner)
            {
                if(!OwnedProject(*Test))return true;FText Policy;if(!CubeShot::CanExecute(Policy)){Test->AddWarning(Policy.ToString());return true;}
                if(!Test->TestTrue(TEXT("Frozen PNG owned source fixture"),Fixture.Initialize()))return true;
                UnrealToolsRuntime::FItemSpec Spec;Spec.MeshPath=Fixture.Mesh->GetPathName();Spec.Options=CubeShot::Service::DefaultOptions();Spec.Options->SetNumberField(TEXT("resolution"),256);
                Spec.Options->SetBoolField(TEXT("export_png"),true);Spec.Options->SetBoolField(TEXT("trim_transparent"),false);
                Runner=CubeShot::Service::CreateRunnerForAutomation(Spec);Deadline=FPlatformTime::Seconds()+60;return false;
            }
            if(!Edited&&Runner->GetStage()==TEXT("png_publish"))
            {
                auto Texture=LoadObject<UTexture2D>(nullptr,*Fixture.Outputs[0].GetObjectPath());FImage Source;
                if(!Test->TestTrue(TEXT("Saved texture source available before publisher"),Texture&&Texture->Source.GetMipImage(Source,0)))return true;
                Source.CopyTo(Expected,ERawImageFormat::BGRA8,EGammaSpace::sRGB);FImage Red(256,256,ERawImageFormat::BGRA8,EGammaSpace::sRGB);for(auto& Pixel:Red.AsBGRA8())Pixel=FColor::Red;
                Texture->Source.Init(Red);Texture->MarkPackageDirty();Edited=true;
            }
            const auto State=Runner->Step(false,false);
            if(State==UnrealToolsRuntime::EStepResult::Continue&&FPlatformTime::Seconds()<Deadline)return false;
            Test->TestTrue(TEXT("Output texture changed between package commit and PNG publication"),Edited);
            Test->TestTrue(TEXT("Queue succeeds despite unrelated output edit"),State==UnrealToolsRuntime::EStepResult::Succeeded);
            FImage Png,Converted;const FString Filename=CubeShot::Image::GetPngFilename(Fixture.Outputs[0].GetObjectPath(),false,8,0);
            if(Test->TestTrue(TEXT("Queue PNG exists"),FImageUtils::LoadImage(*Filename,Png)))
            {Png.CopyTo(Converted,ERawImageFormat::BGRA8,EGammaSpace::sRGB);Test->TestTrue(TEXT("Queue PNG preserves immutable generated pixels, not later Texture.Source edits"),Expected.RawData==Converted.RawData);}
            Test->TestTrue(TEXT("Successful publisher cleans staging after worker completion"),UnrealToolsRuntime::GetString(Runner->GetResult(),TEXT("recovery_staging")).IsEmpty());return true;
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotFrozenQueuePngTest,"CubeShot.Editor.Application.FrozenQueuePngSource",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotFrozenQueuePngTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FFrozenQueuePngCommand(this));return true;}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotLegacyPngPolicyTest,"CubeShot.Editor.Application.LegacyPngOverwritePolicy",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotLegacyPngPolicyTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;FOwnedFixture Fixture;if(!TestTrue(TEXT("Owned PNG source fixture"),Fixture.Initialize()))return false;FCubeShotGenerationResult Written;
    if(!TestTrue(TEXT("PNG output fixture"),Fixture.WriteFlat(Written)))return false;
    auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("texture_path"),Fixture.Outputs[0].GetObjectPath());auto First=CubeShot::Service::ExportPng(Args);
    if(!TestTrue(TEXT("Legacy PNG call succeeds"),UnrealToolsRuntime::GetBool(First,TEXT("success"))))return false;
    const FString Filename=UnrealToolsRuntime::GetString(First,TEXT("png_path"));TArray<uint8> Before,After;FFileHelper::LoadFileToArray(Before,*Filename);
    TestTrue(TEXT("Omitted overwrite preserves legacy allowed policy"),UnrealToolsRuntime::GetBool(First,TEXT("overwrite_allowed")));
    Args->SetBoolField(TEXT("overwrite"),false);TestFalse(TEXT("Explicit false protects existing output"),UnrealToolsRuntime::GetBool(CubeShot::Service::ExportPng(Args),TEXT("success")));FFileHelper::LoadFileToArray(After,*Filename);TestTrue(TEXT("Protected output bytes are unchanged"),Before==After);
    Args->RemoveField(TEXT("overwrite"));auto Repeated=CubeShot::Service::ExportPng(Args);TestTrue(TEXT("Repeated legacy export succeeds"),UnrealToolsRuntime::GetBool(Repeated,TEXT("success")));
    TestTrue(TEXT("Existing file advisory is published"),UnrealToolsRuntime::GetBool(Repeated,TEXT("existing_file_observed")));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotManualCameraParityTest,"CubeShot.Editor.Application.ManualCameraPreflightPreviewExecutionParity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotManualCameraParityTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;FText Policy;if(!CubeShot::CanExecute(Policy)){AddWarning(Policy.ToString());return true;}
    auto Mesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));if(!TestNotNull(TEXT("Camera parity source"),Mesh))return false;
    const double Boundary=Mesh->GetBounds().BoxExtent.GetMax()+CubeShot::MinimumNearPlane;
    for(const bool Perspective:{false,true})for(int32 Case=0;Case<4;++Case)
    {
        const bool AutoFit=Case==3;const double Distance=Case==1?Boundary:Case==2?Boundary+1:.01;const bool Expected=AutoFit||Distance>Boundary;
        auto Json=CubeShot::Service::DefaultOptions();Json->SetBoolField(TEXT("auto_fit"),AutoFit);Json->SetNumberField(TEXT("camera_distance"),Distance);Json->SetNumberField(TEXT("resolution"),256);
        Json->SetStringField(TEXT("projection"),Perspective?TEXT("perspective"):TEXT("orthographic"));
        FCubeShotOptions Options;Options.StaticMesh=Mesh;Options.bAutoFit=AutoFit;Options.CameraDistance=Distance;Options.Resolution=256;
        Options.ProjectionMode=Perspective?ECubeShotProjectionMode::Perspective:ECubeShotProjectionMode::Orthographic;
        FText Error,Warning;const bool Valid=CubeShot::ValidateOptions(Options,Error,Warning);const auto Plan=CubeShot::Service::BuildPreflight(Mesh->GetPathName(),Json);
        FCubeShotViewModel Preview;Preview.SetStaticMesh(Mesh);Preview.SetProjectionMode(Options.ProjectionMode);Preview.SetAutoFit(AutoFit);Preview.SetCameraDistance(Distance);Preview.SetResolution(256);
        FCubeShotCaptureService Capture;FText CaptureError;const bool Executable=Capture.Initialize(Options,CaptureError);
        const FString Label=FString::Printf(TEXT("%s case %d"),Perspective?TEXT("Perspective"):TEXT("Orthographic"),Case);
        TestEqual(Label+TEXT(" validation"),Valid,Expected);TestEqual(Label+TEXT(" preflight"),UnrealToolsRuntime::GetBool(Plan,TEXT("can_proceed")),Expected);
        TestEqual(Label+TEXT(" preview"),Preview.CanGenerate(),Expected);TestEqual(Label+TEXT(" execution preparation"),Executable,Expected);
        if(!Expected)
        {TestEqual(Label+TEXT(" preflight reason"),UnrealToolsRuntime::GetString(Plan,TEXT("error")),Error.ToString());TestEqual(Label+TEXT(" preview reason"),Preview.GetValidationError().ToString(),Error.ToString());TestEqual(Label+TEXT(" execution reason"),CaptureError.ToString(),Error.ToString());}
        Capture.Shutdown();
    }
    if(!CubeShot::Service::Jobs().IsBusy())
    {
        const FString Package=TEXT("/Game/UnrealToolsQA/V12CameraNoLoad_")+FGuid::NewGuid().ToString(EGuidFormats::Digits),Path=Package+TEXT(".Mesh");
        auto Request=MakeShared<FJsonObject>();Request->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));Request->SetArrayField(TEXT("mesh_paths"),{MakeShared<FJsonValueString>(Path)});
        auto Options=CubeShot::Service::DefaultOptions();Options->SetBoolField(TEXT("auto_fit"),false);Options->SetNumberField(TEXT("camera_distance"),.01);Request->SetObjectField(TEXT("options"),Options);
        TestNull(TEXT("No-load fixture package initially absent"),FindPackage(nullptr,*Package));const auto Accepted=CubeShot::Service::Submit(Request);
        TestTrue(TEXT("Admission remains numeric-only even for a distance requiring later bounds inspection"),UnrealToolsRuntime::GetBool(Accepted,TEXT("success")));
        TestNull(TEXT("Job admission loads no mesh package"),FindPackage(nullptr,*Package));
        if(UnrealToolsRuntime::GetBool(Accepted,TEXT("success")))
        {auto Control=MakeShared<FJsonObject>();Control->SetStringField(TEXT("job_id"),UnrealToolsRuntime::GetString(Accepted,TEXT("job_id")));Control->SetStringField(TEXT("action"),TEXT("cancel"));CubeShot::Service::Jobs().Control(Control);CubeShot::Service::Jobs().AdvanceOneStepForTesting();}
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotRedirectorReservationTest,"CubeShot.Editor.Application.RedirectorReservationPathGuard",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotRedirectorReservationTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;FOwnedFixture Fixture;if(!TestTrue(TEXT("Redirector owned destination fixture"),Fixture.Initialize()))return false;
    auto AliasPackage=CreatePackage(*(TEXT("/Game/UnrealToolsQA/V12Alias_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    auto Alias=NewObject<UObjectRedirector>(AliasPackage,TEXT("SM_Former"),RF_Public|RF_Standalone);Alias->DestinationObject=Fixture.Mesh;Fixture.ExtraAssets.Add(Alias);FAssetRegistryModule::AssetCreated(Alias);
    const FString AliasPath=Alias->GetPathName();FSavePackageArgs SaveArgs;SaveArgs.TopLevelFlags=RF_Public|RF_Standalone;SaveArgs.SaveFlags=SAVE_NoError;
    const FString File=FPackageName::LongPackageNameToFilename(AliasPackage->GetName(),FPackageName::GetAssetPackageExtension());IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
    if(!TestTrue(TEXT("Persist owned redirector fixture"),UPackage::SavePackage(AliasPackage,Alias,*File,SaveArgs)))return false;
    auto Followed=LoadObject<UStaticMesh>(nullptr,*AliasPath);
    TestTrue(TEXT("Reproduction: UE LoadObject follows a redirector to a different canonical mesh path"),Followed==Fixture.Mesh&&Followed->GetPathName()!=AliasPath);
    UnrealToolsRuntime::FItemSpec AliasSpec,CanonicalSpec;AliasSpec.MeshPath=AliasPath;AliasSpec.Options=CubeShot::Service::DefaultOptions();CanonicalSpec.MeshPath=Fixture.Mesh->GetPathName();CanonicalSpec.Options=CubeShot::Service::DefaultOptions();
    const auto AliasKeys=CubeShot::Service::Reserve(AliasSpec),CanonicalKeys=CubeShot::Service::Reserve(CanonicalSpec);
    TestTrue(TEXT("Reproduction: accepted alias reservations differ from actual destination output reservations"),AliasKeys.Num()==6&&CanonicalKeys.Num()==6&&AliasKeys!=CanonicalKeys);
    FCubeShotOptions Options;FText Error;TestFalse(TEXT("Shared source loading refuses a redirected path before any capture/output mutation"),CubeShot::Service::LoadOptions(AliasPath,AliasSpec.Options,Options,Error));
    TestTrue(TEXT("Refusal identifies the exact canonical path to use"),Error.ToString().Contains(Fixture.Mesh->GetPathName()));TestNull(TEXT("Refused options cannot expose the unreserved source"),Options.StaticMesh);
    TestTrue(TEXT("Canonical object path remains supported"),CubeShot::Service::LoadOptions(Fixture.Mesh->GetPathName(),CanonicalSpec.Options,Options,Error));
    if(!CubeShot::Service::Jobs().IsBusy()&&CubeShot::CanExecute(Error))
    {
        auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("mesh_path"),AliasPath);auto Result=CubeShot::Service::GenerateInline(Args);
        TestFalse(TEXT("Redirected generation fails safely"),UnrealToolsRuntime::GetBool(Result,TEXT("success")));TestTrue(TEXT("Redirected generation saves no output outside its reservation"),UnrealToolsRuntime::GetArray(Result,TEXT("saved")).IsEmpty());
    }
    for(const auto& Output:Fixture.Outputs)TestNull(TEXT("Canonical output was never created from the alias reservation"),FindObject<UTexture2D>(nullptr,*Output.GetObjectPath()));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotConcurrentPngProtectionTest,"CubeShot.Editor.Application.ConcurrentPngCreateIsProtected",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotConcurrentPngProtectionTest::RunTest(const FString&)
{
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("UnrealTools/V12/Images"));
    const FString Directory=Root/(TEXT("Concurrent_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));const FString Filename=Directory/TEXT("sample.png");
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));
    FEvent* Ready=FPlatformProcess::GetSynchEventFromPool(false);FEvent* Continue=FPlatformProcess::GetSynchEventFromPool(false);
    auto Future=Async(EAsyncExecution::ThreadPool,[Ready,Continue,Filename]()
    {
        FImage Image(2,2,ERawImageFormat::BGRA8,EGammaSpace::sRGB);for(auto& Pixel:Image.AsBGRA8())Pixel=FColor::Red;FText Error;
        return CubeShot::Image::SavePngAtomic(Filename,Image,false,Error,nullptr,[Ready,Continue](){Ready->Trigger();Continue->Wait(5000);});
    });
    const bool Waiting=Ready->Wait(5000);TestTrue(TEXT("Competing publisher pauses after its PNG is complete"),Waiting);
    FImage Original(2,2,ERawImageFormat::BGRA8,EGammaSpace::sRGB);for(auto& Pixel:Original.AsBGRA8())Pixel=FColor(12,34,56,78);FText Error;
    TestTrue(TEXT("Another writer creates target in the rename race window"),CubeShot::Image::SavePngAtomic(Filename,Original,false,Error));TArray<uint8> Before,After;FFileHelper::LoadFileToArray(Before,*Filename);
    Continue->Trigger();TestFalse(TEXT("No-replace rename rejects concurrent target creation"),Future.Get());FFileHelper::LoadFileToArray(After,*Filename);
    TestTrue(TEXT("Concurrent target remains byte-for-byte intact"),Before==After);
    FPlatformProcess::ReturnSynchEventToPool(Ready);FPlatformProcess::ReturnSynchEventToPool(Continue);
    if(FPaths::IsUnderDirectory(Directory,Root))IFileManager::Get().DeleteDirectory(*Directory,false,true);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotLongPngPathTest,"CubeShot.Editor.Application.LongPngPathAtomicPolicy",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotLongPngPathTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("UnrealTools/V12/Images"));
    const FString OwnedDirectory=Root/(TEXT("LongPng_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));FString Directory=OwnedDirectory;
    for(int32 Index=0;Directory.Len()+12<280;++Index)Directory/=TEXT("segment_")+FString::ChrN(48,TEXT('p'))+FString::FromInt(Index);
    const FString Filename=Directory/TEXT("capture.png");TestTrue(TEXT("Owned PNG path exceeds the legacy 260 character limit"),Filename.Len()>=270);
    TArray<FString> Components;Filename.ParseIntoArray(Components,TEXT("/"),true);for(const FString& Component:Components)TestTrue(TEXT("Each fixture path component fits the filesystem limit"),Component.Len()<255);
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));FImage Image(4,3,ERawImageFormat::BGRA8,EGammaSpace::sRGB);
    for(auto& Pixel:Image.AsBGRA8())Pixel=FColor(12,34,56,78);Image.AsBGRA8()[1]=FColor(111,222,123,0);
    FText Error;bool Observed=false;
    const bool Created=CubeShot::Image::SavePngAtomic(Filename,Image,false,Error,&Observed);
    TestTrue(TEXT("New PNG can be published at the long path"),Created);if(!Created)AddError(Error.ToString());
    TArray<uint8> Before,After;TestTrue(TEXT("Long-path PNG bytes can be read"),FFileHelper::LoadFileToArray(Before,*Filename));
    Image.AsBGRA8()[0]=FColor::Red;
    TestFalse(TEXT("overwrite=false refuses an existing long-path PNG"),CubeShot::Image::SavePngAtomic(Filename,Image,false,Error,&Observed));
    TestTrue(TEXT("Existing long-path observation is reported"),Observed);TestTrue(TEXT("Protected long-path PNG remains readable"),FFileHelper::LoadFileToArray(After,*Filename));
    TestTrue(TEXT("Protected long-path PNG bytes are unchanged"),!Before.IsEmpty()&&Before==After);
    const bool Replaced=CubeShot::Image::SavePngAtomic(Filename,Image,true,Error,&Observed);
    TestTrue(TEXT("overwrite=true atomically replaces the long-path PNG"),Replaced);if(!Replaced)AddError(Error.ToString());
    FImage Loaded,Converted;if(TestTrue(TEXT("Replaced long-path PNG decodes"),FImageUtils::LoadImage(*Filename,Loaded)))
    {Loaded.CopyTo(Converted,ERawImageFormat::BGRA8,EGammaSpace::sRGB);TestTrue(TEXT("Long-path replacement preserves every RGBA pixel"),Converted.RawData==Image.RawData);}
    AddInfo(TEXT("Verified owned PNG path: ")+Filename);
    if(FPaths::IsUnderDirectory(OwnedDirectory,Root))IFileManager::Get().DeleteDirectory(*OwnedDirectory,false,true);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotAsyncFailureParityTest,"CubeShot.Editor.Application.AsyncAdmissionErrorParity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotAsyncFailureParityTest::RunTest(const FString&)
{
    if(!OwnedProject(*this))return true;FText Policy;if(CubeShot::Service::Jobs().IsBusy()||!CubeShot::CanExecute(Policy)){AddWarning(TEXT("Async admission parity requires idle rendering QA Editor."));return true;}
    auto Request=MakeShared<FJsonObject>();Request->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    const FString Mesh=TEXT("/Game/UnrealToolsQA/NotLoaded_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".Mesh");Request->SetArrayField(TEXT("mesh_paths"),{MakeShared<FJsonValueString>(Mesh)});
    auto Accepted=CubeShot::Service::Submit(Request);if(!TestTrue(TEXT("No-load queue admits value-only fixture"),UnrealToolsRuntime::GetBool(Accepted,TEXT("success"))))return false;
    auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("mesh_path"),Mesh);const auto Tool=IModelContextProtocolModule::GetChecked().FindTool(TEXT("cubeshot_generate"));
    const auto Direct=Tool->Run(Args);const auto DirectData=UnrealToolsRuntime::GetObject(Direct.JsonObject,TEXT("structuredContent"));int32 Callbacks=0;UnrealToolsRuntime::FObject AsyncData;
    Tool->RunAsync(FModelContextProtocolToolRequestId(),Args,[&](const FModelContextProtocolToolResult& Reply){++Callbacks;AsyncData=UnrealToolsRuntime::GetObject(Reply.JsonObject,TEXT("structuredContent"));});
    TestEqual(TEXT("Async rejected request responds exactly once"),Callbacks,1);TestEqual(TEXT("Run and RunAsync preserve the same structured admission error"),UnrealToolsRuntime::GetString(AsyncData,TEXT("error_code")),UnrealToolsRuntime::GetString(DirectData,TEXT("error_code")));
    auto Control=MakeShared<FJsonObject>();Control->SetStringField(TEXT("job_id"),UnrealToolsRuntime::GetString(Accepted,TEXT("job_id")));Control->SetStringField(TEXT("action"),TEXT("cancel"));CubeShot::Service::Jobs().Control(Control);CubeShot::Service::Jobs().AdvanceOneStepForTesting();return true;
}
#endif

#if defined(CUBESHOT_APPLICATION_RESTORE_LOADIMAGE)
#pragma pop_macro("LoadImage")
#undef CUBESHOT_APPLICATION_RESTORE_LOADIMAGE
#endif
