// Included once per plugin; these tests require no RHI or project content.
#if WITH_DEV_AUTOMATION_TESTS
#include "UnrealToolsRuntime.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

// UE registers tests by the C++ class name, independently of PrettyName. Expand
// the provider prefix before entering UE's #TClass/##TClass registration macro.
#define UNREAL_TOOLS_RUNTIME_JOIN_INNER(A, B) A##B
#define UNREAL_TOOLS_RUNTIME_JOIN(A, B) UNREAL_TOOLS_RUNTIME_JOIN_INNER(A, B)
#define UNREAL_TOOLS_RUNTIME_CLASS(Name) UNREAL_TOOLS_RUNTIME_JOIN(UNREAL_TOOLS_TEST_CLASS_PREFIX, Name)
#define UNREAL_TOOLS_RUNTIME_IMPLEMENT_INNER(ClassName, PrettyName, Flags) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, PrettyName, Flags)
#define UNREAL_TOOLS_RUNTIME_IMPLEMENT(Name, PrettyName, Flags) UNREAL_TOOLS_RUNTIME_IMPLEMENT_INNER(UNREAL_TOOLS_RUNTIME_CLASS(Name), PrettyName, Flags)

namespace
{
using namespace UnrealToolsRuntime;
class FRuntimeFakeRunner final : public IItemRunner
{
public:
    explicit FRuntimeFakeRunner(const FItemSpec& InSpec) : Spec(InSpec), Result(Ok()) {}
    TFunction<void()> ShutdownObserver;
    void Shutdown() override { if (ShutdownObserver) ShutdownObserver(); }
    EStepResult Step(bool bCancel, bool) override
    {
        if (bCancel && CanCancel()) { Result = UnrealToolsRuntime::Error(TEXT("Cancelled before commit."), TEXT("CANCELLED")); return EStepResult::Cancelled; }
        if (GetBool(Spec.Options, TEXT("fail")) && Phase == 1)
        { Result = UnrealToolsRuntime::Error(TEXT("Injected stage failure."), TEXT("TEST_FAILURE")); return EStepResult::Failed; }
        ++Phase;
        if (Phase == 3) Result->SetArrayField(TEXT("saved"), Strings({Spec.MeshPath + TEXT(".mock_output")}));
        return Phase == 3 ? EStepResult::Succeeded : EStepResult::Continue;
    }
    FString GetStage() const override { return Phase == 0 ? TEXT("validation") : Phase == 1 ? TEXT("capturing") : Phase == 2 ? TEXT("commit") : TEXT("done"); }
    bool CanCancel() const override { return Phase < 2; }
    bool IsCommitPhase() const override { return Phase == 2; }
    FObject GetResult() const override { return Result; }
private:
    FItemSpec Spec;
    FObject Result;
    int32 Phase = 0;
};
class FRuntimeWaitingRunner final : public IItemRunner
{
public:
    int32* Reads = nullptr;
    TFunction<void()> Observer;
    TFunction<void()> CommitObserver;
    FObject Result = Ok();
    int32 Phase = 0, Waits = 100;
    uint64 GetSnapshotRevision() const override { return static_cast<uint64>(Phase); }
    FObject GetResult() const override { if (Reads) ++*Reads; return Result; }
    FString GetStage() const override { return Phase == 0 ? TEXT("validation") : Phase == 1 ? TEXT("worker_pending") : Phase == 2 ? TEXT("commit") : TEXT("done"); }
    bool CanCancel() const override { return Phase < 2; }
    bool IsCommitPhase() const override { return Phase == 2; }
    EStepResult Step(bool bCancel, bool) override
    {
        if (Observer) Observer();
        if (bCancel && CanCancel()) { Result = UnrealToolsRuntime::Error(TEXT("Cancelled."), TEXT("CANCELLED")); ++Phase; return EStepResult::Cancelled; }
        if (Phase == 1 && Waits-- > 0) return EStepResult::Continue;
        if (Phase == 2 && CommitObserver) CommitObserver();
        ++Phase; return Phase == 3 ? EStepResult::Succeeded : EStepResult::Continue;
    }
};
struct FRuntimeFixture
{
    FString Name = FString(TEXT("UnrealToolsTests/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / Name);
    FProfileStore Profiles {TEXT("runtime_test"), Name};
    FObject Defaults = Parse(TEXT("{\"fail\":false}"));
    TUniquePtr<FJobService> Jobs;
    FRuntimeFixture() { Create(); }
    ~FRuntimeFixture()
    {
        Jobs.Reset();
        const FString SafeParent = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealToolsTests"));
        if (FPaths::IsUnderDirectory(Root, SafeParent)) IFileManager::Get().DeleteDirectory(*Root, false, true);
    }
    void Create()
    {
        Jobs = MakeUnique<FJobService>(TEXT("runtime_test"), Name, Profiles, Defaults,
            [](FObject& Options, FString& Message)
            { return ValidateSchema(Options, Parse(TEXT("{\"type\":\"object\",\"properties\":{\"fail\":{\"type\":\"boolean\"}},\"additionalProperties\":false}")), Message); },
            [this](const FItemSpec& Spec) { return TArray<FString>{Root / TEXT("Outputs") / FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(Spec.MeshPath))}; },
            [](const FItemSpec& Spec, const FString&) -> TUniquePtr<IItemRunner> { return MakeUnique<FRuntimeFakeRunner>(Spec); });
    }
    FObject Input(TArray<FString> Meshes = {TEXT("/Engine/BasicShapes/Cube.Cube")}) const
    {
        FObject Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        Object->SetArrayField(TEXT("mesh_paths"), Strings(Meshes));
        return Object;
    }
    static FObject ById(const FString& Id)
    { FObject Object = MakeShared<FJsonObject>(); Object->SetStringField(TEXT("job_id"), Id); return Object; }
};
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeShutdownOrderTest, UNREAL_TOOLS_TEST_PREFIX ".ShutdownPersistenceOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeShutdownOrderTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    Fixture.Jobs.Reset();
    bool bWorkerJoined = false, bPersistedBeforeJoin = false;
    FString JobId;
    Fixture.Jobs = MakeUnique<FJobService>(TEXT("runtime_test"), Fixture.Name, Fixture.Profiles, Fixture.Defaults,
        nullptr, nullptr,
        [&](const FItemSpec& Spec, const FString&) -> TUniquePtr<IItemRunner>
        {
            auto Runner = MakeUnique<FRuntimeFakeRunner>(Spec);
            Runner->ShutdownObserver = [&]()
            {
                bWorkerJoined = true;
                FString Text;
                FFileHelper::LoadFileToString(Text, *(Fixture.Root / TEXT("Jobs") / (JobId + TEXT(".json"))));
                bPersistedBeforeJoin = GetString(Parse(Text), TEXT("state")) == TEXT("interrupted");
            };
            return Runner;
        });
    JobId = GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("job_id"));
    Fixture.Jobs->AdvanceOneStepForTesting();
    Fixture.Jobs->Shutdown();
    TestTrue(TEXT("Worker shutdown called"), bWorkerJoined);
    TestTrue(TEXT("Interrupted record exists before worker join/resource release"), bPersistedBeforeJoin);
    const FObject Interrupted = Fixture.Jobs->Status(FRuntimeFixture::ById(JobId)); FDateTime Finished;
    TestTrue(TEXT("Shutdown interruption has a completion timestamp"), FDateTime::ParseIso8601(*GetString(Interrupted, TEXT("finished_at")), Finished));
    TestEqual(TEXT("Interrupted target explains why work stopped"), GetString(GetArray(Interrupted, TEXT("items"))[0]->AsObject(), TEXT("reason")), FString(TEXT("editor_interrupted")));
    TestFalse(TEXT("Shutdown blocks new admission"), GetBool(Fixture.Jobs->Start(Fixture.Input()), TEXT("success")));
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeAdmissionTest, UNREAL_TOOLS_TEST_PREFIX ".AdmissionPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeAdmissionTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    bool bAllowed = false;
    Fixture.Jobs->SetAdmissionValidator([&bAllowed](const TArray<FItemSpec>& Specs, FString& Message)
    {
        if (Specs.Num() != 1 || !bAllowed) { Message = TEXT("Editor execution is blocked."); return false; }
        return true;
    });
    FObject Input = Fixture.Input();
    TestFalse(TEXT("Blocked policy rejects admission"), GetBool(Fixture.Jobs->Start(Input), TEXT("success")));
    TestEqual(TEXT("Rejection creates no durable job"), Fixture.Jobs->GetJobs().Num(), 0);
    bAllowed = true;
    const FObject Accepted = Fixture.Jobs->Start(Input);
    TestTrue(TEXT("Allowed policy accepts"), GetBool(Accepted, TEXT("success")));
    bAllowed = false;
    TestEqual(TEXT("Replay works after policy changes"), GetString(Fixture.Jobs->Start(Input), TEXT("job_id")), GetString(Accepted, TEXT("job_id")));
    bAllowed = true;
    const FObject ProfileSave = Parse(TEXT(R"({"action":"save","profile":{"schema_version":1,"provider_id":"runtime_test","id":"replay_profile","display_name":"Replay profile","options":{"fail":false}}})"));
    TestTrue(TEXT("Replay fixture saves a valid user profile"), GetBool(Fixture.Profiles.Handle(ProfileSave), TEXT("success")));
    FObject ProfileRequest = Fixture.Input({TEXT("/Game/ProfileReplay.ProfileReplay")}); ProfileRequest->SetStringField(TEXT("profile_id"), TEXT("replay_profile"));
    const FObject ProfileAccepted = Fixture.Jobs->Start(ProfileRequest); const FString ProfileJobId = GetString(ProfileAccepted, TEXT("job_id"));
    TestTrue(TEXT("Profile request is durably accepted before profile deletion"), GetBool(ProfileAccepted, TEXT("success")));
    TestTrue(TEXT("Replay fixture deletes its user profile"), GetBool(Fixture.Profiles.Handle(Parse(TEXT("{\"action\":\"delete\",\"id\":\"replay_profile\"}"))), TEXT("success")));
    TestFalse(TEXT("Deleted profile is actually absent"), Fixture.Profiles.Get(TEXT("replay_profile")).IsValid());
    bAllowed = false;
    const FObject Replay = Fixture.Jobs->Start(Clone(ProfileRequest));
    TestTrue(TEXT("Retained UUID bypasses deleted profile and changed policy: ") + JsonText(Replay), GetBool(Replay, TEXT("success")));
    TestEqual(TEXT("Deleted profile replay returns the original job"), GetString(Replay, TEXT("job_id")), ProfileJobId);
    // Finish the two accepted fake jobs so a new UUID cannot be rejected merely
    // because it shares the retained request's output reservation.
    for (int32 Step = 0; Step < 6; ++Step) Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Accepted job uses its stored options after profile deletion"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(ProfileJobId)), TEXT("state")), FString(TEXT("succeeded")));
    bAllowed = true;
    FObject NewRequest = Clone(ProfileRequest); NewRequest->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    const int32 BeforeJobs = Fixture.Jobs->GetJobs().Num(); const FObject MissingProfile = Fixture.Jobs->Start(NewRequest);
    TestFalse(TEXT("A new UUID must resolve the now-missing profile: ") + JsonText(MissingProfile), GetBool(MissingProfile, TEXT("success")));
    TestEqual(TEXT("Missing-profile new request keeps its structured argument error"), GetString(MissingProfile, TEXT("error_code")), FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("New request rejection identifies the deleted profile"), GetString(MissingProfile, TEXT("error")).Contains(TEXT("Profile not found")));
    TestEqual(TEXT("Missing-profile new request creates no job"), Fixture.Jobs->GetJobs().Num(), BeforeJobs);
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeSchemaTest, UNREAL_TOOLS_TEST_PREFIX ".SchemaValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeSchemaTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FString Message;
    FObject Schema = Parse(TEXT(R"({"type":"object","required":["limit"],"properties":{"limit":{"type":"integer","minimum":1,"maximum":200},"names":{"type":"array","items":{"type":"string"},"uniqueItems":true}},"additionalProperties":false})"));
    TestTrue(TEXT("Valid object"), ValidateSchema(Parse(TEXT("{\"limit\":2,\"names\":[\"A\",\"B\"]}")), Schema, Message));
    for (const TCHAR* Invalid : {TEXT("{}"), TEXT("{\"limit\":2.5}"), TEXT("{\"limit\":201}"), TEXT("{\"limit\":true}"), TEXT("{\"limit\":2,\"typo\":3}"), TEXT("{\"limit\":2,\"names\":[\"A\",\"A\"]}")})
        TestFalse(TEXT("Reject malformed input"), ValidateSchema(Parse(Invalid), Schema, Message));
    TestTrue(TEXT("Success matches output schema"), ValidateSchema(Ok(), OutputSchema(), Message));
    TestTrue(TEXT("Error matches output schema"), ValidateSchema(UnrealToolsRuntime::Error(TEXT("test")), OutputSchema(), Message));
    TestFalse(TEXT("Ambiguous output rejected"), ValidateSchema(Parse(TEXT("{\"success\":false}")), OutputSchema(), Message));
    TArray<FAutomationTestInfo> Registered;
    auto& Framework = FAutomationTestFramework::Get(); Framework.GetValidTestNames(Registered);
    for (const auto& Provider : {TPair<FString, FString>{TEXT("CubeShot"), TEXT("FCubeShot")}, TPair<FString, FString>{TEXT("FBX_Exporter"), TEXT("FFBXExporter")}})
    {
        if (!FModuleManager::Get().IsModuleLoaded(*Provider.Key)) continue;
        const FString PrettyPrefix = Provider.Key + TEXT(".Editor.Runtime."); int32 Count = 0;
        for (const auto& Info : Registered) if (Info.GetFullTestPath().StartsWith(PrettyPrefix)) ++Count;
        TestEqual(PrettyPrefix + TEXT("all shared cases are registered"), Count, 16);
        for (const TCHAR* Suffix : {TEXT("RuntimeShutdownOrderTest"), TEXT("RuntimeAdmissionTest"), TEXT("RuntimeSchemaTest"), TEXT("RuntimeProfileTest"), TEXT("RuntimeIdempotencyTest"), TEXT("RuntimeFailureCancelTest"), TEXT("RuntimeRecoveryTest"), TEXT("RuntimeControlDurabilityTest"), TEXT("RuntimeWaitingCheckpointTest"), TEXT("RuntimeTerminalWriteTest"), TEXT("RuntimeDiagnosticRetryTest"), TEXT("RuntimeHistoryRetentionTest"), TEXT("RuntimePrepareAndInvalidSchemaTest"), TEXT("RuntimeRepairedGuardTest"), TEXT("RuntimeMaintenanceRevalidationTest"), TEXT("RuntimeFrameSchedulerTest")})
        {
            const FString ClassName = Provider.Value + Suffix;
            TestTrue(TEXT("Provider-specific engine registry key: ") + ClassName, Framework.ContainsTest(ClassName));
        }
    }
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeProfileTest, UNREAL_TOOLS_TEST_PREFIX ".Profiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeProfileTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    Fixture.Profiles.AddBuiltin(TEXT("default"), TEXT("Default"), Fixture.Defaults);
    FObject Save = Parse(TEXT(R"({"action":"save","profile":{"schema_version":1,"provider_id":"runtime_test","id":"user","display_name":"User","options":{"fail":false}}})"));
    TestTrue(TEXT("Save user profile"), GetBool(Fixture.Profiles.Handle(Save), TEXT("success")));
    TestFalse(TEXT("Existing profile requires explicit overwrite"), GetBool(Fixture.Profiles.Handle(Save), TEXT("success")));
    Save->SetBoolField(TEXT("overwrite"), true);
    TestTrue(TEXT("Explicit overwrite succeeds"), GetBool(Fixture.Profiles.Handle(Save), TEXT("success")));
    GetObject(GetObject(Save, TEXT("profile")), TEXT("options"))->SetBoolField(TEXT("overwrite"), true);
    TestFalse(TEXT("Approval cannot be stored"), GetBool(Fixture.Profiles.Handle(Save), TEXT("success")));
    TestFalse(TEXT("Builtin deletion rejected"), GetBool(Fixture.Profiles.Handle(Parse(TEXT("{\"action\":\"delete\",\"id\":\"default\"}"))), TEXT("success")));
    TestFalse(TEXT("Traversal rejected"), Fixture.Profiles.Get(TEXT("../outside")).IsValid());
    TestEqual(TEXT("Built-in and user listed"), Fixture.Profiles.GetAll().Num(), 2);
    Fixture.Profiles.SetValidator([](FObject& Options, FString&) { Options = Parse(TEXT("{\"fail\":false}")); return true; });
    FObject Replaced = Parse(TEXT(R"({"action":"save","profile":{"schema_version":1,"provider_id":"runtime_test","id":"normalized","display_name":"Normalized","options":{}}})"));
    TestTrue(TEXT("Replacing validator pointer saves normalized options"), GetBool(Fixture.Profiles.Handle(Replaced), TEXT("success")));
    TestTrue(TEXT("Validator replacement is written back into the profile"), GetObject(Fixture.Profiles.Get(TEXT("normalized")), TEXT("options"))->HasTypedField<EJson::Boolean>(TEXT("fail")));
    Fixture.Profiles.SetValidator([](FObject& Options, FString& Message)
    { return ValidateSchema(Options, Parse(TEXT(R"({"type":"object","properties":{"fail":{"type":"boolean"}},"additionalProperties":false})")), Message); });
    const FObject Typed = Parse(TEXT(R"({"schema_version":1,"provider_id":"runtime_test","id":"typed_read","display_name":"Typed read","options":{"fail":false}})"));
    auto ReadProbe = [&](const FObject& Candidate, bool Expected, const FString& Label)
    {
        // Each read-validation example is independent. Repeated atomic replacement
        // of one file is already covered by the explicit overwrite tests above.
        const FString ProbeId = TEXT("typed_read_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        const FString TypedFile = Fixture.Root / TEXT("Profiles") / (ProbeId + TEXT(".json"));
        const FObject Probe = Clone(Candidate); Probe->SetStringField(TEXT("id"), ProbeId);
        FString Message, Before, After;
        const bool bWritten = AtomicJson(TypedFile, Probe, Message);
        if (!bWritten)
        {
            AddError(Label + TEXT(" fixture preparation failed at ") + TypedFile + TEXT(": ") + Message);
            return;
        }
        TestTrue(Label + TEXT(" fixture read before validation: ") + TypedFile, FFileHelper::LoadFileToString(Before, *TypedFile));
        TestTrue(Label + TEXT(" fixture uses a valid profile ID: ") + ProbeId, ProfileIdValid(ProbeId));
        TestEqual(Label + TEXT(" stored ID matches the fixture filename"), GetString(Parse(Before), TEXT("id")), FPaths::GetBaseFilename(TypedFile));
        FObject ReadRequest = MakeShared<FJsonObject>(); ReadRequest->SetStringField(TEXT("action"), TEXT("get")); ReadRequest->SetStringField(TEXT("id"), ProbeId);
        const FObject ReadResult = Fixture.Profiles.Handle(ReadRequest);
        TestEqual(Label + TEXT(" typed read result at ") + TypedFile + TEXT("; native response: ") + JsonText(ReadResult),
            GetBool(ReadResult, TEXT("success")) && GetObject(ReadResult, TEXT("profile")).IsValid(), Expected);
        TestTrue(Label + TEXT(" fixture read after validation: ") + TypedFile, FFileHelper::LoadFileToString(After, *TypedFile));
        TestEqual(Label + TEXT(" read preserves stored bytes"), After, Before);
    };
    ReadProbe(Typed, true, TEXT("Valid user profile"));
    FObject Invalid = Clone(Typed); Invalid->SetStringField(TEXT("options"), TEXT("not an object")); ReadProbe(Invalid, false, TEXT("Malformed options metadata"));
    Invalid = Clone(Typed); Invalid->SetNumberField(TEXT("display_name"), 7); ReadProbe(Invalid, false, TEXT("Malformed display_name metadata"));
    Invalid = Clone(Typed); GetObject(Invalid, TEXT("options"))->SetNumberField(TEXT("fail"), 3); ReadProbe(Invalid, false, TEXT("Invalid provider option type"));
    Invalid = Clone(Typed); GetObject(Invalid, TEXT("options"))->SetBoolField(TEXT("overwrite"), true); ReadProbe(Invalid, false, TEXT("Stored overwrite approval"));
    Invalid = Clone(Typed); Invalid->SetNumberField(TEXT("schema_version"), 2); ReadProbe(Invalid, false, TEXT("Unsupported profile version"));
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeIdempotencyTest, UNREAL_TOOLS_TEST_PREFIX ".IdempotencyAndReservations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeIdempotencyTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    FObject Input = Fixture.Input(), Accepted = Fixture.Jobs->Start(Input);
    const FString Id = GetString(Accepted, TEXT("job_id"));
    TestTrue(TEXT("Accepted"), GetBool(Accepted, TEXT("success")));
    TestEqual(TEXT("Same request returns same id"), GetString(Fixture.Jobs->Start(Clone(Input)), TEXT("job_id")), Id);
    FObject Changed = Clone(Input); Changed->SetBoolField(TEXT("overwrite"), true);
    TestEqual(TEXT("Different request cannot reuse UUID"), GetString(Fixture.Jobs->Start(Changed), TEXT("error_code")), TEXT("REQUEST_CONFLICT"));
    TestEqual(TEXT("Output reserved while queued"), GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("error_code")), TEXT("OUTPUT_CONFLICT"));
    Fixture.Jobs->AdvanceOneStepForTesting(); Fixture.Jobs->AdvanceOneStepForTesting(); Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Three explicit stages complete"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), TEXT("succeeded"));
    TestTrue(TEXT("Reservation released on success"), GetBool(Fixture.Jobs->Start(Fixture.Input()), TEXT("success")));
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeFailureCancelTest, UNREAL_TOOLS_TEST_PREFIX ".FailFastAndCancelBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeFailureCancelTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    FObject Input = Fixture.Input({TEXT("/Game/A.A"), TEXT("/Game/B.B")});
    Input->SetObjectField(TEXT("options"), Parse(TEXT("{\"fail\":true}")));
    FString Id = GetString(Fixture.Jobs->Start(Input), TEXT("job_id"));
    Fixture.Jobs->AdvanceOneStepForTesting(); Fixture.Jobs->AdvanceOneStepForTesting();
    FObject Job = Fixture.Jobs->Status(FRuntimeFixture::ById(Id));
    TestEqual(TEXT("Failure stops job"), GetString(Job, TEXT("state")), TEXT("failed"));
    FDateTime Finished;
    TestTrue(TEXT("Failure has a completion timestamp"), FDateTime::ParseIso8601(*GetString(Job, TEXT("finished_at")), Finished));
    TestEqual(TEXT("Following target skipped"), GetString(GetArray(Job, TEXT("items"))[1]->AsObject(), TEXT("state")), TEXT("skipped"));
    Input = Fixture.Input(); Id = GetString(Fixture.Jobs->Start(Input), TEXT("job_id"));
    Fixture.Jobs->AdvanceOneStepForTesting();
    FObject Control = FRuntimeFixture::ById(Id); Control->SetStringField(TEXT("action"), TEXT("cancel"));
    Fixture.Jobs->Control(Control); Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Capture boundary cancellation"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), TEXT("cancelled"));
    TestTrue(TEXT("Cancellation has a completion timestamp"), FDateTime::ParseIso8601(*GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("finished_at")), Finished));
    Input = Fixture.Input(); Id = GetString(Fixture.Jobs->Start(Input), TEXT("job_id"));
    Fixture.Jobs->AdvanceOneStepForTesting(); Fixture.Jobs->AdvanceOneStepForTesting();
    Control = FRuntimeFixture::ById(Id); Control->SetStringField(TEXT("action"), TEXT("cancel"));
    Fixture.Jobs->Control(Control); Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Last commit finishes despite late cancellation"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), TEXT("succeeded"));
    TestTrue(TEXT("Success has a completion timestamp"), FDateTime::ParseIso8601(*GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("finished_at")), Finished));
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeRecoveryTest, UNREAL_TOOLS_TEST_PREFIX ".InterruptedRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeRecoveryTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    FObject Input = Fixture.Input(), Record = Fixture.Jobs->Start(Input);
    const FString Id = GetString(Record, TEXT("job_id"));
    Fixture.Jobs.Reset();
    Record->RemoveField(TEXT("success"));
    Record->SetStringField(TEXT("state"), TEXT("running"));
    FObject Item = GetArray(Record, TEXT("items"))[0]->AsObject();
    Item->SetStringField(TEXT("state"), TEXT("running"));
    Item->SetBoolField(TEXT("commit_pending"), true);
    FString Message;
    TestTrue(TEXT("Simulate journal at crash boundary"), AtomicJson(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json")), Record, Message));
    Fixture.Create();
    FObject Loaded = Fixture.Jobs->Status(FRuntimeFixture::ById(Id));
    TestEqual(TEXT("Previous job interrupted"), GetString(Loaded, TEXT("state")), TEXT("interrupted"));
    TestEqual(TEXT("Restarted target records an unexecuted reason"), GetString(GetArray(Loaded, TEXT("items"))[0]->AsObject(), TEXT("reason")), FString(TEXT("editor_restarted")));
    TestTrue(TEXT("Output reuse blocked pending review"), GetBool(Loaded, TEXT("recovery_required")));
    TestEqual(TEXT("Cannot overwrite recovery output"), GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("error_code")), TEXT("OUTPUT_CONFLICT"));
    FObject Control = FRuntimeFixture::ById(Id); Control->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
    Fixture.Jobs->SetRecoveryAcknowledger([](const FObject&, FString& Error) { Error = TEXT("Injected acknowledgement failure."); return false; });
    TestFalse(TEXT("Domain journal must acknowledge first"), GetBool(Fixture.Jobs->Control(Control), TEXT("success")));
    Fixture.Jobs->SetRecoveryAcknowledger([](const FObject&, FString&) { return true; });
    TestTrue(TEXT("Explicit review releases block"), GetBool(Fixture.Jobs->Control(Control), TEXT("success")));
    TestTrue(TEXT("New work permitted after acknowledgement"), GetBool(Fixture.Jobs->Start(Fixture.Input()), TEXT("success")));
    return true;
}
UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeControlDurabilityTest, UNREAL_TOOLS_TEST_PREFIX ".ControlDurability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeControlDurabilityTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    const FString Id = GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("job_id"));
    FObject Cancel = FRuntimeFixture::ById(Id); Cancel->SetStringField(TEXT("action"), TEXT("cancel"));
    Fixture.Jobs->SetPersistGateForTesting([](const FObject& Job, FString& Message)
    { if (GetBool(Job, TEXT("cancel_requested"))) { Message = TEXT("Injected control write failure."); return false; } return true; });
    TestEqual(TEXT("Failed control preserves error code"), GetString(Fixture.Jobs->Control(Cancel), TEXT("error_code")), FString(TEXT("IO_ERROR")));
    TestFalse(TEXT("Failed control does not change live cancellation"), GetBool(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("cancel_requested")));
    FString Text; FFileHelper::LoadFileToString(Text, *(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json"))));
    TestFalse(TEXT("Failed control does not change disk cancellation"), GetBool(Parse(Text), TEXT("cancel_requested")));
    FItemSpec Spec; Spec.MeshPath = TEXT("/Game/A.A"); Spec.Options = Fixture.Defaults;
    TestEqual(TEXT("Structured async submission retains BUSY"), GetString(Fixture.Jobs->StartLegacyStructured(Spec, [](FObject) {}), TEXT("error_code")), FString(TEXT("BUSY")));
    Fixture.Jobs->SetPersistGateForTesting(nullptr);
    TestTrue(TEXT("Same cancel retry succeeds"), GetBool(Fixture.Jobs->Control(Cancel), TEXT("success")));
    Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Retry cancellation is effective"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), FString(TEXT("cancelled")));
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeWaitingCheckpointTest, UNREAL_TOOLS_TEST_PREFIX ".WaitingCheckpointAndReentrancy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeWaitingCheckpointTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture; Fixture.Jobs.Reset();
    int32 Writes = 0, Reads = 0, Events = 0; bool bCommitDurable = false; FString Id;
    Fixture.Jobs = MakeUnique<FJobService>(TEXT("runtime_test"), Fixture.Name, Fixture.Profiles, Fixture.Defaults, nullptr, nullptr,
        [&](const FItemSpec&, const FString&) -> TUniquePtr<IItemRunner>
        {
            auto Runner = MakeUnique<FRuntimeWaitingRunner>(); Runner->Reads = &Reads;
            Runner->CommitObserver = [&]() { FString Text; FFileHelper::LoadFileToString(Text, *(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json")))); const auto Items = GetArray(Parse(Text), TEXT("items")); bCommitDurable = !Items.IsEmpty() && GetBool(Items[0]->AsObject(), TEXT("commit_pending")); };
            return Runner;
        });
    Fixture.Jobs->SetPersistGateForTesting([&](const FObject&, FString&) { ++Writes; return true; });
    Fixture.Jobs->OnChanged.AddLambda([&]() { ++Events; });
    Id = GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("job_id")); Fixture.Jobs->AdvanceOneStepForTesting();
    const int32 BeforeWrites = Writes, BeforeReads = Reads;
    const int32 BeforeBuilds = Fixture.Jobs->GetSnapshotBuildCountForTesting(), BeforeEvents = Events;
    for (int32 Index = 0; Index < 100; ++Index) Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Stable worker wait does not write journals"), Writes, BeforeWrites);
    TestEqual(TEXT("Stable runner revision does not rebuild result"), Reads, BeforeReads);
    TestEqual(TEXT("Stable worker wait does not clone/hash result snapshots"), Fixture.Jobs->GetSnapshotBuildCountForTesting(), BeforeBuilds);
    TestEqual(TEXT("Stable worker wait does not broadcast UI rebuilds"), Events, BeforeEvents);
    Fixture.Jobs->AdvanceOneStepForTesting(); Fixture.Jobs->AdvanceOneStepForTesting();
    TestTrue(TEXT("Commit Step observes a durable precommit marker"), bCommitDurable);
    Fixture.Jobs.Reset();
    bool bRequested = false; const FString Marker = TEXT("post_control_step_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Fixture.Jobs = MakeUnique<FJobService>(TEXT("runtime_test"), Fixture.Name, Fixture.Profiles, Fixture.Defaults, nullptr, nullptr,
        [&](const FItemSpec&, const FString& JobId) -> TUniquePtr<IItemRunner>
        {
            auto Runner = MakeUnique<FRuntimeWaitingRunner>();
            const FObject StepResult = Runner->Result;
            Runner->Observer = [&, JobId, StepResult]()
            {
                if (!bRequested)
                {
                    bRequested = true;
                    auto Cancel = FRuntimeFixture::ById(JobId); Cancel->SetStringField(TEXT("action"), TEXT("cancel"));
                    TestTrue(TEXT("Reentrant control succeeds before the stage result changes"), GetBool(Fixture.Jobs->Control(Cancel), TEXT("success")));
                    StepResult->SetStringField(TEXT("reentrant_step_marker"), Marker);
                }
            };
            return Runner;
        });
    Id = GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("job_id")); Fixture.Jobs->AdvanceOneStepForTesting();
    TestTrue(TEXT("Reentrant control survives post-Step item updates"), GetBool(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("cancel_requested")));
    FString Durable;
    if (!TestTrue(TEXT("Read the post-Step durable reentrant snapshot"), FFileHelper::LoadFileToString(Durable, *(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json")))))) return false;
    for (const FObject& Snapshot : {Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), Parse(Durable)})
    {
        const auto SnapshotItems = GetArray(Snapshot, TEXT("items"));
        if (!TestEqual(TEXT("Reentrant snapshot keeps one original item"), SnapshotItems.Num(), 1)) return false;
        const FObject Item = SnapshotItems[0]->AsObject();
        TestTrue(TEXT("Live/durable snapshot keeps reentrant cancellation alongside its new result"), GetBool(Snapshot, TEXT("cancel_requested")));
        TestEqual(TEXT("Live/durable job stage advances after reentrant control"), GetString(Snapshot, TEXT("stage")), FString(TEXT("worker_pending")));
        TestEqual(TEXT("Live/durable item stage advances after reentrant control"), GetString(Item, TEXT("stage")), FString(TEXT("worker_pending")));
        TestEqual(TEXT("Live/durable stage result retains its unique post-control marker"), GetString(GetObject(Item, TEXT("result")), TEXT("reentrant_step_marker")), Marker);
    }
    Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Reentrant cancel reaches terminal state"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), FString(TEXT("cancelled")));
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeFrameSchedulerTest, UNREAL_TOOLS_TEST_PREFIX ".FrameDriverSeparatesCoreTicker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeFrameSchedulerTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture; Fixture.Jobs.Reset();
    FTSTicker StreamingTicker;
    int32 Steps = 0, StreamingPumps = 0, Completions = 0;
    bool bInCoreTickerCallback = false, bInStreamingTicker = false, bUnsafeStep = false;
    StreamingTicker.AddTicker(FTickerDelegate::CreateLambda([&](float)
    {
        TGuardValue<bool> Guard(bInStreamingTicker, true); ++StreamingPumps;
        Fixture.Jobs->AdvanceMaintenanceTickerForTesting();
        return true;
    }));
    Fixture.Jobs = MakeUnique<FJobService>(TEXT("runtime_test"), Fixture.Name, Fixture.Profiles, Fixture.Defaults, nullptr, nullptr,
        [&](const FItemSpec&, const FString&) -> TUniquePtr<IItemRunner>
        {
            auto Runner = MakeUnique<FRuntimeWaitingRunner>(); Runner->Waits = 0;
            Runner->Observer = [&]()
            {
                ++Steps; bUnsafeStep |= bInCoreTickerCallback || bInStreamingTicker;
                // Model the engine baking wait without recursively ticking the
                // global CoreTicker inside the automation framework itself.
                StreamingTicker.Tick(0.0f);
            };
            return Runner;
        });
    TestTrue(TEXT("Frame driver owns a removable EndFrame delegate"), FCoreDelegates::OnEndFrame.IsBoundToObject(Fixture.Jobs.Get()));
    FItemSpec Spec; Spec.MeshPath = TEXT("/Game/FrameDriver.FrameDriver"); Spec.Options = Fixture.Defaults;
    const FObject Accepted = Fixture.Jobs->StartLegacyStructured(Spec, [&](FObject Result)
    { ++Completions; TestTrue(TEXT("Scheduled legacy completion retains native success"), GetBool(Result, TEXT("success"))); });
    const FString Id = GetString(Accepted, TEXT("job_id"));
    TestTrue(TEXT("Legacy async admission is still busy before execution"), Fixture.Jobs->IsBusy());
    TestEqual(TEXT("Retained request replay remains available while the frame is pending"), GetString(Fixture.Jobs->Start(Clone(GetObject(Accepted, TEXT("request")))), TEXT("job_id")), Id);
    for (int32 Index = 0; Index < 5; ++Index)
    {
        TGuardValue<bool> Guard(bInCoreTickerCallback, true); Fixture.Jobs->AdvanceMaintenanceTickerForTesting();
    }
    TestEqual(TEXT("CoreTicker callbacks never construct or step a runner"), Steps, 0);
    TestEqual(TEXT("CoreTicker callbacks leave the admitted job queued"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), FString(TEXT("queued")));
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const int32 Before = Steps; Fixture.Jobs->AdvanceFrameForTesting();
        TestEqual(TEXT("One frame callback performs one runner step"), Steps, Before + 1);
        TGuardValue<bool> Guard(bInCoreTickerCallback, true); Fixture.Jobs->AdvanceMaintenanceTickerForTesting();
        TestEqual(TEXT("Interleaved CoreTicker callback performs no additional runner step"), Steps, Before + 1);
    }
    TestFalse(TEXT("Runner execution never occurs within the simulated Core/streaming ticker callback"), bUnsafeStep);
    TestEqual(TEXT("Streaming wait safely pumps a separate ticker for every step"), StreamingPumps, 3);
    TestEqual(TEXT("Frame-driven legacy job succeeds"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("state")), FString(TEXT("succeeded")));
    TestEqual(TEXT("Frame-driven async completion is delivered exactly once"), Completions, 1);
    Fixture.Jobs->AdvanceFrameForTesting(); TestEqual(TEXT("Idle frame cannot repeat completion"), Completions, 1);
    const FObject Cancelled = Fixture.Jobs->StartLegacyStructured(Spec, [&](FObject) { ++Completions; });
    const FString CancelledId = GetString(Cancelled, TEXT("job_id"));
    FObject Cancel = FRuntimeFixture::ById(CancelledId); Cancel->SetStringField(TEXT("action"), TEXT("cancel"));
    TestTrue(TEXT("Queued cancellation persists before its execution frame"), GetBool(Fixture.Jobs->Control(Cancel), TEXT("success")));
    Fixture.Jobs->AdvanceFrameForTesting();
    TestEqual(TEXT("Frame driver respects queued cancellation"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(CancelledId)), TEXT("state")), FString(TEXT("cancelled")));
    TestEqual(TEXT("Cancelled-before-start job performs no runner step"), Steps, 3);
    TestEqual(TEXT("Cancellation callback is also delivered once"), Completions, 2);
    Fixture.Jobs->Shutdown();
    TestFalse(TEXT("Shutdown removes the EndFrame delegate before releasing runner resources"), FCoreDelegates::OnEndFrame.IsBoundToObject(Fixture.Jobs.Get()));
    Fixture.Jobs->AdvanceFrameForTesting(); Fixture.Jobs->AdvanceMaintenanceTickerForTesting();
    TestEqual(TEXT("Stopped scheduler cannot step a runner"), Steps, 3);
    TestEqual(TEXT("Stopped scheduler cannot repeat callbacks"), Completions, 2);
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeTerminalWriteTest, UNREAL_TOOLS_TEST_PREFIX ".TerminalJournalFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeTerminalWriteTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture; FObject Completed; int32 CompletionCount = 0;
    Fixture.Jobs->SetPersistGateForTesting([](const FObject& Job, FString& Message)
    { if (GetString(Job, TEXT("state")) == TEXT("succeeded")) { Message = TEXT("Injected terminal write failure."); return false; } return true; });
    FItemSpec Spec; Spec.MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube"); Spec.Options = Fixture.Defaults;
    FObject Accepted = Fixture.Jobs->StartLegacyStructured(Spec, [&](FObject Result) { ++CompletionCount; Completed = Result; }); const FString Id = GetString(Accepted, TEXT("job_id"));
    for (int32 Index = 0; Index < 3; ++Index) Fixture.Jobs->AdvanceOneStepForTesting();
    const FObject Job = Fixture.Jobs->Status(FRuntimeFixture::ById(Id));
    TestEqual(TEXT("Journal failure uses existing failed terminal state"), GetString(Job, TEXT("state")), FString(TEXT("failed")));
    TestTrue(TEXT("Failure holds recovery reservation"), GetBool(Job, TEXT("recovery_required")));
    TestFalse(TEXT("Legacy completion cannot report native success"), GetBool(Completed, TEXT("success")));
    TestEqual(TEXT("Legacy completion retains IO_ERROR"), GetString(Completed, TEXT("error_code")), FString(TEXT("IO_ERROR")));
    TestEqual(TEXT("Native output paths survive error overlay"), GetArray(Completed, TEXT("saved")).Num(), 1);
    for (int32 Index = 0; Index < 5; ++Index) Fixture.Jobs->AdvanceOneStepForTesting();
    TestEqual(TEXT("Terminal write failure completes its callback exactly once"), CompletionCount, 1);
    TestEqual(TEXT("Same outputs remain blocked"), GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("error_code")), FString(TEXT("OUTPUT_CONFLICT")));
    Fixture.Jobs.Reset(); Fixture.Create();
    TestTrue(TEXT("Persisted failure reservation survives restart"), GetBool(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("recovery_required")));
    for (int32 Variant = 0; Variant < 2; ++Variant)
    {
        FRuntimeFixture Inline;
        Inline.Jobs->SetPersistGateForTesting([Variant](const FObject& Record, FString& Message)
        {
            const FString State = GetString(Record, TEXT("state"));
            if (State == TEXT("succeeded") || (Variant == 1 && (State == TEXT("failed") || State == TEXT("cancelled") || State == TEXT("interrupted"))))
            { Message = TEXT("Injected inline terminal persistence failure."); return false; }
            return true;
        });
        FString InlineError; const FObject Result = Inline.Jobs->RunInline(Spec, InlineError);
        TestFalse(TEXT("Inline terminal failure cannot report native success"), GetBool(Result, TEXT("success")));
        TestEqual(TEXT("Inline terminal failure returns structured IO_ERROR"), GetString(Result, TEXT("error_code")), FString(TEXT("IO_ERROR")));
        TestEqual(TEXT("Inline failure returns the native output path"), GetArray(Result, TEXT("saved")).Num(), 1);
        const auto Jobs = Inline.Jobs->GetJobs();
        TestEqual(TEXT("Inline call retains one normal job record"), Jobs.Num(), 1); if (Jobs.Num() != 1) return false;
        const FString InlineId = GetString(Jobs[0], TEXT("job_id"));
        const FObject Live = Inline.Jobs->Status(FRuntimeFixture::ById(InlineId)); FDateTime Finished;
        TestEqual(TEXT("Inline persistence failure is the failed terminal state"), GetString(Live, TEXT("state")), FString(TEXT("failed")));
        TestTrue(TEXT("Inline terminal failure has a completion timestamp"), FDateTime::ParseIso8601(*GetString(Live, TEXT("finished_at")), Finished));
        TestTrue(TEXT("Inline persistence failure retains its reservation"), GetBool(Live, TEXT("recovery_required")));
        FString DurableBytes;
        TestTrue(TEXT("Inline failure retains a durable record"), FFileHelper::LoadFileToString(DurableBytes, *(Inline.Root / TEXT("Jobs") / (InlineId + TEXT(".json")))));
        if (Variant == 1)
        {
            const FObject Checkpoint = Parse(DurableBytes);
            TestEqual(TEXT("All final writes failing leaves the earlier running checkpoint"), GetString(Checkpoint, TEXT("state")), FString(TEXT("running")));
            TestTrue(TEXT("Earlier checkpoint still records the commit boundary"), GetBool(GetArray(Checkpoint, TEXT("items"))[0]->AsObject(), TEXT("commit_pending")));
        }
        Inline.Jobs.Reset(); Inline.Create();
        const FObject Reloaded = Inline.Jobs->Status(FRuntimeFixture::ById(InlineId));
        TestEqual(TEXT("Reload recovers from the last durable state"), GetString(Reloaded, TEXT("state")), FString(Variant == 1 ? TEXT("interrupted") : TEXT("failed")));
        TestTrue(TEXT("Reload retains the inline failure reservation"), GetBool(Reloaded, TEXT("recovery_required")));
        TestEqual(TEXT("Recovered inline output cannot be reused"), GetString(Inline.Jobs->Start(Inline.Input()), TEXT("error_code")), FString(TEXT("OUTPUT_CONFLICT")));
        Inline.Jobs->AdvanceOneStepForTesting();
        TestEqual(TEXT("Reload never resumes the interrupted inline runner"), GetString(Inline.Jobs->Status(FRuntimeFixture::ById(InlineId)), TEXT("state")), GetString(Reloaded, TEXT("state")));
        TestEqual(TEXT("Returned inline result remains available after service reload"), GetArray(Result, TEXT("saved")).Num(), 1);
    }
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeDiagnosticRetryTest, UNREAL_TOOLS_TEST_PREFIX ".DiagnosticQuarantineRetry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeDiagnosticRetryTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture; const FString Broken = Fixture.Root / TEXT("Jobs/broken.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Broken), true); FFileHelper::SaveStringToFile(TEXT("malformed source bytes"), *Broken);
    const FString Id = GetString(Fixture.Jobs->RegisterRecordIssue(Broken, TEXT("Injected damaged journal.")), TEXT("job_id"));
    TestEqual(TEXT("Diagnostic is available with the existing status selector"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("record_kind")), FString(TEXT("diagnostic")));
    TestEqual(TEXT("Unknown outputs conservatively block admission"), GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("error_code")), FString(TEXT("RECOVERY_REQUIRED")));
    TestEqual(TEXT("Diagnostics do not become ordinary history"), Fixture.Jobs->GetJobs().Num(), 0);
    auto Ack = FRuntimeFixture::ById(Id); Ack->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
    bool bVerifiedRawBeforePreparing = false;
    Fixture.Jobs->SetPersistGateForTesting([&](const FObject& Job, FString& Message)
    {
        if (GetString(Job, TEXT("phase")) == TEXT("prepared"))
        {
            TArray<uint8> Bytes; FSHAHash Hash;
            if (FFileHelper::LoadFileToArray(Bytes, *GetString(Job, TEXT("raw_path"))))
            { FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash); bVerifiedRawBeforePreparing = Hash.ToString() == GetString(Job, TEXT("raw_hash")); }
        }
        if (GetString(Job, TEXT("phase")) == TEXT("acknowledged")) { Message = TEXT("Injected final guard failure."); return false; } return true;
    });
    TestFalse(TEXT("Final guard failure does not release reservation"), GetBool(Fixture.Jobs->Control(Ack), TEXT("success")));
    TestTrue(TEXT("Prepared guard receives independently verified raw evidence"), bVerifiedRawBeforePreparing);
    const FObject Prepared = Fixture.Jobs->Status(FRuntimeFixture::ById(Id)); FString Raw;
    FFileHelper::LoadFileToString(Raw, *GetString(Prepared, TEXT("raw_path")));
    TestEqual(TEXT("Raw evidence preserves the exact original bytes"), Raw, FString(TEXT("malformed source bytes")));
    TestFalse(TEXT("Only damaged metadata was removed"), IFileManager::Get().FileExists(*Broken));
    TestEqual(TEXT("Prepared guard remains durable"), GetString(Prepared, TEXT("phase")), FString(TEXT("prepared")));
    Fixture.Jobs.Reset(); Fixture.Create();
    TestEqual(TEXT("Prepared guard blocks restart even without source JSON"), Fixture.Jobs->GetRecordIssues().Num(), 1);
    TestTrue(TEXT("Same acknowledgement retry finishes after restart"), GetBool(Fixture.Jobs->Control(Ack), TEXT("success")));
    TestEqual(TEXT("Completed guard releases the blocker"), Fixture.Jobs->GetRecordIssues().Num(), 0);
    TestTrue(TEXT("New admission works after reviewed quarantine"), GetBool(Fixture.Jobs->Start(Fixture.Input()), TEXT("success")));
    {
        FRuntimeFixture Retention;
        const FString Source = Retention.Root / TEXT("Recovery/retention_seed.json");
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Source), true);
        TestTrue(TEXT("Diagnostic retention raw fixture write"), FFileHelper::SaveStringToFile(TEXT("retained diagnostic raw bytes"), *Source));
        const FString SeedId = GetString(Retention.Jobs->RegisterRecordIssue(Source, TEXT("Retention seed.")), TEXT("job_id"));
        FObject SeedAck = FRuntimeFixture::ById(SeedId); SeedAck->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
        TestTrue(TEXT("Diagnostic retention seed is durably acknowledged"), GetBool(Retention.Jobs->Control(SeedAck), TEXT("success")));
        const FObject GuardTemplate = Retention.Jobs->Status(FRuntimeFixture::ById(SeedId));
        const FString RawPath = GetString(GuardTemplate, TEXT("raw_path")); TArray<uint8> OriginalRaw;
        TestTrue(TEXT("Diagnostic raw evidence exists before retention"), FFileHelper::LoadFileToArray(OriginalRaw, *RawPath));
        FObject Dependent = Retention.Jobs->Start(Retention.Input({TEXT("/Game/Dependent.Dependent")}));
        const FString DependentId = GetString(Dependent, TEXT("job_id"));
        Retention.Jobs.Reset();
        IFileManager::Get().Delete(*(Retention.Root / TEXT("Diagnostics") / (SeedId + TEXT(".json"))), false, true);
        TArray<FString> GuardIds; FString DependentSource; FString Message;
        for (int32 Index = 0; Index < 109; ++Index)
        {
            const FString GuardId = TEXT("runtime_test-diagnostic-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            FObject Guard = Clone(GuardTemplate); Guard->SetStringField(TEXT("job_id"), GuardId);
            const FString GuardSource = Retention.Root / TEXT("Recovery") / FString::Printf(TEXT("history_%03d.json"), Index);
            Guard->SetStringField(TEXT("source_path"), GuardSource);
            Guard->SetStringField(TEXT("finished_at"), (FDateTime::UtcNow() - (Index >= 105 ? FTimespan::FromDays(8) : FTimespan::FromSeconds(Index))).ToIso8601());
            if (Index >= 107) { Guard->SetStringField(TEXT("phase"), Index == 107 ? TEXT("prepared") : TEXT("issue")); Guard->SetBoolField(TEXT("recovery_required"), true); }
            const FString Filename = Retention.Root / TEXT("Diagnostics") / (GuardId + TEXT(".json"));
            const bool bWritten = AtomicJson(Filename, Guard, Message);
            TestTrue(TEXT("Diagnostic retention fixture: ") + Message, bWritten); if (!bWritten) return false;
            GuardIds.Add(GuardId); if (Index == 106) DependentSource = GuardSource;
        }
        Dependent->SetStringField(TEXT("state"), TEXT("failed")); Dependent->SetBoolField(TEXT("recovery_required"), true);
        Dependent->SetStringField(TEXT("finished_at"), (FDateTime::UtcNow() - FTimespan::FromDays(8)).ToIso8601());
        FObject Item = GetArray(Dependent, TEXT("items"))[0]->AsObject(); Item->SetStringField(TEXT("state"), TEXT("failed"));
        FObject Result = UnrealToolsRuntime::Error(TEXT("Review dependent recovery.")); Result->SetStringField(TEXT("recovery_journal"), DependentSource); Item->SetObjectField(TEXT("result"), Result);
        const bool bDependentWritten = AtomicJson(Retention.Root / TEXT("Jobs") / (DependentId + TEXT(".json")), Dependent, Message);
        TestTrue(TEXT("Dependent recovery fixture: ") + Message, bDependentWritten); if (!bDependentWritten) return false;
        Retention.Create();
        for (int32 Tick = 0; Tick < 25; ++Tick) Retention.Jobs->RunMaintenanceForTesting();
        const int32 DiagnosticCount = Retention.Jobs->GetDiagnosticSummaries(0, 100).Num() + Retention.Jobs->GetDiagnosticSummaries(100, 100).Num();
        TestEqual(TEXT("Acknowledged diagnostics retain 100 ordinary plus protected dependent/prepared/issue guards"), DiagnosticCount, 103);
        for (int32 Index = 0; Index < 106; ++Index)
            TestEqual(TEXT("Diagnostic count/age retention keeps the newest 100 ordinary guards"), GetString(Retention.Jobs->Status(FRuntimeFixture::ById(GuardIds[Index])), TEXT("error_code")) == TEXT("NOT_FOUND"), Index >= 100);
        TestTrue(TEXT("Recovery references protect an acknowledged diagnostic older than seven days"), Retention.Jobs->WasRecordIssueAcknowledged(DependentSource));
        TestEqual(TEXT("Prepared and issue guards are excluded from diagnostic retention"), Retention.Jobs->GetRecordIssues().Num(), 2);
        TArray<uint8> RemainingRaw;
        TestTrue(TEXT("Diagnostic cleanup never removes raw evidence"), FFileHelper::LoadFileToArray(RemainingRaw, *RawPath));
        TestTrue(TEXT("Diagnostic cleanup preserves exact raw bytes"), RemainingRaw == OriginalRaw);
        FObject DependentAck = FRuntimeFixture::ById(DependentId); DependentAck->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
        TestTrue(TEXT("Dependent normal job can release only its own recovery reference"), GetBool(Retention.Jobs->Control(DependentAck), TEXT("success")));
        Retention.Jobs->PinJob(GuardIds[106]); Retention.Jobs->RunMaintenanceForTesting();
        TestTrue(TEXT("Pinned acknowledged diagnostic survives after its recovery dependency ends"), GetBool(Retention.Jobs->Status(FRuntimeFixture::ById(GuardIds[106])), TEXT("success")));
        Retention.Jobs->UnpinJob(GuardIds[106]);
        for (int32 Tick = 0; Tick < 3; ++Tick) Retention.Jobs->RunMaintenanceForTesting();
        TestEqual(TEXT("Old acknowledged diagnostic expires after dependency and consumer pin end"), GetString(Retention.Jobs->Status(FRuntimeFixture::ById(GuardIds[106])), TEXT("error_code")), FString(TEXT("NOT_FOUND")));
        TestTrue(TEXT("Raw evidence survives dependent guard removal too"), FFileHelper::LoadFileToArray(RemainingRaw, *RawPath));
        TestTrue(TEXT("Remaining orphan/prepared issues still block new work after retention"), !GetBool(Retention.Jobs->Start(Retention.Input()), TEXT("success")));
    }
    return true;
}

UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeHistoryRetentionTest, UNREAL_TOOLS_TEST_PREFIX ".BoundedHistoryAndReceipts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeHistoryRetentionTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture; FObject Template = Fixture.Jobs->Start(Fixture.Input()); const FString TemplateId = GetString(Template, TEXT("job_id"));
    Fixture.Jobs.Reset(); IFileManager::Get().Delete(*(Fixture.Root / TEXT("Jobs") / (TemplateId + TEXT(".json"))), false, true);
    FObject ExpiredRequest; FString ExpiredId;
    for (int32 Index = 0; Index < 206; ++Index)
    {
        FObject Job = Clone(Template); FObject Request = Clone(GetObject(Job, TEXT("request")));
        const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower); Request->SetStringField(TEXT("request_id"), RequestId);
        const FString Id = TEXT("runtime_test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Job->SetStringField(TEXT("job_id"), Id); Job->SetStringField(TEXT("request_id"), RequestId); Job->SetObjectField(TEXT("request"), Request); Job->SetStringField(TEXT("request_hash"), RequestHash(Request));
        Job->SetStringField(TEXT("state"), TEXT("succeeded")); Job->SetStringField(TEXT("finished_at"), (FDateTime::UtcNow() - (Index == 205 ? FTimespan::FromDays(8) : FTimespan::FromSeconds(Index))).ToIso8601());
        Job->SetNumberField(TEXT("current_index"), 1); GetObject(Job, TEXT("progress"))->SetNumberField(TEXT("completed"), 1);
        FObject Item = GetArray(Job, TEXT("items"))[0]->AsObject(); Item->SetStringField(TEXT("state"), TEXT("succeeded")); Item->SetObjectField(TEXT("result"), Ok());
        FString Message; TestTrue(TEXT("History fixture write"), AtomicJson(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json")), Job, Message));
        if (Index == 205) { ExpiredRequest = Request; ExpiredId = Id; }
    }
    Fixture.Create();
    for (int32 Tick = 0; Tick < 25; ++Tick) Fixture.Jobs->RunMaintenanceForTesting();
    TestEqual(TEXT("Recent history is bounded to 100"), Fixture.Jobs->GetJobSummaries(0, 100, false).Num(), 100);
    TestEqual(TEXT("Archive history is bounded to 100"), Fixture.Jobs->GetJobSummaries(0, 100, true).Num(), 100);
    TestEqual(TEXT("Expired job status is NOT_FOUND"), GetString(Fixture.Jobs->Status(FRuntimeFixture::ById(ExpiredId)), TEXT("error_code")), FString(TEXT("NOT_FOUND")));
    TestTrue(TEXT("Full terminal cache is bounded"), Fixture.Jobs->GetCachedRecordCountForTesting() <= 20);
    TestTrue(TEXT("Expired UUID is intentionally eligible for a new execution"), GetBool(Fixture.Jobs->Start(ExpiredRequest), TEXT("success")));
    {
        FRuntimeFixture Protected;
        const FObject Base = Protected.Jobs->Start(Protected.Input());
        const FString BaseId = GetString(Base, TEXT("job_id")); Protected.Jobs.Reset();
        IFileManager::Get().Delete(*(Protected.Root / TEXT("Jobs") / (BaseId + TEXT(".json"))), false, true);
        TArray<FString> Ids, Files; FString PinnedRequestId, Message;
        for (int32 Index = 0; Index < 14; ++Index)
        {
            FObject Job = Clone(Base), Request = Clone(GetObject(Job, TEXT("request")));
            const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            const FString Id = TEXT("runtime_test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            Request->SetStringField(TEXT("request_id"), RequestId); Job->SetStringField(TEXT("job_id"), Id);
            Job->SetStringField(TEXT("request_id"), RequestId); Job->SetObjectField(TEXT("request"), Request); Job->SetStringField(TEXT("request_hash"), RequestHash(Request));
            Job->SetStringField(TEXT("state"), TEXT("succeeded")); Job->SetStringField(TEXT("finished_at"), (FDateTime::UtcNow() - FTimespan::FromDays(8)).ToIso8601());
            Job->SetNumberField(TEXT("current_index"), 1); GetObject(Job, TEXT("progress"))->SetNumberField(TEXT("completed"), 1);
            FObject Item = GetArray(Job, TEXT("items"))[0]->AsObject(); Item->SetStringField(TEXT("state"), TEXT("succeeded")); Item->SetObjectField(TEXT("result"), Ok());
            if (Index == 1) Job->SetBoolField(TEXT("recovery_required"), true);
            const FString Filename = Protected.Root / TEXT("Jobs") / (Id + TEXT(".json"));
            const bool bWritten = AtomicJson(Filename, Job, Message); TestTrue(TEXT("Protected history fixture: ") + Message, bWritten); if (!bWritten) return false;
            Ids.Add(Id); Files.Add(Filename); if (Index == 0) PinnedRequestId = RequestId;
        }
        TArray<FString> Evidence;
        for (const TCHAR* Directory : {TEXT("Outputs"), TEXT("Staging"), TEXT("Backups"), TEXT("Quarantine"), TEXT("Verification")})
        {
            const FString Filename = Protected.Root / Directory / TEXT("untouched.json");
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
            TestTrue(TEXT("Protected output/evidence fixture write"), FFileHelper::SaveStringToFile(TEXT("original evidence bytes"), *Filename)); Evidence.Add(Filename);
        }
        auto Remaining = [&]() { int32 Count = 0; for (const FString& Filename : Files) if (IFileManager::Get().FileExists(*Filename)) ++Count; return Count; };
        Protected.Create(); Protected.Jobs->PinJob(Ids[0]); Protected.Jobs->PinJob(Ids[0]);
        const FString ActiveId = GetString(Protected.Jobs->Start(Protected.Input({TEXT("/Game/ActiveGuard.ActiveGuard")})), TEXT("job_id"));
        TestEqual(TEXT("Fixture has queued work before maintenance"), GetString(Protected.Jobs->Status(FRuntimeFixture::ById(ActiveId)), TEXT("state")), FString(TEXT("queued")));
        Protected.Jobs->RunMaintenanceForTesting(100);
        TestEqual(TEXT("Maintenance does no file work while a job is queued"), Remaining(), 14);
        Protected.Jobs->AdvanceOneStepForTesting(); Protected.Jobs->RunMaintenanceForTesting(100);
        TestEqual(TEXT("Fixture has active work before maintenance"), GetString(Protected.Jobs->Status(FRuntimeFixture::ById(ActiveId)), TEXT("state")), FString(TEXT("running")));
        TestEqual(TEXT("Maintenance does no file work while a runner is active"), Remaining(), 14);
        FObject Cancel = FRuntimeFixture::ById(ActiveId); Cancel->SetStringField(TEXT("action"), TEXT("cancel"));
        TestTrue(TEXT("Protected-history active fixture can stop"), GetBool(Protected.Jobs->Control(Cancel), TEXT("success"))); Protected.Jobs->AdvanceOneStepForTesting();
        Protected.Jobs->RunMaintenanceForTesting(100);
        TestEqual(TEXT("A requested larger maintenance budget is capped at ten file operations"), Remaining(), 4);
        TestTrue(TEXT("Consumer pin protects expired metadata"), IFileManager::Get().FileExists(*Files[0]));
        TestTrue(TEXT("Unresolved recovery protects expired metadata"), IFileManager::Get().FileExists(*Files[1]));
        FObject Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("request_id"), PinnedRequestId);
        TestEqual(TEXT("Consumer pin protects its UUID receipt"), GetString(Protected.Jobs->Status(Selector), TEXT("job_id")), Ids[0]);
        Protected.Jobs->UnpinJob(Ids[0]); Protected.Jobs->RunMaintenanceForTesting();
        TestTrue(TEXT("One of two consumer unpins cannot remove the protected record"), IFileManager::Get().FileExists(*Files[0]));
        Protected.Jobs->UnpinJob(Ids[0]); Protected.Jobs->RunMaintenanceForTesting();
        TestFalse(TEXT("Expired metadata is eligible after the final consumer releases it"), IFileManager::Get().FileExists(*Files[0]));
        TestEqual(TEXT("Final unpin permits receipt cleanup too"), GetString(Protected.Jobs->Status(Selector), TEXT("error_code")), FString(TEXT("NOT_FOUND")));
        TestTrue(TEXT("Unresolved recovery survives all cleanup passes"), IFileManager::Get().FileExists(*Files[1]));
        for (const FString& Filename : Evidence)
        {
            FString Bytes; TestTrue(TEXT("Cleanup preserves output/staging/backup/raw/verification files"), FFileHelper::LoadFileToString(Bytes, *Filename));
            TestEqual(TEXT("Cleanup preserves original output/evidence bytes"), Bytes, FString(TEXT("original evidence bytes")));
        }
    }
    {
        FRuntimeFixture Legacy;
        FObject Record = Legacy.Jobs->Start(Legacy.Input()); const FString Id = GetString(Record, TEXT("job_id")); Legacy.Jobs.Reset();
        Record->SetStringField(TEXT("state"), TEXT("succeeded")); Record->RemoveField(TEXT("finished_at"));
        Record->SetStringField(TEXT("created_at"), (FDateTime::UtcNow() - FTimespan::FromDays(9)).ToIso8601());
        Record->SetNumberField(TEXT("current_index"), 1); GetObject(Record, TEXT("progress"))->SetNumberField(TEXT("completed"), 1);
        FObject Item = GetArray(Record, TEXT("items"))[0]->AsObject(); Item->SetStringField(TEXT("state"), TEXT("succeeded")); Item->SetObjectField(TEXT("result"), Ok());
        const FString Filename = Legacy.Root / TEXT("Jobs") / (Id + TEXT(".json")); FString Message;
        const bool bWritten = AtomicJson(Filename, Record, Message); TestTrue(TEXT("Legacy timestamp fixture: ") + Message, bWritten); if (!bWritten) return false;
        const FDateTime Intended = FDateTime::UtcNow() - FTimespan::FromDays(8); IFileManager::Get().SetTimeStamp(*Filename, Intended);
        const FDateTime OriginalTime = IFileManager::Get().GetTimeStamp(*Filename);
        TestTrue(TEXT("Legacy fixture has the intended old file time"), FMath::Abs((OriginalTime - Intended).GetTotalSeconds()) < 2.0);
        FObject Untrusted = Clone(Record), Request = Clone(GetObject(Record, TEXT("request")));
        const FString UntrustedId = TEXT("runtime_test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString UntrustedRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower); Request->SetStringField(TEXT("request_id"), UntrustedRequestId);
        Untrusted->SetStringField(TEXT("job_id"), UntrustedId); Untrusted->SetStringField(TEXT("request_id"), UntrustedRequestId);
        Untrusted->SetObjectField(TEXT("request"), Request); Untrusted->SetStringField(TEXT("request_hash"), RequestHash(Request));
        const FString UntrustedFile = Legacy.Root / TEXT("Jobs") / (UntrustedId + TEXT(".json"));
        const bool bUntrustedWritten = AtomicJson(UntrustedFile, Untrusted, Message); TestTrue(TEXT("Untrusted timestamp fixture: ") + Message, bUntrustedWritten); if (!bUntrustedWritten) return false;
        IFileManager::Get().SetTimeStamp(*UntrustedFile, FDateTime::UtcNow() + FTimespan::FromDays(2));
        Legacy.Create(); const FObject Loaded = Legacy.Jobs->Status(FRuntimeFixture::ById(Id));
        TestEqual(TEXT("Legacy finished time uses the original file mtime captured before rewriting"), GetString(Loaded, TEXT("finished_at")), OriginalTime.ToIso8601());
        TestEqual(TEXT("Legacy finished time records its provenance"), GetString(Loaded, TEXT("finished_at_source")), FString(TEXT("legacy_file_mtime")));
        TestTrue(TEXT("Migration rewrites metadata after retaining the original timestamp"), IFileManager::Get().GetTimeStamp(*Filename) > OriginalTime);
        TestTrue(TEXT("Untrustworthy legacy file time is excluded from cleanup"), GetBool(Legacy.Jobs->Status(FRuntimeFixture::ById(UntrustedId)), TEXT("retention_time_untrusted")));
        Legacy.Jobs.Reset(); Legacy.Create();
        TestEqual(TEXT("A second startup does not replace the migrated finished time"), GetString(Legacy.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("finished_at")), OriginalTime.ToIso8601());
        Legacy.Jobs->RunMaintenanceForTesting();
        TestEqual(TEXT("Migrated old completion expires according to its original timestamp"), GetString(Legacy.Jobs->Status(FRuntimeFixture::ById(Id)), TEXT("error_code")), FString(TEXT("NOT_FOUND")));
        TestTrue(TEXT("Untrusted timestamp remains protected after restart and maintenance"), IFileManager::Get().FileExists(*UntrustedFile));
    }
    return true;
}
UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimePrepareAndInvalidSchemaTest, UNREAL_TOOLS_TEST_PREFIX ".PreparationAndUnsupportedJournal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimePrepareAndInvalidSchemaTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture; FObject Input = Fixture.Input({TEXT("/Game/A.A"), TEXT("/Game/B.B"), TEXT("/Game/C.C")});
    Input->SetObjectField(TEXT("overrides"), Parse(TEXT(R"({"/Game/A.A":{"fail":3},"/Game/B.B":{"typo":true}})")));
    FPreparedBatch Prepared; FString Message;
    TestFalse(TEXT("Cheap preparation rejects the invalid batch"), Fixture.Jobs->PrepareBatch(Input, Prepared, Message));
    TestEqual(TEXT("Preparation keeps all ordered input rows"), Prepared.Specs.Num(), 3);
    TestTrue(TEXT("Preparation collects both row errors"), Prepared.Errors.Num() >= 2);
    TestFalse(TEXT("First malformed option row cannot be inspected"), Prepared.ItemResolved[0]);
    TestTrue(TEXT("Later valid row is still available for inspection"), Prepared.ItemResolved[2]);
    TestEqual(TEXT("Inspection creates no durable job"), Fixture.Jobs->GetJobs().Num(), 0);
    FObject Record = Fixture.Jobs->Start(Fixture.Input()); const FString Id = GetString(Record, TEXT("job_id"));
    Fixture.Jobs.Reset(); Record->SetNumberField(TEXT("schema_version"), 2);
    TestTrue(TEXT("Unsupported fixture write"), AtomicJson(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json")), Record, Message));
    Fixture.Create();
    TestEqual(TEXT("Unsupported version becomes a visible recovery blocker"), Fixture.Jobs->GetRecordIssues().Num(), 1);
    TestEqual(TEXT("Unsupported version cannot silently allow new writes"), GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("error_code")), FString(TEXT("RECOVERY_REQUIRED")));
    FString Original; FFileHelper::LoadFileToString(Original, *(Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json"))));
    TestEqual(TEXT("Detection leaves unsupported source metadata unchanged"), GetNumber(Parse(Original), TEXT("schema_version")), 2.0);
    return true;
}
UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeRepairedGuardTest, UNREAL_TOOLS_TEST_PREFIX ".RepairedDiagnosticGuardKeepsOriginalReservation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeRepairedGuardTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    FRuntimeFixture Fixture;
    const FString Original = Fixture.Root / TEXT("Jobs/original_damaged.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Original), true);
    FFileHelper::SaveStringToFile(TEXT("original damaged bytes"), *Original);
    const FString OriginalIssueId = GetString(Fixture.Jobs->RegisterRecordIssue(Original, TEXT("Original damaged source.")), TEXT("job_id"));
    FObject OriginalAck = FRuntimeFixture::ById(OriginalIssueId); OriginalAck->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
    Fixture.Jobs->SetPersistGateForTesting([](const FObject& Record, FString& Message)
    { if (GetString(Record, TEXT("phase")) == TEXT("acknowledged")) { Message = TEXT("Keep the original issue prepared for this regression."); return false; } return true; });
    TestFalse(TEXT("Original guard deliberately remains prepared"), GetBool(Fixture.Jobs->Control(OriginalAck), TEXT("success")));
    TestFalse(TEXT("Prepared original guard has already quarantined its source"), IFileManager::Get().FileExists(*Original));
    const FString OriginalGuard = Fixture.Root / TEXT("Diagnostics") / (OriginalIssueId + TEXT(".json"));
    FString HealthyGuardBytes;
    TestTrue(TEXT("Preserve the healthy prepared guard before damaging it"), FFileHelper::LoadFileToString(HealthyGuardBytes, *OriginalGuard));
    Fixture.Jobs.Reset();
    FFileHelper::SaveStringToFile(TEXT("damaged guard metadata"), *OriginalGuard, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    Fixture.Create();
    const auto Issues = Fixture.Jobs->GetRecordIssues();
    TestEqual(TEXT("Damaged guard creates a separate diagnostic issue"), Issues.Num(), 1);
    if (Issues.Num() != 1) return false;
    const FString GuardIssueId = GetString(Issues[0], TEXT("job_id"));
    TestNotEqual(TEXT("The guard corruption and original issue have distinct IDs"), GuardIssueId, OriginalIssueId);
    FFileHelper::SaveStringToFile(HealthyGuardBytes, *OriginalGuard, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FObject GuardAck = FRuntimeFixture::ById(GuardIssueId); GuardAck->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
    const FObject ChangedAck = Fixture.Jobs->Control(GuardAck);
    AddInfo(TEXT("Changed-source acknowledgement response: ") + JsonText(ChangedAck));
    TestEqual(TEXT("Changed guard bytes require a fresh review"), GetString(ChangedAck, TEXT("error_code")), FString(TEXT("RECOVERY_REQUIRED")));
    const FObject RepairedAck = Fixture.Jobs->Control(GuardAck);
    AddInfo(TEXT("Repaired-guard acknowledgement response: ") + JsonText(RepairedAck));
    AddInfo(TEXT("Repaired-guard diagnostic snapshot after retry: ") + JsonText(Fixture.Jobs->Status(FRuntimeFixture::ById(GuardIssueId))));
    TestTrue(TEXT("Acknowledging the repaired guard resolves only its corruption issue"), GetBool(RepairedAck, TEXT("success")));
    TestTrue(TEXT("Healthy prepared guard is never deleted as damaged metadata"), IFileManager::Get().FileExists(*OriginalGuard));
    TestEqual(TEXT("Original prepared issue still blocks the provider"), GetString(Fixture.Jobs->Start(Fixture.Input()), TEXT("error_code")), FString(TEXT("RECOVERY_REQUIRED")));
    Fixture.Jobs.Reset(); Fixture.Create();
    const FObject Reloaded = Fixture.Jobs->Status(FRuntimeFixture::ById(OriginalIssueId));
    TestEqual(TEXT("Original guard remains independently selectable after restart"), GetString(Reloaded, TEXT("phase")), FString(TEXT("prepared")));
    TestTrue(TEXT("Restart preserves the original recovery reservation"), GetBool(Reloaded, TEXT("recovery_required")));
    TestEqual(TEXT("Original pending review survives repaired-guard acknowledgement"), Fixture.Jobs->GetRecordIssues().Num(), 1);
    TestTrue(TEXT("Original issue can be explicitly acknowledged independently"), GetBool(Fixture.Jobs->Control(OriginalAck), TEXT("success")));
    TestEqual(TEXT("Both issues resolve only after both explicit acknowledgements"), Fixture.Jobs->GetRecordIssues().Num(), 0);
    return true;
}
UNREAL_TOOLS_RUNTIME_IMPLEMENT(RuntimeMaintenanceRevalidationTest, UNREAL_TOOLS_TEST_PREFIX ".MaintenanceRechecksChangedDiskRecord",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool UNREAL_TOOLS_RUNTIME_CLASS(RuntimeMaintenanceRevalidationTest)::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    for (int32 Variant = 0; Variant < 2; ++Variant)
    {
        FRuntimeFixture Fixture; const FObject OriginalRequest = Fixture.Input();
        const FString Id = GetString(Fixture.Jobs->Start(OriginalRequest), TEXT("job_id"));
        for (int32 Step = 0; Step < 3; ++Step) Fixture.Jobs->AdvanceOneStepForTesting();
        FObject Record = Fixture.Jobs->Status(FRuntimeFixture::ById(Id));
        Record->SetStringField(TEXT("finished_at"), (FDateTime::UtcNow() - FTimespan::FromDays(8)).ToIso8601());
        const FString Filename = Fixture.Root / TEXT("Jobs") / (Id + TEXT(".json")); FString Message;
        Fixture.Jobs.Reset(); TestTrue(TEXT("Expired normal record fixture write"), AtomicJson(Filename, Record, Message));
        Fixture.Create(); // The initial index/cache now considers this record eligible.
        if (Variant == 0)
        {
            FFileHelper::SaveStringToFile(TEXT("changed damaged record bytes"), *Filename);
        }
        else
        {
            Record->SetBoolField(TEXT("recovery_required"), true); Record->SetStringField(TEXT("state"), TEXT("failed"));
            Record->SetStringField(TEXT("error"), TEXT("Unresolved recovery written after indexing.")); Record->SetStringField(TEXT("error_code"), TEXT("EXTERNAL_RECOVERY"));
            FObject Item = GetArray(Record, TEXT("items"))[0]->AsObject(); Item->SetStringField(TEXT("state"), TEXT("failed"));
            FObject Result = Clone(GetObject(Item, TEXT("result"))); Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), TEXT("Unresolved recovery.")); Item->SetObjectField(TEXT("result"), Result);
            GetObject(Record, TEXT("progress"))->SetNumberField(TEXT("completed"), 0);
            TestTrue(TEXT("Protected recovery record changed on disk after indexing"), AtomicJson(Filename, Record, Message));
        }
        Fixture.Jobs->RunMaintenanceForTesting();
        TestTrue(TEXT("Changed protected/damaged source metadata is never purged"), IFileManager::Get().FileExists(*Filename));
        const FObject NewSubmission = Fixture.Jobs->Start(Fixture.Input());
        TestEqual(TEXT("Changed record continues blocking its outputs/provider"), GetString(NewSubmission, TEXT("error_code")), FString(Variant == 0 ? TEXT("RECOVERY_REQUIRED") : TEXT("OUTPUT_CONFLICT")));
        if (Variant == 0)
        {
            TestTrue(TEXT("Damaged disk record becomes a visible diagnostic instead of disappearing"), !Fixture.Jobs->GetRecordIssues().IsEmpty());
        }
        else
        {
            const FObject Live = Fixture.Jobs->Status(FRuntimeFixture::ById(Id));
            TestTrue(TEXT("Maintenance refreshes the live recovery reservation"), GetBool(Live, TEXT("recovery_required")));
            FObject Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("request_id"), GetString(OriginalRequest, TEXT("request_id")));
            TestEqual(TEXT("Protected UUID receipt still resolves to the original job"), GetString(Fixture.Jobs->Status(Selector), TEXT("job_id")), Id);
        }
        const int32 ActiveBefore = Fixture.Jobs->GetActiveJobs().Num(); Fixture.Jobs->Start(OriginalRequest);
        TestEqual(TEXT("Retained UUID cannot enqueue another execution after maintenance"), Fixture.Jobs->GetActiveJobs().Num(), ActiveBefore);
    }
    return true;
}
#undef UNREAL_TOOLS_RUNTIME_IMPLEMENT
#undef UNREAL_TOOLS_RUNTIME_IMPLEMENT_INNER
#undef UNREAL_TOOLS_RUNTIME_CLASS
#undef UNREAL_TOOLS_RUNTIME_JOIN
#undef UNREAL_TOOLS_RUNTIME_JOIN_INNER
#endif
