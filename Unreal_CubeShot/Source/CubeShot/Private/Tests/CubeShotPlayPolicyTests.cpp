#include "CubeShotJobService.h"
#include "CubeShotTypes.h"
#include "Editor.h"
#include "GameFramework/GameModeBase.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "LevelEditor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    class FCubeShotRealPlayPolicyCommand final : public IAutomationLatentCommand
    {
        FAutomationTestBase* Test;
        TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
        TStrongObjectPtr<ULevelEditorPlaySettings> OriginalSettings;
        TUniquePtr<FConfigFile> OriginalConfig;
        TArray<uint8> OriginalFile;
        FString PreferencesFile;
        bool PreferencesExisted=false;
        int32 Mode=0,Phase=0;
        double Deadline=0;
        bool OwnsSession=false;
        void RestorePreferences()
        {
            if(!OriginalSettings)return;
            ULevelEditorPlaySettings* Defaults=GetMutableDefault<ULevelEditorPlaySettings>();
            for(TFieldIterator<FProperty> Property(ULevelEditorPlaySettings::StaticClass());Property;++Property)
                if(Property->HasAnyPropertyFlags(CPF_Config))Property->CopyCompleteValue_InContainer(Defaults,OriginalSettings.Get());
            if(OriginalConfig)if(FConfigFile* Config=GConfig->FindConfigFile(GEditorPerProjectIni))*Config=*OriginalConfig;
            if(PreferencesExisted)
            {
                const FString Temporary=PreferencesFile+TEXT(".qa_restore.tmp");FString Error;
                if(!FFileHelper::SaveArrayToFile(OriginalFile,*Temporary)||!UnrealToolsRuntime::AtomicReplace(Temporary,PreferencesFile,Error))Test->AddError(TEXT("Could not restore QA Editor preferences: ")+Error);
            }
            else IFileManager::Get().Delete(*PreferencesFile,false,true);
            OriginalSettings.Reset();
        }
        bool SafeProject()const
        {
            FString Project=FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());Project.ReplaceInline(TEXT("\\"),TEXT("/"));
            return Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/"),ESearchCase::IgnoreCase);
        }
        TSharedPtr<FJsonObject> Structured(const TCHAR* Tool,const TSharedPtr<FJsonObject>& Args)
        {
            auto Registered=IModelContextProtocolModule::GetChecked().FindTool(Tool);
            if(!Registered){Test->AddError(TEXT("Missing policy tool: ")+FString(Tool));return nullptr;}
            const auto Reply=Registered->Run(Args);const TSharedPtr<FJsonObject>* Json;
            if(!Reply.JsonObject->TryGetObjectField(TEXT("structuredContent"),Json)){Test->AddError(TEXT("Policy tool lacks structured response."));return nullptr;}return *Json;
        }
    public:
        explicit FCubeShotRealPlayPolicyCommand(FAutomationTestBase* InTest):Test(InTest){}
        virtual ~FCubeShotRealPlayPolicyCommand()override
        {if(OwnsSession&&GEditor&&SafeProject()){if(!GEditor->PlayWorld)GEditor->CancelRequestPlaySession();GEditor->RequestEndPlayMap();}RestorePreferences();}
        virtual bool Update()override
        {
            using namespace UnrealToolsRuntime;
            if(Phase==0)
            {
                if(!SafeProject()){Test->AddWarning(TEXT("Actual play policy test is restricted to owned Saved/UnrealTools/V12/Projects; no user Editor state was changed."));return true;}
                if(!GEditor||GEditor->PlayWorld||GEditor->bIsSimulatingInEditor||GEditor->GetPlaySessionRequest().IsSet()||CubeShot::Service::Jobs().IsBusy())
                {Test->AddWarning(TEXT("Actual play policy needs an idle owned QA Editor; existing play/work was preserved."));return true;}
                FText Policy;if(!CubeShot::CanExecute(Policy)){Test->AddWarning(TEXT("Actual PIE/SIE verification requires a rendering QA Editor: ")+Policy.ToString());return true;}
                if(!OriginalSettings)
                {
                    PreferencesFile=FPaths::ConvertRelativePathToFull(GEditorPerProjectIni);
                    if(!FPaths::IsUnderDirectory(PreferencesFile,FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))
                    {Test->AddError(TEXT("Actual play test refuses non-isolated Editor preferences."));return true;}
                    OriginalSettings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetMutableDefault<ULevelEditorPlaySettings>(),GetTransientPackage()));
                    if(FConfigFile* Config=GConfig->FindConfigFile(GEditorPerProjectIni))OriginalConfig=MakeUnique<FConfigFile>(*Config);
                    PreferencesExisted=IFileManager::Get().FileExists(*PreferencesFile);if(PreferencesExisted&&!FFileHelper::LoadFileToArray(OriginalFile,*PreferencesFile)){Test->AddError(TEXT("Could not snapshot QA preferences."));return true;}
                }
                Settings.Reset(NewObject<ULevelEditorPlaySettings>());Settings->SetPlayNetMode(PIE_Standalone);Settings->SetRunUnderOneProcess(true);Settings->SetPlayNumberOfClients(1);Settings->bLaunchSeparateServer=false;
                FRequestPlaySessionParams Params;Params.SessionDestination=EPlaySessionDestinationType::InProcess;
                Params.WorldType=Mode==0?EPlaySessionWorldType::PlayInEditor:EPlaySessionWorldType::SimulateInEditor;
                Params.EditorPlaySettings=Settings.Get();Params.bAllowOnlineSubsystem=false;Params.GameModeOverride=AGameModeBase::StaticClass();Params.StartLocation=FVector(0,0,500);
                const auto Viewport=FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor")).GetFirstActiveViewport();
                if(Viewport.IsValid())Params.DestinationSlateViewport.Emplace(Viewport);
                else {Test->AddError(TEXT("Owned QA Editor has no level viewport for PIE/SIE verification."));return true;}
                OwnsSession=true;GEditor->RequestPlaySession(Params);Deadline=FPlatformTime::Seconds()+20;Phase=1;return false;
            }
            if(Phase==1)
            {
                if(!GEditor->PlayWorld){if(FPlatformTime::Seconds()<Deadline)return false;Test->AddError(TEXT("Owned QA play session did not start."));GEditor->CancelRequestPlaySession();GEditor->RequestEndPlayMap();Deadline=FPlatformTime::Seconds()+20;Phase=3;Mode=1;return false;}
                Test->TestEqual(TEXT("Actual PIE/SIE mode"),static_cast<bool>(GEditor->bIsSimulatingInEditor),Mode==1);
                const FString Label=Mode==0?TEXT("Actual PIE"):TEXT("Actual SIE");
                auto Status=Structured(TEXT("cubeshot_status"),MakeShared<FJsonObject>());Test->TestTrue(Label+TEXT(" status marks playing"),GetBool(Status,TEXT("pie_running")));Test->TestFalse(Label+TEXT(" capture unavailable"),GetBool(Status,TEXT("capture_available")));
                auto Args=MakeShared<FJsonObject>();Args->SetArrayField(TEXT("mesh_paths"),Strings({TEXT("/Engine/BasicShapes/Cube.Cube")}));
                auto Plan=Structured(TEXT("cubeshot_preflight"),Args);Test->TestFalse(Label+TEXT(" preflight rejected"),GetBool(Plan,TEXT("success")));Test->TestFalse(Label+TEXT(" cannot proceed"),GetBool(Plan,TEXT("can_proceed")));
                const int32 Before=CubeShot::Service::Jobs().GetJobs().Num();auto Job=MakeShared<FJsonObject>();Job->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));Job->SetArrayField(TEXT("mesh_paths"),{MakeShared<FJsonValueString>(TEXT("/Engine/BasicShapes/Cube.Cube"))});
                auto Admission=Structured(TEXT("cubeshot_job_start"),Job);Test->TestFalse(Label+TEXT(" admission rejected"),GetBool(Admission,TEXT("success")));Test->TestEqual(Label+TEXT(" no job persisted"),CubeShot::Service::Jobs().GetJobs().Num(),Before);
                GEditor->RequestEndPlayMap();Deadline=FPlatformTime::Seconds()+20;Phase=3;return false;
            }
            if(Phase==3)
            {
                if(GEditor->PlayWorld||GEditor->bIsSimulatingInEditor||GEditor->GetPlaySessionRequest().IsSet())
                {if(FPlatformTime::Seconds()<Deadline)return false;Test->AddError(TEXT("Owned QA play session did not stop."));return true;}
                OwnsSession=false;Settings.Reset();++Mode;if(Mode<2){Phase=0;return false;}RestorePreferences();FText Error;Test->TestTrue(TEXT("Rendering policy restored after own play sessions"),CubeShot::CanExecute(Error));return true;
            }
            return true;
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotRealPlayPolicyTest,"CubeShot.Editor.MCP.RealPlaySessionPolicy",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotRealPlayPolicyTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FCubeShotRealPlayPolicyCommand(this));return true;}
#endif
