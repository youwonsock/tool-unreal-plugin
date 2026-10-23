#pragma once

// Canonical source: tool-unreal-plugin/Shared/Native/UnrealToolsRuntime.h.
// Scripts/sync_runtime.py vendors this header into each independent Editor plugin.
#include "CoreMinimal.h"
#include "UnrealToolsServiceInstance.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#ifdef LoadImage
#undef LoadImage
#endif
#ifdef GetObject
#undef GetObject
#endif
#endif

namespace UnrealToolsRuntime
{
using FObject = TSharedPtr<FJsonObject>;

inline FString GetString(const FObject& Object, const FString& Key, const FString& Default = FString())
{
    FString Value;
    return Object.IsValid() && Object->TryGetStringField(Key, Value) ? Value : Default;
}
inline bool GetBool(const FObject& Object, const FString& Key, const bool Default = false)
{
    bool Value;
    return Object.IsValid() && Object->TryGetBoolField(Key, Value) ? Value : Default;
}
inline double GetNumber(const FObject& Object, const FString& Key, const double Default = 0)
{
    double Value;
    return Object.IsValid() && Object->TryGetNumberField(Key, Value) ? Value : Default;
}
inline FObject GetObject(const FObject& Object, const FString& Key)
{
    const FObject* Value = nullptr;
    return Object.IsValid() && Object->TryGetObjectField(Key, Value) ? *Value : nullptr;
}
inline TArray<TSharedPtr<FJsonValue>> GetArray(const FObject& Object, const FString& Key)
{
    const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
    return Object.IsValid() && Object->TryGetArrayField(Key, Value) ? *Value : TArray<TSharedPtr<FJsonValue>>();
}
inline FString JsonText(const FObject& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
    return Text;
}
inline FObject Parse(const FString& Text)
{
    FObject Object;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
    return Object;
}
inline FObject Clone(const FObject& Object)
{
    return Object.IsValid() ? Parse(JsonText(Object)) : MakeShared<FJsonObject>();
}
inline FObject Ok()
{
    FObject Object = MakeShared<FJsonObject>();
    Object->SetBoolField(TEXT("success"), true);
    return Object;
}
inline FObject Error(const FString& Message, const FString& Code = TEXT("INVALID_ARGUMENT"))
{
    FObject Object = MakeShared<FJsonObject>();
    Object->SetBoolField(TEXT("success"), false);
    Object->SetStringField(TEXT("error"), Message);
    Object->SetStringField(TEXT("error_code"), Code);
    return Object;
}
inline TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
    return Result;
}
inline bool CheckFields(const FObject& Object, const TArray<FString>& Allowed, FString& OutError)
{
    if (!Object.IsValid()) { OutError = TEXT("Expected an arguments object."); return false; }
    for (const auto& Pair : Object->Values)
    {
        const FString Key(FStringView(Pair.Key));
        if (!Allowed.Contains(Key)) { OutError = TEXT("Unknown argument: ") + Key; return false; }
    }
    return true;
}
inline FObject Merge(const FObject& Base, const FObject& Override)
{
    FObject Result = Clone(Base);
    if (Override.IsValid())
        for (const auto& Pair : Override->Values)
        {
            const FString Key(FStringView(Pair.Key));
            if (Pair.Value->Type == EJson::Object && Result->HasTypedField<EJson::Object>(Key))
                Result->SetObjectField(Key, Merge(GetObject(Result, Key), Pair.Value->AsObject()));
            else Result->SetField(Key, Pair.Value);
        }
    return Clone(Result);
}
inline TSharedPtr<FJsonValue> CanonicalValue(const TSharedPtr<FJsonValue>& Value)
{
    if (Value->Type == EJson::Object)
    {
        FObject Sorted = MakeShared<FJsonObject>();
        TArray<FString> Keys;
        for (const auto& Pair : Value->AsObject()->Values) Keys.Add(FString(FStringView(Pair.Key)));
        Keys.Sort();
        for (const FString& Key : Keys) Sorted->SetField(Key, CanonicalValue(Value->AsObject()->TryGetField(Key)));
        return MakeShared<FJsonValueObject>(Sorted);
    }
    if (Value->Type == EJson::Array)
    {
        TArray<TSharedPtr<FJsonValue>> Sorted;
        for (const auto& Child : Value->AsArray()) Sorted.Add(CanonicalValue(Child));
        return MakeShared<FJsonValueArray>(Sorted);
    }
    return Value;
}
inline FString RequestHash(const FObject& Object)
{
    const FString Text = JsonText(CanonicalValue(MakeShared<FJsonValueObject>(Object))->AsObject());
    FTCHARToUTF8 Bytes(*Text);
    FSHAHash Hash;
    FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Hash.Hash);
    return Hash.ToString();
}
inline FString CanonicalPath(FString Path)
{
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    FPaths::CollapseRelativeDirectories(Path);
    while (Path.EndsWith(TEXT("/"))) Path.LeftChopInline(1);
    return Path.ToLower();
}
inline bool PathsOverlap(const FString& First, const FString& Second)
{
    const FString A = CanonicalPath(First), B = CanonicalPath(Second);
    return A == B || A.StartsWith(B + TEXT("/")) || B.StartsWith(A + TEXT("/"));
}
#if PLATFORM_WINDOWS
inline FString WindowsFilePath(const FString& Path, bool bForceExtended = false)
{
    FString Full = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Full); FPaths::CollapseRelativeDirectories(Full);
    Full.ReplaceInline(TEXT("/"), TEXT("\\"));
    // Match the engine platform file's extended-path normalization. Keep the
    // logical (slash-separated) paths in journals and use this only at Win32 IO.
    if ((bForceExtended || Full.Len() >= MAX_PATH) && !Full.StartsWith(TEXT("\\\\?\\")))
        Full = Full.StartsWith(TEXT("\\\\")) ? TEXT("\\\\?\\UNC\\") + Full.Mid(2) : TEXT("\\\\?\\") + Full;
    return Full;
}
#endif
inline bool AtomicReplace(const FString& Temporary, const FString& Destination, FString& OutError)
{
#if PLATFORM_WINDOWS
    const FString Source = WindowsFilePath(Temporary);
    const FString Target = WindowsFilePath(Destination);
    if (::MoveFileExW(*Source, *Target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    OutError = FString::Printf(TEXT("Atomic file replacement failed (%lu): %s"), ::GetLastError(), *Target);
    return false;
#else
    if (IFileManager::Get().FileExists(*Destination))
    { OutError = TEXT("Replacing existing files is supported on Win64 only."); return false; }
    if (IFileManager::Get().Move(*Destination, *Temporary, false, false)) return true;
    OutError = TEXT("File rename failed: ") + Destination;
    return false;
#endif
}
inline bool AtomicJson(const FString& Filename, const FObject& Object, FString& OutError)
{
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
    { OutError = TEXT("Cannot create record directory: ") + Filename; return false; }
    const FString Temporary = Filename + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(JsonText(Object), *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { OutError = TEXT("Cannot write record: ") + Temporary; return false; }
    const bool Success = AtomicReplace(Temporary, Filename, OutError);
    if (!Success) IFileManager::Get().Delete(*Temporary, false, true);
    return Success;
}
inline FObject OutputSchema()
{
    return Parse(TEXT(R"({"type":"object","required":["success"],"properties":{"success":{"type":"boolean"}},"oneOf":[{"properties":{"success":{"const":true}}},{"required":["error"],"properties":{"success":{"const":false},"error":{"type":"string"},"error_code":{"type":"string"}}}],"additionalProperties":true})"));
}
inline bool ValidateSchema(const TSharedPtr<FJsonValue>& Value, const FObject& Schema, FString& OutError, const FString& Path = TEXT("$"))
{
    if (!Value.IsValid() || !Schema.IsValid()) { OutError = Path + TEXT(": missing value or schema."); return false; }
    auto Fail = [&OutError, &Path](const FString& Message) { OutError = Path + TEXT(": ") + Message; return false; };
    const FString Type = GetString(Schema, TEXT("type"));
    if ((Type == TEXT("object") && Value->Type != EJson::Object) ||
        (Type == TEXT("array") && Value->Type != EJson::Array) ||
        (Type == TEXT("string") && Value->Type != EJson::String) ||
        (Type == TEXT("boolean") && Value->Type != EJson::Boolean) ||
        ((Type == TEXT("number") || Type == TEXT("integer")) && Value->Type != EJson::Number) ||
        (Type == TEXT("null") && Value->Type != EJson::Null)) return Fail(TEXT("expected ") + Type);
    auto ValueHash = [](const TSharedPtr<FJsonValue>& Candidate)
    { FObject Wrapper = MakeShared<FJsonObject>(); Wrapper->SetField(TEXT("value"), Candidate); return RequestHash(Wrapper); };
    if (Schema->HasField(TEXT("const")) && ValueHash(Value) != ValueHash(Schema->TryGetField(TEXT("const")))) return Fail(TEXT("value does not match const."));
    if (Schema->HasTypedField<EJson::Array>(TEXT("enum")))
    {
        const FString Hash = ValueHash(Value);
        if (!GetArray(Schema, TEXT("enum")).ContainsByPredicate([&](const auto& Candidate) { return ValueHash(Candidate) == Hash; })) return Fail(TEXT("value is outside enum."));
    }
    for (const FString& Keyword : {FString(TEXT("oneOf")), FString(TEXT("anyOf"))})
    {
        const auto Branches = GetArray(Schema, Keyword);
        if (Branches.IsEmpty()) continue;
        int32 Matches = 0;
        for (const auto& Branch : Branches)
        { FString Ignore; if (Branch->Type == EJson::Object && ValidateSchema(Value, Branch->AsObject(), Ignore, Path)) ++Matches; }
        if (Matches == 0 || (Keyword == TEXT("oneOf") && Matches != 1)) return Fail(TEXT("value does not satisfy ") + Keyword);
    }
    if (Value->Type == EJson::Number)
    {
        const double Number = Value->AsNumber();
        if (!FMath::IsFinite(Number)) return Fail(TEXT("number must be finite."));
        if (Type == TEXT("integer") && FMath::FloorToDouble(Number) != Number) return Fail(TEXT("number must be integral."));
        if (Schema->HasField(TEXT("minimum")) && Number < GetNumber(Schema, TEXT("minimum"))) return Fail(TEXT("number is below minimum."));
        if (Schema->HasField(TEXT("maximum")) && Number > GetNumber(Schema, TEXT("maximum"))) return Fail(TEXT("number is above maximum."));
    }
    if (Value->Type == EJson::String)
    {
        const FString String = Value->AsString();
        if (String.Len() < GetNumber(Schema, TEXT("minLength"))) return Fail(TEXT("string is too short."));
        if (Schema->HasField(TEXT("maxLength")) && String.Len() > GetNumber(Schema, TEXT("maxLength"))) return Fail(TEXT("string is too long."));
        const FString Pattern = GetString(Schema, TEXT("pattern"));
        if (!Pattern.IsEmpty()) { FRegexMatcher Matcher(FRegexPattern(Pattern), String); if (!Matcher.FindNext()) return Fail(TEXT("string does not match pattern.")); }
    }
    if (Value->Type == EJson::Array)
    {
        const auto& Array = Value->AsArray();
        if (Array.Num() < GetNumber(Schema, TEXT("minItems"))) return Fail(TEXT("array is too short."));
        if (Schema->HasField(TEXT("maxItems")) && Array.Num() > GetNumber(Schema, TEXT("maxItems"))) return Fail(TEXT("array is too long."));
        TSet<FString> Seen;
        const bool Unique = GetBool(Schema, TEXT("uniqueItems"));
        FObject ItemSchema = GetObject(Schema, TEXT("items"));
        for (int32 Index = 0; Index < Array.Num(); ++Index)
        {
            const FString Hash = ValueHash(Array[Index]);
            if (Unique && Seen.Contains(Hash)) return Fail(TEXT("array contains duplicate items."));
            Seen.Add(Hash);
            if (ItemSchema.IsValid() && !ValidateSchema(Array[Index], ItemSchema, OutError, Path + FString::Printf(TEXT("[%d]"), Index))) return false;
        }
    }
    if (Value->Type == EJson::Object)
    {
        FObject Object = Value->AsObject(), Properties = GetObject(Schema, TEXT("properties"));
        for (const auto& Required : GetArray(Schema, TEXT("required")))
            if (!Object->HasField(Required->AsString())) return Fail(TEXT("missing required field ") + Required->AsString());
        const bool AllowUnknown = GetBool(Schema, TEXT("additionalProperties"), true);
        const FObject ExtraSchema = GetObject(Schema, TEXT("additionalProperties"));
        for (const auto& Pair : Object->Values)
        {
            const FString Key(FStringView(Pair.Key));
            FObject ChildSchema = GetObject(Properties, Key);
            if (!ChildSchema.IsValid())
            {
                if (!AllowUnknown) return Fail(TEXT("unknown field ") + Key);
                ChildSchema = ExtraSchema;
            }
            if (ChildSchema.IsValid() && !ValidateSchema(Pair.Value, ChildSchema, OutError, Path + TEXT(".") + Key)) return false;
        }
    }
    return true;
}
inline bool ValidateSchema(const FObject& Value, const FObject& Schema, FString& OutError)
{
    return Value.IsValid() ? ValidateSchema(MakeShared<FJsonValueObject>(Value), Schema, OutError) : (OutError = TEXT("Expected an object."), false);
}
inline bool ProfileIdValid(const FString& Id)
{
    if (Id.IsEmpty() || Id.Len() > 64) return false;
    for (const TCHAR Char : Id)
        if (!(Char >= TEXT('a') && Char <= TEXT('z')) && !(Char >= TEXT('0') && Char <= TEXT('9')) && Char != TEXT('_') && Char != TEXT('-')) return false;
    return true;
}

class FProfileStore
{
public:
    using FValidator = TFunction<bool(FObject&, FString&)>;
    FProfileStore(FString InProvider, FString PluginName)
        : Provider(MoveTemp(InProvider)), Root(FPaths::ProjectSavedDir() / PluginName / TEXT("Profiles")) {}
    void SetValidator(FValidator InValidator) { Validator = MoveTemp(InValidator); }
    void AddBuiltin(const FString& Id, const FString& Name, const FObject& Options)
    {
        FObject Profile = Ok();
        Profile->SetNumberField(TEXT("schema_version"), 1);
        Profile->SetStringField(TEXT("provider_id"), Provider);
        Profile->SetStringField(TEXT("id"), Id);
        Profile->SetStringField(TEXT("display_name"), Name);
        Profile->SetObjectField(TEXT("options"), Clone(Options));
        Profile->SetBoolField(TEXT("builtin"), true);
        Builtins.Add(Id, Profile);
    }
    FObject Get(const FString& Id) const
    {
        if (const FObject* Found = Builtins.Find(Id)) return Clone(*Found);
        if (!ProfileIdValid(Id)) return nullptr;
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *(Root / (Id + TEXT(".json"))))) return nullptr;
        FObject Profile = Parse(Text);
        if (!Profile.IsValid() || GetString(Profile, TEXT("id")) != Id ||
            GetString(Profile, TEXT("provider_id")) != Provider || GetNumber(Profile, TEXT("schema_version")) != 1 ||
            !Profile->HasTypedField<EJson::String>(TEXT("display_name")) || GetString(Profile, TEXT("display_name")).TrimStartAndEnd().IsEmpty()) return nullptr;
        FObject Options = Clone(GetObject(Profile, TEXT("options")));
        if (!Profile->HasTypedField<EJson::Object>(TEXT("options")) || Options->HasField(TEXT("overwrite"))) return nullptr;
        FString Message;
        if (Validator && !Validator(Options, Message)) return nullptr;
        Profile->SetObjectField(TEXT("options"), Clone(Options));
        Profile->SetBoolField(TEXT("builtin"), false);
        return Profile;
    }
    FObject Resolve(const FString& Id, const FObject& Defaults, FString& OutError) const
    {
        if (Id.IsEmpty()) return Clone(Defaults);
        FObject Profile = Get(Id);
        if (!Profile.IsValid()) { OutError = TEXT("Profile not found or unsupported: ") + Id; return nullptr; }
        FObject Options = GetObject(Profile, TEXT("options"));
        if (!Options.IsValid()) { OutError = TEXT("Profile has no options object."); return nullptr; }
        return Merge(Defaults, Options);
    }
    TArray<FObject> GetAll() const
    {
        TArray<FObject> Result;
        TArray<FString> Ids;
        Builtins.GetKeys(Ids);
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *(Root / TEXT("*.json")), true, false);
        for (const FString& File : Files) Ids.AddUnique(FPaths::GetBaseFilename(File));
        Ids.Sort();
        for (const FString& Id : Ids) if (FObject Profile = Get(Id)) Result.Add(Profile);
        return Result;
    }
    FObject Handle(const FObject& Args)
    {
        FString Message;
        if (!CheckFields(Args, {TEXT("action"), TEXT("id"), TEXT("profile"), TEXT("overwrite")}, Message)) return Error(Message);
        const FString Action = GetString(Args, TEXT("action"));
        if (Action == TEXT("list"))
        {
            FObject Reply = Ok();
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const FObject& Profile : GetAll()) Values.Add(MakeShared<FJsonValueObject>(Profile));
            Reply->SetArrayField(TEXT("profiles"), Values);
            return Reply;
        }
        if (Action == TEXT("get"))
        {
            FObject Profile = Get(GetString(Args, TEXT("id")));
            if (!Profile.IsValid()) return Error(TEXT("Profile not found."), TEXT("NOT_FOUND"));
            FObject Reply = Ok(); Reply->SetObjectField(TEXT("profile"), Profile); return Reply;
        }
        if (Action == TEXT("delete"))
        {
            const FString Id = GetString(Args, TEXT("id"));
            if (!ProfileIdValid(Id) || Builtins.Contains(Id)) return Error(TEXT("Built-in or invalid profile cannot be deleted."));
            if (!Get(Id).IsValid()) return Error(TEXT("Profile not found."), TEXT("NOT_FOUND"));
            if (!IFileManager::Get().Delete(*(Root / (Id + TEXT(".json"))), false, true)) return Error(TEXT("Profile deletion failed."), TEXT("IO_ERROR"));
            return Ok();
        }
        if (Action != TEXT("save")) return Error(TEXT("action must be list, get, save or delete."));
        FObject Profile = Clone(GetObject(Args, TEXT("profile")));
        if (!CheckFields(Profile, {TEXT("schema_version"), TEXT("provider_id"), TEXT("id"), TEXT("display_name"), TEXT("options")}, Message)) return Error(Message);
        const FString Id = GetString(Profile, TEXT("id"));
        if (!ProfileIdValid(Id) || Builtins.Contains(Id)) return Error(TEXT("A user profile needs a valid non-built-in id."));
        if (GetNumber(Profile, TEXT("schema_version")) != 1 || GetString(Profile, TEXT("provider_id")) != Provider || GetString(Profile, TEXT("display_name")).IsEmpty())
            return Error(TEXT("Profile needs schema_version=1, matching provider_id and display_name."));
        FObject Options = GetObject(Profile, TEXT("options"));
        if (!Options.IsValid() || Options->HasField(TEXT("overwrite"))) return Error(TEXT("Profile options must be an object without overwrite approval."));
        if (Validator && !Validator(Options, Message)) return Error(Message);
        Profile->SetObjectField(TEXT("options"), Clone(Options));
        if (Args->HasField(TEXT("overwrite")) && !Args->HasTypedField<EJson::Boolean>(TEXT("overwrite"))) return Error(TEXT("overwrite must be boolean."));
        if (Get(Id).IsValid() && !GetBool(Args, TEXT("overwrite"))) return Error(TEXT("Profile exists; explicit overwrite is required."), TEXT("OUTPUT_CONFLICT"));
        if (!AtomicJson(Root / (Id + TEXT(".json")), Profile, Message)) return Error(Message, TEXT("IO_ERROR"));
        FObject Reply = Ok(); Reply->SetObjectField(TEXT("profile"), Get(Id)); return Reply;
    }
