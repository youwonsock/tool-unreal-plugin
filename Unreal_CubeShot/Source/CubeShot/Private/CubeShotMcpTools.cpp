#include "CubeShotMcpTools.h"

#include "CubeShotJobService.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CubeShot::Mcp
{
namespace
{

    FDelegateHandle RefreshHandle;
    TArray<TSharedRef<IModelContextProtocolTool>> RegisteredTools;
    bool bRegistered=false;
    uint64 RegistrationGeneration=0;

    FModelContextProtocolToolResult Result(const TSharedRef<FJsonObject>& Data, bool bError = false)
    {
        Data->SetBoolField(TEXT("success"), !bError);
        FString Text;
        FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Text));
        FModelContextProtocolToolResult Output = UE::ModelContextProtocol::MakeTextResult(Text);
        Output.JsonObject->SetObjectField(TEXT("structuredContent"), Data);
        Output.JsonObject->SetBoolField(TEXT("isError"), bError);
        return Output;
    }

    FModelContextProtocolToolResult Error(const FString& Message)
    {
        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("error"), Message);
        Data->SetStringField(TEXT("error_code"), TEXT("invalid_argument"));
        return Result(Data, true);
    }

    TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& Value : Values) Out.Add(MakeShared<FJsonValueString>(Value));
        return Out;
    }

    TSharedPtr<FJsonObject> OptionsFromParams(const TSharedPtr<FJsonObject>& Params)
    {
        TSharedPtr<FJsonObject> Options = UnrealToolsRuntime::Clone(Params);
        for (const TCHAR* Key : {TEXT("mesh_path"), TEXT("mesh_paths"), TEXT("overwrite")}) Options->RemoveField(Key);
        return Options;
    }
    FModelContextProtocolToolResult ObjectResult(const TSharedPtr<FJsonObject>& Data)
    {
        bool Success = false;
        return Result(Data.ToSharedRef(), !Data->TryGetBoolField(TEXT("success"), Success) || !Success);
    }
    FModelContextProtocolToolResult Status(const TSharedPtr<FJsonObject>&)
    {
        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        FText PolicyError; const bool Available = CubeShot::CanExecute(PolicyError);
        const bool Scanned=CubeShot::Service::Jobs().IsInitialScanComplete();
        const auto Issues=CubeShot::Service::Jobs().GetRecordIssues();
        Data->SetStringField(TEXT("project"), FApp::GetProjectName());
        Data->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
        Data->SetBoolField(TEXT("pie_running"), GEditor && (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor));
        Data->SetBoolField(TEXT("capture_available"), Available && !CubeShot::Service::Jobs().IsBusy() && Scanned && Issues.IsEmpty());
        const FString Blocked=!Available?PolicyError.ToString():!Scanned?TEXT("Saved job records are still being scanned."):!Issues.IsEmpty()?TEXT("Review unresolved saved record diagnostics."):CubeShot::Service::Jobs().IsBusy()?TEXT("A CubeShot job is queued or running."):TEXT("");
        Data->SetStringField(TEXT("blocked_reason"),Blocked);Data->SetBoolField(TEXT("initial_scan_complete"),Scanned);
        Data->SetStringField(TEXT("api_version"), TEXT("1.2.0")); Data->SetStringField(TEXT("plugin_version"), TEXT("1.2.0"));
        Data->SetArrayField(TEXT("capabilities"), Strings({TEXT("jobs"),TEXT("profiles"),TEXT("batch_preflight"),TEXT("capture_options"),TEXT("extended_options"),TEXT("png_framing"),TEXT("registered_mount_search"),TEXT("recovery_acknowledgement"),TEXT("png_overwrite"),TEXT("history_retention"),TEXT("record_diagnostics")}));
        Data->SetStringField(TEXT("output_root"), TEXT("/Game/CubeShot"));
        Data->SetStringField(TEXT("capture_backend"), TEXT("CubeShotCaptureService"));
        Data->SetArrayField(TEXT("directions"), Strings({TEXT("Front"), TEXT("Back"), TEXT("Left"), TEXT("Right"), TEXT("Top"), TEXT("Bottom")}));
        TArray<TSharedPtr<FJsonValue>> Active;
        for (const auto& Job : CubeShot::Service::Jobs().GetActiveJobs())
        { const FString State = Job->GetStringField(TEXT("state")); if (State == TEXT("queued") || State == TEXT("running")) Active.Add(MakeShared<FJsonValueObject>(Job)); }
        Data->SetArrayField(TEXT("active_jobs"), Active);
        Data->SetObjectField(TEXT("retention_policy"),CubeShot::Service::Jobs().GetRetentionPolicy());
        TArray<TSharedPtr<FJsonValue>> Diagnostics;for(const auto& Issue:Issues)Diagnostics.Add(MakeShared<FJsonValueObject>(Issue));
        Data->SetArrayField(TEXT("record_diagnostics"),Diagnostics);
        return Result(Data);
    }

    FModelContextProtocolToolResult FindMeshes(const TSharedPtr<FJsonObject>& Params)
    {
        FString Query;
        FString Root = TEXT("/Game");
        if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty()) return Error(TEXT("query must be nonempty."));
        if (Params->HasField(TEXT("root")) && !Params->TryGetStringField(TEXT("root"), Root)) return Error(TEXT("root must be a package path."));
        TArray<FString> Mounts; FPackageName::QueryRootContentPaths(Mounts);
        bool Mounted = false; for (const FString& Mount : Mounts) Mounted |= Root == Mount.LeftChop(1) || Root.StartsWith(Mount);
        if (!Mounted) return Error(TEXT("Search root must be under a registered content mount."));
        if (Root.Contains(TEXT("..")) || Root.Contains(TEXT("\\"))) return Error(TEXT("Invalid search root."));
        int32 Limit = 40;
        if (Params->HasField(TEXT("limit")))
        {
            double Number; if (!Params->HasTypedField<EJson::Number>(TEXT("limit")) || !Params->TryGetNumberField(TEXT("limit"), Number) || !FMath::IsFinite(Number) || Number < 1 || Number > 200 || Number != FMath::FloorToDouble(Number)) return Error(TEXT("limit must be an integer from 1 to 200."));
            Limit = static_cast<int32>(Number);
        }
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Root));
        Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
        Filter.bRecursivePaths = true;
        TArray<FAssetData> Assets;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
        TArray<FString> Matches;
        for (const FAssetData& Asset : Assets)
        {
            if (Asset.AssetName.ToString().Contains(Query)) Matches.Add(Asset.GetObjectPathString());
        }
        Matches.Sort();
        const int32 Total = Matches.Num();
        if (Matches.Num() > Limit) Matches.SetNum(Limit);
        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("meshes"), Strings(Matches));
        Data->SetNumberField(TEXT("total_matches"), Total);
        Data->SetBoolField(TEXT("truncated"), Total > Limit);
        return Result(Data);
    }

    FModelContextProtocolToolResult Preflight(const TSharedPtr<FJsonObject>& Params)
    {
        const bool Single=Params->HasField(TEXT("mesh_path")), Batch=Params->HasField(TEXT("mesh_paths"));
        if(Single==Batch)return Error(TEXT("Supply exactly one of mesh_path or mesh_paths."));
        auto Request=MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        Request->SetObjectField(TEXT("options"),OptionsFromParams(Params));
        Request->SetArrayField(TEXT("mesh_paths"),Single ? TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(Params->GetStringField(TEXT("mesh_path")))} : Params->GetArrayField(TEXT("mesh_paths")));
        auto Plan=CubeShot::Service::InspectBatch(Request);
        if(!Single)return ObjectResult(Plan);
        const auto Items=UnrealToolsRuntime::GetArray(Plan,TEXT("items"));
        if(Items.IsEmpty())return ObjectResult(Plan);
        auto Item=UnrealToolsRuntime::Clone(Items[0]->AsObject());
        if(!UnrealToolsRuntime::GetBool(Plan,TEXT("can_proceed")))
        {
            Item->SetBoolField(TEXT("success"),false);Item->SetBoolField(TEXT("can_proceed"),false);
            if(UnrealToolsRuntime::GetString(Item,TEXT("error")).IsEmpty())
            {Item->SetStringField(TEXT("error"),UnrealToolsRuntime::GetString(Plan,TEXT("error")));Item->SetStringField(TEXT("error_code"),TEXT("preflight_failed"));}
        }
        return ObjectResult(Item);
    }

    FModelContextProtocolToolResult Generate(const TSharedPtr<FJsonObject>& Params) { return ObjectResult(CubeShot::Service::GenerateInline(Params)); }
    FModelContextProtocolToolResult Profiles(const TSharedPtr<FJsonObject>& Params) { return ObjectResult(CubeShot::Service::HandleProfiles(Params)); }
    FModelContextProtocolToolResult JobStart(const TSharedPtr<FJsonObject>& Params) { return ObjectResult(CubeShot::Service::Submit(Params)); }
    FModelContextProtocolToolResult JobStatus(const TSharedPtr<FJsonObject>& Params) { return ObjectResult(CubeShot::Service::Jobs().Status(Params)); }
    FModelContextProtocolToolResult JobControl(const TSharedPtr<FJsonObject>& Params) { return ObjectResult(CubeShot::Service::Jobs().Control(Params)); }
    FModelContextProtocolToolResult ExportPng(const TSharedPtr<FJsonObject>& Params) { return ObjectResult(CubeShot::Service::ExportPng(Params)); }

    using FExecute = FModelContextProtocolToolResult (*)(const TSharedPtr<FJsonObject>&);

    struct FTool final : IModelContextProtocolTool
    {
        FString Name;
        FString Description;
        TSharedPtr<FJsonObject> Schema;
        FExecute Execute;
        uint64 Generation;

        FTool(const TCHAR* InName, const TCHAR* InDescription, const TCHAR* SchemaText, FExecute InExecute)
            : Name(InName), Description(InDescription), Execute(InExecute), Generation(RegistrationGeneration)
        {
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(SchemaText)), Schema);
            check(Schema.IsValid());
        }
        virtual FString GetName() const override { return Name; }
        virtual FString GetDescription() const override { return Description; }
        virtual TSharedPtr<FJsonObject> GetInputJsonSchema() const override { return Schema; }
        virtual TSharedPtr<FJsonObject> GetOutputJsonSchema() const override { return UnrealToolsRuntime::OutputSchema(); }
        virtual FModelContextProtocolToolResult Run(const TSharedPtr<FJsonObject>& Params) override
        {
            if (!IsInGameThread()) return Error(TEXT("CubeShot tools require the Editor game thread."));
            if(!bRegistered||Generation!=RegistrationGeneration)return ObjectResult(UnrealToolsRuntime::Error(TEXT("CubeShot MCP registration ended before execution."),TEXT("EDITOR_INTERRUPTED")));
            if (!Params.IsValid()) return Error(TEXT("Expected an arguments object."));
            FString Message;
            if (!UnrealToolsRuntime::ValidateSchema(Params, Schema, Message)) return Error(Message);
            return Execute(Params);
        }
        virtual void RunAsync(const FModelContextProtocolToolRequestId&, const TSharedPtr<FJsonObject>& Params,
            const FResultCallback& OnComplete) override
        {
            // Keep the registered tool alive until its game-thread operation has completed.
            const TSharedRef<IModelContextProtocolTool> Self = AsShared();
            const auto OwnedParams=Params.IsValid()?UnrealToolsRuntime::Clone(Params):TSharedPtr<FJsonObject>();
            auto Work = [Self, Params=OwnedParams, OnComplete]()
            {
                const auto Concrete=StaticCastSharedRef<FTool>(Self);
                if(!bRegistered||Concrete->Generation!=RegistrationGeneration)
                {OnComplete(ObjectResult(UnrealToolsRuntime::Error(TEXT("CubeShot MCP registration ended before request execution."),TEXT("EDITOR_INTERRUPTED"))));return;}
                if(!Params.IsValid()){OnComplete(Error(TEXT("Expected an arguments object.")));return;}
                if (Self->GetName() == TEXT("cubeshot_generate"))
                {
                    FString Message;
                    // Validate the exact same arguments before accepting a legacy asynchronous request.
                    if (!UnrealToolsRuntime::ValidateSchema(Params, Concrete->Schema, Message)) { OnComplete(Error(Message)); return; }
                    const auto Accepted=CubeShot::Service::GenerateAsync(Params, [Self, OnComplete](const TSharedPtr<FJsonObject>& Generated) { OnComplete(ObjectResult(Generated)); });
                    if(!UnrealToolsRuntime::GetBool(Accepted,TEXT("success")))OnComplete(ObjectResult(Accepted));
                }
                else OnComplete(Self->Run(Params));
            };
            if (IsInGameThread()) Work();
            else AsyncTask(ENamedThreads::GameThread, MoveTemp(Work));
        }
    };

    void AddTools()
    {
        if(!bRegistered)return;
        IModelContextProtocolModule& Module = IModelContextProtocolModule::GetChecked();
        for (const auto& Tool : RegisteredTools) Module.RemoveTool(Tool);
        RegisteredTools.Reset();
        auto Add = [&Module](const TCHAR* Name, const TCHAR* Description, const TCHAR* Schema, FExecute Execute)
        {
            TSharedRef<IModelContextProtocolTool> Tool = MakeShared<FTool>(Name, Description, Schema, Execute);
            if (Module.AddTool(Tool)) RegisteredTools.Add(Tool);
        };
        Add(TEXT("cubeshot_status"), TEXT("Read CubeShot capture availability, current project and output policy. Check this before capture; it never starts PIE or changes a level."),
            TEXT(R"({"type":"object","properties":{},"additionalProperties":false})"), Status);
        Add(TEXT("cubeshot_find_meshes"), TEXT("Search the current project's StaticMesh assets by name. Returns exact object paths without loading meshes. Narrow query/root when truncated."),
            TEXT(R"({"type":"object","properties":{"query":{"type":"string","minLength":1},"root":{"type":"string","default":"/Game"},"limit":{"type":"integer","minimum":1,"maximum":200,"default":40}},"required":["query"],"additionalProperties":false})"), FindMeshes);
        const FString OptionProperties = TEXT(R"("resolution":{"type":"integer","minimum":256,"maximum":4096,"default":512},"projection":{"type":"string","enum":["orthographic","perspective"],"default":"orthographic"},"background":{"type":"string","enum":["transparent","solid","texture"],"default":"transparent"},"background_texture":{"type":"string"},"solid_color":{"type":"array","minItems":4,"maxItems":4,"items":{"type":"number","minimum":0,"maximum":1}},"auto_fit":{"type":"boolean","default":true},"camera_distance":{"type":"number","minimum":0.01,"maximum":10000000},"field_of_view":{"type":"number","minimum":1,"maximum":170},"ortho_width":{"type":"number","minimum":0.01,"maximum":10000000},"export_png":{"type":"boolean","default":false},"png_directions":{"type":"array","minItems":1,"maxItems":6,"uniqueItems":true,"items":{"type":"string","enum":["Front","Back","Left","Right","Top","Bottom"]}},"trim_transparent":{"type":"boolean","default":true},"padding_pixels":{"type":"integer","minimum":0,"maximum":64,"default":8},"quarter_turns":{"type":"integer","minimum":0,"maximum":3,"default":0})");
        const FString PreflightSchema = TEXT("{\"type\":\"object\",\"properties\":{\"mesh_path\":{\"type\":\"string\"},\"mesh_paths\":{\"type\":\"array\",\"minItems\":1,\"uniqueItems\":true,\"items\":{\"type\":\"string\"}},") + OptionProperties + TEXT("},\"oneOf\":[{\"required\":[\"mesh_path\"]},{\"required\":[\"mesh_paths\"]}],\"additionalProperties\":false}");
        Add(TEXT("cubeshot_preflight"), TEXT("Validate one mesh_path or a mesh_paths batch without generating assets. Returns six Texture2D paths, selected PNG paths, errors, warnings and overwrite requirements. Supports all capture options and registered mounts."), *PreflightSchema, Preflight);
        const FString GenerateSchema = TEXT("{\"type\":\"object\",\"properties\":{\"mesh_path\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\",\"default\":false},") + OptionProperties + TEXT("},\"required\":[\"mesh_path\"],\"additionalProperties\":false}");
        Add(TEXT("cubeshot_generate"), TEXT("Capture and save six Texture2D views. The asynchronous MCP request completes after the staged operation; direct Run executes the same steps inline. Requires rendering Editor with PIE/SIE stopped. Defaults remain 512/orthographic/transparent. Existing assets retain identity. Partial saves report dirty packages and staging."), *GenerateSchema, Generate);
        Add(TEXT("cubeshot_profiles"), TEXT("List/get/save/delete CubeShot profiles. Builtins editor_default and item_icon are immutable. Save takes a versioned provider/id/display_name/options profile JSON; overwrite approvals are never stored."),
            TEXT(R"({"type":"object","properties":{"action":{"type":"string","enum":["list","get","save","delete"]},"id":{"type":"string"},"overwrite":{"type":"boolean","default":false},"profile":{"type":"object"}},"required":["action"],"additionalProperties":false})"), Profiles);
        const FString OptionsSchema = TEXT("{\"type\":\"object\",\"properties\":{") + OptionProperties + TEXT("},\"additionalProperties\":false}");
        const FString StartSchema = TEXT("{\"type\":\"object\",\"properties\":{\"request_id\":{\"type\":\"string\"},\"mesh_paths\":{\"type\":\"array\",\"minItems\":1,\"uniqueItems\":true,\"items\":{\"type\":\"string\"}},\"profile_id\":{\"type\":\"string\"},\"options\":") + OptionsSchema + TEXT(",\"overrides\":{\"type\":\"object\",\"additionalProperties\":") + OptionsSchema + TEXT("},\"overwrite\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"request_id\",\"mesh_paths\"],\"additionalProperties\":false}");
        Add(TEXT("cubeshot_job_start"), TEXT("Enqueue an ordered CubeShot batch with a durable request_id. Same ID and same request returns the original job; changed request is rejected. Options priority: overrides, common options, profile, API defaults. Admission performs no mesh loads. Overwrite defaults false for Texture2D and PNG."), *StartSchema, JobStart);
        Add(TEXT("cubeshot_job_status"), TEXT("Read a persisted job by job_id or request_id. success denotes successful lookup; state denotes actual execution outcome. Reports ordered items, progress, output and recovery paths."), TEXT(R"({"type":"object","properties":{"job_id":{"type":"string","minLength":1},"request_id":{"type":"string","minLength":1}},"oneOf":[{"required":["job_id"]},{"required":["request_id"]}],"additionalProperties":false})"), JobStatus);
        Add(TEXT("cubeshot_job_control"), TEXT("Request cancel at a safe precommit boundary or acknowledge_recovery to release reviewed output reservations. Acknowledgement does not restore/delete files. Cancellation acceptance differs from application."), TEXT(R"({"type":"object","properties":{"action":{"type":"string","enum":["cancel","acknowledge_recovery"]},"job_id":{"type":"string","minLength":1},"request_id":{"type":"string","minLength":1}},"required":["action"],"oneOf":[{"required":["job_id"]},{"required":["request_id"]}],"additionalProperties":false})"), JobControl);
        Add(TEXT("cubeshot_export_png"), TEXT("Export a generated CubeShot texture to Saved/CubeShot/MCPExports. Optionally trim transparent margins with padding and rotate clockwise in 90-degree steps for item-slot framing. Captured color/alpha pixels are preserved without resampling; the source texture is unchanged. overwrite defaults true for legacy compatibility; false protects existing files. existing_file_observed is advisory and does not prove a concurrent replacement. Returns dimensions, applied options and alpha coverage."),
            TEXT(R"({"type":"object","properties":{"texture_path":{"type":"string"},"trim_transparent":{"type":"boolean","default":false},"padding_pixels":{"type":"integer","minimum":0,"maximum":64,"default":8},"quarter_turns":{"type":"integer","minimum":0,"maximum":3,"default":0},"overwrite":{"type":"boolean","default":true}},"required":["texture_path"],"additionalProperties":false})"), ExportPng);
    }
}

void Register()
{
    if(bRegistered)return;
    ++RegistrationGeneration;
    CubeShot::Service::Initialize();
    bRegistered=true;
    FModuleManager::LoadModuleChecked<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
    if (!RefreshHandle.IsValid())
        RefreshHandle = IModelContextProtocolModule::GetChecked().OnRefreshTools().AddStatic(&AddTools);
    AddTools();
}

void Unregister()
{
    bRegistered=false;++RegistrationGeneration;
    if (IModelContextProtocolModule* Module = IModelContextProtocolModule::Get())
    {
        Module->OnRefreshTools().Remove(RefreshHandle);
        for (const TSharedRef<IModelContextProtocolTool>& Tool : RegisteredTools) Module->RemoveTool(Tool);
    }
    RefreshHandle.Reset();
    RegisteredTools.Reset();
    CubeShot::Service::Shutdown();
}
}
