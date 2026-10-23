#include "FBX_ExporterWorkspaceController.h"
#include "FBX_ExporterApplication.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"

using namespace UnrealToolsRuntime;
namespace
{
FObject InitialFbxOptions(bool ReadSettings)
{
    FFBXExporterOptions Options;
    if (ReadSettings)
        Options.OutputRoot = FBXExporter::LastOutputRoot();
    if (Options.OutputRoot.IsEmpty())
        Options.OutputRoot =
            FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Exports/StaticMeshTextures"));
    return FBXExporter::OptionsToJson(Options);
}
} // namespace
FFBXExporterWorkspaceController::FFBXExporterWorkspaceController(bool ReadSettings, TFunction<void()> InRecoveryRefresh)
    : FController(InitialFbxOptions(ReadSettings), TEXT("pbr"), &FBXExporter::NormalizeJobOptions,
                  &FBXExporter::Profiles, &FBXExporter::Jobs),
      RecoveryRefresh(MoveTemp(InRecoveryRefresh))
{
    if (!RecoveryRefresh)
        RecoveryRefresh = &FBXExporter::RefreshRecoveryDiagnostics;
    Annotate = &FBXExporter::AnnotateRecoveryStatus;
    SetProfileName(TEXT("My FBX Profile"));
    SetUserProfileId(TEXT("my_fbx_profile"));
}
void FFBXExporterWorkspaceController::RefreshRecoveryHistory()
{
    if (IsStopped())
        return;
    RecoveryRefresh();
    HistoryMode(false, true);
}
const TArray<FString>& FFBXExporterWorkspaceController::MapNames()
{
    static const TArray<FString> Names = {TEXT("BaseColor"), TEXT("Normal"),   TEXT("Mask"),
                                          TEXT("Occlusion"), TEXT("Emission"), TEXT("Opacity")};
    return Names;
}
bool FFBXExporterWorkspaceController::SetMapEnabled(const UnrealToolsWorkspace::FEditToken& Token, const FString& Map,
                                                    bool Enabled)
{
    if (!IsCurrent(Token) || !MapNames().Contains(Map))
        return false;
    // A valid JSON draft must be committed before the later checkbox action.
    // A malformed draft remains intact; typed changes accumulate alongside it.
    Flush(Token);
    const auto Options = CopyEditCandidate(Token.Scope);
    auto Maps = GetArray(Options, TEXT("maps"));
    const bool Present = Maps.ContainsByPredicate([&](const auto& Entry) { return Entry->AsString() == Map; });
    if (!Enabled)
        Maps.RemoveAll([&](const auto& Entry) { return Entry->AsString() == Map; });
    else if (!Present)
        Maps.Add(MakeShared<FJsonValueString>(Map));
    auto Values = MakeShared<FJsonObject>();
    Values->SetArrayField(TEXT("maps"), Maps);
    PatchAfterJson(Token, Values);
    return true;
}
bool FFBXExporterWorkspaceController::SetAllMaps(const UnrealToolsWorkspace::FEditToken& Token, bool Enabled)
{
    if (!IsCurrent(Token))
        return false;
    Flush(Token);
    auto Values = MakeShared<FJsonObject>();
    Values->SetArrayField(TEXT("maps"), Enabled ? Strings(MapNames()) : TArray<TSharedPtr<FJsonValue>>());
    PatchAfterJson(Token, Values);
    return true;
}
bool FFBXExporterWorkspaceController::Inspect(FObject Request, bool ForExecution)
{
    TArray<FString> InputErrors;
    if (!Request)
        Request = BuildRequest(true, &InputErrors);
    if (!Request)
    {
        SetPlan(Error());
        return false;
    }
    const auto Batch = FBXExporter::Application::InspectBatch(Request);
    TArray<FString> Errors = Batch.Errors;
    FString Text;
    int32 Planned = 0, Existing = 0;
    for (const auto& Item : Batch.Items)
    {
        const auto& Plan = Item.Plan;
        Text += Item.Spec.MeshPath + TEXT("\n  Folder: ") + Plan.FinalDirectory + TEXT("\n");
        for (const auto& Error : Plan.Errors)
            Text += TEXT("  ERROR: ") + Error + TEXT("\n");
        for (const auto& Warning : Plan.Warnings)
            Text += TEXT("  WARNING: ") + Warning + TEXT("\n");
        for (const auto& File : Plan.PlannedFiles)
            Text += TEXT("  Planned file: ") + File + TEXT("\n");
        Planned += Plan.PlannedFiles.Num();
        Existing += Plan.ReplacedFiles.Num();
        if (Plan.bRequiresOverwrite)
        {
            Text += TEXT("  REPLACEMENT SCOPE: complete folder ") + Plan.FinalDirectory +
                    TEXT(" (including user files and subdirectories)\n");
            for (const auto& File : Plan.ReplacedFiles)
                Text += TEXT("  Affected existing file: ") + File + TEXT("\n");
            if (ForExecution && !GetBool(Request, TEXT("overwrite")))
                Errors.Add(Item.Spec.MeshPath + TEXT(": Explicit whole-folder overwrite approval is required."));
        }
    }
    // Draft errors are projected by Error(); do not retain another global copy.
    SetError(FString::Join(Errors, TEXT("\n")));
    Errors.Append(InputErrors);
    const auto Summary =
        FString::Printf(TEXT("Output plan: %d targets, %d planned files, %d existing files affected, %d errors. "
                             "Expand details for exact filenames and replacement scope."),
                        Batch.Items.Num(), Planned, Existing, Errors.Num());
    for (const auto& Error : Errors)
        Text += TEXT("ERROR: ") + Error + TEXT("\n");
    SetPlan(Text, Summary);
    return Errors.IsEmpty();
}
void FFBXExporterWorkspaceController::Start(FObject Request)
{
    if (!BeginSubmission())
        return;
    ON_SCOPE_EXIT
    {
        EndSubmission();
    };
    if (!Request)
        Request = BuildRequest();
    if (!Request)
    {
        Inspect();
        return;
    }
    Request->SetBoolField(TEXT("overwrite"), bOverwrite);
    if (!Inspect(Request, true))
        return;
    const auto Result = FBXExporter::Application::Submit(Request);
    SetError(GetString(Result, TEXT("error")));
    if (GetBool(Result, TEXT("success")))
    {
        const auto Defaults = CopyOptions({});
        GConfig->SetString(TEXT("/Script/FBX_Exporter.FBX_ExporterSettings"), TEXT("LastOutputRoot"),
                           *GetString(Defaults, TEXT("output_root")), GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
        bOverwrite = false;
        SelectJob(GetString(Result, TEXT("job_id")));
    }
}
void FFBXExporterWorkspaceController::Retry(const FString& Wanted)
{
    auto Request = RetryRequest(Wanted);
    if (Request)
        Start(Request);
}