private:
    FString Provider, Root;
    TMap<FString, FObject> Builtins;
    FValidator Validator;
};

enum class EExecutionMode : uint8 { Unattended, Interactive };
struct FItemSpec
{
    FString MeshPath;
    FObject Options;
    bool bOverwrite = false;
    EExecutionMode ExecutionMode = EExecutionMode::Unattended;
};
struct FPreparedBatch
{
    FObject Request;
    FString RequestId, Hash;
    TArray<FItemSpec> Specs;
    TArray<bool> ItemResolved;
    TArray<FString> ItemErrors, ReservedPaths, Errors;
};
enum class EStepResult : uint8 { Continue, Succeeded, Failed, Cancelled };
class IItemRunner
{
public:
    virtual ~IItemRunner() = default;
    virtual EStepResult Step(bool bCancelRequested, bool bInlineMode) = 0;
    virtual FString GetStage() const = 0;
    virtual bool CanCancel() const = 0;
    virtual bool IsCommitPhase() const = 0;
    virtual FObject GetResult() const = 0;
    // MAX_uint64 is the compatibility fallback: obtain and compare a fresh result.
    // Production runners increment a cheap revision only when visible state changes.
    virtual uint64 GetSnapshotRevision() const { return MAX_uint64; }
    virtual void Shutdown() {}
};

class FJobService
{
public:
    using FNormalize = TFunction<bool(FObject&, FString&)>;
    using FReserve = TFunction<TArray<FString>(const FItemSpec&)>;
    using FFactory = TFunction<TUniquePtr<IItemRunner>(const FItemSpec&, const FString&)>;
    using FCompletion = TFunction<void(FObject)>;
    using FRecoveryAcknowledger = TFunction<bool(const FObject&, FString&)>;
    // Cheap editor policy only. Asset loading and full preflight remain runner stages.
    using FAdmissionValidator = TFunction<bool(const TArray<FItemSpec>&, FString&)>;
    FSimpleMulticastDelegate OnChanged;

