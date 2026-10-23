#include "CubeShotWorkspaceController.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "Misc/AutomationTest.h"
#include "UnrealToolsRuntime.h"
#include "CubeShotMcpTools.h"
#include "CubeShotJobService.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "ModelContextProtocolSession.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotMcpContractTest, "CubeShot.Editor.MCP.InputAndRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotMcpContractTest::RunTest(const FString& Parameters)
{
    IModelContextProtocolModule& Module = IModelContextProtocolModule::GetChecked();
    for (const TCHAR* Name : {TEXT("cubeshot_status"), TEXT("cubeshot_find_meshes"), TEXT("cubeshot_preflight"),
        TEXT("cubeshot_export_png"), TEXT("cubeshot_profiles"), TEXT("cubeshot_job_start"), TEXT("cubeshot_job_status"), TEXT("cubeshot_job_control")})
    {
        const TSharedPtr<IModelContextProtocolTool> Tool = Module.FindTool(Name);
        if (!TestTrue(FString::Printf(TEXT("Tool %s registered"), Name), Tool.IsValid())) return false;
        TestEqual(TEXT("Schema is an object"), Tool->GetInputJsonSchema()->GetStringField(TEXT("type")), FString(TEXT("object")));
        TestTrue(TEXT("Output schema is published"), Tool->GetOutputJsonSchema().IsValid());
    }
    TestFalse(TEXT("Retired generate tool is absent"), Module.FindTool(TEXT("cubeshot_generate")).IsValid());
    const TSharedPtr<IModelContextProtocolTool> Preflight = Module.FindTool(TEXT("cubeshot_preflight"));
    auto Rejects = [this, &Preflight](const TCHAR* Label, const TSharedRef<FJsonObject>& Args)
    {
        const FModelContextProtocolToolResult Result = Preflight->Run(Args);
        bool bError = false;
        TestTrue(Label, Result.JsonObject->TryGetBoolField(TEXT("isError"), bError) && bError);
    };
    TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
    Rejects(TEXT("Missing mesh rejected without a write"), Args);
    Args->SetStringField(TEXT("mesh_path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Rejects(TEXT("Flat mesh_path requests are unsupported"), Args);
    Args->RemoveField(TEXT("mesh_path"));
    Args->SetArrayField(TEXT("mesh_paths"), UnrealToolsRuntime::Strings({TEXT("/Engine/BasicShapes/Cube.Cube")}));
    auto Options = MakeShared<FJsonObject>(); Args->SetObjectField(TEXT("options"), Options);
    Options->SetNumberField(TEXT("resolution"), 16);
    Rejects(TEXT("Undersized resolution rejected"), Args);
    Options->SetNumberField(TEXT("resolution"), 512.5);
    Rejects(TEXT("Fractional resolution rejected"), Args);
    Options->SetStringField(TEXT("resolution"), TEXT("512"));
    Rejects(TEXT("String resolution rejected"), Args);
    Options->SetNumberField(TEXT("resolution"), 512);
    Options->SetStringField(TEXT("projection"), TEXT("unknown"));
    Rejects(TEXT("Unknown projection rejected"), Args);
    Options->RemoveField(TEXT("projection"));
    Args->SetArrayField(TEXT("mesh_paths"), UnrealToolsRuntime::Strings({TEXT("/Game/../Engine/BasicShapes/Cube.Cube")}));
    Rejects(TEXT("Malformed asset path rejected"), Args);
    Args = MakeShared<FJsonObject>();
    Args->SetArrayField(TEXT("mesh_paths"), {MakeShared<FJsonValueString>(TEXT("/Engine/BasicShapes/Cube.Cube"))});
    Options = MakeShared<FJsonObject>(); Options->SetStringField(TEXT("background"), TEXT("texture")); Args->SetObjectField(TEXT("options"), Options);
    // A texture background without a texture path fails normalization without loading a fixture.
    const FModelContextProtocolToolResult BatchFailure = Preflight->Run(Args);
    const TSharedPtr<FJsonObject>* Structured;
    FString SchemaError;
    if (TestTrue(TEXT("Negative batch result structured"), BatchFailure.JsonObject->TryGetObjectField(TEXT("structuredContent"), Structured)))
        TestTrue(TEXT("Negative batch result satisfies output schema"), UnrealToolsRuntime::ValidateSchema(*Structured, Preflight->GetOutputJsonSchema(), SchemaError));
    return true;
}

namespace
{
    struct FRegistryRegressionState
    {TAtomic<bool> CallbackDone{false};TAtomic<bool> RegistrationReset{false};TSharedPtr<FJsonObject> Reply;bool NewStatusSuccess=false;bool WorkspaceSurvived=false;TSharedPtr<FCubeShotWorkspaceController> Workspace;};
    class FCubeShotRegistrationGenerationCommand final : public IAutomationLatentCommand
    {
        FAutomationTestBase* Test;TSharedRef<FRegistryRegressionState> State=MakeShared<FRegistryRegressionState>();bool Started=false;double Deadline=0;
    public:
        explicit FCubeShotRegistrationGenerationCommand(FAutomationTestBase* InTest):Test(InTest){}
        virtual bool Update()override
        {
            if(!Started)
            {
                FString Project=FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());Project.ReplaceInline(TEXT("\\"),TEXT("/"));
                if(!Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/"))||CubeShot::Service::Jobs().IsBusy()){Test->AddWarning(TEXT("Registry lifecycle regression requires idle owned QA project."));return true;}
                State->Workspace=MakeShared<FCubeShotWorkspaceController>();State->Workspace->Add(TEXT("/Engine/BasicShapes/Cube.Cube"));
                auto OldTool=IModelContextProtocolModule::GetChecked().FindTool(TEXT("cubeshot_status"));Started=true;Deadline=FPlatformTime::Seconds()+10;
                // Both tasks are posted from one producer to the same normal-priority game-thread queue.
                // Reset registration runs before the old tool's operation; no game-thread waits/ticks.
                Async(EAsyncExecution::ThreadPool,[OldTool,State=State]()
                {
                    AsyncTask(ENamedThreads::GameThread,[State]()
                    {
                        CubeShot::Mcp::Unregister();CubeShot::Mcp::Register();
                        State->Workspace->ReconcileProfiles();State->Workspace->RefreshHistory(true);State->WorkspaceSurvived=State->Workspace->BuildRequest().IsValid();
                        auto NewTool=IModelContextProtocolModule::GetChecked().FindTool(TEXT("cubeshot_status"));auto Result=NewTool->Run(MakeShared<FJsonObject>());
                        const TSharedPtr<FJsonObject>* Content;State->NewStatusSuccess=Result.JsonObject->TryGetObjectField(TEXT("structuredContent"),Content)&&UnrealToolsRuntime::GetBool(*Content,TEXT("success"));State->RegistrationReset.Store(true);
                    });
                    OldTool->RunAsync(FModelContextProtocolToolRequestId(),MakeShared<FJsonObject>(),[State](const FModelContextProtocolToolResult& Result)
                    {const TSharedPtr<FJsonObject>* Content;if(Result.JsonObject->TryGetObjectField(TEXT("structuredContent"),Content))State->Reply=UnrealToolsRuntime::Clone(*Content);State->CallbackDone.Store(true);});
                });return false;
            }
            if(!State->CallbackDone.Load()){if(FPlatformTime::Seconds()<Deadline)return false;Test->AddError(TEXT("Posted stale MCP callback did not complete."));return true;}
            Test->TestTrue(TEXT("Registration reset ran first"),State->RegistrationReset.Load());Test->TestTrue(TEXT("New registration works"),State->NewStatusSuccess);
            Test->TestTrue(TEXT("Session workspace reconnects to recreated services"),State->WorkspaceSurvived);
            Test->TestFalse(TEXT("Old posted tool rejected after unregister/register"),UnrealToolsRuntime::GetBool(State->Reply,TEXT("success")));
            Test->TestEqual(TEXT("Stale callback returns safe interruption"),UnrealToolsRuntime::GetString(State->Reply,TEXT("error_code")),FString(TEXT("EDITOR_INTERRUPTED")));return true;
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotRegistrationGenerationTest,"CubeShot.Editor.MCP.RegistrationGeneration",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotRegistrationGenerationTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FCubeShotRegistrationGenerationCommand(this));return true;}

#endif
