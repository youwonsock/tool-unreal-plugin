#include "CubeShotWorkspaceController.h"
#include "CubeShotJobService.h"
#include "Engine/Texture2D.h"
#include "Misc/ScopeExit.h"

using namespace UnrealToolsRuntime;
namespace
{
FObject InitialCubeOptions()
{
    FString Error;
    return CubeShot::Service::Profiles().Resolve(TEXT("editor_default"), CubeShot::Service::DefaultOptions(), Error);
}
} // namespace
FCubeShotWorkspaceController::FCubeShotWorkspaceController(FSubmissionServices Services)
    : FController(InitialCubeOptions(), TEXT("editor_default"), &CubeShot::Service::Normalize,
                  &CubeShot::Service::Profiles, &CubeShot::Service::Jobs)
{
    SubmissionServices = MoveTemp(Services);
    if (!SubmissionServices.Inspect)
        SubmissionServices.Inspect = &CubeShot::Service::InspectBatch;
    if (!SubmissionServices.Checkout)
        SubmissionServices.Checkout = &CubeShot::Service::CheckoutForSubmission;
    if (!SubmissionServices.Submit)
        SubmissionServices.Submit = &CubeShot::Service::Submit;
}
FObject FCubeShotWorkspaceController::Inspect(FObject Request, bool Interactive)
{
    TArray<FString> InputErrors;
    if (!Request)
        Request = BuildRequest(true, &InputErrors);
    if (!Request)
    {
        SetPlan(Error());
        return UnrealToolsRuntime::Error(Error());
    }
    auto Plan = SubmissionServices.Inspect(Request, Interactive);
    FString Text;
    if (!InputErrors.IsEmpty())
    {
        auto Errors = GetArray(Plan, TEXT("errors"));
        Errors.Append(Strings(InputErrors));
        Plan->SetArrayField(TEXT("errors"), Errors);
        Plan->SetBoolField(TEXT("success"), false);
        Plan->SetBoolField(TEXT("can_proceed"), false);
        Plan->SetStringField(TEXT("error"), FString::Join(InputErrors, TEXT("\n")));
    }
    for (const auto& Value : GetArray(Plan, TEXT("items")))
    {
        const auto Item = Value->AsObject();
        Text += GetString(Item, TEXT("mesh_path")) + TEXT("\n");
        if (!GetBool(Item, TEXT("can_proceed")))
            Text += TEXT("ERROR: ") + GetString(Item, TEXT("error")) + TEXT("\n");
        else
            Text += GetBool(Item, TEXT("requires_checkout"))    ? TEXT("Checkout required\n")
                    : GetBool(Item, TEXT("requires_overwrite")) ? TEXT("Overwrite required\n")
                                                                : TEXT("Ready\n");
        for (const TCHAR* Key : {TEXT("outputs"), TEXT("png_outputs"), TEXT("warnings")})
            for (const auto& Entry : GetArray(Item, Key))
                Text += Entry->AsString() + TEXT("\n");
    }
    for (const auto& Value : GetArray(Plan, TEXT("errors")))
        Text += TEXT("ERROR: ") + Value->AsString() + TEXT("\n");
    SetPlan(Text);
    return Plan;
}
FObject FCubeShotWorkspaceController::Start(const FSubmissionInteraction& Interaction, const FString& RetryState)
{
    if (!BeginSubmission())
        return nullptr;
    ON_SCOPE_EXIT
    {
        EndSubmission();
    };
    const auto Request = RetryState.IsEmpty() ? BuildRequest() : RetryRequest(RetryState);
    if (!Request)
    {
        const auto Message = Error();
        if (RetryState.IsEmpty())
            Inspect(nullptr, true);
        return UnrealToolsRuntime::Error(Message);
    }
    if (Interaction.Preparing)
        Interaction.Preparing();
    if (!IsSubmitting())
        return UnrealToolsRuntime::Error(TEXT("Workspace submission ended."), TEXT("EDITOR_INTERRUPTED"));
    const auto Plan = Inspect(Request, true);
    if (!IsSubmitting())
        return UnrealToolsRuntime::Error(TEXT("Workspace submission ended."), TEXT("EDITOR_INTERRUPTED"));
    if (!GetBool(Plan, TEXT("can_proceed")))
        return UnrealToolsRuntime::Error(TEXT("Batch was not submitted. Resolve all preflight errors:\n") +
                                         GetString(Plan, TEXT("error")));
    bool Overwrite = false;
    if (GetBool(Plan, TEXT("requires_overwrite")))
    {
        if (!Interaction.ConfirmOverwrite || !Interaction.ConfirmOverwrite())
            return nullptr;
        Overwrite = true;
    }
    if (!IsSubmitting())
        return UnrealToolsRuntime::Error(TEXT("Workspace submission ended."), TEXT("EDITOR_INTERRUPTED"));
    Request->SetBoolField(TEXT("overwrite"), Overwrite);
    FText CheckoutError;
    if (!SubmissionServices.Checkout(Request, CheckoutError, Interaction.Checkout))
        return UnrealToolsRuntime::Error(CheckoutError.ToString());
    if (!IsSubmitting())
        return UnrealToolsRuntime::Error(TEXT("Workspace submission ended."), TEXT("EDITOR_INTERRUPTED"));
    const auto Strict = Inspect(Request);
    if (!IsSubmitting())
        return UnrealToolsRuntime::Error(TEXT("Workspace submission ended."), TEXT("EDITOR_INTERRUPTED"));
    if (!GetBool(Strict, TEXT("can_proceed")) || (!Overwrite && GetBool(Strict, TEXT("requires_overwrite"))))
        return UnrealToolsRuntime::Error(TEXT("Outputs changed or remain unwritable; inspect again.\n") +
                                         GetString(Strict, TEXT("error")));
    const auto Result = SubmissionServices.Submit(Request);
    SetError(GetString(Result, TEXT("error")));
    if (GetBool(Result, TEXT("success")))
        SelectJob(GetString(Result, TEXT("job_id")));
    return Result;
}
FObject FCubeShotWorkspaceController::SaveProfile(const FConfirmProfileReplacement& Confirm)
{
    const auto Selected = GetProfile(ProfileId());
    const auto Id = GetBool(Selected, TEXT("builtin")) ? TEXT("user_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)
                                                       : ProfileId();
    return SaveActiveProfile(Id, Confirm);
}
bool FCubeShotWorkspaceController::ReadEditorOptions(FCubeShotOptions& Options, FText& Error) const
{
    Error = FText::GetEmpty();
    // Asset loading uses the last valid JSON. Display controls retain typed drafts,
    // including a temporarily invalid texture background with no texture selected.
    bool bLoaded = true;
    if (!IsDefault())
    {
        FCubeShotOptions Loaded;
        bLoaded = CubeShot::Service::LoadOptions(EditingPath(), CopyActiveOptions(), Loaded, Error);
        Options.StaticMesh = Loaded.StaticMesh;
    }
    const auto Json = CopyActiveEditorOptions();
    Options.Resolution = Json->GetIntegerField(TEXT("resolution"));
    Options.ProjectionMode = GetString(Json, TEXT("projection")) == TEXT("perspective")
                                 ? ECubeShotProjectionMode::Perspective
                                 : ECubeShotProjectionMode::Orthographic;
    const auto Background = GetString(Json, TEXT("background"));
    Options.BackgroundMode = Background == TEXT("solid")     ? ECubeShotBackgroundMode::SolidColor
                             : Background == TEXT("texture") ? ECubeShotBackgroundMode::Texture
                                                             : ECubeShotBackgroundMode::Transparent;
    Options.bAutoFit = GetBool(Json, TEXT("auto_fit"));
    Options.CameraDistance = GetNumber(Json, TEXT("camera_distance"));
    Options.FieldOfView = GetNumber(Json, TEXT("field_of_view"));
    Options.OrthoWidth = GetNumber(Json, TEXT("ortho_width"));
    const auto Color = GetArray(Json, TEXT("solid_color"));
    Options.SolidColor =
        FLinearColor(Color[0]->AsNumber(), Color[1]->AsNumber(), Color[2]->AsNumber(), Color[3]->AsNumber());
    const auto Texture = GetString(Json, TEXT("background_texture"));
    Options.BackgroundTexture = Texture.IsEmpty() ? nullptr : LoadObject<UTexture2D>(nullptr, *Texture);
    const auto DraftError = ScopeError(Token().Scope);
    if (!DraftError.IsEmpty())
        Error = FText::FromString(DraftError);
    return bLoaded && DraftError.IsEmpty();
}
void FCubeShotWorkspaceController::CommitEditorOptions(const UnrealToolsWorkspace::FEditToken& Token,
                                                       const FCubeShotOptions& Options, const FObject& PngOptions)
{
    if (!IsCurrent(Token))
        return;
    auto Stored = Options;
    if (Stored.bAutoFit)
    {
        const auto Manual = CopyEditorOptions(Token.Scope);
        Stored.CameraDistance = GetNumber(Manual, TEXT("camera_distance"));
        Stored.OrthoWidth = GetNumber(Manual, TEXT("ortho_width"));
    }
    Patch(Token, Merge(CubeShot::Service::SerializeOptions(Stored), PngOptions));
}

void FCubeShotWorkspaceController::SetAutoFit(const UnrealToolsWorkspace::FEditToken& Token, bool Enabled,
                                              const TOptional<FCubeShotAutoFitResult>& Fit)
{
    auto Values = MakeShared<FJsonObject>();
    Values->SetBoolField(TEXT("auto_fit"), Enabled);
    if (!Enabled && Fit.IsSet())
    {
        Values->SetNumberField(TEXT("camera_distance"), Fit->CameraDistance);
        Values->SetNumberField(TEXT("ortho_width"), Fit->OrthoWidth);
    }
    Patch(Token, Values);
}