    FJobService(FString InProvider, FString PluginName, FProfileStore& InProfiles, FObject InDefaults,
                FNormalize InNormalize, FReserve InReserve, FFactory InFactory)
        : Provider(MoveTemp(InProvider)), Root(FPaths::ProjectSavedDir() / PluginName / TEXT("Jobs")),
          StorageRoot(FPaths::ProjectSavedDir() / PluginName),
          Profiles(InProfiles), Defaults(Clone(InDefaults)), Normalize(MoveTemp(InNormalize)),
          Reserve(MoveTemp(InReserve)), Factory(MoveTemp(InFactory))
    {
        Profiles.SetValidator(Normalize);
        LoadRecords();
        // EngineLoop finishes CoreTicker.Tick before broadcasting OnEndFrame.
        // MaterialBaking may pump CoreTicker while streaming textures, so runner
        // Steps must execute after the outer CoreTicker invocation has returned.
        FrameDriver = FCoreDelegates::OnEndFrame.AddRaw(this, &FJobService::ExecuteFrame);
        Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FJobService::Tick));
    }
    ~FJobService() { Shutdown(); }
    void SetRecoveryAcknowledger(FRecoveryAcknowledger Callback) { AcknowledgeRecovery = MoveTemp(Callback); }
    void SetAdmissionValidator(FAdmissionValidator Callback) { AdmissionValidator = MoveTemp(Callback); }
    bool IsBusy() const { return !Queue.IsEmpty() || Runner.IsValid() || bStepping; }
    bool IsInitialScanComplete() const { return bInitialScanComplete; }
    uint64 GetRevision() const { return Revision; }
    uint64 GetInstanceGeneration() const { return InstanceGeneration; }
    uint64 GetJobRevision(const FString& Id) const { return JobRevisions.FindRef(Id); }
    void PinJob(const FString& Id) { ++Pins.FindOrAdd(Id); }
    void UnpinJob(const FString& Id) { if (int32* Count = Pins.Find(Id)) { if (--*Count <= 0) Pins.Remove(Id); } bMaintenanceDue = true; }
    FObject GetRetentionPolicy() const
    {
        FObject Policy = MakeShared<FJsonObject>();
        Policy->SetNumberField(TEXT("recent_limit"), 100); Policy->SetNumberField(TEXT("archive_limit"), 100);
        Policy->SetNumberField(TEXT("max_age_days"), 7); Policy->SetBoolField(TEXT("expired_request_ids_may_execute_again"), true);
        Policy->SetNumberField(TEXT("diagnostic_ack_limit"), 100); Policy->SetNumberField(TEXT("diagnostic_ack_max_age_days"), 7);
        return Policy;
    }
    TArray<FObject> GetJobSummaries(int32 Offset = 0, int32 Limit = 50, bool bArchive = false) const
    {
        TArray<FObject> Result;
        const TArray<FString>& Ids = bArchive ? ArchiveIds : RecentIds;
        for (int32 Index = FMath::Max(0, Offset); Index < Ids.Num() && Result.Num() < FMath::Clamp(Limit, 1, 100); ++Index)
            Result.Add(Clone(Summaries[Ids[Index]]));
        return Result;
    }
    TArray<FObject> GetActiveJobs() const
    {
        TArray<FObject> Result;
        for (const FString& Id : Queue) if (FObject Job = ReadJob(Id)) Result.Add(Clone(Job));
        return Result;
    }
    TArray<FObject> GetRecordIssues() const
    {
        TArray<FObject> Result;
        for (const FString& Id : DiagnosticIds) if (GetBool(Summaries[Id], TEXT("recovery_required"))) Result.Add(Clone(Summaries[Id]));
        return Result;
    }
    TArray<FObject> GetDiagnosticSummaries(int32 Offset = 0, int32 Limit = 50) const
    {
        TArray<FObject> Result;
        for (int32 Index = FMath::Max(0, Offset); Index < DiagnosticIds.Num() && Result.Num() < FMath::Clamp(Limit, 1, 100); ++Index)
            Result.Add(Clone(Summaries[DiagnosticIds[Index]]));
        return Result;
    }
    FObject RegisterRecordIssue(const FString& SourcePath, const FString& Reason)
    {
        const FString Source = FullPath(SourcePath);
        if (!OwnedSource(Source)) return Error(TEXT("Record issue must belong to this provider's saved metadata."));
        FObject Identity = MakeShared<FJsonObject>();
        FString Relative = Source; FPaths::MakePathRelativeTo(Relative, *(FullPath(StorageRoot) + TEXT("/")));
        Identity->SetStringField(TEXT("source"), CanonicalPath(Relative));
        FString Id = Provider + TEXT("-diagnostic-") + RequestHash(Identity);
        const auto ExistingIssues = DiagnosticIds;
        for (const FString& ExistingId : ExistingIssues)
            if (CanonicalPath(GetString(Summaries[ExistingId], TEXT("source_path"))) == CanonicalPath(Source)) { Id = ExistingId; break; }
        // Never overwrite an unreadable guard while reporting a different source.
        // Its own diagnostic must preserve those bytes through explicit review.
        for (int32 Slot = 0; Slot < 100 && !RecordPaths.Contains(Id) && IFileManager::Get().FileExists(*(StorageRoot / TEXT("Diagnostics") / (Id + TEXT(".json")))); ++Slot)
        {
            Identity->SetNumberField(TEXT("guard_slot"), Slot + 1);
            Id = Provider + TEXT("-diagnostic-") + RequestHash(Identity);
        }
        if (!RecordPaths.Contains(Id) && IFileManager::Get().FileExists(*(StorageRoot / TEXT("Diagnostics") / (Id + TEXT(".json"))))) return Error(TEXT("Existing damaged diagnostic guards require review before another guard can be created."), TEXT("RECOVERY_REQUIRED"));
        TArray<uint8> Bytes; FString Hash;
        if (FFileHelper::LoadFileToArray(Bytes, *Source)) Hash = BytesHash(Bytes);
        FObject Previous = ReadJob(Id);
        if (Previous && GetBool(Previous, TEXT("recovery_required")) && GetString(Previous, TEXT("source_hash")) == Hash) return Reply(Previous);
        FObject Issue = MakeShared<FJsonObject>();
        Issue->SetNumberField(TEXT("schema_version"), 1); Issue->SetStringField(TEXT("record_kind"), TEXT("diagnostic"));
        Issue->SetStringField(TEXT("provider_id"), Provider); Issue->SetStringField(TEXT("project_file"), FullPath(FPaths::GetProjectFilePath()));
        Issue->SetStringField(TEXT("job_id"), Id); Issue->SetStringField(TEXT("source_path"), Source);
        Issue->SetStringField(TEXT("source_kind"), Under(Source, Root) ? TEXT("job") : Under(Source, StorageRoot / TEXT("Recovery")) ? TEXT("recovery") : TEXT("diagnostic"));
        Issue->SetStringField(TEXT("source_hash"), Hash); Issue->SetStringField(TEXT("error"), Reason);
        Issue->SetStringField(TEXT("error_code"), TEXT("JOURNAL_INVALID")); Issue->SetStringField(TEXT("state"), TEXT("interrupted"));
        Issue->SetStringField(TEXT("stage"), TEXT("record_review")); Issue->SetStringField(TEXT("phase"), TEXT("issue"));
        Issue->SetStringField(TEXT("created_at"), Previous ? GetString(Previous, TEXT("created_at")) : FDateTime::UtcNow().ToIso8601());
        Issue->SetBoolField(TEXT("recovery_required"), true); Issue->SetArrayField(TEXT("items"), {}); Issue->SetArrayField(TEXT("reserved_paths"), {});
        const FString Filename = StorageRoot / TEXT("Diagnostics") / (Id + TEXT(".json"));
        FString Message; AtomicJson(Filename, Issue, Message);
        Remember(Issue, Filename); OnChanged.Broadcast();
        return Reply(Issue);
    }
    bool WasRecordIssueAcknowledged(const FString& SourcePath) const
    {
        if (!OwnedSource(SourcePath)) return false;
        for (const FString& Id : DiagnosticIds)
        {
            if (CanonicalPath(GetString(Summaries[Id], TEXT("source_path"))) != CanonicalPath(FullPath(SourcePath)) || GetString(Summaries[Id], TEXT("phase")) != TEXT("acknowledged")) continue;
            const FObject Guard = ReadJob(Id); TArray<uint8> Bytes;
            return Guard && Under(GetString(Guard, TEXT("raw_path")), StorageRoot / TEXT("Quarantine")) &&
                FFileHelper::LoadFileToArray(Bytes, *GetString(Guard, TEXT("raw_path"))) && BytesHash(Bytes) == GetString(Guard, TEXT("raw_hash"));
        }
        return false;
    }
#if WITH_DEV_AUTOMATION_TESTS
    void AdvanceOneStepForTesting() { Drive(false); }
    void AdvanceFrameForTesting() { ExecuteFrame(); }
    void AdvanceMaintenanceTickerForTesting() { Tick(0.0f); }
    void SetPersistGateForTesting(TFunction<bool(const FObject&, FString&)> Gate) { PersistGate = MoveTemp(Gate); }
    void RunMaintenanceForTesting(int32 Budget = 10) { PlanMaintenance(); Maintain(Budget); }
    int32 GetCachedRecordCountForTesting() const { return Records.Num(); }
    int32 GetSnapshotBuildCountForTesting() const { return SnapshotBuilds; }
    int32 GetJobPinCountForTesting(const FString& Id) const { return Pins.FindRef(Id); }
