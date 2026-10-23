#pragma once

#include "Misc/Guid.h"
#include "UnrealToolsRuntime.h"
#include "UnrealToolsWorkspace.h"

/** UE adapter for the portable state machine. No Slate or renderer ownership. */
namespace UnrealToolsWorkspace
{
using namespace UnrealToolsRuntime;
struct FJsonCopy
{
    static FObject Clone(const FObject& Value)
    {
        return UnrealToolsRuntime::Clone(Value);
    }
};
using FModel = TModel<FString, FObject, FJsonCopy>;
using FEditToken = FModel::FToken;

struct FTargetView
{
    FString Path, Profile, Error, Json;
    FText Label, ShortLabel;
    bool bSelected = false, bCustom = false;
};
struct FHistoryView
{
    FString JobId, Body, Json;
    FObject Selected;
    FText BodyText, JsonText;
    TArray<FObject> Summaries;
    int32 Offset = 0;
    bool bArchive = false, bDiagnostics = false;
    uint64 Revision = 0, Reads = 0, ResultReads = 0, ResultRevision = 0;
};
struct FOutputPlanView
{
    FString Text, Summary;
    uint64 Revision = 0;
    bool bNeedsInspection = true;
};

class FController
{
public:
    using FNormalize = TFunction<bool(FObject&, FString&)>;
    using FConfirmProfileReplacement = TFunction<bool()>;
    FController(FObject Defaults, FString Builtin, FNormalize InNormalize, TFunction<FProfileStore&()> InProfiles,
                TFunction<FJobService&()> InJobs)
        : Model(Defaults, Builtin), Fallback(MoveTemp(Builtin)), Normalize(MoveTemp(InNormalize)),
          ProfileProvider(MoveTemp(InProfiles)), JobProvider(MoveTemp(InJobs))
    {
    }
    virtual ~FController()
    {
        Shutdown();
    }
    FEditToken Token() const
    {
        return Model.Token();
    }
    FEditToken TokenFor(const FString& Path) const
    {
        return Model.TokenFor(Path);
    }
    bool IsCurrent(const FEditToken& Value) const
    {
        return !bStopped && Model.IsCurrent(Value);
    }
    bool IsStopped() const
    {
        return bStopped;
    }
    bool IsDefault() const
    {
        return !Model.Editing();
    }
    FString EditingPath() const
    {
        return Model.Editing() ? *Model.Editing() : FString();
    }
    uint64 Revision() const
    {
        return Model.Revision();
    }
    FString ProfileId() const
    {
        return Model.Find(Model.Editing())->Profile;
    }
    FObject CopyOptions(const std::optional<FString>& Scope) const
    {
        return Model.CopyOptions(Scope);
    }
    FObject CopyActiveOptions() const
    {
        return CopyOptions(Model.Editing());
    }
    // Only typed GUI drafts are projected into controls. Raw JSON has its own
    // editor and must pass normalization before it becomes typed display data.
    FObject CopyEditorOptions(const std::optional<FString>& Scope) const
    {
        const auto* Draft = Drafts.Find(Scope ? *Scope : FString());
        return Merge(Model.CopyOptions(Scope), Draft ? Draft->Patch : nullptr);
    }
    FObject CopyActiveEditorOptions() const
    {
        return CopyEditorOptions(Model.Editing());
    }
    // Commands can inspect a valid pending JSON patch even when an unrelated
    // typed field prevents the whole row from committing. Never project invalid
    // JSON types into typed controls; this is an edit-boundary copy only.
    FObject CopyEditCandidate(const std::optional<FString>& Scope) const
    {
        auto Options = CopyEditorOptions(Scope);
        const auto* Draft = Drafts.Find(Scope ? *Scope : FString());
        if (Draft && Draft->Json.IsSet())
        {
            const auto Json = Parse(Draft->Json.GetValue());
            auto JsonOptions = Merge(Model.CopyOptions(Scope), Json);
            FString Message;
            if (Json && Normalize(JsonOptions, Message))
                Options = Merge(Options, Json);
        }
        return Draft ? Merge(Options, Draft->AfterJson) : Options;
    }
    const TArray<TSharedPtr<const FTargetView>>& Targets() const
    {
        if (DisplayRevision != Revision())
        {
            DisplayTargets.Reset();
            for (const auto& Row : Model.Rows())
            {
                auto View = MakeShared<FTargetView>();
                View->Path = Row.Path;
                View->Profile = Row.Profile;
                View->bSelected = Row.Selected;
                View->bCustom = Row.Custom;
                const FString Suffix = Row.Custom ? TEXT(" *") : TEXT("");
                View->Label = FText::FromString(Row.Path + Suffix);
                View->ShortLabel = FText::FromString(FPaths::GetBaseFilename(Row.Path) + Suffix);
                View->Json = RenderJson(Row.Path);
                View->Error = ScopeError(Row.Path);
                DisplayTargets.Add(View);
            }
            DisplayRevision = Revision();
            ++DisplayBuilds;
        }
        return DisplayTargets;
    }
    FText TargetLabel(const FString& Path, bool Short = false) const
    {
        for (const auto& Row : Targets())
            if (Row->Path == Path)
                return Short ? Row->ShortLabel : Row->Label;
        return FText::GetEmpty();
    }
    uint64 PresentationBuilds() const
    {
        Targets();
        return DisplayBuilds;
    }
    FString String(const FString& Key) const
    {
        return GetString(ReadField(Key), Key);
    }
    double Number(const FString& Key, double Default = 0) const
    {
        return GetNumber(ReadField(Key), Key, Default);
    }
    bool Bool(const FString& Key) const
    {
        return GetBool(ReadField(Key), Key);
    }
    bool Contains(const FString& Key, const FString& Value) const
    {
        for (const auto& Item : GetArray(ReadField(Key), Key))
            if (Item->AsString() == Value)
                return true;
        return false;
    }
    bool HasArray(const FString& Key) const
    {
        return !GetArray(ReadField(Key), Key).IsEmpty();
    }
    FString ProfileName() const
    {
        return UserName;
    }
    void SetProfileName(const FString& Value)
    {
        UserName = Value;
    }
    FString UserProfileId() const
    {
        return UserId;
    }
    void SetUserProfileId(const FString& Value)
    {
        UserId = Value;
    }
    const FString& Error() const
    {
        if (ErrorModelRevision == Revision() && ErrorOperationRevision == ErrorRevision)
            return ErrorCache;
        TArray<FString> Errors;
        if (!InputError.IsEmpty())
            Errors.Add(InputError);
        const auto Append = [&](const std::optional<FString>& Scope) {
            const auto Message = ScopeError(Scope);
            if (!Message.IsEmpty())
                Errors.Add((Scope ? *Scope : FString(TEXT("Defaults"))) + TEXT(": ") + Message);
        };
        Append({});
        for (const auto& Row : Model.Rows())
            Append(Row.Path);
        ErrorCache = FString::Join(Errors, TEXT("\n"));
        ErrorModelRevision = Revision();
        ErrorOperationRevision = ErrorRevision;
        return ErrorCache;
    }
    void SetError(const FString& Error)
    {
        if (InputError != Error)
        {
            InputError = Error;
            ++ErrorRevision;
        }
    }
    const FString& PlanText() const
    {
        return OutputPlan.Text;
    }
    const FString& PlanSummary() const
    {
        return OutputPlan.Summary;
    }
    const FOutputPlanView& Plan() const
    {
        return OutputPlan;
    }
    void SetPlan(const FString& Text, const FString& Summary = FString())
    {
        OutputPlan.Text = Text;
        OutputPlan.Summary = Summary.IsEmpty() ? Text : Summary;
        OutputPlan.bNeedsInspection = false;
        ++OutputPlan.Revision;
    }
    bool Add(const FString& Path)
    {
        if (Model.Find(Path))
            return true;
        if (!Flush(Model.TokenFor({})))
            return false;
        if (!Model.Add(Path))
            return false;
        InvalidatePlan();
        return true;
    }
    void Edit(const std::optional<FString>& Scope)
    {
        if (bStopped)
            return;
        Flush(Token());
        Model.Edit(Scope);
        ReconcileProfiles();
    }
    void Select(const FString& Path, bool Selected)
    {
        Model.Select(Path, Selected);
    }
    void SelectAll(bool Selected)
    {
        Model.SelectAll(Selected);
    }
    void Move(const FString& Path, int32 Delta)
    {
        Model.Move(Path, Delta);
        InvalidatePlan();
    }
    void Remove(const FString& Path)
    {
        Drafts.Remove(Path);
        Model.Remove(Path);
        InvalidatePlan();
    }
    void RemoveSelected()
    {
        TArray<FString> Paths;
        for (const auto& Row : Model.Rows())
            if (Row.Selected)
                Paths.Add(Row.Path);
        for (const auto& Path : Paths)
            Remove(Path);
    }
    void Clear()
    {
        // Clearing targets must not discard an invalid default draft while leaving
        // its validity flag behind. Defaults belong to the session, not the rows.
        for (const auto& Row : Model.Rows())
            Drafts.Remove(Row.Path);
        Model.Clear();
        InvalidatePlan();
    }
    bool Reset(const FString& Path)
    {
        if (!Flush(Model.TokenFor({})) || !Model.Reset(Path))
            return false;
        Drafts.Remove(Path);
        InvalidatePlan();
        return true;
    }
    bool ApplySelected()
    {
        if (std::none_of(Model.Rows().begin(), Model.Rows().end(), [](const auto& Row) { return Row.Selected; }))
            return true;
        if (!Flush(Token()) || !Model.ApplySelected(Token()))
            return false;
        for (const auto& Row : Model.Rows())
            if (Row.Selected)
                Drafts.Remove(Row.Path);
        InvalidatePlan();
        return true;
    }
    // Text widgets capture the token when they are constructed. OnTextChanged
    // records drafts; focus loss never replays text into a different generation.
    void JsonDraft(const FEditToken& Token, const FString& Text)
    {
        if (!IsCurrent(Token))
            return;
        if (Text == RenderJson(Token.Scope))
            return;
        auto& Draft = DraftFor(Token);
        Draft.Json = Text;
        Draft.AfterJson.Reset(); // A later explicit JSON input wins on its fields.
        Draft.bDirty = true;
        Model.Validate(Token, false);
        InvalidatePlan();
    }
    void Patch(const FEditToken& Token, const FObject& Values)
    {
        PatchDraft(Token, Values, false);
    }
    void PatchAfterJson(const FEditToken& Token, const FObject& Values)
    {
        PatchDraft(Token, Values, true);
    }
    bool Replace(const FEditToken& Token, FObject Options, const FString& Profile, bool Custom = true)
    {
        if (!IsCurrent(Token))
            return false;
        FString Message;
        if (!Normalize(Options, Message))
        {
            SetError(Message);
            return false;
        }
        Model.Replace(Token, Options, Profile, Custom, true);
        Drafts.Remove(ScopeKey(Token));
        InvalidatePlan();
        return true;
    }
    bool Flush(const FEditToken& Token)
    {
        if (!IsCurrent(Token))
            return false;
        auto* Draft = Drafts.Find(ScopeKey(Token));
        if (!Draft || !Draft->bDirty)
            return Model.Find(Token.Scope)->Valid;
        auto Options = Merge(Model.CopyOptions(Token.Scope), Draft->Patch);
        FString Message;
        if (Draft->Json.IsSet())
        {
            const auto Patch = Parse(Draft->Json.GetValue());
            if (!Patch)
                Message = TEXT("Invalid option JSON.");
            else
            {
                // A later typed command may supersede valid JSON fields, but it
                // must not hide a separate semantic error in the raw JSON.
                if (Draft->AfterJson)
                {
                    auto JsonOptions = Merge(Model.CopyOptions(Token.Scope), Patch);
                    if (!Normalize(JsonOptions, Message) && Message.IsEmpty())
                        Message = TEXT("Invalid option JSON.");
                }
                Options = Merge(Options, Patch);
                if (Draft->AfterJson)
                    Options = Merge(Options, Draft->AfterJson);
            }
        }
        if (Message.IsEmpty() && !Normalize(Options, Message) && Message.IsEmpty())
            Message = TEXT("Invalid options.");
        if (!Message.IsEmpty())
        {
            if (Draft->Error != Message)
            {
                Draft->Error = Message;
                ++ErrorRevision;
            }
            Model.Validate(Token, false);
            DisplayRevision = MAX_uint64;
            return false;
        }
        const auto Profile = Model.Find(Token.Scope)->Profile;
        if (RequestHash(Options) == RequestHash(Model.Find(Token.Scope)->Value))
            Model.Validate(Token, true);
        else
            Model.Replace(Token, Options, Profile, true);
        Drafts.Remove(ScopeKey(Token));
        return true;
    }
    FString ScopeError(const std::optional<FString>& Scope) const
    {
        const auto* Draft = Drafts.Find(Scope ? *Scope : FString());
        return Draft ? Draft->Error : FString();
    }
    FString RenderJson(const std::optional<FString>& Scope) const
    {
        const auto* Draft = Drafts.Find(Scope ? *Scope : FString());
        const auto* Row = Model.Find(Scope);
        return Draft && Draft->Json.IsSet() ? Draft->Json.GetValue() : Row ? JsonText(Row->Value) : FString();
    }
    FString DraftText(const std::optional<FString>& Scope) const
    {
        if (Scope)
        {
            for (const auto& Row : Targets())
                if (Row->Path == *Scope)
                    return Row->Json;
        }
        return RenderJson(Scope);
    }
    bool ApplyProfile(const FString& Id, bool PreserveRoot = false)
    {
        if (bStopped)
            return false;
        FString Message;
        auto Options = Profiles().Resolve(Id, Model.CopyOptions({}), Message);
        if (!Options)
        {
            SetError(Message);
            return false;
        }
        if (PreserveRoot)
            Options->SetStringField(TEXT("output_root"),
                                    GetString(Model.Find(Model.Editing())->Value, TEXT("output_root")));
        return Replace(Token(), Options, Id, false);
    }
    void ReconcileProfiles()
    {
        if (bStopped)
            return;
        if (!Profiles().Get(Model.Find({})->Profile))
            Model.ProfileIdentity({}, Fallback);
        for (const auto& Row : Model.Rows())
            if (!Profiles().Get(Row.Profile))
                Model.ProfileIdentity(Row.Path, Fallback);
    }
    FObject SaveProfileRequest(const FString& Id, const FString& Name)
    {
        if (!Flush(Token()))
            return nullptr;
        auto Profile = MakeShared<FJsonObject>();
        Profile->SetNumberField(TEXT("schema_version"), 1);
        Profile->SetStringField(TEXT("provider_id"), GetString(Profiles().Get(Fallback), TEXT("provider_id")));
        Profile->SetStringField(TEXT("id"), Id);
        Profile->SetStringField(TEXT("display_name"), Name);
        Profile->SetObjectField(TEXT("options"), CopyActiveOptions());
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("save"));
        Request->SetObjectField(TEXT("profile"), Profile);
        return Request;
    }
    TArray<FObject> ListProfiles() const
    {
        return bStopped ? TArray<FObject>() : Profiles().GetAll();
    }
    FObject GetProfile(const FString& Id) const
    {
        return bStopped ? nullptr : Profiles().Get(Id);
    }
    FObject SaveActiveProfile(const FString& Id, const FConfirmProfileReplacement& Confirm)
    {
        const auto Source = Token();
        auto Request = SaveProfileRequest(Id, ProfileName());
        if (!Request)
            return nullptr;
        const auto Result = WriteProfile(Request, Confirm, Source);
        if (GetBool(Result, TEXT("success")))
        {
            const auto Profile = GetObject(Result, TEXT("profile"));
            const auto Options = GetObject(Profile, TEXT("options"));
            if (!Options || GetString(Profile, TEXT("id")) != Id)
                return FailOperation(TEXT("Saved profile response has no matching options."), TEXT("IO_ERROR"));
            // The saved request and its source were frozen before a modal prompt.
            // Bind the returned values to that scope, never reread the UI.
            Replace(Source, Clone(Options), Id, false);
        }
        return Result;
    }
    FObject DeleteProfile()
    {
        if (bStopped)
            return FailOperation(TEXT("Workspace has stopped."), TEXT("EDITOR_INTERRUPTED"));
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("delete"));
        Request->SetStringField(TEXT("id"), ProfileId());
        const auto Result = Profiles().Handle(Request);
        SetError(GetString(Result, TEXT("error")));
        ReconcileProfiles(); // Identity changes; stored options and drafts survive.
        return Result;
    }
    FObject ImportProfileText(const FString& Text, const FConfirmProfileReplacement& Confirm)
    {
        auto Profile = Parse(Text);
        if (!Profile)
            return FailOperation(TEXT("Invalid profile JSON."));
        Profile->RemoveField(TEXT("builtin"));
        Profile->RemoveField(TEXT("success"));
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), TEXT("save"));
        Request->SetObjectField(TEXT("profile"), Profile);
        return WriteProfile(Request, Confirm);
    }
    FObject ImportProfileFile(const FString& Filename, const FConfirmProfileReplacement& Confirm)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Filename))
            return FailOperation(TEXT("Cannot read profile file."), TEXT("IO_ERROR"));
        return ImportProfileText(Text, Confirm);
    }
    FObject ExportProfileFile(const FString& Filename)
    {
        auto Profile = GetProfile(ProfileId());
        if (!Profile)
            return FailOperation(TEXT("Profile not found."), TEXT("NOT_FOUND"));
        Profile->RemoveField(TEXT("builtin"));
        Profile->RemoveField(TEXT("success"));
        FString Message;
        if (!AtomicJson(Filename, Profile, Message))
            return FailOperation(Message, TEXT("IO_ERROR"));
        SetError(FString());
        return Ok();
    }
    bool IsJobBusy() const
    {
        return !bStopped && Jobs().IsBusy();
    }
    FObject ControlSelectedJob(const FString& Action)
    {
        if (bStopped)
            return FailOperation(TEXT("Workspace has stopped."), TEXT("EDITOR_INTERRUPTED"));
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("action"), Action);
        Request->SetStringField(TEXT("job_id"), HistoryData.JobId);
        const auto Result = Jobs().Control(Request);
        SetError(GetString(Result, TEXT("error")));
        RefreshHistory(true);
        return Result;
    }
    FObject BuildRequest(bool AllowInvalid = false, TArray<FString>* OutErrors = nullptr)
    {
        if (bStopped)
            return nullptr;
        SetError(FString());
        TArray<FString> Errors;
        for (const auto& Row : Model.Rows())
            if (!Flush(Model.TokenFor(Row.Path)))
                Errors.Add(Row.Path + TEXT(": ") + ScopeError(Row.Path));
        FModel::FSnapshot Snapshot;
        if (OutErrors)
            OutErrors->Append(Errors);
        if (!Errors.IsEmpty() && AllowInvalid)
        {
            for (const auto& Row : Model.Rows())
                Snapshot.emplace_back(Row.Path, FJsonCopy::Clone(Row.Value));
        }
        else if (!Errors.IsEmpty() || !Model.Snapshot(Snapshot))
        {
            if (Errors.IsEmpty())
                SetError(TEXT("Add at least one valid target."));
            return nullptr;
        }
        if (Snapshot.empty())
        {
            SetError(TEXT("Add at least one target."));
            return nullptr;
        }
        auto Request = NewRequest();
        TArray<TSharedPtr<FJsonValue>> Paths;
        auto Overrides = MakeShared<FJsonObject>();
        for (const auto& Item : Snapshot)
        {
            Paths.Add(MakeShared<FJsonValueString>(Item.first));
            Overrides->SetObjectField(Item.first, Item.second);
        }
        Request->SetArrayField(TEXT("mesh_paths"), Paths);
        Request->SetObjectField(TEXT("overrides"), Overrides);
        return Request;
    }
    FObject RetryRequest(const FString& Wanted)
    {
        if (bStopped)
            return nullptr;
        const auto Job = SelectedJob();
        auto Request = NewRequest();
        TArray<TSharedPtr<FJsonValue>> Paths;
        auto Overrides = MakeShared<FJsonObject>();
        for (const auto& Value : GetArray(Job, TEXT("items")))
        {
            const auto Item = Value->AsObject();
            if (GetString(Item, TEXT("state")) == Wanted)
            {
                const auto Path = GetString(Item, TEXT("mesh_path"));
                Paths.Add(MakeShared<FJsonValueString>(Path));
                Overrides->SetObjectField(Path, Clone(GetObject(Item, TEXT("options"))));
            }
        }
        if (Paths.IsEmpty())
        {
            SetError(TEXT("No matching targets to retry."));
            return nullptr;
        }
        Request->SetArrayField(TEXT("mesh_paths"), Paths);
        Request->SetObjectField(TEXT("overrides"), Overrides);
        return Request;
    }
    bool BeginSubmission()
    {
        if (bStopped || bSubmitting)
            return false;
        bSubmitting = true;
        return true;
    }
    void EndSubmission()
    {
        bSubmitting = false;
    }
    bool IsSubmitting() const
    {
        return bSubmitting;
    }
    void AttachView()
    {
        if (bStopped)
            return;
        ++Views;
        RefreshHistory(true);
    }
    void DetachView()
    {
        if (Views > 0 && --Views == 0)
            Unpin();
    }
    void Shutdown()
    {
        if (bStopped)
            return;
        Unpin();
        bStopped = true;
        bSubmitting = false;
    }
    const FHistoryView& History() const
    {
        return HistoryData;
    }
    FObject SelectedJob() const
    {
        return Clone(HistoryData.Selected);
    }
    void SelectJob(const FString& Id)
    {
        HistoryData.JobId = Id;
        RefreshHistory();
    }
    void HistoryMode(bool Archive, bool Diagnostics)
    {
        HistoryData.bArchive = Archive;
        HistoryData.bDiagnostics = Diagnostics;
        HistoryData.Offset = 0;
        HistoryData.JobId.Empty();
        RefreshHistory(true);
    }
    void HistoryPage(int32 Delta)
    {
        HistoryData.Offset = FMath::Max(0, HistoryData.Offset + Delta * 50);
        HistoryData.JobId.Empty();
        RefreshHistory(true);
    }
    void RefreshHistory(bool Force = false)
    {
        if (bStopped)
            return;
        auto& Service = Jobs();
        const auto Revision = Service.GetRevision();
        const auto InstanceGeneration = Service.GetInstanceGeneration();
        // A recreated service may reuse both the address and all job revisions.
        // The old pin disappeared with its owner; never unpin the new service here.
        if (HistoryServiceGeneration != InstanceGeneration)
        {
            ResultCache.Reset();
            ResultOrder.Reset();
            Pinned.Empty();
            HistoryServiceGeneration = InstanceGeneration;
            LastHistoryRevision = MAX_uint64;
        }
        if (!Force && LastHistoryRevision == Revision && PresentedJobId == HistoryData.JobId &&
            (!DisplayedResult || !IsAnnotationCurrent || IsAnnotationCurrent(DisplayedResult->Selected)))
            return;
        const bool RefreshSummaries = Force || LastHistoryRevision != Revision;
        LastHistoryRevision = Revision;
        ++HistoryData.Reads;
        ++HistoryData.Revision;
        if (RefreshSummaries)
            HistoryData.Summaries = HistoryData.bDiagnostics
                                        ? Service.GetDiagnosticSummaries(HistoryData.Offset, 50)
                                        : Service.GetJobSummaries(HistoryData.Offset, 50, HistoryData.bArchive);
        if (HistoryData.JobId.IsEmpty() && !HistoryData.Summaries.IsEmpty())
            HistoryData.JobId = GetString(HistoryData.Summaries[0], TEXT("job_id"));
        auto Result = ReadHistoryResult(Service, HistoryData.JobId, Force);
        if (Result && !GetBool(Result->Selected, TEXT("success")) && !HistoryData.Summaries.IsEmpty())
        {
            HistoryData.JobId = GetString(HistoryData.Summaries[0], TEXT("job_id"));
            Result = ReadHistoryResult(Service, HistoryData.JobId, Force);
        }
        if (DisplayedResult != Result)
        {
            ++HistoryData.ResultRevision;
            DisplayedResult = Result;
            HistoryData.Selected = Result ? Result->Selected : nullptr;
            HistoryData.Json = Result ? Result->Json : FString();
            HistoryData.Body = Result ? Result->Body : FString();
            HistoryData.BodyText = Result ? Result->BodyText : FText::GetEmpty();
            HistoryData.JsonText = Result ? Result->JsonText : FText::GetEmpty();
        }
        PresentedJobId = HistoryData.JobId;
        if (Views && Pinned != HistoryData.JobId)
        {
            Unpin();
            Pinned = HistoryData.JobId;
            if (!Pinned.IsEmpty())
                Jobs().PinJob(Pinned);
        }
    }
    TFunction<void(const FObject&)> Annotate;
    // Providers can cheaply check external dependencies without rereading or
    // serializing a job. A stale annotation rebuilds a fresh service snapshot.
    TFunction<bool(const FObject&)> IsAnnotationCurrent;
    static FString FormatResult(const FObject& Job)
    {
        if (!Job)
            return FString();
        FString Text;
        if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic"))
        {
            Text = TEXT("Diagnostic ID: ") + GetString(Job, TEXT("job_id")) + TEXT("\nPhase: ") +
                   GetString(Job, TEXT("phase"));
            Text += GetString(Job, TEXT("phase")) == TEXT("acknowledged")
                        ? TEXT("\nMetadata review acknowledged. Preserved raw evidence remains available.\n")
                        : TEXT("\nMetadata review required. Review the source metadata and preserved raw evidence "
                               "before acknowledging recovery.\n");
            Text += TEXT("\nSource metadata: ") + GetString(Job, TEXT("source_path"));
            const auto Raw = GetString(Job, TEXT("raw_path"));
            Text += Raw.IsEmpty()
                        ? FString(TEXT(
                              "\nRaw evidence: Not created yet. Inspect the complete JSON before acknowledgement."))
                        : TEXT("\nPreserved raw evidence: ") + Raw;
            Text += TEXT("\nError: ") + GetString(Job, TEXT("error_code")) + TEXT(" ") + GetString(Job, TEXT("error"));
            Text += TEXT("\nReview metadata and preserved raw evidence. Output files, staging and backups are "
                         "untouched by acknowledgement.\n");
            return Text;
        }
        const auto Progress = GetObject(Job, TEXT("progress"));
        Text = FString::Printf(TEXT("%s · %s\nCompleted %.0f / %.0f\n"), *GetString(Job, TEXT("state")),
                               *GetString(Job, TEXT("stage")), GetNumber(Progress, TEXT("completed")),
                               GetNumber(Progress, TEXT("total")));
        const auto State = GetString(Job, TEXT("state"));
        if (GetBool(Job, TEXT("cancel_requested")) && (State == TEXT("queued") || State == TEXT("running")))
            Text += TEXT("Cancellation requested; native work must reach a safe boundary.\n");
        if (GetBool(Job, TEXT("recovery_required")))
            Text +=
                TEXT("Recovery review required. Resolve dirty packages and matching diagnostics before retrying.\n");
        if (!GetString(Job, TEXT("error")).IsEmpty())
            Text += TEXT("Error: ") + GetString(Job, TEXT("error")) + TEXT("\n");
        for (const auto& Value : GetArray(Job, TEXT("items")))
        {
            const auto Item = Value->AsObject(), Result = GetObject(Item, TEXT("result"));
            Text += GetString(Item, TEXT("mesh_path")) + TEXT(" · ") + GetString(Item, TEXT("state")) + TEXT("\n");
            Text +=
                GetBool(Item, TEXT("overwrite")) ? TEXT("Overwrite allowed: yes\n") : TEXT("Overwrite allowed: no\n");
            if (Result && Result->HasField(TEXT("staged_faces")))
                Text += FString::Printf(TEXT("Staged directions: %.0f / 6\n"), GetNumber(Result, TEXT("staged_faces")));
            for (const TCHAR* Key : {TEXT("saved"), TEXT("png_paths"), TEXT("files"), TEXT("failed"),
                                     TEXT("dirty_packages"), TEXT("warnings"), TEXT("errors")})
                for (const auto& Entry : GetArray(Result, Key))
                    Text += FString(Key) + TEXT(": ") + Entry->AsString() + TEXT("\n");
            for (const TCHAR* Key : {TEXT("error"), TEXT("recovery_staging"), TEXT("recovery_backup"),
                                     TEXT("recovery_journal"), TEXT("staging_directory")})
                if (!GetString(Result, Key).IsEmpty())
                    Text += FString(Key) + TEXT(": ") + GetString(Result, Key) + TEXT("\n");
            if (!GetString(Item, TEXT("reason")).IsEmpty())
                Text += TEXT("Reason: ") + GetString(Item, TEXT("reason")) + TEXT("\n");
        }
        return Text;
    }

