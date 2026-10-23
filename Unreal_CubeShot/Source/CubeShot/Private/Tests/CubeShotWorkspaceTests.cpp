#include "CubeShotWorkspaceController.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotWorkspaceContractTest, "CubeShot.Editor.Workspace.OptionsAndSnapshots",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCubeShotWorkspaceContractTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    auto Model = MakeShared<FCubeShotWorkspaceController>();
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    Model->Add(A);
    Model->Add(B);
    auto Defaults = Model->CopyActiveOptions();
    Defaults->SetNumberField(TEXT("resolution"), 512);
    Model->Replace(Model->Token(), Defaults, TEXT("editor_default"));
    TestEqual(TEXT("Old row keeps editor default"), GetNumber(Model->CopyOptions(A), TEXT("resolution")), 1024.0);
    Model->Select(A, true);
    Model->ApplySelected();
    Model->Edit(A);
    auto Request = Model->BuildRequest();
    TestTrue(TEXT("Snapshot has no live profile dependency"),
             Request.IsValid() && !Request->HasField(TEXT("profile_id")));
    const auto OtherRed = GetArray(Model->CopyOptions(B), TEXT("solid_color"))[0]->AsNumber();
    const auto FrozenRed =
        GetArray(GetObject(GetObject(Request, TEXT("overrides")), A), TEXT("solid_color"))[0]->AsNumber();
    auto Options = Model->CopyActiveOptions();
    auto Color = GetArray(Options, TEXT("solid_color"));
    Color[0] = MakeShared<FJsonValueNumber>(.9);
    Options->SetArrayField(TEXT("solid_color"), Color);
    Model->Replace(Model->Token(), Options, TEXT("editor_default"));
    TestEqual(TEXT("Nested JSON copy cannot mutate another row"),
              GetArray(Model->CopyOptions(B), TEXT("solid_color"))[0]->AsNumber(), OtherRed);
    TestEqual(TEXT("Nested snapshot is detached"),
              GetArray(GetObject(GetObject(Request, TEXT("overrides")), A), TEXT("solid_color"))[0]->AsNumber(),
              FrozenRed);
    TestEqual(TEXT("Earlier snapshot is detached"),
              GetNumber(GetObject(GetObject(Request, TEXT("overrides")), A), TEXT("resolution")), 512.0);
    const auto Token = Model->Token();
    FCubeShotAutoFitResult Fit;
    Fit.CameraDistance = 321;
    Fit.OrthoWidth = 654;
    Model->SetAutoFit(Token, false, TOptional<FCubeShotAutoFitResult>(Fit));
    TestEqual(TEXT("Explicit auto-fit off adopts derived camera"), Model->Number(TEXT("camera_distance")), 321.0);
    TestEqual(TEXT("Explicit auto-fit off adopts derived width"), Model->Number(TEXT("ortho_width")), 654.0);
    Model->Edit({});
    Model->JsonDraft(Model->Token(), TEXT("{\"resolution\":"));
    TestFalse(TEXT("Invalid default input cannot seed a new row"), Model->Add(TEXT("/Engine/BasicShapes/Plane.Plane")));
    TestTrue(TEXT("Independent valid rows remain executable"), Model->BuildRequest().IsValid());
    TestFalse(TEXT("Invalid active defaults cannot be saved"),
              Model->SaveProfileRequest(TEXT("not_written"), TEXT("No write")).IsValid());
    auto SecondView = Model;
    const auto Count = Model->Targets().Num();
    Model.Reset();
    TestEqual(TEXT("Session owner outlives a view"), SecondView->Targets().Num(), Count);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotDraftRebindTest, "CubeShot.Editor.Workspace.DraftRebind",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCubeShotDraftRebindTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    auto Model = MakeShared<FCubeShotWorkspaceController>();
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    Model->Add(A);
    Model->Add(B);
    Model->Edit(A);
    auto Patch = MakeShared<FJsonObject>();
    Patch->SetStringField(TEXT("background"), TEXT("texture"));
    Model->Patch(Model->Token(), Patch);
    TestEqual(TEXT("Invalid typed input leaves last valid options intact"),
              GetString(Model->CopyOptions(A), TEXT("background")), FString(TEXT("transparent")));
    Model->Edit(B);
    Model->Edit(A);
    FCubeShotOptions Display;
    FText Error;
    TestFalse(TEXT("Rebinding reports the pending texture error"), Model->ReadEditorOptions(Display, Error));
    TestTrue(TEXT("Rebinding restores the typed background mode"),
             Display.BackgroundMode == ECubeShotBackgroundMode::Texture);
    TestFalse(TEXT("The rebound draft has a visible error"), Error.IsEmpty());
    TestFalse(TEXT("Pending invalid draft cannot be submitted"), Model->BuildRequest().IsValid());
    Patch = MakeShared<FJsonObject>();
    Patch->SetNumberField(TEXT("resolution"), 512);
    Model->Patch(Model->Token(), Patch);
    TestEqual(TEXT("Another typed edit does not discard the background draft"), Model->String(TEXT("background")),
              FString(TEXT("texture")));
    Model->ReadEditorOptions(Display, Error);
    TestEqual(TEXT("Rebind also restores other pending typed values"), Display.Resolution, 512);
    TestTrue(TEXT("Rebind retains the invalid background"), Display.BackgroundMode == ECubeShotBackgroundMode::Texture);
    TestTrue(TEXT("Explicit reset recovers the row"), Model->Reset(A));
    TestTrue(TEXT("Reset clears the projected input error"), Model->Error().IsEmpty());
    TestTrue(TEXT("The repaired workspace can build a request"), Model->BuildRequest().IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotSubmissionFlowTest, "CubeShot.Editor.Workspace.SubmissionFlow",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCubeShotSubmissionFlowTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube");
    TArray<FString> Steps;
    FObject Frozen;
    int32 Submissions = 0;
    bool CheckoutAllowed = true, StrictAllowed = true;
    FCubeShotWorkspaceController::FSubmissionServices Services;
    Services.Inspect = [&](const FObject& Request, bool Interactive) {
        Steps.Add(Interactive ? TEXT("inspect") : TEXT("reinspect"));
        if (!Frozen)
            Frozen = Request;
        TestTrue(TEXT("Both inspections use the same request"), Frozen.Get() == Request.Get());
        TestEqual(TEXT("Workspace changes cannot alter captured options"),
                  GetNumber(GetObject(GetObject(Request, TEXT("overrides")), A), TEXT("resolution")), 1024.0);
        auto Plan = Ok();
        Plan->SetBoolField(TEXT("can_proceed"), Interactive || StrictAllowed);
        Plan->SetBoolField(TEXT("requires_overwrite"), true);
        return Plan;
    };
    Services.Checkout = [&](const FObject& Request, FText&, FCubeShotWorkspaceController::FCheckout) {
        Steps.Add(TEXT("checkout"));
        TestTrue(TEXT("Checkout uses the inspected request"), Frozen.Get() == Request.Get());
        TestTrue(TEXT("Approval is only a request argument"), GetBool(Request, TEXT("overwrite")));
        return CheckoutAllowed;
    };
    Services.Submit = [&](const FObject& Request) {
        Steps.Add(TEXT("submit"));
        ++Submissions;
        TestTrue(TEXT("Admission uses the inspected request and UUID"), Frozen.Get() == Request.Get());
        return UnrealToolsRuntime::Error(TEXT("Test backend reached; no real job was created."));
    };
    auto Model = MakeShared<FCubeShotWorkspaceController>(Services);
    Model->Add(A);
    FCubeShotWorkspaceController::FSubmissionInteraction Interaction;
    Interaction.Preparing = [&]() { Steps.Add(TEXT("prepare")); };
    Interaction.ConfirmOverwrite = [&]() {
        Steps.Add(TEXT("approve"));
        TestFalse(TEXT("Reentrant admission is rejected"), Model->Start({}).IsValid());
        auto Patch = MakeShared<FJsonObject>();
        Patch->SetNumberField(TEXT("resolution"), 512);
        Model->Patch(Model->Token(), Patch);
        return true;
    };
    Model->Start(Interaction);
    TestEqual(TEXT("Controller owns the complete admission order"), FString::Join(Steps, TEXT(",")),
              FString(TEXT("prepare,inspect,approve,checkout,reinspect,submit")));
    TestEqual(TEXT("Only one request reaches admission"), Submissions, 1);
    TestFalse(TEXT("Failed admission releases submission state"), Model->IsSubmitting());
    const auto FirstId = GetString(Frozen, TEXT("request_id"));
    Steps.Reset();
    Frozen.Reset();
    Interaction.ConfirmOverwrite = []() { return false; };
    Model->Start(Interaction);
    TestEqual(TEXT("Cancelled approval stops before checkout"), FString::Join(Steps, TEXT(",")),
              FString(TEXT("prepare,inspect")));
    TestTrue(TEXT("A new attempt receives a new UUID"), FirstId != GetString(Frozen, TEXT("request_id")));
    TestFalse(TEXT("Cancellation releases submission state"), Model->IsSubmitting());
    TestEqual(TEXT("Cancellation does not enqueue another job"), Submissions, 1);
    Steps.Reset();
    Frozen.Reset();
    Interaction.ConfirmOverwrite = []() { return true; };
    CheckoutAllowed = false;
    Model->Start(Interaction);
    TestEqual(TEXT("Checkout refusal stops before reinspection and admission"), FString::Join(Steps, TEXT(",")),
              FString(TEXT("prepare,inspect,checkout")));
    TestFalse(TEXT("Checkout refusal releases submission state"), Model->IsSubmitting());
    TestEqual(TEXT("Checkout refusal cannot enqueue a job"), Submissions, 1);
    Steps.Reset();
    Frozen.Reset();
    CheckoutAllowed = true;
    StrictAllowed = false;
    Model->Start(Interaction);
    TestEqual(TEXT("A failed strict reinspection prevents admission"), FString::Join(Steps, TEXT(",")),
              FString(TEXT("prepare,inspect,checkout,reinspect")));
    TestFalse(TEXT("Strict failure releases submission state"), Model->IsSubmitting());
    TestEqual(TEXT("Strict failure cannot enqueue a job"), Submissions, 1);
    Steps.Reset();
    Frozen.Reset();
    StrictAllowed = true;
    Interaction.ConfirmOverwrite = [&]() {
        Model->Shutdown();
        return true;
    };
    Model->Start(Interaction);
    TestEqual(TEXT("Shutdown during approval stops before checkout"), FString::Join(Steps, TEXT(",")),
              FString(TEXT("prepare,inspect")));
    TestEqual(TEXT("Shutdown prevents admission"), Submissions, 1);
    return true;
}
#endif
