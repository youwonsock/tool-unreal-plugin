#include "FBX_ExporterWorkspaceController.h"
#include "FBX_ExporterService.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FRecoveryHistoryRunner final : public UnrealToolsRuntime::IItemRunner
{
public:
    explicit FRecoveryHistoryRunner(const FString& Journal)
    {
        using namespace UnrealToolsRuntime;
        Result = UnrealToolsRuntime::Error(TEXT("Fixture requires recovery."), TEXT("RECOVERY_REQUIRED"));
        Result->SetBoolField(TEXT("recovery_required"), true);
        Result->SetStringField(TEXT("recovery_journal"), Journal);
        Result->SetArrayField(TEXT("warnings"), Strings({TEXT("Keep the original runner warning.")}));
    }
    UnrealToolsRuntime::EStepResult Step(bool, bool) override { return UnrealToolsRuntime::EStepResult::Failed; }
    FString GetStage() const override { return TEXT("done"); }
    bool CanCancel() const override { return true; }
    bool IsCommitPhase() const override { return false; }
    UnrealToolsRuntime::FObject GetResult() const override { return Result; }
    uint64 GetSnapshotRevision() const override { return 0; }
private:
    UnrealToolsRuntime::FObject Result;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXWorkspaceRecoveryCacheTest, "FBX_Exporter.Editor.Workspace.RecoveryFileCache",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXWorkspaceRecoveryCacheTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    const FString Name = TEXT("UnrealToolsTests/RecoveryHistory/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / Name);
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    IFileManager::Get().MakeDirectory(*Root, true);
    const FString JournalA = Root / TEXT("a.json"), JournalB = Root / TEXT("b.json");
    const FString MeshA = TEXT("/Game/CacheA.CacheA"), MeshB = TEXT("/Game/CacheB.CacheB");
    if (!TestTrue(TEXT("Two recovery file fixtures prepared"),
                  FFileHelper::SaveStringToFile(TEXT("{}"), *JournalA) && FFileHelper::SaveStringToFile(TEXT("{}"), *JournalB))) return false;
    FProfileStore Profiles(TEXT("recovery_cache_test"), Name);
    const auto Defaults = MakeShared<FJsonObject>();
    FJobService Jobs(TEXT("recovery_cache_test"), Name, Profiles, Defaults,
        [](FObject&, FString&) { return true; },
        [&](const FItemSpec& Spec) { return TArray<FString>{Root / FPackageName::GetShortName(Spec.MeshPath)}; },
        [&](const FItemSpec& Spec, const FString&) -> TUniquePtr<IItemRunner> {
            return MakeUnique<FRecoveryHistoryRunner>(Spec.MeshPath == MeshA ? JournalA : JournalB);
        });
    const auto Submit = [&](const FString& Mesh) {
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        Request->SetArrayField(TEXT("mesh_paths"), Strings({Mesh}));
        return GetString(Jobs.Start(Request), TEXT("job_id"));
    };
    const auto A = Submit(MeshA), B = Submit(MeshB);
    if (!TestTrue(TEXT("Both fake jobs admitted"), !A.IsEmpty() && !B.IsEmpty())) return false;
    Jobs.AdvanceOneStepForTesting(); Jobs.AdvanceOneStepForTesting();
    auto Model = MakeShared<UnrealToolsWorkspace::FController>(Defaults, TEXT("fixture"),
        [](FObject&, FString&) { return true; }, [&]() -> FProfileStore& { return Profiles; }, [&]() -> FJobService& { return Jobs; });
    Model->Annotate = &FBXExporter::AnnotateRecoveryStatus;
    Model->IsAnnotationCurrent = &FBXExporter::IsRecoveryStatusCurrent;
    Model->AttachView(); Model->SelectJob(A);
    const auto ItemResult = [](const FObject& Job) { return GetObject(GetArray(Job, TEXT("items"))[0]->AsObject(), TEXT("result")); };
    if (!TestTrue(TEXT("Fake job exposes a recovery result"), GetBool(Model->History().Selected, TEXT("recovery_required")))) return false;
    const auto Original = Model->History().Selected;
    const auto ServiceRevision = Jobs.GetRevision(), JobRevision = Jobs.GetJobRevision(A);
    auto Reads = Model->History().ResultReads;
    const auto DisplayRevision = Model->History().ResultRevision;
    TestFalse(TEXT("Existing journal is annotated as present"), GetBool(ItemResult(Original), TEXT("recovery_journal_missing")));
    for (int32 Index = 0; Index < 5; ++Index) Model->RefreshHistory();
    TestEqual(TEXT("Unchanged files do not reread the selected job"), Model->History().ResultReads, Reads);
    TestEqual(TEXT("Unchanged files do not recreate display text"), Model->History().ResultRevision, DisplayRevision);
    if (!TestTrue(TEXT("Delete only the external recovery file"), IFileManager::Get().Delete(*JournalA))) return false;
    Model->RefreshHistory();
    TestEqual(TEXT("File deletion refreshes the selected result without force"), Model->History().ResultReads, Reads + 1);
    const auto Missing = Model->History().Selected;
    TestTrue(TEXT("Deleted journal updates the missing flag"), GetBool(ItemResult(Missing), TEXT("recovery_journal_missing")));
    TestEqual(TEXT("Missing annotation adds one warning to the runner warning"), GetArray(ItemResult(Missing), TEXT("warnings")).Num(), 2);
    TestTrue(TEXT("Both formatted text and copied JSON contain the new warning"),
             Model->History().Body.Contains(TEXT("Recorded recovery journal is missing")) &&
             Model->History().Json.Contains(TEXT("Recorded recovery journal is missing")));
    TestFalse(TEXT("Refreshing does not mutate the prior snapshot"), GetBool(ItemResult(Original), TEXT("recovery_journal_missing")));
    TestEqual(TEXT("External invalidation keeps exactly one selected pin"), Jobs.GetJobPinCountForTesting(A), 1);
    Reads = Model->History().ResultReads;
    Model->RefreshHistory();
    TestEqual(TEXT("An unchanged missing file is cached too"), Model->History().ResultReads, Reads);
    if (!TestTrue(TEXT("Restore the journal"), FFileHelper::SaveStringToFile(TEXT("{}"), *JournalA))) return false;
    Model->RefreshHistory();
    TestEqual(TEXT("Restoration refreshes the result once"), Model->History().ResultReads, Reads + 1);
    TestFalse(TEXT("Restoration clears the missing flag"), GetBool(ItemResult(Model->History().Selected), TEXT("recovery_journal_missing")));
    TestEqual(TEXT("Restoration removes only the transient warning"), GetArray(ItemResult(Model->History().Selected), TEXT("warnings")).Num(), 1);
    TestTrue(TEXT("Restoration retains the runner warning"), Model->History().Body.Contains(TEXT("Keep the original runner warning.")));
    TestFalse(TEXT("Restoration clears the transient warning from displayed JSON"), Model->History().Json.Contains(TEXT("Recorded recovery journal is missing")));
    TestTrue(TEXT("Restoration does not mutate the detached missing snapshot"), GetBool(ItemResult(Missing), TEXT("recovery_journal_missing")));
    Model->SelectJob(B);
    Reads = Model->History().ResultReads;
    if (!TestTrue(TEXT("Change an unselected cached job's file"), IFileManager::Get().Delete(*JournalA))) return false;
    Model->RefreshHistory();
    TestEqual(TEXT("Unselected file changes do not reread the displayed job"), Model->History().ResultReads, Reads);
    Model->SelectJob(A);
    TestEqual(TEXT("Selecting an old cache entry rechecks its external dependency"), Model->History().ResultReads, Reads + 1);
    TestTrue(TEXT("Reselected cached job has the current missing flag"), GetBool(ItemResult(Model->History().Selected), TEXT("recovery_journal_missing")));
    TestEqual(TEXT("File changes do not change the service revision"), Jobs.GetRevision(), ServiceRevision);
    TestEqual(TEXT("File changes do not change the job revision"), Jobs.GetJobRevision(A), JobRevision);
    auto Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("job_id"), A);
    TestFalse(TEXT("Annotations never enter service-owned records"), ItemResult(Jobs.Status(Selector))->HasField(TEXT("recovery_journal_missing")));
    Model->DetachView();
    TestEqual(TEXT("Detach releases the selected pin after external refresh"), Jobs.GetJobPinCountForTesting(A), 0);
    Model->Shutdown();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXWorkspaceRecoveryCommandTest, "FBX_Exporter.Editor.Workspace.RecoveryCommand",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXWorkspaceRecoveryCommandTest::RunTest(const FString&)
{
    int32 Scans = 0;
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false, [&]() { ++Scans; });
    Model->HistoryMode(true, false);
    Model->HistoryPage(1);
    Model->RefreshRecoveryHistory();
    TestEqual(TEXT("The controller requests a diagnostic scan"), Scans, 1);
    TestTrue(TEXT("The same command selects the first diagnostic page"),
             Model->History().bDiagnostics && !Model->History().bArchive && Model->History().Offset == 0);
    const auto Revision = Model->History().Revision;
    Model->Shutdown();
    Model->RefreshRecoveryHistory();
    TestEqual(TEXT("Shutdown rejects later recovery commands"), Scans, 1);
    TestEqual(TEXT("Rejected commands preserve displayed history"), Model->History().Revision, Revision);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXWorkspaceContractTest, "FBX_Exporter.Editor.Workspace.DraftsAndSnapshots",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXWorkspaceContractTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false);
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    Model->Add(A);
    Model->Add(B);
    Model->Edit(A);
    const auto Before = JsonText(Model->CopyOptions(A));
    Model->Edit(B);
    Model->Edit(A);
    TestEqual(TEXT("Opening an editor never changes options"), JsonText(Model->CopyOptions(A)), Before);
    auto Token = Model->Token();
    Model->JsonDraft(Token, TEXT("{\"maps\":[]}"));
    auto Request = Model->BuildRequest();
    TestTrue(TEXT("Partial input commits before snapshot"), Request.IsValid());
    TestEqual(TEXT("Omitted scalar preserved"), Model->Number(TEXT("resolution")), 2048.0);
    TestFalse(TEXT("Explicit empty array is applied"), Model->HasArray(TEXT("maps")));
    const auto Stable = JsonText(Model->CopyOptions(A));
    Model->JsonDraft(Token, TEXT("{}"));
    Model->Flush(Token);
    TestEqual(TEXT("Empty patch is inert"), JsonText(Model->CopyOptions(A)), Stable);
    Model->JsonDraft(Token, TEXT("{\"resolution\":"));
    TestFalse(TEXT("Invalid draft blocks snapshot before focus loss"), Model->BuildRequest().IsValid());
    Model->Reset(A);
    Model->JsonDraft(Token, TEXT("{\"resolution\":4096}"));
    TestEqual(TEXT("Pre-reset token is rejected"), Model->Number(TEXT("resolution")), 2048.0);
    auto Frozen = Model->BuildRequest();
    Model->Edit({});
    auto Patch = MakeShared<FJsonObject>();
    Patch->SetNumberField(TEXT("resolution"), 4096);
    Model->Patch(Model->Token(), Patch);
    TestEqual(TEXT("Snapshot preserves original row settings"),
              GetNumber(GetObject(GetObject(Frozen, TEXT("overrides")), A), TEXT("resolution")), 2048.0);
    const auto Builds = Model->PresentationBuilds();
    Model->Targets();
    Model->Targets();
    TestEqual(TEXT("Same revision reuses presentation"), Model->PresentationBuilds(), Builds);
    TestTrue(TEXT("Snapshot does not require a profile ID"), !Frozen->HasField(TEXT("profile_id")));
    Model->JsonDraft(Model->Token(), TEXT("{\"resolution\":"));
    Model->Clear();
    TestFalse(TEXT("Clearing rows preserves an invalid default draft"), Model->Add(A));
    Model->JsonDraft(Model->Token(), TEXT("{}"));
    TestTrue(TEXT("Corrected defaults can seed rows after clear"), Model->Add(A));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXWorkspaceRecoveryDisplayTest,
                                 "FBX_Exporter.Editor.Workspace.ErrorsAndPlanInvalidation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXWorkspaceRecoveryDisplayTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false);
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    Model->Add(A);
    Model->Add(B);
    Model->JsonDraft(Model->TokenFor(A), TEXT("{\"resolution\":"));
    Model->JsonDraft(Model->TokenFor(B), TEXT("{\"maps\":"));
    TestFalse(TEXT("All malformed rows refuse the request"), Model->BuildRequest().IsValid());
    TestTrue(TEXT("Both scope errors are projected"), Model->Error().Contains(A) && Model->Error().Contains(B));
    Model->Reset(A);
    TestTrue(TEXT("Reset clears only the repaired scope"), !Model->Error().Contains(A) && Model->Error().Contains(B));
    Model->Edit(B);
    Model->ApplyProfile(TEXT("pbr"), true);
    TestTrue(TEXT("Applying a preset clears the remaining draft error"), Model->Error().IsEmpty());
    Model->JsonDraft(Model->TokenFor(A), TEXT("{\"resolution\":"));
    Model->BuildRequest();
    Model->Edit({});
    Model->Select(A, true);
    Model->ApplySelected();
    TestTrue(TEXT("Explicit bulk overwrite also clears the projected error"), Model->Error().IsEmpty());
    Model->SetPlan(TEXT("Previous inspected files"), TEXT("0 errors"));
    const auto PlanRevision = Model->Plan().Revision;
    auto Patch = MakeShared<FJsonObject>();
    Patch->SetNumberField(TEXT("resolution"), 512);
    Model->Patch(Model->TokenFor(A), Patch);
    TestTrue(TEXT("Options invalidate the plan revision"),
             Model->Plan().Revision > PlanRevision && Model->Plan().bNeedsInspection);
    TestEqual(TEXT("Plan summary and body are invalidated together"), Model->PlanSummary(), Model->PlanText());
    Model->SetPlan(TEXT("Fresh files"), TEXT("Fresh summary"));
    Model->SelectAll(false);
    TestFalse(TEXT("Check selection does not invalidate an output plan"), Model->Plan().bNeedsInspection);
    Model->Remove(A);
    TestTrue(TEXT("Target removal invalidates summary and body"),
             Model->Plan().bNeedsInspection && Model->PlanSummary() == Model->PlanText());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXWorkspaceProfileFlowTest, "FBX_Exporter.Editor.Workspace.ProfileOperations",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXWorkspaceProfileFlowTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false);
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    const auto Id = TEXT("workspace_test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const auto ProfileFile = FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Profiles") / (Id + TEXT(".json"));
    const auto ExportFile = FPaths::ProjectSavedDir() / (Id + TEXT(".json"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ProfileFile);
        IFileManager::Get().Delete(*ExportFile);
    };
    Model->Add(A);
    Model->Add(B);
    Model->Edit(A);
    Model->SetUserProfileId(Id);
    Model->SetProfileName(TEXT("Workspace regression fixture"));
    int32 Confirmations = 0;
    const auto Decline = [&]() {
        ++Confirmations;
        return false;
    };
    auto Result = Model->SaveProfile(Decline);
    if (!TestTrue(TEXT("A new profile saves through the controller"), GetBool(Result, TEXT("success"))))
        return false;
    TestEqual(TEXT("A new profile needs no replacement prompt"), Confirmations, 0);
    const auto Saved = JsonText(GetObject(Model->GetProfile(Id), TEXT("options")));
    auto Patch = MakeShared<FJsonObject>();
    Patch->SetNumberField(TEXT("resolution"), 512);
    Model->Patch(Model->Token(), Patch);
    const auto Edited = JsonText(Model->CopyOptions(A));
    const auto SelectedProfile = Model->ProfileId();
    Model->SetProfileName(TEXT(" \t\r\n "));
    const auto BeforeInvalid = Confirmations;
    TestFalse(TEXT("Whitespace-only names are rejected before asking for replacement"),
              GetBool(Model->SaveProfile(Decline), TEXT("success")));
    TestEqual(TEXT("Invalid names do not prompt"), Confirmations, BeforeInvalid);
    TestEqual(TEXT("Invalid saves preserve current row options"), JsonText(Model->CopyOptions(A)), Edited);
    TestEqual(TEXT("Invalid saves preserve the selected profile"), Model->ProfileId(), SelectedProfile);
    TestEqual(TEXT("Invalid saves preserve stored options"), JsonText(GetObject(Model->GetProfile(Id), TEXT("options"))), Saved);
    Model->SetProfileName(TEXT("Workspace regression fixture"));
    TestFalse(TEXT("Replacement can be cancelled"), Model->SaveProfile(Decline).IsValid());
    TestEqual(TEXT("Cancellation retains the stored profile"),
              JsonText(GetObject(Model->GetProfile(Id), TEXT("options"))), Saved);
    Result = Model->SaveProfile([&]() {
        Model->Edit(B);
        return true;
    });
    TestFalse(TEXT("Changing scope during confirmation rejects the pending save"), GetBool(Result, TEXT("success")));
    TestEqual(TEXT("A stale confirmation cannot replace the profile"),
              JsonText(GetObject(Model->GetProfile(Id), TEXT("options"))), Saved);
    Model->Edit(A);
    Result = Model->SaveProfile([]() { return true; });
    TestTrue(TEXT("Explicit replacement succeeds"), GetBool(Result, TEXT("success")));
    const auto Kept = JsonText(Model->CopyOptions(A));
    TestTrue(TEXT("Controller exports profile JSON"), GetBool(Model->ExportProfileFile(ExportFile), TEXT("success")));
    FString Exported;
    FFileHelper::LoadFileToString(Exported, *ExportFile);
    const auto Profile = Parse(Exported);
    TestTrue(TEXT("Export excludes transient metadata and approval"),
             Profile && !Profile->HasField(TEXT("builtin")) && !Profile->HasField(TEXT("success")) &&
                 !GetObject(Profile, TEXT("options"))->HasField(TEXT("overwrite")));
    TestTrue(TEXT("Deletion goes through the controller"), GetBool(Model->DeleteProfile(), TEXT("success")));
    TestEqual(TEXT("Deleted profile identity falls back"), Model->ProfileId(), FString(TEXT("pbr")));
    TestEqual(TEXT("Deleting a profile preserves explicit row options"), JsonText(Model->CopyOptions(A)), Kept);
    TestTrue(TEXT("Controller imports the exported profile"),
             GetBool(Model->ImportProfileFile(ExportFile, Decline), TEXT("success")));
    TestTrue(TEXT("Import does not silently apply options"),
             Model->ProfileId() == TEXT("pbr") && JsonText(Model->CopyOptions(A)) == Kept);
    TestFalse(TEXT("Malformed imported options are rejected"),
              GetBool(Model->ImportProfileText(TEXT("{\"options\":{\"resolution\":1}}"), Decline), TEXT("success")));
    // Both GUI entry points must ask even when the existing file cannot load.
    for (int32 Variant = 0; Variant < 2; ++Variant)
    {
        auto Unsupported = Clone(Profile); Unsupported->SetNumberField(TEXT("schema_version"), 2);
        const FString Original = Variant == 0 ? TEXT("{damaged profile") : JsonText(Unsupported);
        if (!TestTrue(TEXT("Unreadable GUI profile fixture written"), FFileHelper::SaveStringToFile(Original, *ProfileFile))) return false;
        const auto BeforeDecline = Confirmations;
        TestFalse(TEXT("Damaged-file save can be declined"), Model->SaveProfile(Decline).IsValid());
        TestFalse(TEXT("Damaged-file import can be declined"), Model->ImportProfileText(Exported, Decline).IsValid());
        TestEqual(TEXT("Save and import both prompt for damaged files"), Confirmations, BeforeDecline + 2);
        FString Retained; TestTrue(TEXT("Declined damaged file is retained"), FFileHelper::LoadFileToString(Retained, *ProfileFile));
        TestEqual(TEXT("Declined save/import preserve damaged bytes"), Retained, Original);
        TestEqual(TEXT("Declined save/import preserve workspace options"), JsonText(Model->CopyOptions(A)), Kept);
        Result = Model->SaveProfile([&]() { Model->Edit(B); return true; });
        TestFalse(TEXT("Damaged-file approval also rejects a stale scope"), GetBool(Result, TEXT("success")));
        TestTrue(TEXT("Stale confirmation leaves the file readable as raw text"), FFileHelper::LoadFileToString(Retained, *ProfileFile));
        TestEqual(TEXT("Stale confirmation preserves damaged bytes"), Retained, Original);
        Model->Edit(A);
        Result = Variant == 0 ? Model->SaveProfile([]() { return true; })
                              : Model->ImportProfileText(Exported, []() { return true; });
        TestTrue(TEXT("Explicit GUI approval repairs the profile"), GetBool(Result, TEXT("success")));
        TestTrue(TEXT("Repaired GUI profile can be loaded"), Model->GetProfile(Id).IsValid());
        TestEqual(TEXT("Repair preserves workspace options"), JsonText(Model->CopyOptions(A)), Kept);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXWorkspaceMapDraftTest, "FBX_Exporter.Editor.Workspace.MapsAccumulateInDrafts",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXWorkspaceMapDraftTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false);
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    Model->Add(A);
    Model->Add(B);
    Model->Edit(A);
    auto Token = Model->Token();
    Model->SetAllMaps(Token, true);
    const auto OriginalRoot = Model->String(TEXT("output_root"));
    const auto LastValid = JsonText(Model->CopyOptions(A));
    const auto OtherRow = JsonText(Model->CopyOptions(B));
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("output_root"), TEXT(""));
    Model->Patch(Token, Root);
    TestFalse(TEXT("An invalid path keeps a visible draft error"), Model->ScopeError(A).IsEmpty());
    TestTrue(TEXT("The first map edit is accepted into the draft"),
             Model->SetMapEnabled(Token, TEXT("BaseColor"), false));
    TestTrue(TEXT("The second map edit is accepted into the same draft"),
             Model->SetMapEnabled(Token, TEXT("Normal"), false));
    TestFalse(TEXT("A later edit cannot restore BaseColor"), Model->Contains(TEXT("maps"), TEXT("BaseColor")));
    TestFalse(TEXT("The Normal edit is also retained"), Model->Contains(TEXT("maps"), TEXT("Normal")));
    TestEqual(TEXT("Both edits accumulate"), GetArray(Model->CopyActiveEditorOptions(), TEXT("maps")).Num(), 4);
    TestEqual(TEXT("Invalid drafts never replace the last valid row"), JsonText(Model->CopyOptions(A)), LastValid);
    TestFalse(TEXT("The invalid root still blocks the batch"), Model->BuildRequest().IsValid());
    Model->SetAllMaps(Token, false);
    TestFalse(TEXT("Clear-all updates the pending map array"), Model->HasArray(TEXT("maps")));
    Model->SetMapEnabled(Token, TEXT("Mask"), true);
    Model->SetMapEnabled(Token, TEXT("Normal"), true);
    Model->SetMapEnabled(Token, TEXT("Normal"), true);
    TestEqual(TEXT("Individual enables accumulate without duplicates"),
              GetArray(Model->CopyActiveEditorOptions(), TEXT("maps")).Num(), 2);
    TestTrue(TEXT("Mask stays selected after another enable"), Model->Contains(TEXT("maps"), TEXT("Mask")));
    Root->SetStringField(TEXT("output_root"), OriginalRoot);
    Model->Patch(Token, Root);
    auto Request = Model->BuildRequest();
    if (!TestTrue(TEXT("Repairing only the path commits the accumulated maps"), Request.IsValid()))
        return false;
    TestEqual(TEXT("The request carries both chosen maps"),
              GetArray(GetObject(GetObject(Request, TEXT("overrides")), A), TEXT("maps")).Num(), 2);
    TestTrue(TEXT("Both accumulated choices survive normalization"),
             Model->Contains(TEXT("maps"), TEXT("Mask")) && Model->Contains(TEXT("maps"), TEXT("Normal")));
    TestFalse(TEXT("The cleared BaseColor does not return after path repair"),
              Model->Contains(TEXT("maps"), TEXT("BaseColor")));
    TestTrue(TEXT("Path correction clears its error"), Model->ScopeError(A).IsEmpty());

    // A malformed JSON draft is independent of typed edits and stays visible.
    const FString InvalidJson = TEXT("{\"resolution\":");
    Model->JsonDraft(Token, InvalidJson);
    Model->SetAllMaps(Token, true);
    Model->SetMapEnabled(Token, TEXT("BaseColor"), false);
    Model->SetMapEnabled(Token, TEXT("Normal"), false);
    TestEqual(TEXT("Map edits preserve malformed JSON text"), Model->RenderJson(A), InvalidJson);
    TestFalse(TEXT("Map edits cannot clear malformed JSON errors"), Model->BuildRequest().IsValid());
    TestTrue(TEXT("Map edits still accumulate beside a JSON syntax error"),
             !Model->Contains(TEXT("maps"), TEXT("BaseColor")) && !Model->Contains(TEXT("maps"), TEXT("Normal")));
    Model->JsonDraft(Token, TEXT("{}"));
    TestTrue(TEXT("Correcting JSON retains the typed map edits"), Model->BuildRequest().IsValid());
    TestFalse(TEXT("Corrected JSON does not restore the earlier maps"),
              Model->Contains(TEXT("maps"), TEXT("BaseColor")));

    // A valid JSON map patch may coexist with a separate invalid typed path.
    // Correcting that path must not replay the older JSON over later map edits.
    Root->SetStringField(TEXT("output_root"), TEXT(""));
    Model->Patch(Token, Root);
    const FString MapJson = TEXT("{\"maps\":[\"BaseColor\"]}");
    Model->JsonDraft(Token, MapJson);
    Model->SetMapEnabled(Token, TEXT("Normal"), true);
    TestTrue(TEXT("Map commands build on a valid JSON patch despite an unrelated path error"),
             Model->Contains(TEXT("maps"), TEXT("BaseColor")) && Model->Contains(TEXT("maps"), TEXT("Normal")));
    TestEqual(TEXT("The pending JSON text stays intact until the row can commit"), Model->RenderJson(A), MapJson);
    TestFalse(TEXT("The path error still refuses admission"), Model->BuildRequest().IsValid());
    Root->SetStringField(TEXT("output_root"), OriginalRoot);
    Model->Patch(Token, Root);
    TestTrue(TEXT("Path repair commits both the JSON base and later checkbox edit"), Model->BuildRequest().IsValid());
    TestTrue(TEXT("Older JSON cannot restore maps over a later checkbox command"),
             Model->Contains(TEXT("maps"), TEXT("BaseColor")) && Model->Contains(TEXT("maps"), TEXT("Normal")));
    TestEqual(TEXT("The merged result has exactly the two requested maps"),
              GetArray(Model->CopyOptions(A), TEXT("maps")).Num(), 2);

    const FString InvalidMaps = TEXT("{\"maps\":[\"NotAKnownMap\"]}");
    Model->JsonDraft(Token, InvalidMaps);
    Model->SetAllMaps(Token, false);
    TestFalse(TEXT("Typed map edits cannot mask a semantic JSON error"), Model->BuildRequest().IsValid());
    TestEqual(TEXT("Semantic JSON errors retain their raw text"), Model->RenderJson(A), InvalidMaps);
    Model->JsonDraft(Token, TEXT("{}"));
    TestTrue(TEXT("Explicit JSON correction accepts the typed empty map set"), Model->BuildRequest().IsValid());
    TestFalse(TEXT("The pending clear-all survives JSON correction"), Model->HasArray(TEXT("maps")));

    // The later checkbox action must win even before the JSON widget loses focus.
    Model->JsonDraft(Token, TEXT("{\"maps\":[\"BaseColor\"]}"));
    Model->SetMapEnabled(Token, TEXT("Normal"), true);
    TestTrue(TEXT("A valid pending JSON edit commits before the later map action"),
             Model->Contains(TEXT("maps"), TEXT("BaseColor")) && Model->Contains(TEXT("maps"), TEXT("Normal")));
    TestEqual(TEXT("Only the two explicit maps are selected"), GetArray(Model->CopyOptions(A), TEXT("maps")).Num(), 2);
    TestEqual(TEXT("Map changes never alter another row"), JsonText(Model->CopyOptions(B)), OtherRow);

    Model->Reset(A);
    const auto ResetOptions = JsonText(Model->CopyOptions(A));
    TestFalse(TEXT("Stale checkbox events are rejected"), Model->SetMapEnabled(Token, TEXT("BaseColor"), false));
    TestFalse(TEXT("Stale bulk map events are rejected"), Model->SetAllMaps(Token, false));
    TestEqual(TEXT("Stale events cannot alter the reset row"), JsonText(Model->CopyOptions(A)), ResetOptions);
    Model->Edit({});
    Model->SetAllMaps(Model->Token(), false);
    const auto Defaults = JsonText(Model->CopyActiveOptions());
    Model->Edit(A);
    Token = Model->Token();
    Model->SetAllMaps(Token, false);
    Model->SetMapEnabled(Token, TEXT("Mask"), true);
    TestEqual(TEXT("Row edits build their own map set"), GetArray(Model->CopyOptions(A), TEXT("maps")).Num(), 1);
    TestEqual(TEXT("Row map edits leave defaults independent"), JsonText(Model->CopyOptions({})), Defaults);
    return true;
}
#endif