#endif
    TArray<FObject> GetJobs() const
    {
        TArray<FString> Ids; Summaries.GetKeys(Ids);
        Ids.Sort([this](const FString& A, const FString& B) { return Newer(A, B); });
        TArray<FObject> Result;
        for (const FString& Id : Ids) if (GetString(Summaries[Id], TEXT("record_kind")) != TEXT("diagnostic")) if (FObject Job = ReadJob(Id)) Result.Add(Clone(Job));
        return Result;
    }
    bool CheckReservations(const TArray<FString>& Paths, FString& Message, const FString& IgnoreJobId = FString()) const
    {
        if (!bInitialScanComplete) { Message = TEXT("Job records are still being indexed."); return false; }
        for (const auto& Pair : Summaries)
        {
            if (Pair.Key == IgnoreJobId) continue;
            const FObject& Record = Pair.Value;
            if (GetString(Record, TEXT("record_kind")) == TEXT("diagnostic") && GetBool(Record, TEXT("recovery_required")))
            { Message = TEXT("Review damaged record before starting work: ") + Pair.Key; return false; }
            const FString State = GetString(Record, TEXT("state"));
            if (State != TEXT("queued") && State != TEXT("running") && !GetBool(Record, TEXT("recovery_required"))) continue;
            for (const FString& Path : Paths)
                for (const auto& Reserved : GetArray(Record, TEXT("reserved_paths")))
                    if (PathsOverlap(Path, Reserved->AsString())) { Message = TEXT("Output is reserved by job ") + Pair.Key + TEXT(": ") + Path; return false; }
        }
        return true;
    }
    bool PrepareBatch(const FObject& Input, FPreparedBatch& Prepared, FString& Message) const
    {
        Prepared = FPreparedBatch();
        auto Add = [&](const FString& ErrorText) { Prepared.Errors.Add(ErrorText); };
        FString Fields;
        if (!CheckFields(Input, {TEXT("request_id"), TEXT("mesh_paths"), TEXT("profile_id"), TEXT("options"), TEXT("overrides"), TEXT("overwrite")}, Fields))
        { Add(Fields); Message = Fields; return false; }
        FGuid Guid;
        if (!FGuid::Parse(GetString(Input, TEXT("request_id")), Guid) || !Guid.IsValid()) Add(TEXT("request_id must be a nonzero UUID."));
        else Prepared.RequestId = Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
        Prepared.Request = Clone(Input); Prepared.Request->SetStringField(TEXT("request_id"), Prepared.RequestId);
        Prepared.Hash = RequestHash(Prepared.Request);
        if (!Input->HasTypedField<EJson::Array>(TEXT("mesh_paths"))) Add(TEXT("mesh_paths must be an array."));
        const auto Meshes = GetArray(Input, TEXT("mesh_paths"));
        if (Meshes.IsEmpty()) Add(TEXT("At least one mesh path is required."));
        if (Input->HasField(TEXT("overwrite")) && !Input->HasTypedField<EJson::Boolean>(TEXT("overwrite"))) Add(TEXT("overwrite must be boolean."));
        if (Input->HasField(TEXT("profile_id")) && !Input->HasTypedField<EJson::String>(TEXT("profile_id"))) Add(TEXT("profile_id must be a string."));
        for (const FString& Key : {FString(TEXT("options")), FString(TEXT("overrides"))})
            if (Input->HasField(Key) && !Input->HasTypedField<EJson::Object>(Key)) Add(Key + TEXT(" must be an object."));
        FString ProfileError;
        FObject Base = Profiles.Resolve(GetString(Input, TEXT("profile_id")), Defaults, ProfileError);
        if (!Base) { Add(ProfileError); Base = Clone(Defaults); }
        Base = Merge(Base, GetObject(Input, TEXT("options")));
        const FObject Overrides = GetObject(Input, TEXT("overrides"));
        TSet<FString> Seen;
        for (int32 Index = 0; Index < Meshes.Num(); ++Index)
        {
            FItemSpec Spec; FString ItemError, Mesh;
            const bool ValidPath = Meshes[Index].IsValid() && Meshes[Index]->TryGetString(Mesh) && FPackageName::IsValidObjectPath(Mesh) && !Mesh.Contains(TEXT(":"));
            Spec.MeshPath = Mesh; Spec.bOverwrite = GetBool(Input, TEXT("overwrite"));
            if (!ValidPath) ItemError = TEXT("mesh_paths must contain exact Static Mesh object paths.");
            else if (Seen.Contains(Mesh.ToLower())) ItemError = TEXT("Duplicate target: ") + Mesh;
            Seen.Add(Mesh.ToLower());
            const FObject Override = GetObject(Overrides, Mesh);
            const bool InvalidOverride = Overrides && Overrides->HasField(Mesh) && !Override;
            if (InvalidOverride) ItemError = TEXT("Each target override must be an object.");
            Spec.Options = Merge(Base, Override);
            FString OptionError;
            if (Spec.Options->HasField(TEXT("overwrite"))) OptionError = TEXT("overwrite must be a job argument, not a profile/option.");
            else if (Normalize && !Normalize(Spec.Options, OptionError) && OptionError.IsEmpty()) OptionError = TEXT("Invalid options.");
            if (!OptionError.IsEmpty()) ItemError += (ItemError.IsEmpty() ? TEXT("") : TEXT(" ")) + OptionError;
            if (!ProfileError.IsEmpty()) ItemError += (ItemError.IsEmpty() ? TEXT("") : TEXT(" ")) + ProfileError;
            const bool Resolved = ValidPath && !InvalidOverride && OptionError.IsEmpty() && ProfileError.IsEmpty();
            if (Resolved)
            {
                const auto ItemPaths = Reserve ? Reserve(Spec) : TArray<FString>();
                for (const FString& Path : ItemPaths)
                    for (const FString& Previous : Prepared.ReservedPaths)
                        if (PathsOverlap(Path, Previous)) ItemError = TEXT("Batch targets resolve to overlapping outputs: ") + Path;
                Prepared.ReservedPaths.Append(ItemPaths);
            }
            Prepared.Specs.Add(Spec); Prepared.ItemErrors.Add(ItemError); Prepared.ItemResolved.Add(Resolved);
            if (!ItemError.IsEmpty()) Add((Mesh.IsEmpty() ? FString::Printf(TEXT("mesh_paths[%d]"), Index) : Mesh) + TEXT(": ") + ItemError);
        }
        if (Overrides) for (const auto& Pair : Overrides->Values)
        {
            const FString Key(FStringView(Pair.Key));
            if (!Meshes.ContainsByPredicate([&](const auto& Value) { return Value && Value->Type == EJson::String && Value->AsString() == Key; })) Add(TEXT("Override target is not in mesh_paths: ") + Key);
        }
        FString ReservationError;
        if (!CheckReservations(Prepared.ReservedPaths, ReservationError)) Add(ReservationError);
        Message = FString::Join(Prepared.Errors, TEXT("\n"));
        return Prepared.Errors.IsEmpty();
    }
    FObject Start(const FObject& Input, EExecutionMode Mode = EExecutionMode::Unattended)
    {
        if (bStopping) return Error(TEXT("Plugin is shutting down."), TEXT("BUSY"));
        FString Message;
        if (!CheckFields(Input, {TEXT("request_id"), TEXT("mesh_paths"), TEXT("profile_id"), TEXT("options"), TEXT("overrides"), TEXT("overwrite")}, Message)) return Error(Message);
        FGuid RequestGuid;
        if (!FGuid::Parse(GetString(Input, TEXT("request_id")), RequestGuid) || !RequestGuid.IsValid()) return Error(TEXT("request_id must be a nonzero UUID."));
        FObject Request = Clone(Input);
        const FString RequestId = RequestGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
        Request->SetStringField(TEXT("request_id"), RequestId);
        const FString Hash = RequestHash(Request);
        if (const FString* PreviousId = RequestIds.Find(RequestId))
        {
            FObject Previous = ReadJob(*PreviousId);
            if (!Previous) return Error(TEXT("Stored request cannot be read; review record issues."), TEXT("RECOVERY_REQUIRED"));
            if (GetString(Previous, TEXT("request_hash")) != Hash) return Error(TEXT("request_id was used with a different request."), TEXT("REQUEST_CONFLICT"));
            return Reply(Previous);
        }
        FPreparedBatch Prepared;
        if (!PrepareBatch(Input, Prepared, Message))
        {
            const FString Code = !GetRecordIssues().IsEmpty() ? TEXT("RECOVERY_REQUIRED") :
                Message.Contains(TEXT("overlapping outputs")) || Message.Contains(TEXT("reserved by job")) ? TEXT("OUTPUT_CONFLICT") : TEXT("INVALID_ARGUMENT");
            FObject Failure = Error(Message, Code); Failure->SetArrayField(TEXT("errors"), Strings(Prepared.Errors)); return Failure;
        }
        TArray<TSharedPtr<FJsonValue>> Items;
        for (FItemSpec& Spec : Prepared.Specs)
        {
            Spec.ExecutionMode = Mode;
            FObject Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("mesh_path"), Spec.MeshPath);
            Item->SetObjectField(TEXT("options"), Spec.Options);
            Item->SetBoolField(TEXT("overwrite"), Spec.bOverwrite);
            Item->SetStringField(TEXT("state"), TEXT("pending"));
            Item->SetStringField(TEXT("stage"), TEXT("queued"));
            Item->SetStringField(TEXT("execution_mode"), Mode == EExecutionMode::Interactive ? TEXT("interactive") : TEXT("unattended"));
            Items.Add(MakeShared<FJsonValueObject>(Item));
        }
        // Idempotent replays were resolved above, even if editor policy has since changed.
        if (AdmissionValidator && !AdmissionValidator(Prepared.Specs, Message)) return Error(Message, TEXT("EXECUTION_BLOCKED"));
        FObject Job = MakeShared<FJsonObject>();
        const FString Id = Provider + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Job->SetNumberField(TEXT("schema_version"), 1);
        Job->SetStringField(TEXT("record_kind"), TEXT("job"));
        Job->SetStringField(TEXT("provider_id"), Provider);
        Job->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
        Job->SetStringField(TEXT("job_id"), Id);
        Job->SetStringField(TEXT("request_id"), RequestId);
        Job->SetStringField(TEXT("request_hash"), Hash);
        Job->SetObjectField(TEXT("request"), Request);
        Job->SetStringField(TEXT("created_at"), FDateTime::UtcNow().ToIso8601());
        Job->SetStringField(TEXT("state"), TEXT("queued"));
        Job->SetStringField(TEXT("stage"), TEXT("queued"));
        Job->SetArrayField(TEXT("items"), Items);
        Job->SetArrayField(TEXT("reserved_paths"), Strings(Prepared.ReservedPaths));
        Job->SetBoolField(TEXT("cancel_requested"), false);
        Job->SetBoolField(TEXT("cancel_effective"), false);
        Job->SetBoolField(TEXT("recovery_required"), false);
        SetProgress(Job, 0, Items.Num());
        if (!Persist(Job, Message)) return Error(Message, TEXT("IO_ERROR"));
        Queue.Add(Id); Remember(Job, Root / (Id + TEXT(".json")));
        OnChanged.Broadcast();
        return Reply(Job);
    }
    FObject Status(const FObject& Input) const
    {
        FString Message;
        if (!bInitialScanComplete) return Error(TEXT("Job records are still being indexed."), TEXT("BUSY"));
        if (!CheckFields(Input, {TEXT("job_id"), TEXT("request_id")}, Message)) return Error(Message);
        const FString Id = GetString(Input, TEXT("job_id")), Request = GetString(Input, TEXT("request_id"));
        if ((Id.IsEmpty() && Request.IsEmpty()) || (!Id.IsEmpty() && !Request.IsEmpty())) return Error(TEXT("Pass exactly one job_id or request_id."));
        FString ActualId = Id;
        if (!Request.IsEmpty())
        {
            FGuid Guid;
            if (!FGuid::Parse(Request, Guid)) return Error(TEXT("request_id must be a UUID."));
            const FString* Found = RequestIds.Find(Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
            if (!Found) return Error(TEXT("Job request not found."), TEXT("NOT_FOUND"));
            ActualId = *Found;
        }
        if (!bInitialScanComplete) return Error(TEXT("Job records are still being indexed."), TEXT("BUSY"));
        FObject Job = ReadJob(ActualId);
        return Job ? Reply(Job) : Error(TEXT("Job not found."), TEXT("NOT_FOUND"));
    }
    FObject Control(const FObject& Input)
    {
        FString Message;
        if (!CheckFields(Input, {TEXT("job_id"), TEXT("request_id"), TEXT("action")}, Message)) return Error(Message);
        FObject Selector = MakeShared<FJsonObject>();
        for (const FString& Key : {FString(TEXT("job_id")), FString(TEXT("request_id"))})
            if (Input->HasField(Key)) Selector->SetField(Key, Input->TryGetField(Key));
        FObject Lookup = Status(Selector);
        if (!GetBool(Lookup, TEXT("success"))) return Lookup;
        FObject Current = ReadJob(GetString(Lookup, TEXT("job_id")));
        if (!Current) return Error(TEXT("Job not found."), TEXT("NOT_FOUND"));
        FObject Job = Clone(Current);
        const FString Action = GetString(Input, TEXT("action")), State = GetString(Job, TEXT("state"));
        if (Action == TEXT("cancel"))
        {
            if (State == TEXT("queued") || State == TEXT("running")) Job->SetBoolField(TEXT("cancel_requested"), true);
        }
        else if (Action == TEXT("acknowledge_recovery"))
        {
            if (State == TEXT("queued") || State == TEXT("running")) return Error(TEXT("Active work cannot acknowledge recovery."));
            if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic")) return AcknowledgeDiagnostic(Current);
            if (AcknowledgeRecovery && !AcknowledgeRecovery(Job, Message)) return Error(Message, TEXT("RECOVERY_REQUIRED"));
            Job->SetBoolField(TEXT("recovery_required"), false);
            Job->SetStringField(TEXT("recovery_acknowledged_at"), FDateTime::UtcNow().ToIso8601());
        }
        else return Error(TEXT("action must be cancel or acknowledge_recovery."));
        if (!Persist(Job, Message)) return Error(Message, TEXT("IO_ERROR"));
        // Keep identity and item references held by a reentrant Drive() intact.
        for (const TCHAR* Key : {TEXT("cancel_requested"), TEXT("recovery_required"), TEXT("recovery_acknowledged_at")})
            if (Job->HasField(Key)) Current->SetField(Key, Job->TryGetField(Key));
        Remember(Current, RecordPaths.FindRef(GetString(Current, TEXT("job_id"))));
        bMaintenanceDue = true;
        OnChanged.Broadcast();
        return Reply(Current);
    }
    FObject RunInline(const FItemSpec& Spec, FString& OutError)
    {
        // Direct synchronous callers run in their current game-thread context.
        // Do not invoke material baking from a CoreTicker delegate: the engine's
        // streaming loop may tick that same ticker recursively. GUI submissions
        // and MCP RunAsync use the queued frame driver instead.
        if (IsBusy() || bStopping) { OutError = TEXT("A queued or active job is using this plugin."); return Error(OutError, TEXT("BUSY")); }
        FObject Accepted = Start(SingleRequest(Spec), Spec.ExecutionMode);
        if (!GetBool(Accepted, TEXT("success"))) { OutError = GetString(Accepted, TEXT("error")); return Accepted; }
        const FString Id = GetString(Accepted, TEXT("job_id"));
        while (Queue.Contains(Id) && !bStopping) Drive(true);
        const FObject Job = ReadJob(Id);
        const auto Items = GetArray(Job, TEXT("items"));
        FObject Result = Items.IsEmpty() ? Error(TEXT("Job has no item result.")) : Clone(GetObject(Items[0]->AsObject(), TEXT("result")));
        if (!Result->HasField(TEXT("success"))) Result = Error(GetString(Job, TEXT("error"), TEXT("Job interrupted.")));
        if (GetString(Job, TEXT("error_code")) == TEXT("IO_ERROR")) { Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), GetString(Job, TEXT("error"))); Result->SetStringField(TEXT("error_code"), TEXT("IO_ERROR")); }
        return Result;
    }
    FObject StartLegacyStructured(const FItemSpec& Spec, FCompletion Completion)
    {
        if (IsBusy() || bStopping) return Error(TEXT("A queued or active job is using this plugin."), TEXT("BUSY"));
        FObject Accepted = Start(SingleRequest(Spec), Spec.ExecutionMode);
        if (GetBool(Accepted, TEXT("success"))) Completions.Add(GetString(Accepted, TEXT("job_id")), MoveTemp(Completion));
        return Accepted;
    }
    bool StartLegacy(const FItemSpec& Spec, FCompletion Completion, FString& OutError)
    {
        FObject Accepted = StartLegacyStructured(Spec, MoveTemp(Completion));
        if (!GetBool(Accepted, TEXT("success"))) { OutError = GetString(Accepted, TEXT("error")); return false; }
        return true;
    }
    void Shutdown()
    {
        if (bStopping) return;
        bStopping = true;
        if (FrameDriver.IsValid()) { FCoreDelegates::OnEndFrame.Remove(FrameDriver); FrameDriver.Reset(); }
        if (Ticker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(Ticker); Ticker.Reset(); }
        // Persist the last known stage and recovery paths before joining workers or
        // releasing their strong UObject/render-resource references.
        if (Runner && !Queue.IsEmpty())
        {
            FObject Active = Records[Queue[0]];
            const auto Items = GetArray(Active, TEXT("items"));
            const int32 Index = static_cast<int32>(GetNumber(Active, TEXT("current_index")));
            if (Items.IsValidIndex(Index))
            {
                FObject Item = Items[Index]->AsObject();
                Item->SetStringField(TEXT("stage"), Runner->GetStage());
                Item->SetObjectField(TEXT("result"), Clone(Runner->GetResult()));
                Item->SetBoolField(TEXT("commit_pending"), GetBool(Item, TEXT("commit_pending")) || Runner->IsCommitPhase());
            }
        }
        for (const FString& Id : Queue)
        {
            FObject Job = Records[Id];
            Job->SetStringField(TEXT("state"), TEXT("interrupted"));
            Job->SetStringField(TEXT("finished_at"), FDateTime::UtcNow().ToIso8601());
            Job->SetStringField(TEXT("error_code"), TEXT("EDITOR_INTERRUPTED"));
            Job->SetStringField(TEXT("error"), TEXT("Editor or plugin stopped before completion."));
            for (const auto& Value : GetArray(Job, TEXT("items")))
            {
                FObject Item = Value->AsObject();
                if (GetBool(Item, TEXT("commit_pending"))) Job->SetBoolField(TEXT("recovery_required"), true);
                const FString State = GetString(Item, TEXT("state"));
                if (State == TEXT("pending") || State == TEXT("running"))
                {
                    Item->SetStringField(TEXT("state"), TEXT("interrupted"));
                    Item->SetStringField(TEXT("reason"), TEXT("editor_interrupted"));
                }
            }
            FString Message;
            if (!Persist(Job, Message)) UE_LOG(LogTemp, Warning, TEXT("Cannot persist interrupted job: %s"), *Message);
            Remember(Job, RecordPaths.FindRef(Id));
        }
        if (Runner) { Runner->Shutdown(); Runner.Reset(); }
        for (const FString& Id : Queue) Complete(Id);
        Queue.Reset();
        Completions.Reset();
        OnChanged.Clear();
    }