protected:
    FProfileStore& Profiles() const
    {
        return ProfileProvider();
    }
    FJobService& Jobs() const
    {
        return JobProvider();
    }
    void InvalidatePlan()
    {
        OutputPlan.Text = OutputPlan.Summary = TEXT("Options changed. Inspect outputs again.");
        OutputPlan.bNeedsInspection = true;
        ++OutputPlan.Revision;
        DisplayRevision = MAX_uint64;
    }

private:
    struct FHistoryResult
    {
        uint64 Revision = 0;
        FObject Selected;
        FString Body, Json;
        FText BodyText, JsonText;
    };
    TSharedPtr<const FHistoryResult> ReadHistoryResult(FJobService& Service, const FString& Id, bool Force)
    {
        if (Id.IsEmpty())
            return nullptr;
        const auto Revision = Service.GetJobRevision(Id);
        const auto* Existing = ResultCache.Find(Id);
        if (!Force && Existing && (*Existing)->Revision == Revision &&
            (!IsAnnotationCurrent || IsAnnotationCurrent((*Existing)->Selected)))
        {
            ResultOrder.Remove(Id);
            ResultOrder.Add(Id);
            return *Existing;
        }
        auto Selector = MakeShared<FJsonObject>();
        Selector->SetStringField(TEXT("job_id"), Id);
        auto Result = MakeShared<FHistoryResult>();
        Result->Revision = Revision;
        Result->Selected = Service.Status(Selector);
        ++HistoryData.ResultReads;
        if (Annotate && Result->Selected)
            Annotate(Result->Selected);
        Result->Json = Result->Selected ? JsonText(Result->Selected) : FString();
        Result->Body = FormatResult(Result->Selected);
        Result->BodyText = FText::FromString(Result->Body);
        Result->JsonText = FText::FromString(Result->Json);
        // BUSY/NOT_FOUND may resolve without a retained per-job revision.
        if (GetBool(Result->Selected, TEXT("success")))
        {
            ResultCache.Add(Id, Result);
            ResultOrder.Remove(Id);
            ResultOrder.Add(Id);
            while (ResultOrder.Num() > 20)
            {
                ResultCache.Remove(ResultOrder[0]);
                ResultOrder.RemoveAt(0);
            }
        }
        return Result;
    }
    void PatchDraft(const FEditToken& Token, const FObject& Values, bool AfterJson)
    {
        if (!IsCurrent(Token))
            return;
        const auto* Existing = Drafts.Find(ScopeKey(Token));
        if (!Existing &&
            RequestHash(Merge(Model.CopyOptions(Token.Scope), Values)) == RequestHash(Model.Find(Token.Scope)->Value))
            return;
        auto& Draft = DraftFor(Token);
        Draft.Patch = Merge(Draft.Patch, Values);
        if (AfterJson && Draft.Json.IsSet())
            Draft.AfterJson = Merge(Draft.AfterJson, Values);
        Draft.bDirty = true;
        Flush(Token);
        InvalidatePlan();
    }
    FObject FailOperation(const FString& Message, const FString& Code = TEXT("INVALID_ARGUMENT"))
    {
        SetError(Message);
        return UnrealToolsRuntime::Error(Message, Code);
    }
    FObject WriteProfile(FObject Request, const FConfirmProfileReplacement& Confirm,
                         const std::optional<FEditToken>& Source = std::nullopt)
    {
        if (bStopped || bChangingProfile)
            return FailOperation(TEXT("Workspace profile operation is unavailable."), TEXT("EDITOR_INTERRUPTED"));
        TGuardValue<bool> Guard(bChangingProfile, true);
        Request = Clone(Request);
        const auto Profile = GetObject(Request, TEXT("profile"));
        FString Message;
        if (!Profiles().ValidateUserProfile(Profile, Message))
            return FailOperation(Message);
        if (Profiles().UserFileExists(GetString(Profile, TEXT("id"))))
        {
            if (!Confirm || !Confirm())
                return nullptr;
            Request->SetBoolField(TEXT("overwrite"), true);
        }
        if (bStopped || (Source && !IsCurrent(*Source)))
            return FailOperation(TEXT("Workspace changed during profile confirmation."), TEXT("EDITOR_INTERRUPTED"));
        const auto Result = Profiles().Handle(Request);
        SetError(GetString(Result, TEXT("error")));
        ReconcileProfiles();
        return Result;
    }
    struct FDraft
    {
        FEditToken Token;
        TOptional<FString> Json;
        FObject Patch, AfterJson;
        FString Error;
        bool bDirty = false;
    };
    static FString ScopeKey(const FEditToken& Token)
    {
        return Token.Scope ? *Token.Scope : FString();
    }
    FDraft& DraftFor(const FEditToken& Token)
    {
        auto& Draft = Drafts.FindOrAdd(ScopeKey(Token));
        if (Draft.Token.Generation != Token.Generation)
        {
            Draft.Token = Token;
        }
        return Draft;
    }
    const FObject& ReadField(const FString& Key) const
    {
        const auto* Draft = Drafts.Find(EditingPath());
        if (Draft && Draft->Patch && Draft->Patch->HasField(Key))
            return Draft->Patch;
        return Model.Find(Model.Editing())->Value;
    }
    static FObject NewRequest()
    {
        auto Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        Request->SetObjectField(TEXT("options"), MakeShared<FJsonObject>());
        Request->SetBoolField(TEXT("overwrite"), false);
        return Request;
    }
    void Unpin()
    {
        if (!Pinned.IsEmpty())
        {
            auto& Service = Jobs();
            // Detach/shutdown can precede the next history refresh after a service
            // replacement. Do not decrement another observer's pin in that case.
            if (Service.GetInstanceGeneration() == HistoryServiceGeneration)
                Service.UnpinJob(Pinned);
            Pinned.Empty();
        }
    }
    FModel Model;
    FString Fallback, InputError, Pinned, UserName = TEXT("My Profile"), UserId = TEXT("my_profile");
    FOutputPlanView OutputPlan;
    FNormalize Normalize;
    // MCP registration can recreate the CubeShot services within one Editor session.
    // Resolve them at each operation rather than retaining references to a previous registration.
    TFunction<FProfileStore&()> ProfileProvider;
    TFunction<FJobService&()> JobProvider;
    TMap<FString, FDraft> Drafts;
    FHistoryView HistoryData;
    TMap<FString, TSharedPtr<const FHistoryResult>> ResultCache;
    TArray<FString> ResultOrder;
    TSharedPtr<const FHistoryResult> DisplayedResult;
    uint64 HistoryServiceGeneration = 0;
    FString PresentedJobId;
    uint64 LastHistoryRevision = MAX_uint64;
    uint64 ErrorRevision = 0;
    int32 Views = 0;
    bool bStopped = false, bSubmitting = false, bChangingProfile = false;
    mutable uint64 DisplayRevision = MAX_uint64, DisplayBuilds = 0;
    mutable uint64 ErrorModelRevision = MAX_uint64, ErrorOperationRevision = MAX_uint64;
    mutable FString ErrorCache;
    mutable TArray<TSharedPtr<const FTargetView>> DisplayTargets;
};
} // namespace UnrealToolsWorkspace