private:
    static FString FullPath(const FString& Path)
    {
        FString Result = FPaths::ConvertRelativePathToFull(Path); FPaths::NormalizeFilename(Result); FPaths::CollapseRelativeDirectories(Result); return Result;
    }
    static bool Under(const FString& Path, const FString& Parent)
    { return CanonicalPath(FullPath(Path)).StartsWith(CanonicalPath(FullPath(Parent)) + TEXT("/")); }
    bool OwnedSource(const FString& Path) const
    { return Under(Path, Root) || Under(Path, StorageRoot / TEXT("Recovery")) || Under(Path, StorageRoot / TEXT("Diagnostics")); }
    static FString BytesHash(const TArray<uint8>& Bytes)
    { FSHAHash Hash; FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash); return Hash.ToString(); }
    static bool Terminal(const FObject& Job)
    {
        const FString State = GetString(Job, TEXT("state"));
        return State == TEXT("succeeded") || State == TEXT("failed") || State == TEXT("cancelled") || State == TEXT("interrupted");
    }
    static bool Date(const FString& Text, FDateTime& Value)
    { return !Text.IsEmpty() && FDateTime::ParseIso8601(*Text, Value); }
    bool ValidateRecord(const FObject& Job, const FString& Filename, FString& Message) const
    {
        auto Fail = [&](const FString& Reason) { Message = Reason; return false; };
        if (!Job || GetNumber(Job, TEXT("schema_version")) != 1) return Fail(TEXT("Unsupported or unreadable journal schema_version."));
        if (GetString(Job, TEXT("provider_id")) != Provider || CanonicalPath(GetString(Job, TEXT("project_file"))) != CanonicalPath(FullPath(FPaths::GetProjectFilePath()))) return Fail(TEXT("Journal provider/project identity does not match."));
        const FString Id = GetString(Job, TEXT("job_id"));
        if (Id != FPaths::GetBaseFilename(Filename)) return Fail(TEXT("Journal job_id does not match its filename."));
        if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic"))
        {
            const FString Phase = GetString(Job, TEXT("phase"));
            if (!Id.StartsWith(Provider + TEXT("-diagnostic-")) || !OwnedSource(GetString(Job, TEXT("source_path"))) ||
                !Job->HasTypedField<EJson::Boolean>(TEXT("recovery_required")) ||
                (Phase != TEXT("issue") && Phase != TEXT("prepared") && Phase != TEXT("acknowledged"))) return Fail(TEXT("Invalid diagnostic guard."));
            if (Phase != TEXT("issue") && (!Under(GetString(Job, TEXT("raw_path")), StorageRoot / TEXT("Quarantine")) || GetString(Job, TEXT("raw_hash")).Len() != 40)) return Fail(TEXT("Invalid diagnostic evidence path/hash."));
            if (GetBool(Job, TEXT("recovery_required")) != (Phase != TEXT("acknowledged"))) return Fail(TEXT("Diagnostic phase disagrees with recovery reservation."));
            return true;
        }
        if (Job->HasField(TEXT("record_kind")) && GetString(Job, TEXT("record_kind")) != TEXT("job")) return Fail(TEXT("Unknown record_kind."));
        FGuid Guid; const FString RequestId = GetString(Job, TEXT("request_id"));
        FDateTime Created;
        if (!Id.StartsWith(Provider + TEXT("-")) || !FGuid::Parse(RequestId, Guid) || !Guid.IsValid() || GetString(Job, TEXT("request_hash")).Len() != 40 || !GetObject(Job, TEXT("request")) || !Date(GetString(Job, TEXT("created_at")), Created)) return Fail(TEXT("Invalid request identity or creation time."));
        FObject Request = GetObject(Job, TEXT("request"));
        if (RequestHash(Request) != GetString(Job, TEXT("request_hash")) || GetString(Request, TEXT("request_id")) != Guid.ToString(EGuidFormats::DigitsWithHyphensLower)) return Fail(TEXT("Request snapshot/hash does not match."));
        const FString State = GetString(Job, TEXT("state"));
        if (State != TEXT("queued") && State != TEXT("running") && !Terminal(Job)) return Fail(TEXT("Invalid job state."));
        for (const TCHAR* Key : {TEXT("cancel_requested"), TEXT("cancel_effective"), TEXT("recovery_required")}) if (!Job->HasTypedField<EJson::Boolean>(Key)) return Fail(TEXT("Invalid job control flags."));
        if (!Job->HasTypedField<EJson::Array>(TEXT("items")) || !Job->HasTypedField<EJson::Array>(TEXT("reserved_paths"))) return Fail(TEXT("items/reserved_paths must be arrays."));
        const auto Items = GetArray(Job, TEXT("items")); if (Items.IsEmpty()) return Fail(TEXT("Job contains no items."));
        for (const auto& Path : GetArray(Job, TEXT("reserved_paths"))) if (!Path || Path->Type != EJson::String || Path->AsString().IsEmpty()) return Fail(TEXT("Invalid reserved path."));
        for (const auto& Value : Items)
        {
            if (!Value || Value->Type != EJson::Object) return Fail(TEXT("Job item must be an object."));
            const FObject Item = Value->AsObject(); const FString ItemState = GetString(Item, TEXT("state"));
            if (!FPackageName::IsValidObjectPath(GetString(Item, TEXT("mesh_path"))) || !GetObject(Item, TEXT("options")) || !Item->HasTypedField<EJson::Boolean>(TEXT("overwrite")) || !Item->HasTypedField<EJson::String>(TEXT("stage"))) return Fail(TEXT("Invalid item fields."));
            if (ItemState != TEXT("pending") && ItemState != TEXT("running") && ItemState != TEXT("succeeded") && ItemState != TEXT("failed") && ItemState != TEXT("cancelled") && ItemState != TEXT("interrupted") && ItemState != TEXT("skipped")) return Fail(TEXT("Invalid item state."));
            if ((Item->HasField(TEXT("result")) && !Item->HasTypedField<EJson::Object>(TEXT("result"))) || (Item->HasField(TEXT("commit_pending")) && !Item->HasTypedField<EJson::Boolean>(TEXT("commit_pending")))) return Fail(TEXT("Invalid item result/commit flag."));
        }
        const FObject Progress = GetObject(Job, TEXT("progress"));
        const double Index = GetNumber(Job, TEXT("current_index"));
        if (!Progress || !Progress->HasTypedField<EJson::Number>(TEXT("total")) || !Progress->HasTypedField<EJson::Number>(TEXT("completed")) ||
            (Job->HasField(TEXT("current_index")) && !Job->HasTypedField<EJson::Number>(TEXT("current_index"))) ||
            GetNumber(Progress, TEXT("total"), -1) != Items.Num() || GetNumber(Progress, TEXT("completed"), -1) < 0 || GetNumber(Progress, TEXT("completed")) > Items.Num() ||
            !FMath::IsFinite(Index) || !FMath::IsFinite(GetNumber(Progress, TEXT("completed"))) || FMath::FloorToDouble(GetNumber(Progress, TEXT("completed"))) != GetNumber(Progress, TEXT("completed")) || Index < 0 || Index > Items.Num() || FMath::FloorToDouble(Index) != Index) return Fail(TEXT("Invalid progress/index."));
        return true;
    }
    bool Newer(const FString& A, const FString& B) const
    {
        for (const TCHAR* Key : {TEXT("finished_at"), TEXT("created_at")})
        {
            const FString First = GetString(Summaries[A], Key), Second = GetString(Summaries[B], Key);
            if (First != Second) return First > Second;
        }
        return A < B;
    }
    void SortHistory()
    {
        RecentIds.Reset(); ArchiveIds.Reset(); DiagnosticIds.Reset();
        for (const auto& Pair : Summaries)
        {
            if (GetString(Pair.Value, TEXT("record_kind")) == TEXT("diagnostic")) DiagnosticIds.Add(Pair.Key);
            else if (GetBool(Pair.Value, TEXT("archived"))) ArchiveIds.Add(Pair.Key); else RecentIds.Add(Pair.Key);
        }
        auto Sort = [this](const FString& A, const FString& B) { return Newer(A, B); };
        RecentIds.Sort(Sort); ArchiveIds.Sort(Sort); DiagnosticIds.Sort(Sort);
    }
    void Remember(const FObject& Job, const FString& Filename)
    {
        const FString Id = GetString(Job, TEXT("job_id")); if (Id.IsEmpty()) return;
        Records.Add(Id, Job); if (!Filename.IsEmpty()) RecordPaths.Add(Id, Filename);
        FObject Summary = MakeShared<FJsonObject>();
        for (const TCHAR* Key : {TEXT("job_id"), TEXT("record_kind"), TEXT("provider_id"), TEXT("created_at"), TEXT("finished_at"), TEXT("state"), TEXT("stage"), TEXT("progress"), TEXT("recovery_required"), TEXT("cancel_requested"), TEXT("retention_time_untrusted"), TEXT("phase"), TEXT("source_path"), TEXT("source_kind"), TEXT("error"), TEXT("error_code"), TEXT("raw_path")})
            if (Job->HasField(Key)) Summary->SetField(Key, Job->TryGetField(Key));
        Summary = Clone(Summary); Summary->SetBoolField(TEXT("archived"), Under(RecordPaths.FindRef(Id), Root / TEXT("Archive")));
        Summary->SetArrayField(TEXT("reserved_paths"), Strings({}));
        if (!Terminal(Job) || GetBool(Job, TEXT("recovery_required"))) Summary->SetArrayField(TEXT("reserved_paths"), GetArray(Clone(Job), TEXT("reserved_paths")));
        TArray<FString> Journals;
        if (GetBool(Job, TEXT("recovery_required"))) for (const auto& Value : GetArray(Job, TEXT("items")))
        { const FString Journal = GetString(GetObject(Value->AsObject(), TEXT("result")), TEXT("recovery_journal")); if (!Journal.IsEmpty()) Journals.AddUnique(FullPath(Journal)); }
        Summary->SetArrayField(TEXT("recovery_journals"), Strings(Journals));
        Summary->SetNumberField(TEXT("revision"), static_cast<double>(++Revision)); JobRevisions.Add(Id, Revision);
        Summaries.Add(Id, Summary);
        if (GetString(Job, TEXT("record_kind")) != TEXT("diagnostic"))
        {
            const FString Request = GetString(Job, TEXT("request_id"));
            for (auto It = RequestIds.CreateIterator(); It; ++It) if (It.Value() == Id && It.Key() != Request) It.RemoveCurrent();
            if (!Request.IsEmpty()) RequestIds.Add(Request, Id);
        }
        CacheOrder.Remove(Id); CacheOrder.Add(Id); SortHistory();
    }
    FObject ReadJob(const FString& Id) const
    {
        if (const FObject* Found = Records.Find(Id)) { CacheOrder.Remove(Id); CacheOrder.Add(Id); return *Found; }
        const FString* Filename = RecordPaths.Find(Id); if (!Filename) return nullptr;
        FString Text; if (!FFileHelper::LoadFileToString(Text, **Filename)) { const_cast<FJobService*>(this)->RegisterRecordIssue(*Filename, TEXT("Indexed journal cannot be read.")); return nullptr; }
        FObject Job = Parse(Text); FString Message;
        if (!ValidateRecord(Job, *Filename, Message)) { const_cast<FJobService*>(this)->RegisterRecordIssue(*Filename, Message); return nullptr; }
        Records.Add(Id, Job); CacheOrder.Remove(Id); CacheOrder.Add(Id); Evict(Id); return Job;
    }
    void Evict(const FString& Keep = FString()) const
    {
        int32 Ordinary = 0;
        for (const auto& Pair : Records) if (Terminal(Pair.Value) && !GetBool(Pair.Value, TEXT("recovery_required")) && !Pins.Contains(Pair.Key) && GetString(Pair.Value, TEXT("record_kind")) != TEXT("diagnostic")) ++Ordinary;
        const auto Order = CacheOrder;
        for (const FString& Id : Order)
        {
            if (Ordinary <= 20) break;
            const FObject* Job = Records.Find(Id);
            if (!Job || Id == Keep || Queue.Contains(Id) || Pins.Contains(Id) || !Terminal(*Job) || GetBool(*Job, TEXT("recovery_required")) || GetString(*Job, TEXT("record_kind")) == TEXT("diagnostic")) continue;
            Records.Remove(Id); CacheOrder.Remove(Id); --Ordinary;
        }
    }
    FObject AcknowledgeDiagnostic(const FObject& Current)
    {
        if (GetString(Current, TEXT("phase")) == TEXT("acknowledged")) return Reply(Current);
        FObject Candidate = Clone(Current); const FString Source = GetString(Current, TEXT("source_path"));
        if (!OwnedSource(Source)) return Error(TEXT("Invalid diagnostic source ownership."), TEXT("RECOVERY_REQUIRED"));
        FString Message;
        if (GetString(Candidate, TEXT("phase")) != TEXT("prepared"))
        {
            TArray<uint8> Bytes;
            if (!FFileHelper::LoadFileToArray(Bytes, *Source)) return Error(TEXT("Cannot preserve the damaged record; reservation remains."), TEXT("IO_ERROR"));
            const FString Hash = BytesHash(Bytes);
            if (Hash != GetString(Current, TEXT("source_hash"))) { RegisterRecordIssue(Source, TEXT("Record changed since inspection; review its current bytes.")); return Error(TEXT("Record changed since inspection."), TEXT("RECOVERY_REQUIRED")); }
            // A manually repaired healthy job is retained, never quarantined as
            // damaged metadata. The user still explicitly acknowledges the guard.
            FString SourceText, CheckError;
            if ((Under(Source, Root) || Under(Source, StorageRoot / TEXT("Diagnostics"))) && FFileHelper::LoadFileToString(SourceText, *Source))
            {
                const FObject Repaired = Parse(SourceText);
                if (ValidateRecord(Repaired, Source, CheckError)) Candidate->SetBoolField(TEXT("preserve_valid_source"), true);
            }
            FString Relative = FullPath(Source); FPaths::MakePathRelativeTo(Relative, *(FullPath(StorageRoot) + TEXT("/")));
            const FString Raw = FullPath(StorageRoot / TEXT("Quarantine") / (Relative + TEXT(".") + Hash + TEXT(".raw")));
            if (!Under(Raw, StorageRoot / TEXT("Quarantine"))) return Error(TEXT("Invalid evidence location."));
            TArray<uint8> Existing;
            if (IFileManager::Get().FileExists(*Raw))
            { if (!FFileHelper::LoadFileToArray(Existing, *Raw) || BytesHash(Existing) != Hash) return Error(TEXT("Preserved evidence does not match."), TEXT("IO_ERROR")); }
            else
            {
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(Raw), true);
                const FString Temp = Raw + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
                if (!FFileHelper::SaveArrayToFile(Bytes, *Temp) || !AtomicReplace(Temp, Raw, Message))
                { IFileManager::Get().Delete(*Temp, false, true); return Error(TEXT("Could not preserve damaged record bytes: ") + Message, TEXT("IO_ERROR")); }
            }
            TArray<uint8> VerifiedRaw;
            if (!FFileHelper::LoadFileToArray(VerifiedRaw, *Raw) || BytesHash(VerifiedRaw) != Hash)
                return Error(TEXT("Raw evidence verification failed before preparing the guard."), TEXT("IO_ERROR"));
            Candidate->SetStringField(TEXT("raw_path"), Raw); Candidate->SetStringField(TEXT("raw_hash"), Hash);
            Candidate->SetStringField(TEXT("phase"), TEXT("prepared")); Candidate->SetBoolField(TEXT("recovery_required"), true);
            if (!Persist(Candidate, Message)) return Error(Message, TEXT("IO_ERROR"));
            Current->Values = Candidate->Values; Remember(Current, RecordPaths.FindRef(GetString(Current, TEXT("job_id"))));
        }
        TArray<uint8> RawBytes;
        if (!FFileHelper::LoadFileToArray(RawBytes, *GetString(Candidate, TEXT("raw_path"))) || BytesHash(RawBytes) != GetString(Candidate, TEXT("raw_hash"))) return Error(TEXT("Prepared evidence is missing or changed; reservation remains."), TEXT("RECOVERY_REQUIRED"));
        if (IFileManager::Get().FileExists(*Source))
        {
            TArray<uint8> Bytes;
            if (!FFileHelper::LoadFileToArray(Bytes, *Source)) return Error(TEXT("Source cannot be rechecked; reservation remains."), TEXT("IO_ERROR"));
            if (GetBool(Candidate, TEXT("preserve_valid_source")))
            {
                FString SourceText, CheckError; FObject Repaired;
                if (!FFileHelper::LoadFileToString(SourceText, *Source) || !ValidateRecord(Repaired = Parse(SourceText), Source, CheckError)) return Error(TEXT("Repaired source changed again; reservation remains."), TEXT("RECOVERY_REQUIRED"));
                const FString RepairedId = GetString(Repaired, TEXT("job_id"));
                const FString* Existing = RequestIds.Find(GetString(Repaired, TEXT("request_id")));
                if (RepairedId == GetString(Current, TEXT("job_id")) || Queue.Contains(RepairedId) || (Existing && *Existing != RepairedId)) return Error(TEXT("Repaired source conflicts with an existing job/request."), TEXT("RECOVERY_REQUIRED"));
                if (!Terminal(Repaired))
                {
                    Repaired->SetStringField(TEXT("state"), TEXT("interrupted")); Repaired->SetStringField(TEXT("finished_at"), FDateTime::UtcNow().ToIso8601());
                    for (const auto& Value : GetArray(Repaired, TEXT("items")))
                    { const FObject Item = Value->AsObject(); if (GetBool(Item, TEXT("commit_pending"))) Repaired->SetBoolField(TEXT("recovery_required"), true); if (GetString(Item, TEXT("state")) == TEXT("pending") || GetString(Item, TEXT("state")) == TEXT("running")) Item->SetStringField(TEXT("state"), TEXT("interrupted")); }
                    if (!AtomicJson(Source, Repaired, Message)) return Error(Message, TEXT("IO_ERROR"));
                }
                Remember(Repaired, Source);
            }
            else
            {
                if (BytesHash(Bytes) != GetString(Candidate, TEXT("raw_hash"))) { RegisterRecordIssue(Source, TEXT("Source changed; inspect its new bytes before acknowledgement.")); return Error(TEXT("Source changed; it was not removed."), TEXT("RECOVERY_REQUIRED")); }
                if (!IFileManager::Get().Delete(*Source, false, true)) return Error(TEXT("Cannot quarantine the source metadata; reservation remains."), TEXT("IO_ERROR"));
            }
        }
        Candidate->SetStringField(TEXT("phase"), TEXT("acknowledged")); Candidate->SetBoolField(TEXT("recovery_required"), false);
        Candidate->SetStringField(TEXT("recovery_acknowledged_at"), FDateTime::UtcNow().ToIso8601()); Candidate->SetStringField(TEXT("finished_at"), FDateTime::UtcNow().ToIso8601());
        if (!Persist(Candidate, Message)) { OnChanged.Broadcast(); return Error(Message, TEXT("IO_ERROR")); }
        Current->Values = Candidate->Values; Remember(Current, RecordPaths.FindRef(GetString(Current, TEXT("job_id"))));
        bMaintenanceDue = true; OnChanged.Broadcast(); return Reply(Current);
    }
    void PlanMaintenance()
    {
        if (IsBusy() || !bInitialScanComplete) { bMaintenanceDue = true; return; }
        Maintenance.Reset(); const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromDays(7);
        TArray<FString> Eligible, Acknowledged;
        for (const auto& Pair : Summaries)
        {
            const FObject& Record = Pair.Value;
            if (!Terminal(Record) || GetBool(Record, TEXT("recovery_required")) || GetBool(Record, TEXT("retention_time_untrusted")) || Pins.Contains(Pair.Key)) continue;
            if (GetString(Record, TEXT("record_kind")) == TEXT("diagnostic"))
            {
                bool Dependent = false;
                for (const auto& JobPair : Summaries)
                    for (const auto& Journal : GetArray(JobPair.Value, TEXT("recovery_journals")))
                        if (CanonicalPath(Journal->AsString()) == CanonicalPath(GetString(Record, TEXT("source_path")))) Dependent = true;
                if (!Dependent) Acknowledged.Add(Pair.Key);
            }
            else Eligible.Add(Pair.Key);
        }
        auto Sort = [this](const FString& A, const FString& B) { return Newer(A, B); }; Eligible.Sort(Sort); Acknowledged.Sort(Sort);
        for (int32 Index = 0; Index < Eligible.Num(); ++Index)
        {
            const FString& Id = Eligible[Index]; FDateTime Finished;
            if (!Date(GetString(Summaries[Id], TEXT("finished_at")), Finished)) continue;
            const int32 Action = Finished < Cutoff || Index >= 200 ? 2 : Index >= 100 ? 1 : 0;
            const bool Archived = GetBool(Summaries[Id], TEXT("archived"));
            if (Action == 2 || Archived != (Action == 1)) Maintenance.Add({Id, Action});
        }
        for (int32 Index = 0; Index < Acknowledged.Num(); ++Index)
        {
            FDateTime Finished; const FString& Id = Acknowledged[Index];
            if (Date(GetString(Summaries[Id], TEXT("finished_at")), Finished) && (Finished < Cutoff || Index >= 100)) Maintenance.Add({Id, 2});
        }
        LastMaintenance = FDateTime::UtcNow(); bMaintenanceDue = false;
    }
    void Maintain(int32 Budget)
    {
        if (IsBusy()) return;
        bool Changed = false;
        for (int32 Count = 0; Count < FMath::Clamp(Budget, 0, 10) && !Maintenance.IsEmpty(); ++Count)
        {
            const FMaintenanceTask Task = Maintenance[0]; Maintenance.RemoveAt(0);
            const FObject* Summary = Summaries.Find(Task.Id); const FString Source = RecordPaths.FindRef(Task.Id);
            if (!Summary || Pins.Contains(Task.Id) || !Terminal(*Summary) || GetBool(*Summary, TEXT("recovery_required")) || GetBool(*Summary, TEXT("retention_time_untrusted")) ||
                !(Under(Source, Root) || Under(Source, StorageRoot / TEXT("Diagnostics")))) continue;
            bool ProtectedSource = false;
            for (const auto& Pair : Summaries)
                if (GetString(Pair.Value, TEXT("record_kind")) == TEXT("diagnostic") && GetBool(Pair.Value, TEXT("recovery_required")) &&
                    CanonicalPath(GetString(Pair.Value, TEXT("source_path"))) == CanonicalPath(FullPath(Source))) ProtectedSource = true;
            if (ProtectedSource) continue;
            TArray<uint8> SourceBytes; FString SourceText, Validation;
            if (!FFileHelper::LoadFileToArray(SourceBytes, *Source))
            {
                // A reviewed quarantine may already have removed its source. Other
                // missing records remain an explicit issue instead of losing receipts.
                if (!WasRecordIssueAcknowledged(Source)) { Records.Remove(Task.Id); CacheOrder.Remove(Task.Id); RegisterRecordIssue(Source, TEXT("Record could not be re-read before maintenance.")); continue; }
            }
            else
            {
                FFileHelper::BufferToString(SourceText, SourceBytes.GetData(), SourceBytes.Num());
                const FObject DiskRecord = Parse(SourceText);
                if (!ValidateRecord(DiskRecord, Source, Validation))
                { Records.Remove(Task.Id); CacheOrder.Remove(Task.Id); RegisterRecordIssue(Source, Validation); continue; }
                // Refresh the authority used by admission/status before reevaluating
                // protection and rank. Never trust a cached pre-mutation summary.
                FDateTime Finished;
                if (Terminal(DiskRecord) && !Date(GetString(DiskRecord, TEXT("finished_at")), Finished)) DiskRecord->SetBoolField(TEXT("retention_time_untrusted"), true);
                Remember(DiskRecord, Source);
                Changed = true;
                const FObject Fresh = Summaries[Task.Id];
                if (!Terminal(Fresh) || GetBool(Fresh, TEXT("recovery_required")) || GetBool(Fresh, TEXT("retention_time_untrusted")) || Pins.Contains(Task.Id)) { Changed = true; continue; }
                TArray<uint8> Rechecked;
                if (!FFileHelper::LoadFileToArray(Rechecked, *Source) || BytesHash(Rechecked) != BytesHash(SourceBytes))
                { Records.Remove(Task.Id); CacheOrder.Remove(Task.Id); RegisterRecordIssue(Source, TEXT("Record changed immediately before maintenance; review its current contents.")); continue; }
            }
            Summary = Summaries.Find(Task.Id);
            if (GetString(*Summary, TEXT("record_kind")) == TEXT("diagnostic"))
            {
                bool Dependent = false;
                for (const auto& Pair : Summaries) for (const auto& Journal : GetArray(Pair.Value, TEXT("recovery_journals")))
                    if (CanonicalPath(Journal->AsString()) == CanonicalPath(GetString(*Summary, TEXT("source_path")))) Dependent = true;
                if (Dependent) continue;
            }
            // A record may have moved into the retained range since the work list
            // was planned. Recompute its deterministic rank from the fresh index.
            const FObject Fresh = *Summary; FDateTime Finished;
            if (!Date(GetString(Fresh, TEXT("finished_at")), Finished)) continue;
            const bool IsDiagnostic = GetString(Fresh, TEXT("record_kind")) == TEXT("diagnostic");
            int32 Rank = 0;
            for (const auto& Pair : Summaries)
            {
                const FObject& Other = Pair.Value;
                if (Pair.Key == Task.Id || !Terminal(Other) || GetBool(Other, TEXT("recovery_required")) || GetBool(Other, TEXT("retention_time_untrusted")) || Pins.Contains(Pair.Key) ||
                    (GetString(Other, TEXT("record_kind")) == TEXT("diagnostic")) != IsDiagnostic) continue;
                if (Newer(Pair.Key, Task.Id)) ++Rank;
            }
            const int32 CurrentAction = Finished < FDateTime::UtcNow() - FTimespan::FromDays(7) || Rank >= (IsDiagnostic ? 100 : 200) ? 2 : IsDiagnostic ? -1 : Rank >= 100 ? 1 : 0;
            if (CurrentAction != Task.Action) { bMaintenanceDue = true; continue; }
            if (Task.Action == 2)
            {
                if (!IFileManager::Get().Delete(*Source, false, true)) { bMaintenanceDue = true; continue; }
                for (auto It = RequestIds.CreateIterator(); It; ++It) if (It.Value() == Task.Id) It.RemoveCurrent();
                Records.Remove(Task.Id); Summaries.Remove(Task.Id); RecordPaths.Remove(Task.Id); JobRevisions.Remove(Task.Id); CacheOrder.Remove(Task.Id);
            }
            else
            {
                const FString Destination = (Task.Action == 1 ? Root / TEXT("Archive") : Root) / (Task.Id + TEXT(".json"));
                if (FullPath(Source) == FullPath(Destination)) continue;
                if (!Under(Destination, Root) || IFileManager::Get().FileExists(*Destination) || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true) || !IFileManager::Get().Move(*Destination, *Source, false, false)) { bMaintenanceDue = true; continue; }
                RecordPaths[Task.Id] = Destination; (*Summary)->SetBoolField(TEXT("archived"), Task.Action == 1);
            }
            Changed = true; ++Revision;
        }
        if (Changed) { SortHistory(); OnChanged.Broadcast(); }
        Evict();
    }
    static FObject SingleRequest(const FItemSpec& Spec)
    {
        FObject Input = MakeShared<FJsonObject>();
        Input->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        Input->SetArrayField(TEXT("mesh_paths"), Strings({Spec.MeshPath}));
        Input->SetObjectField(TEXT("options"), Clone(Spec.Options));
        Input->SetBoolField(TEXT("overwrite"), Spec.bOverwrite);
        return Input;
    }
    static void SetProgress(const FObject& Job, int32 Completed, int32 Total)
    {
        FObject Progress = MakeShared<FJsonObject>();
        Progress->SetNumberField(TEXT("completed"), Completed);
        Progress->SetNumberField(TEXT("total"), Total);
        Job->SetObjectField(TEXT("progress"), Progress);
    }
    FObject Reply(const FObject& Job) const
    {
        FObject Result = Clone(Job);
        Result->SetBoolField(TEXT("success"), true);
        Result->SetNumberField(TEXT("revision"), static_cast<double>(GetJobRevision(GetString(Job, TEXT("job_id")))));
        Result->SetBoolField(TEXT("archived"), Under(RecordPaths.FindRef(GetString(Job, TEXT("job_id"))), Root / TEXT("Archive")));
        Result->SetBoolField(TEXT("cancel_possible"), GetString(Job, TEXT("state")) == TEXT("queued") ||
            (Runner && Queue.Num() && Queue[0] == GetString(Job, TEXT("job_id")) && Runner->CanCancel()));
        return Result;
    }
    bool Persist(const FObject& Job, FString& Message) const
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (PersistGate && !PersistGate(Job, Message)) return false;
#endif
        const FString Id = GetString(Job, TEXT("job_id"));
        const FString Filename = RecordPaths.Contains(Id) ? RecordPaths[Id] :
            (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic") ? StorageRoot / TEXT("Diagnostics") : Root) / (Id + TEXT(".json"));
        return AtomicJson(Filename, Job, Message);
    }
    void LoadRecords()
    {
        // Complete the index before permitting admission or definitive NOT_FOUND.
        // Quarantine is deliberately outside these explicit JSON scan roots.
        bInitialScanComplete = false;
        TArray<FString> Files;
        for (const FString& Directory : {StorageRoot / TEXT("Diagnostics"), Root, Root / TEXT("Archive")})
        {
            TArray<FString> Names; IFileManager::Get().FindFiles(Names, *(Directory / TEXT("*.json")), true, false);
            for (const FString& Name : Names) Files.Add(Directory / Name);
        }
        for (const FString& Filename : Files)
        {
            const FDateTime OriginalTime = IFileManager::Get().GetTimeStamp(*Filename);
            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *Filename)) { RegisterRecordIssue(Filename, TEXT("Journal cannot be read.")); continue; }
            FObject Job = Parse(Text);
            FString Validation;
            if (!ValidateRecord(Job, Filename, Validation)) { RegisterRecordIssue(Filename, Validation); continue; }
            const FString Id = GetString(Job, TEXT("job_id"));
            if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic")) { Remember(Job, Filename); continue; }
            const FString Request = GetString(Job, TEXT("request_id"));
            if (RequestIds.Contains(Request)) { RegisterRecordIssue(Filename, TEXT("Duplicate persisted request UUID; review both records.")); continue; }
            const FString State = GetString(Job, TEXT("state"));
            bool bChanged = false;
            if (State == TEXT("queued") || State == TEXT("running"))
            {
                Job->SetStringField(TEXT("state"), TEXT("interrupted"));
                Job->SetStringField(TEXT("finished_at"), FDateTime::UtcNow().ToIso8601());
                Job->SetStringField(TEXT("error_code"), TEXT("EDITOR_INTERRUPTED"));
                Job->SetStringField(TEXT("error"), TEXT("Previous Editor stopped before job completion."));
                for (const auto& Value : GetArray(Job, TEXT("items")))
                {
                    FObject Item = Value->AsObject();
                    if (GetBool(Item, TEXT("commit_pending"))) Job->SetBoolField(TEXT("recovery_required"), true);
                    if (GetString(Item, TEXT("state")) == TEXT("running") || GetString(Item, TEXT("state")) == TEXT("pending"))
                    {
                        Item->SetStringField(TEXT("state"), TEXT("interrupted"));
                        Item->SetStringField(TEXT("reason"), TEXT("editor_restarted"));
                    }
                }
                bChanged = true;
            }
            if (Terminal(Job) && !Job->HasField(TEXT("finished_at")))
            {
                if (OriginalTime.GetTicks() > 0 && OriginalTime <= FDateTime::UtcNow() + FTimespan::FromDays(1))
                { Job->SetStringField(TEXT("finished_at"), OriginalTime.ToIso8601()); Job->SetStringField(TEXT("finished_at_source"), TEXT("legacy_file_mtime")); }
                else Job->SetBoolField(TEXT("retention_time_untrusted"), true);
                bChanged = true;
            }
            FDateTime Finished;
            if (Terminal(Job) && (!Date(GetString(Job, TEXT("finished_at")), Finished) || Finished > FDateTime::UtcNow() + FTimespan::FromDays(1))) Job->SetBoolField(TEXT("retention_time_untrusted"), true);
            Remember(Job, Filename);
            if (bChanged)
            {
                FString Message;
                if (!Persist(Job, Message)) { Job->SetBoolField(TEXT("recovery_required"), true); Job->SetStringField(TEXT("error_code"), TEXT("IO_ERROR")); Job->SetStringField(TEXT("error"), Message); Remember(Job, Filename); }
            }
        }
        bInitialScanComplete = true; bMaintenanceDue = true; Evict();
    }
    void ExecuteFrame()
    {
        check(IsInGameThread());
        if (!bStopping && !bStepping) Drive(false);
    }
    bool Tick(float)
    {
        // This callback can be pumped by MaterialBaking's streaming wait. It
        // performs no runner work, including when another provider is baking.
        if (!bStopping && !IsBusy())
        { if (bMaintenanceDue || (FDateTime::UtcNow() - LastMaintenance).GetTotalSeconds() >= 60) PlanMaintenance(); Maintain(10); }
        return !bStopping;
    }
    FObject RunnerSnapshot()
    {
        const uint64 Value = Runner->GetSnapshotRevision();
        if (!CachedRunnerResult || Value == MAX_uint64 || Value != CachedRunnerRevision)
        {
            CachedRunnerResult = Clone(Runner->GetResult()); CachedRunnerRevision = Value;
            CachedRunnerResultHash = RequestHash(CachedRunnerResult);
#if WITH_DEV_AUTOMATION_TESTS
            ++SnapshotBuilds;
#endif
        }
        return CachedRunnerResult;
    }
    FString JournalStamp(const FObject& Job, const FObject& Item) const
    {
        return GetString(Job, TEXT("state")) + TEXT("|") + GetString(Job, TEXT("stage")) + TEXT("|") +
            FString::Printf(TEXT("%.0f|%d|%d|%d|"), GetNumber(Job, TEXT("current_index")), GetBool(Job, TEXT("cancel_requested")), GetBool(Job, TEXT("recovery_required")), GetBool(Item, TEXT("commit_pending"))) +
            GetString(Item, TEXT("state")) + TEXT("|") + CachedRunnerResultHash;
    }
    void JournalFailure(const FObject& Job, const FObject& Item, const FString& Message)
    {
        FObject Result = Clone(GetObject(Item, TEXT("result")));
        Result->SetBoolField(TEXT("native_success"), GetBool(Result, TEXT("success")));
        Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), Message); Result->SetStringField(TEXT("error_code"), TEXT("IO_ERROR"));
        Result->SetBoolField(TEXT("recovery_required"), true); Item->SetObjectField(TEXT("result"), Result); Item->SetStringField(TEXT("state"), TEXT("failed"));
        Job->SetBoolField(TEXT("recovery_required"), true); Job->SetStringField(TEXT("error"), Message); Job->SetStringField(TEXT("error_code"), TEXT("IO_ERROR"));
        Finish(GetString(Job, TEXT("job_id")), TEXT("failed"));
    }
    void Drive(bool bInline)
    {
        if (Queue.IsEmpty() || bStepping) return;
        TGuardValue<bool> Guard(bStepping, true);
        const FString Id = Queue[0];
        FObject Job = Records[Id];
        const auto Items = GetArray(Job, TEXT("items"));
        const int32 Index = static_cast<int32>(GetNumber(Job, TEXT("current_index")));
        if (Index >= Items.Num()) { Finish(Id, TEXT("succeeded")); return; }
        FObject Item = Items[Index]->AsObject();
        if (!Runner)
        {
            if (GetBool(Job, TEXT("cancel_requested"))) { Finish(Id, TEXT("cancelled")); return; }
            FItemSpec Spec; Spec.MeshPath = GetString(Item, TEXT("mesh_path")); Spec.Options = GetObject(Item, TEXT("options")); Spec.bOverwrite = GetBool(Item, TEXT("overwrite"));
            Spec.ExecutionMode = GetString(Item, TEXT("execution_mode")) == TEXT("interactive") ? EExecutionMode::Interactive : EExecutionMode::Unattended;
            Runner = Factory(Spec, Id);
            if (!Runner) { Item->SetObjectField(TEXT("result"), Error(TEXT("Runner could not be created."))); Item->SetStringField(TEXT("state"), TEXT("failed")); Finish(Id, TEXT("failed")); return; }
            CachedRunnerResult.Reset(); CachedRunnerRevision = MAX_uint64; CachedRunnerResultHash.Empty(); LastPersistedStamp.Empty();
            Item->SetStringField(TEXT("state"), TEXT("running"));
            Job->SetStringField(TEXT("state"), TEXT("running"));
        }
        Item->SetStringField(TEXT("stage"), Runner->GetStage());
        Item->SetBoolField(TEXT("commit_pending"), Runner->IsCommitPhase());
        FObject BeforeSnapshot = RunnerSnapshot();
        if (GetObject(Item, TEXT("result")) != BeforeSnapshot) Item->SetObjectField(TEXT("result"), BeforeSnapshot);
        Job->SetStringField(TEXT("stage"), Runner->GetStage());
        FString Message;
        // This includes the first running snapshot and every new critical marker.
        // No mutation Step may run until its recovery paths/marker are durable.
        const FString Before = JournalStamp(Job, Item);
        const bool bBeforeChanged = Before != LastPersistedStamp;
        if (bBeforeChanged && !Persist(Job, Message))
        {
            JournalFailure(Job, Item, Message); return;
        }
        LastPersistedStamp = Before;
        if (bBeforeChanged) Remember(Job, RecordPaths.FindRef(Id));
        const EStepResult Step = Runner->Step(GetBool(Job, TEXT("cancel_requested")), bInline);
        Item->SetStringField(TEXT("stage"), Runner->GetStage());
        FObject AfterSnapshot = RunnerSnapshot();
        if (GetObject(Item, TEXT("result")) != AfterSnapshot) Item->SetObjectField(TEXT("result"), AfterSnapshot);
        Job->SetStringField(TEXT("stage"), Runner->GetStage());
        if (GetBool(CachedRunnerResult, TEXT("recovery_required"))) Job->SetBoolField(TEXT("recovery_required"), true);
        if (Step != EStepResult::Continue)
        {
            Item->SetBoolField(TEXT("commit_pending"), false);
            Item->SetStringField(TEXT("state"), Step == EStepResult::Succeeded ? TEXT("succeeded") : Step == EStepResult::Cancelled ? TEXT("cancelled") : TEXT("failed"));
            Runner->Shutdown(); Runner.Reset();
            if (Step == EStepResult::Failed) { Finish(Id, TEXT("failed")); return; }
            if (Step == EStepResult::Cancelled) { Finish(Id, TEXT("cancelled")); return; }
            Job->SetNumberField(TEXT("current_index"), Index + 1);
            SetProgress(Job, Index + 1, Items.Num());
            if (Index + 1 == Items.Num()) { Finish(Id, TEXT("succeeded")); return; }
        }
        const FString After = JournalStamp(Job, Item);
        if (After != LastPersistedStamp)
        {
            if (!Persist(Job, Message)) { JournalFailure(Job, Item, Message); return; }
            LastPersistedStamp = After; Remember(Job, RecordPaths.FindRef(Id)); OnChanged.Broadcast();
        }
        else if (bBeforeChanged) OnChanged.Broadcast();
    }
    void Finish(const FString& Id, const FString& State)
    {
        FObject Job = Records[Id];
        Job->SetStringField(TEXT("state"), State);
        Job->SetStringField(TEXT("finished_at"), FDateTime::UtcNow().ToIso8601());
        Job->SetBoolField(TEXT("cancel_effective"), State == TEXT("cancelled"));
        int32 Completed = 0;
        const auto Items = GetArray(Job, TEXT("items"));
        for (const auto& Value : Items)
        {
            FObject Item = Value->AsObject();
            const FString ItemState = GetString(Item, TEXT("state"));
            if (ItemState == TEXT("succeeded")) ++Completed;
            if (ItemState == TEXT("pending"))
            {
                Item->SetStringField(TEXT("state"), TEXT("skipped"));
                Item->SetStringField(TEXT("reason"), State == TEXT("cancelled") ? TEXT("job_cancelled") : TEXT("fail_fast"));
            }
            if (ItemState == TEXT("failed"))
            {
                FObject Result = GetObject(Item, TEXT("result"));
                Job->SetStringField(TEXT("error"), GetString(Result, TEXT("error"), GetString(Result, TEXT("message"), TEXT("Item failed."))));
                Job->SetStringField(TEXT("error_code"), GetString(Result, TEXT("error_code"), TEXT("ITEM_FAILED")));
            }
        }
        SetProgress(Job, Completed, Items.Num());
        if (Runner) { Runner->Shutdown(); Runner.Reset(); }
        FString Message;
        if (!Persist(Job, Message))
        {
            Job->SetStringField(TEXT("state"), TEXT("failed")); Job->SetStringField(TEXT("error"), Message); Job->SetStringField(TEXT("error_code"), TEXT("IO_ERROR")); Job->SetBoolField(TEXT("recovery_required"), true);
            // Preserve every native path while ensuring both inline and async legacy
            // callers observe journal failure rather than native success.
            for (const auto& Value : Items)
            {
                FObject Item = Value->AsObject(); FObject Result = GetObject(Item, TEXT("result"));
                if (Result) { Result->SetBoolField(TEXT("native_success"), GetBool(Result, TEXT("success"))); Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), Message); Result->SetStringField(TEXT("error_code"), TEXT("IO_ERROR")); }
            }
            UE_LOG(LogTemp, Warning, TEXT("Cannot save terminal job record; output reservation retained: %s"), *Message);
            FString RetryMessage; Persist(Job, RetryMessage);
        }
        Remember(Job, RecordPaths.FindRef(Id)); bMaintenanceDue = true;
        Queue.Remove(Id);
        Complete(Id);
        OnChanged.Broadcast();
        Evict();
    }
    void Complete(const FString& Id)
    {
        FCompletion Completion;
        if (!Completions.RemoveAndCopyValue(Id, Completion)) return;
        const auto Items = GetArray(Records[Id], TEXT("items"));
        FObject Result = Items.IsEmpty() ? nullptr : GetObject(Items[0]->AsObject(), TEXT("result"));
        if (!Result.IsValid() || !Result->HasField(TEXT("success"))) Result = Error(GetString(Records[Id], TEXT("error"), TEXT("Job interrupted.")), TEXT("EDITOR_INTERRUPTED"));
        if (GetString(Records[Id], TEXT("error_code")) == TEXT("IO_ERROR")) { Result = Clone(Result); Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error_code"), TEXT("IO_ERROR")); Result->SetStringField(TEXT("error"), GetString(Records[Id], TEXT("error"))); }
        Completion(Clone(Result));
    }
    struct FMaintenanceTask { FString Id; int32 Action = 0; };
    const uint64 InstanceGeneration = NextServiceInstanceGeneration();
    FString Provider, Root, StorageRoot;
    FProfileStore& Profiles;
    FObject Defaults;
    FNormalize Normalize;
    FReserve Reserve;
    FFactory Factory;
    FRecoveryAcknowledger AcknowledgeRecovery;
    FAdmissionValidator AdmissionValidator;
    mutable TMap<FString, FObject> Records;
    TMap<FString, FObject> Summaries;
    TMap<FString, FString> RecordPaths;
    TMap<FString, uint64> JobRevisions;
    TMap<FString, int32> Pins;
    mutable TArray<FString> CacheOrder;
    TArray<FString> RecentIds, ArchiveIds, DiagnosticIds;
    TArray<FMaintenanceTask> Maintenance;
    FDateTime LastMaintenance;
    uint64 Revision = 0, CachedRunnerRevision = MAX_uint64;
    FObject CachedRunnerResult;
    FString LastPersistedStamp, CachedRunnerResultHash;
    bool bInitialScanComplete = false, bMaintenanceDue = true;
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<bool(const FObject&, FString&)> PersistGate;
    int32 SnapshotBuilds = 0;
#endif
    TMap<FString, FString> RequestIds;
    TMap<FString, FCompletion> Completions;
    TArray<FString> Queue;
    TUniquePtr<IItemRunner> Runner;
    FDelegateHandle FrameDriver;
    FTSTicker::FDelegateHandle Ticker;
    bool bStopping = false, bStepping = false;
};
}
