#include "CubeShotJobService.h"
#include "CubeShotAssetWriter.h"
#include "Async/Async.h"
#include "CubeShotCaptureService.h"
#include "CubeShotImageUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunctionInterface.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CubeShot::Service
{
using namespace UnrealToolsRuntime;
namespace
{
    TUniquePtr<FProfileStore> ProfileStore;
    TUniquePtr<FJobService> JobService;
    TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
    { TArray<TSharedPtr<FJsonValue>> Out; for (const FString& Value : Values) Out.Add(MakeShared<FJsonValueString>(Value)); return Out; }
    FObject GenerationJson(const FCubeShotGenerationResult& Result, const FString& MeshPath)
    {
        FObject Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("success"), Result.bSucceeded);
        Json->SetNumberField(TEXT("staged_faces"), Result.StagedFaces);
        Json->SetNumberField(TEXT("total_faces"), 6);
        Json->SetStringField(TEXT("mesh_path"), MeshPath);
        Json->SetStringField(TEXT("message"), Result.Message.ToString());
        Json->SetStringField(TEXT("error_code"), Result.ErrorCode);
        if (!Result.bSucceeded) Json->SetStringField(TEXT("error"), Result.Message.ToString());
        Json->SetArrayField(TEXT("saved"), StringValues(Result.SavedObjectPaths));
        Json->SetArrayField(TEXT("failed"), StringValues(Result.FailedObjectPaths));
        Json->SetArrayField(TEXT("dirty_packages"), StringValues(Result.DirtyPackages));
        Json->SetArrayField(TEXT("errors"), StringValues(Result.Errors));
        Json->SetArrayField(TEXT("warnings"), StringValues(Result.Warnings));
        Json->SetStringField(TEXT("warning"), FString::Join(Result.Warnings, TEXT("\n")));
        Json->SetStringField(TEXT("recovery_staging"), Result.StagingDirectory);
        Json->SetBoolField(TEXT("recovery_required"), !Result.DirtyPackages.IsEmpty());
        return Json;
    }
    struct FPngWorkResult { bool bSucceeded=false; FString Filename; FString Error; FCubeShotPngExportInfo Info; };
    FCubeShotAssetWriterConfig QueueWriterConfig()
    {
        FCubeShotAssetWriterConfig Config; Config.bPromptForCheckout = false;
        Config.bRetainSuccessfulStaging = true; return Config;
    }
    class FRunner final : public IItemRunner
    {
        FItemSpec Spec;
        FCubeShotOptions Options;
        TStrongObjectPtr<UStaticMesh> Mesh;
        TStrongObjectPtr<UTexture2D> Background;
        TUniquePtr<FCubeShotCaptureService> Capture;
        TUniquePtr<FCubeShotAssetWriter::FExecution> Execution;
        FCubeShotAssetWriter Writer{QueueWriterConfig()};
        FCubeShotGenerationResult Generated;
        FObject Result;
        uint64 SnapshotRevision = 0;
        mutable uint64 CachedRevision = MAX_uint64;
        mutable FObject CachedResult;
        TSet<UObject*> Dependencies;
        TArray<TStrongObjectPtr<UObject>> HeldDependencies;
        FDelegateHandle ModifiedHandle;
        FDelegateHandle PropertyHandle;
        bool bSourceChanged = false;
        bool bCancel = false;
        bool bCommitted = false;
        int32 Stage = 0;
        int32 PngIndex = 0;
        TArray<FString> PngPaths;
        TFuture<FPngWorkResult> PngFuture;
        FString PreflightWarning;
        FString AbortReason;
        TArray<ECubeShotDirection> PngDirections;
        EStepResult Fail(const FString& Message, const FString& Code)
        {
            if (Execution && !bCommitted) { bCancel = true; Execution->Step(); }
            Result = Execution ? GenerationJson(Execution->GetResult(), Spec.MeshPath) : UnrealToolsRuntime::Error(Message, Code);
            Result->SetBoolField(TEXT("success"), false); Result->SetStringField(TEXT("error"), Message); Result->SetStringField(TEXT("error_code"), Code);
            Result->SetStringField(TEXT("mesh_path"), Spec.MeshPath); Shutdown(); return EStepResult::Failed;
        }
    public:
        explicit FRunner(const FItemSpec& InSpec) : Spec(InSpec) {}
        virtual ~FRunner() override { Shutdown(); }
        virtual void Shutdown() override
        {
            FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle); ModifiedHandle.Reset();
            FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);PropertyHandle.Reset();
            if(PngFuture.IsValid())
            {
                PngFuture.Wait(); const FPngWorkResult Written = PngFuture.Get(); PngFuture=TFuture<FPngWorkResult>();
                if (Written.bSucceeded) PngPaths.AddUnique(Written.Filename);
                else { Generated.bSucceeded = false; Generated.ErrorCode = TEXT("png_failed");
                    Generated.Message = FText::FromString(Written.Error); Generated.Errors.AddUnique(Written.Error); }
                Result = GenerationJson(Generated, Spec.MeshPath);
                Result->SetArrayField(TEXT("png_paths"), StringValues(PngPaths)); ++SnapshotRevision;
                if(!Generated.bSucceeded&&!Generated.StagingDirectory.IsEmpty())Result->SetBoolField(TEXT("recovery_required"),true);
            }
            if (Capture) Capture->Shutdown();
            Capture.Reset(); Execution.Reset(); HeldDependencies.Reset(); Mesh.Reset(); Background.Reset();
            if(Result&& !PngPaths.IsEmpty())Result->SetArrayField(TEXT("png_paths"),StringValues(PngPaths));
            ++SnapshotRevision;
        }
        virtual bool CanCancel() const override { return !bCommitted; }
        virtual bool IsCommitPhase() const override { return bCommitted || (Execution && Execution->IsCritical()); }
        virtual FString GetStage() const override
        { return Stage == 0 ? TEXT("validate") : Stage == 1 && Execution ? Execution->GetStage() : Stage == 2 ? TEXT("png_publish") : TEXT("complete"); }
        virtual uint64 GetSnapshotRevision() const override { return SnapshotRevision; }
        virtual FObject GetResult() const override
        {
            if (CachedRevision != SnapshotRevision)
            {
                CachedResult = Result.IsValid() ? Clone(Result) : Execution ? GenerationJson(Execution->GetResult(), Spec.MeshPath) : MakeShared<FJsonObject>();
                CachedRevision = SnapshotRevision;
            }
            return CachedResult;
        }
        virtual EStepResult Step(bool CancelRequested, bool InlineMode) override
        {
            const FString BeforeStage = GetStage(); const FObject BeforeResult = Result;
            const int32 BeforeStageIndex = Stage;
            const bool HadExecution = Execution.IsValid();
            const int32 BeforeFaces = Execution ? Execution->GetResult().StagedFaces : 0;
            const int32 BeforePngCount = PngPaths.Num();
            const bool BeforeCommit = bCommitted;
            const EStepResult Outcome = StepInternal(CancelRequested, InlineMode);
            if (BeforeStageIndex != Stage || HadExecution != Execution.IsValid() || BeforeStage != GetStage() || BeforeResult != Result || BeforeCommit != bCommitted ||
                BeforeFaces != (Execution ? Execution->GetResult().StagedFaces : 0) || BeforePngCount != PngPaths.Num() || Outcome != EStepResult::Continue)
                ++SnapshotRevision;
            return Outcome;
        }
        EStepResult StepInternal(bool CancelRequested, bool InlineMode)
        {
            bCancel = CancelRequested;
            FText PolicyError;
            if (!bCommitted && !CubeShot::CanExecute(PolicyError)) AbortReason=PolicyError.ToString();
            if(!AbortReason.IsEmpty())
            {
                if(Execution){bCancel=true;if(!Execution->Step(InlineMode))return EStepResult::Continue;}
                return Fail(AbortReason,TEXT("execution_blocked"));
            }
            if (CancelRequested && !bCommitted)
            {
                if (Execution) { bCancel = true; if(!Execution->Step(InlineMode))return EStepResult::Continue; Result = GenerationJson(Execution->GetResult(), Spec.MeshPath); }
                else Result = UnrealToolsRuntime::Error(TEXT("Cancelled before output mutation."), TEXT("cancelled"));
                const bool Applied=!Execution || Execution->GetResult().bCancelled;
                Shutdown(); return Applied?EStepResult::Cancelled:EStepResult::Failed;
            }
            if (Stage == 0)
            {
                FText ParseError;
                if (!LoadOptions(Spec.MeshPath, Spec.Options, Options, ParseError)) return Fail(ParseError.ToString(), TEXT("invalid_options"));
                Mesh.Reset(Options.StaticMesh); Background.Reset(Options.BackgroundTexture);
                Dependencies.Add(Mesh.Get());
                if (Background) Dependencies.Add(Background.Get());
                for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials()) if (UMaterialInterface* Material = Slot.MaterialInterface)
                {
                    Dependencies.Add(Material);
                    TArray<UMaterialFunctionInterface*> Functions;Material->GetDependentFunctions(Functions);
                    for(UMaterialFunctionInterface* Function:Functions)if(Function)Dependencies.Add(Function);
                    if (UMaterial* BaseMaterial = Material->GetMaterial()) Dependencies.Add(BaseMaterial);
                    UMaterialInterface* Parent = Material;
                    TSet<UMaterialInterface*> VisitedParents;
                    while (UMaterialInstance* Instance = Cast<UMaterialInstance>(Parent))
                    {
                        if (VisitedParents.Contains(Instance)) break;
                        VisitedParents.Add(Instance);
                        Parent = Instance->Parent;
                        if (!Parent) break;
                        Dependencies.Add(Parent);
                    }
                    TArray<UTexture*> Textures; Material->GetUsedTextures(Textures);
                    for (UTexture* Texture : Textures) Dependencies.Add(Texture);
                }
                for (UObject* Object : Dependencies) HeldDependencies.Emplace(Object);
                ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object)
                {
                    for (UObject* Dependency : Dependencies)
                        if (Object == Dependency || (Object && Dependency && Object->GetOutermost() == Dependency->GetOutermost())) bSourceChanged = true;
                });
                PropertyHandle=FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* Object,FPropertyChangedEvent&)
                {
                    for(UObject* Dependency:Dependencies)
                        if(Object&&Dependency&&Object->GetOutermost()==Dependency->GetOutermost())bSourceChanged=true;
                });
                FCubeShotPreflightResult Preflight;
                if (!Writer.BuildPreflight(Options, Preflight)) return Fail(Preflight.Error.ToString(), TEXT("preflight_failed"));
                PreflightWarning = Preflight.Warning.ToString();
                if (Preflight.bHasExistingTextures && !Spec.bOverwrite) return Fail(TEXT("Texture outputs exist; pass overwrite=true after reviewing preflight."), TEXT("overwrite_required"));
                if (Spec.Options->GetBoolField(TEXT("export_png")))
                {
                    const TArray<TSharedPtr<FJsonValue>>& Directions = Spec.Options->GetArrayField(TEXT("png_directions"));
                    for (const TSharedPtr<FJsonValue>& Value : Directions)
                        for (const FCubeShotDirectionInfo& Info : CubeShot::GetDirectionInfos())
                            if (Value->AsString() == Info.Label)
                            {
                                PngDirections.Add(Info.Direction);
                                const FString Filename = CubeShot::Image::GetPngFilename(Preflight.Outputs[static_cast<int32>(Info.Direction)].GetObjectPath(), Spec.Options->GetBoolField(TEXT("trim_transparent")), Spec.Options->GetIntegerField(TEXT("padding_pixels")), Spec.Options->GetIntegerField(TEXT("quarter_turns")));
                                if (!Spec.bOverwrite && IFileManager::Get().FileExists(*Filename)) return Fail(TEXT("PNG output exists: ") + Filename, TEXT("overwrite_required"));
                            }
                }
                Capture = MakeUnique<FCubeShotCaptureService>();
                if (!Capture->Initialize(Options, ParseError)) return Fail(ParseError.ToString(), TEXT("capture_failed"));
                // Shader/resource preparation is complete before the change watcher becomes authoritative.
                bSourceChanged = false;
                Execution = Writer.BeginPreparedExecution(Options, [this](const FCubeShotOptions& InOptions, ECubeShotDirection Direction, int32 Resolution, FCubeShotPixelWork& Work, FText& Error)
                    { return Capture->PrepareCaptureWork(InOptions,Direction,Resolution,Work,Error); }, Preflight, Spec.bOverwrite, {}, [this]() { return bCancel; });
                Stage = 1; return EStepResult::Continue;
            }
            if (Stage == 1)
            {
                if (Execution->IsCritical())
                {
                    if (bSourceChanged) return Fail(TEXT("Source mesh or a material/texture dependency changed during capture."), TEXT("source_changed"));
                    bCommitted = true;
                }
                if (!Execution->Step(InlineMode)) return EStepResult::Continue;
                Generated = Execution->GetResult();
                TArray<FString> PreflightWarnings; PreflightWarning.ParseIntoArrayLines(PreflightWarnings);
                for (const FString& Warning : PreflightWarnings) Generated.Warnings.AddUnique(Warning);
                Result = GenerationJson(Generated, Spec.MeshPath);
                if (!Generated.bSucceeded) { const bool Cancelled = Generated.bCancelled; Shutdown(); return Cancelled ? EStepResult::Cancelled : EStepResult::Failed; }
                Capture->Shutdown(); Capture.Reset();
                Stage = 2;
                if (PngDirections.IsEmpty()) { Writer.FinalizeStaging(Generated); Result=GenerationJson(Generated,Spec.MeshPath); Stage = 3; Shutdown(); return EStepResult::Succeeded; }
                return EStepResult::Continue;
            }
            if (Stage == 2)
            {
                if(!PngFuture.IsValid())
                {
                    TArray<FCubeShotOutputDescriptor> Outputs;FText ErrorText;
                    FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(FPackageName::ObjectPathToPackageName(Spec.MeshPath),FPackageName::ObjectPathToObjectName(Spec.MeshPath),TEXT("/Game/CubeShot"),Outputs,ErrorText);
                    const FString TexturePath=Outputs[static_cast<int32>(PngDirections[PngIndex])].GetObjectPath();
                    const int32 FaceIndex = static_cast<int32>(PngDirections[PngIndex]);
                    if (!Generated.StagedFilenames.IsValidIndex(FaceIndex))
                    { Generated.bSucceeded=false;Generated.ErrorCode=TEXT("png_failed");Generated.Message=FText::FromString(TEXT("Immutable staged face is missing."));Result=GenerationJson(Generated,Spec.MeshPath);Result->SetBoolField(TEXT("recovery_required"),true);Shutdown();return EStepResult::Failed; }
                    const FString StagedFilename = Generated.StagedFilenames[FaceIndex];
                    const bool Trim=Spec.Options->GetBoolField(TEXT("trim_transparent"));const int32 Padding=Spec.Options->GetIntegerField(TEXT("padding_pixels"));const int32 Turns=Spec.Options->GetIntegerField(TEXT("quarter_turns"));
                    const FString Filename=CubeShot::Image::GetPngFilename(TexturePath,Trim,Padding,Turns);
                    auto Work=[StagedFilename,Filename,Trim,Padding,Turns,Overwrite=Spec.bOverwrite]() mutable
                    {
                        FPngWorkResult Output;Output.Filename=Filename;FImage Source;FImage Image;FText Error;
                        if (!FImageUtils::LoadImage(*StagedFilename,Source))
                        { Output.Error=TEXT("Cannot read immutable staged face: ")+StagedFilename; return Output; }
                        Source.CopyTo(Image,ERawImageFormat::BGRA8,EGammaSpace::sRGB); Source.Reset();
                        Output.bSucceeded=CubeShot::Image::ExportOwnedImage(Image,Filename,Trim,Padding,Turns,Overwrite,Output.Info,Error);
                        Output.Error=Error.ToString();return Output;
                    };
                    if(InlineMode){TPromise<FPngWorkResult> Promise;PngFuture=Promise.GetFuture();Promise.SetValue(Work());}
                    else {PngFuture=Async(EAsyncExecution::ThreadPool,MoveTemp(Work));return EStepResult::Continue;}
                }
                if(!PngFuture.IsReady())return EStepResult::Continue;
                const FPngWorkResult Written=PngFuture.Get();PngFuture=TFuture<FPngWorkResult>();
                if(!Written.bSucceeded)
                {Generated.bSucceeded=false;Generated.ErrorCode=TEXT("png_failed");Generated.Message=FText::FromString(Written.Error);Generated.Errors.Add(Written.Error);Result=GenerationJson(Generated,Spec.MeshPath);Result->SetBoolField(TEXT("recovery_required"),true);Result->SetArrayField(TEXT("png_paths"),StringValues(PngPaths));Shutdown();return EStepResult::Failed;}
                PngPaths.Add(Written.Filename);++PngIndex;Result->SetArrayField(TEXT("png_paths"),StringValues(PngPaths));
                if(PngIndex==PngDirections.Num()){Writer.FinalizeStaging(Generated);Result=GenerationJson(Generated,Spec.MeshPath);Result->SetArrayField(TEXT("png_paths"),StringValues(PngPaths));Stage=3;Shutdown();return EStepResult::Succeeded;}
                return EStepResult::Continue;
            }
            return EStepResult::Succeeded;
        }
    };
}

FObject DefaultOptions()
{
    FObject Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("resolution"), 512); Json->SetStringField(TEXT("projection"), TEXT("orthographic"));
    Json->SetStringField(TEXT("background"), TEXT("transparent")); Json->SetBoolField(TEXT("auto_fit"), true);
    Json->SetNumberField(TEXT("camera_distance"), 100); Json->SetNumberField(TEXT("field_of_view"), 45); Json->SetNumberField(TEXT("ortho_width"), 100);
    Json->SetArrayField(TEXT("solid_color"), { MakeShared<FJsonValueNumber>(0.014443844f), MakeShared<FJsonValueNumber>(0.014443844f), MakeShared<FJsonValueNumber>(0.014443844f), MakeShared<FJsonValueNumber>(1) });
    Json->SetStringField(TEXT("background_texture"), TEXT("")); Json->SetBoolField(TEXT("export_png"), false);
    Json->SetArrayField(TEXT("png_directions"), { MakeShared<FJsonValueString>(TEXT("Front")) });
    Json->SetBoolField(TEXT("trim_transparent"), true); Json->SetNumberField(TEXT("padding_pixels"), 8); Json->SetNumberField(TEXT("quarter_turns"), 0);
    return Json;
}

bool Normalize(FObject& Json, FString& ErrorText)
{
    const TSet<FString> Allowed = {TEXT("resolution"), TEXT("projection"), TEXT("background"), TEXT("auto_fit"), TEXT("camera_distance"), TEXT("field_of_view"), TEXT("ortho_width"), TEXT("solid_color"), TEXT("background_texture"), TEXT("export_png"), TEXT("png_directions"), TEXT("trim_transparent"), TEXT("padding_pixels"), TEXT("quarter_turns")};
    if (!Json) { ErrorText = TEXT("Options must be an object."); return false; }
    for (const auto& Pair : Json->Values) if (!Allowed.Contains(FString(*Pair.Key))) { ErrorText = TEXT("Unknown CubeShot option: ") + FString(*Pair.Key); return false; }
    FObject Defaults = DefaultOptions(); for (const auto& Pair : Defaults->Values) if (!Json->HasField(Pair.Key)) Json->SetField(Pair.Key, Pair.Value);
    auto Number = [&](const TCHAR* Key, double Min, double Max, bool Integer)
    { double Value; if (!Json->HasTypedField<EJson::Number>(Key) || !Json->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || Value < Min || Value > Max || (Integer && Value != FMath::FloorToDouble(Value))) { ErrorText = FString::Printf(TEXT("%s must be %s in [%g,%g]."), Key, Integer ? TEXT("an integer") : TEXT("a finite number"), Min, Max); return false; } return true; };
    if (!Number(TEXT("resolution"), 256, 4096, true) || !Number(TEXT("camera_distance"), .01, 10000000, false) || !Number(TEXT("field_of_view"), 1, 170, false) || !Number(TEXT("ortho_width"), .01, 10000000, false) || !Number(TEXT("padding_pixels"), 0, 64, true) || !Number(TEXT("quarter_turns"), 0, 3, true)) return false;
    for (const TCHAR* Key : {TEXT("auto_fit"), TEXT("export_png"), TEXT("trim_transparent")}) if (!Json->HasTypedField<EJson::Boolean>(Key)) { ErrorText = FString(Key) + TEXT(" must be boolean."); return false; }
    FString Projection, BackgroundMode, TexturePath;
    if (!Json->TryGetStringField(TEXT("projection"), Projection) || (Projection != TEXT("perspective") && Projection != TEXT("orthographic"))) { ErrorText = TEXT("projection must be perspective or orthographic."); return false; }
    if (!Json->TryGetStringField(TEXT("background"), BackgroundMode) || (BackgroundMode != TEXT("transparent") && BackgroundMode != TEXT("solid") && BackgroundMode != TEXT("texture"))) { ErrorText = TEXT("background must be transparent, solid or texture."); return false; }
    if (!Json->TryGetStringField(TEXT("background_texture"), TexturePath) || (!TexturePath.IsEmpty() && !FPackageName::IsValidObjectPath(TexturePath)) || (BackgroundMode == TEXT("texture") && TexturePath.IsEmpty())) { ErrorText = TEXT("background_texture must be an exact texture object path for texture background."); return false; }
    const TArray<TSharedPtr<FJsonValue>>* Color;
    if (!Json->TryGetArrayField(TEXT("solid_color"), Color) || Color->Num() != 4) { ErrorText = TEXT("solid_color must be four linear RGBA values."); return false; }
    for (const auto& Value : *Color) { double NumberValue; if (!Value->TryGetNumber(NumberValue) || !FMath::IsFinite(NumberValue) || NumberValue < 0 || NumberValue > 1) { ErrorText = TEXT("solid_color values must be finite numbers in [0,1]."); return false; } }
    const TArray<TSharedPtr<FJsonValue>>* Directions;
    if (!Json->TryGetArrayField(TEXT("png_directions"), Directions) || Directions->IsEmpty() || Directions->Num() > 6) { ErrorText = TEXT("png_directions must have one to six unique direction names."); return false; }
    TSet<FString> Seen;
    for (const auto& Value : *Directions) { FString Name; if (!Value->TryGetString(Name) || Seen.Contains(Name)) { ErrorText = TEXT("png_directions contains an invalid or duplicate direction."); return false; } bool Found = false; for (const auto& Info : CubeShot::GetDirectionInfos()) Found |= Name == Info.Label; if (!Found) { ErrorText = TEXT("Unknown PNG direction: ") + Name; return false; } Seen.Add(Name); }
    return true;
}

bool LoadOptions(const FString& MeshPath, const FObject& Json, FCubeShotOptions& Options, FText& ErrorText)
{
    if (!FPackageName::IsValidObjectPath(MeshPath)) { ErrorText = FText::FromString(TEXT("mesh_path must be an exact StaticMesh object path.")); return false; }
    FObject Copy = Clone(Json); FString NormalizeError;
    if (!Normalize(Copy, NormalizeError)) { ErrorText = FText::FromString(NormalizeError); return false; }
    Options.StaticMesh = LoadObject<UStaticMesh>(nullptr, *MeshPath); Options.Resolution = Copy->GetIntegerField(TEXT("resolution"));
    if(Options.StaticMesh&&!Options.StaticMesh->GetPathName().Equals(MeshPath,ESearchCase::IgnoreCase))
    {
        const FString CanonicalPath=Options.StaticMesh->GetPathName();Options.StaticMesh=nullptr;
        ErrorText=FText::FromString(TEXT("mesh_path resolves to a different asset: ")+CanonicalPath
            +TEXT(". Use that exact StaticMesh object path; redirected paths cannot reserve their generated outputs."));
        return false;
    }
    Options.ProjectionMode = Copy->GetStringField(TEXT("projection")) == TEXT("perspective") ? ECubeShotProjectionMode::Perspective : ECubeShotProjectionMode::Orthographic;
    const FString Mode = Copy->GetStringField(TEXT("background")); Options.BackgroundMode = Mode == TEXT("solid") ? ECubeShotBackgroundMode::SolidColor : Mode == TEXT("texture") ? ECubeShotBackgroundMode::Texture : ECubeShotBackgroundMode::Transparent;
    Options.bAutoFit = Copy->GetBoolField(TEXT("auto_fit")); Options.CameraDistance = Copy->GetNumberField(TEXT("camera_distance")); Options.FieldOfView = Copy->GetNumberField(TEXT("field_of_view")); Options.OrthoWidth = Copy->GetNumberField(TEXT("ortho_width"));
    const auto& Color = Copy->GetArrayField(TEXT("solid_color")); Options.SolidColor = FLinearColor(Color[0]->AsNumber(), Color[1]->AsNumber(), Color[2]->AsNumber(), Color[3]->AsNumber());
    const FString TexturePath = Copy->GetStringField(TEXT("background_texture")); if (!TexturePath.IsEmpty()) Options.BackgroundTexture = LoadObject<UTexture2D>(nullptr, *TexturePath);
    FText Warning; return CubeShot::ValidateOptions(Options, ErrorText, Warning);
}

FObject SerializeOptions(const FCubeShotOptions& Options)
{
    FObject Json = DefaultOptions(); Json->SetNumberField(TEXT("resolution"), Options.Resolution);
    Json->SetStringField(TEXT("projection"), Options.ProjectionMode == ECubeShotProjectionMode::Perspective ? TEXT("perspective") : TEXT("orthographic"));
    Json->SetStringField(TEXT("background"), Options.BackgroundMode == ECubeShotBackgroundMode::SolidColor ? TEXT("solid") : Options.BackgroundMode == ECubeShotBackgroundMode::Texture ? TEXT("texture") : TEXT("transparent"));
    Json->SetStringField(TEXT("background_texture"), Options.BackgroundTexture ? Options.BackgroundTexture->GetPathName() : TEXT(""));
    Json->SetBoolField(TEXT("auto_fit"), Options.bAutoFit); Json->SetNumberField(TEXT("camera_distance"), Options.CameraDistance); Json->SetNumberField(TEXT("field_of_view"), Options.FieldOfView); Json->SetNumberField(TEXT("ortho_width"), Options.OrthoWidth);
    Json->SetArrayField(TEXT("solid_color"), {MakeShared<FJsonValueNumber>(Options.SolidColor.R),MakeShared<FJsonValueNumber>(Options.SolidColor.G),MakeShared<FJsonValueNumber>(Options.SolidColor.B),MakeShared<FJsonValueNumber>(Options.SolidColor.A)}); return Json;
}

TArray<FString> Reserve(const FItemSpec& Spec)
{
    TArray<FCubeShotOutputDescriptor> Outputs; FText ErrorText;
    FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(FPackageName::ObjectPathToPackageName(Spec.MeshPath), FPackageName::ObjectPathToObjectName(Spec.MeshPath), TEXT("/Game/CubeShot"), Outputs, ErrorText);
    TArray<FString> Keys;
    for (const auto& Output : Outputs) Keys.Add(Output.GetObjectPath().ToLower());
    if (Spec.Options->GetBoolField(TEXT("export_png")))
        for (const auto& Output : Outputs) Keys.Add(CubeShot::Image::GetPngFilename(Output.GetObjectPath(), Spec.Options->GetBoolField(TEXT("trim_transparent")), Spec.Options->GetIntegerField(TEXT("padding_pixels")), Spec.Options->GetIntegerField(TEXT("quarter_turns"))).ToLower());
    return Keys;
}

FObject BuildPreflight(const FString& MeshPath, const FObject& Json, bool bAllowInteractiveCheckout)
{
    FCubeShotOptions Options; FText ErrorText;
    if(!CubeShot::CanExecute(ErrorText))
    {
        FObject Blocked=UnrealToolsRuntime::Error(ErrorText.ToString(),TEXT("execution_blocked"));
        Blocked->SetBoolField(TEXT("can_proceed"),false);Blocked->SetBoolField(TEXT("requires_overwrite"),false);
        Blocked->SetStringField(TEXT("mesh_path"),MeshPath);Blocked->SetStringField(TEXT("warning"),TEXT(""));
        Blocked->SetArrayField(TEXT("warnings"),{});
        Blocked->SetNumberField(TEXT("resolution"),GetNumber(Json,TEXT("resolution"),512));
        Blocked->SetArrayField(TEXT("outputs"),{});Blocked->SetArrayField(TEXT("png_outputs"),{});return Blocked;
    }
    if (!LoadOptions(MeshPath, Json, Options, ErrorText))
    {
        auto Invalid=UnrealToolsRuntime::Error(ErrorText.ToString(),TEXT("invalid_options"));
        Invalid->SetBoolField(TEXT("can_proceed"),false);Invalid->SetBoolField(TEXT("requires_overwrite"),false);
        Invalid->SetStringField(TEXT("mesh_path"),MeshPath);Invalid->SetStringField(TEXT("warning"),TEXT(""));Invalid->SetArrayField(TEXT("warnings"),{});
        Invalid->SetArrayField(TEXT("outputs"),{});Invalid->SetArrayField(TEXT("png_outputs"),{});return Invalid;
    }
    FCubeShotAssetWriterConfig Config; Config.bPromptForCheckout = bAllowInteractiveCheckout;
    FCubeShotPreflightResult Preflight; FCubeShotAssetWriter(Config).BuildPreflight(Options, Preflight);
    FObject Result = MakeShared<FJsonObject>(); Result->SetBoolField(TEXT("success"), Preflight.bCanProceed); Result->SetBoolField(TEXT("can_proceed"), Preflight.bCanProceed);
    Result->SetStringField(TEXT("mesh_path"), MeshPath); Result->SetBoolField(TEXT("requires_overwrite"), Preflight.bHasExistingTextures);
    Result->SetStringField(TEXT("error"), Preflight.Error.ToString()); Result->SetStringField(TEXT("warning"), Preflight.Warning.ToString()); Result->SetNumberField(TEXT("resolution"), Options.Resolution);
    Result->SetStringField(TEXT("error_code"), Preflight.bCanProceed ? TEXT("") : TEXT("preflight_failed"));
    TArray<FString> Warnings; Preflight.Warning.ToString().ParseIntoArrayLines(Warnings);
    Result->SetArrayField(TEXT("warnings"), StringValues(Warnings));
    Result->SetStringField(TEXT("capture_geometry"), TEXT("fallback_lod0"));
    bool NeedsCheckout = false;
    for (const auto& Output : Preflight.Outputs)
    {
        const FString Filename = FPackageName::LongPackageNameToFilename(Output.LongPackageName,FPackageName::GetAssetPackageExtension());
        NeedsCheckout |= IFileManager::Get().FileExists(*Filename) && IFileManager::Get().IsReadOnly(*Filename);
    }
    Result->SetBoolField(TEXT("requires_checkout"), NeedsCheckout);
    TArray<FString> Paths; for (const auto& Output : Preflight.Outputs) Paths.Add(Output.GetObjectPath()); Result->SetArrayField(TEXT("outputs"), StringValues(Paths));
    FObject Normalized = Clone(Json); FString Ignored; Normalize(Normalized, Ignored);
    TArray<FString> Pngs; bool ExistingPng = false;
    if (Normalized->GetBoolField(TEXT("export_png")))
        for (const auto& Direction : Normalized->GetArrayField(TEXT("png_directions")))
            for (const auto& Output : Preflight.Outputs) if (Direction->AsString() == CubeShot::GetDirectionInfo(Output.Direction).Label)
            { const FString Filename = CubeShot::Image::GetPngFilename(Output.GetObjectPath(), Normalized->GetBoolField(TEXT("trim_transparent")), Normalized->GetIntegerField(TEXT("padding_pixels")), Normalized->GetIntegerField(TEXT("quarter_turns"))); Pngs.Add(Filename); ExistingPng |= IFileManager::Get().FileExists(*Filename); }
    Result->SetArrayField(TEXT("png_outputs"), StringValues(Pngs)); Result->SetBoolField(TEXT("requires_overwrite"), Preflight.bHasExistingTextures || ExistingPng); return Result;
}

FObject InspectBatch(const FObject& Request, bool bAllowInteractiveCheckout)
{
    FPreparedBatch Prepared; FString Message;
    Resolve(Request, Prepared, Message);
    TArray<FString> Errors = Prepared.Errors;
    TArray<TSharedPtr<FJsonValue>> Items;
    bool Overwrite = false, Checkout = false;
    const auto InputPaths = GetArray(Request,TEXT("mesh_paths"));
    for (int32 Index = 0; Index < Prepared.Specs.Num(); ++Index)
    {
        const FItemSpec& Spec = Prepared.Specs[Index];
        FString Path=Spec.MeshPath;if(InputPaths.IsValidIndex(Index))InputPaths[Index]->TryGetString(Path);
        const FString ItemError = Prepared.ItemErrors.IsValidIndex(Index) ? Prepared.ItemErrors[Index] : FString();
        const bool Resolved=Prepared.ItemResolved.IsValidIndex(Index)&&Prepared.ItemResolved[Index];
        FObject Item = Resolved ? BuildPreflight(Spec.MeshPath,Spec.Options,bAllowInteractiveCheckout)
            : UnrealToolsRuntime::Error(ItemError,TEXT("invalid_argument"));
        if(!Resolved){Item->SetBoolField(TEXT("can_proceed"),false);Item->SetStringField(TEXT("warning"),TEXT(""));Item->SetArrayField(TEXT("warnings"),{});}
        if(Resolved&&!ItemError.IsEmpty())
        {
            const FString DomainError=GetString(Item,TEXT("error"));
            Item->SetBoolField(TEXT("success"),false);Item->SetBoolField(TEXT("can_proceed"),false);
            Item->SetStringField(TEXT("error"),DomainError.IsEmpty()?ItemError:DomainError+TEXT("\n")+ItemError);
            Item->SetStringField(TEXT("error_code"),TEXT("preflight_failed"));
        }
        Item->SetStringField(TEXT("mesh_path"),Path);
        if (!GetBool(Item,TEXT("can_proceed"))) Errors.AddUnique(Path+TEXT(": ")+GetString(Item,TEXT("error")));
        Overwrite |= GetBool(Item,TEXT("requires_overwrite")); Checkout |= GetBool(Item,TEXT("requires_checkout"));
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    if (!Prepared.ReservedPaths.IsEmpty() && !Jobs().CheckReservations(Prepared.ReservedPaths,Message)) Errors.AddUnique(Message);
    if (Items.IsEmpty() && Errors.IsEmpty()) Errors.Add(Message.IsEmpty()?TEXT("At least one mesh is required."):Message);
    FObject Plan = MakeShared<FJsonObject>();
    Plan->SetBoolField(TEXT("success"),Errors.IsEmpty());Plan->SetBoolField(TEXT("can_proceed"),Errors.IsEmpty());
    Plan->SetBoolField(TEXT("requires_overwrite"),Overwrite);Plan->SetBoolField(TEXT("requires_checkout"),Checkout);
    Plan->SetArrayField(TEXT("items"),Items);Plan->SetArrayField(TEXT("errors"),StringValues(Errors));
    if (!Errors.IsEmpty()) {Plan->SetStringField(TEXT("error"),FString::Join(Errors,TEXT("\n")));Plan->SetStringField(TEXT("error_code"),TEXT("preflight_failed"));}
    return Plan;
}

bool Resolve(const FObject& Request, FPreparedBatch& Batch, FString& Error) { return Jobs().PrepareBatch(Request,Batch,Error); }

namespace
{
    bool ResolveGeneration(const FObject& Request,FItemSpec& Spec,FString& Message)
    {
        if(!Request||!Request->TryGetStringField(TEXT("mesh_path"),Spec.MeshPath)||!FPackageName::IsValidObjectPath(Spec.MeshPath))
        {Message=TEXT("mesh_path must be an exact StaticMesh object path.");return false;}
        Spec.Options=Clone(Request);Spec.Options->RemoveField(TEXT("mesh_path"));Spec.Options->RemoveField(TEXT("overwrite"));
        if(!Normalize(Spec.Options,Message))return false;
        if(Request->HasField(TEXT("overwrite"))&&!Request->TryGetBoolField(TEXT("overwrite"),Spec.bOverwrite))
        {Message=TEXT("overwrite must be boolean.");return false;}
        return true;
    }
}
FObject GenerateInline(const FObject& Request)
{
    FItemSpec Spec;FString Message;if(!ResolveGeneration(Request,Spec,Message))return UnrealToolsRuntime::Error(Message,TEXT("invalid_argument"));
    return Jobs().RunInline(Spec,Message);
}
FObject GenerateAsync(const FObject& Request,FJobService::FCompletion Completion)
{
    FItemSpec Spec;FString Message;if(!ResolveGeneration(Request,Spec,Message))return UnrealToolsRuntime::Error(Message,TEXT("invalid_argument"));
    return Jobs().StartLegacyStructured(Spec,MoveTemp(Completion));
}
FObject Submit(const FObject& Request) { return Jobs().Start(Request); }

bool CheckoutForSubmission(const FObject& Request, FText& Error,
    TFunction<bool(const TArray<UPackage*>&, FText&)> CheckoutCallback)
{
    check(IsInGameThread());
    FPreparedBatch Prepared; FString Message;
    if (!Resolve(Request,Prepared,Message)) {Error=FText::FromString(FString::Join(Prepared.Errors,TEXT("\n")));return false;}
    if (!Jobs().CheckReservations(Prepared.ReservedPaths,Message)) {Error=FText::FromString(Message);return false;}
    FCubeShotAssetWriterConfig Config;Config.bPromptForCheckout=true;Config.CheckoutCallback=MoveTemp(CheckoutCallback);FCubeShotAssetWriter Writer(Config);
    TArray<FCubeShotOutputDescriptor> Outputs;bool NeedsCheckout=false;
    for (const FItemSpec& Spec : Prepared.Specs)
    {
        FCubeShotOptions Options;FCubeShotPreflightResult Preflight;
        if (!LoadOptions(Spec.MeshPath,Spec.Options,Options,Error) || !Writer.BuildPreflight(Options,Preflight))
        {if(Error.IsEmpty())Error=Preflight.Error;return false;}
        for (const auto& Output:Preflight.Outputs)
        {
            const FString Filename=FPackageName::LongPackageNameToFilename(Output.LongPackageName,FPackageName::GetAssetPackageExtension());
            NeedsCheckout|=IFileManager::Get().FileExists(*Filename)&&IFileManager::Get().IsReadOnly(*Filename);
            Outputs.Add(Output);
        }
    }
    return !NeedsCheckout || Writer.CheckoutForInteractiveSubmission(Outputs,Error);
}

FObject HandleProfiles(const FObject& Request)
{
    FObject Copy=Clone(Request);
    if (GetString(Copy,TEXT("action"))==TEXT("save"))
    {
        FObject Profile=GetObject(Copy,TEXT("profile"));FObject Options=GetObject(Profile,TEXT("options"));FString Message;
        if(!Options)return UnrealToolsRuntime::Error(TEXT("profile.options is required."));
        if(!Normalize(Options,Message))return UnrealToolsRuntime::Error(Message);
        Profile->SetObjectField(TEXT("options"),Options);
    }
    return Profiles().Handle(Copy);
}

FObject ExportPng(const FObject& Request)
{
    check(IsInGameThread());
    FString Message;
    if(!CheckFields(Request,{TEXT("texture_path"),TEXT("trim_transparent"),TEXT("padding_pixels"),TEXT("quarter_turns"),TEXT("overwrite")},Message))return UnrealToolsRuntime::Error(Message);
    bool Trim=false,Overwrite=true;
    for(const TCHAR* Key:{TEXT("trim_transparent"),TEXT("overwrite")})
        if(Request->HasField(Key)&&!Request->HasTypedField<EJson::Boolean>(Key))return UnrealToolsRuntime::Error(FString(Key)+TEXT(" must be boolean."));
    Request->TryGetBoolField(TEXT("trim_transparent"),Trim);Request->TryGetBoolField(TEXT("overwrite"),Overwrite);
    int32 Padding=8,Turns=0;
    auto Integer=[&](const TCHAR* Key,int32 Max,int32& Value)
    {if(!Request->HasField(Key))return true;double Number;if(!Request->HasTypedField<EJson::Number>(Key)||!Request->TryGetNumberField(Key,Number)||!FMath::IsFinite(Number)||Number<0||Number>Max||Number!=FMath::FloorToDouble(Number))return false;Value=static_cast<int32>(Number);return true;};
    if(!Integer(TEXT("padding_pixels"),64,Padding)||!Integer(TEXT("quarter_turns"),3,Turns))return UnrealToolsRuntime::Error(TEXT("PNG padding or rotation is invalid."));
    const FString Path=GetString(Request,TEXT("texture_path"));
    if(!Path.StartsWith(TEXT("/Game/CubeShot/"))||!FPackageName::IsValidObjectPath(Path))return UnrealToolsRuntime::Error(TEXT("texture_path must name an output texture under /Game/CubeShot/."));
    UTexture2D* Texture=LoadObject<UTexture2D>(nullptr,*Path);FImage Source,Owned;
    if(!Texture||!Texture->Source.GetMipImage(Source,0))return UnrealToolsRuntime::Error(TEXT("Texture source mip could not be read."));
    Source.CopyTo(Owned,ERawImageFormat::BGRA8,EGammaSpace::sRGB);Source.Reset();
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));
    const FString Filename=CubeShot::Image::GetPngFilename(Path,Trim,Padding,Turns);
    FCubeShotPngExportInfo Info;FText Error;
    if(!CubeShot::Image::ExportOwnedImage(Owned,Filename,Trim,Padding,Turns,Overwrite,Info,Error))return UnrealToolsRuntime::Error(Error.ToString(),TEXT("png_failed"));
    FObject Result=Ok();Result->SetStringField(TEXT("texture_path"),Path);Result->SetStringField(TEXT("png_path"),Filename);
    Result->SetBoolField(TEXT("trim_transparent"),Trim);Result->SetNumberField(TEXT("padding_pixels"),Padding);Result->SetNumberField(TEXT("quarter_turns"),Turns);
    Result->SetNumberField(TEXT("source_width"),Info.SourceWidth);Result->SetNumberField(TEXT("source_height"),Info.SourceHeight);
    Result->SetNumberField(TEXT("width"),Info.Width);Result->SetNumberField(TEXT("height"),Info.Height);
    Result->SetNumberField(TEXT("visible_pixels"),Info.VisiblePixels);Result->SetNumberField(TEXT("transparent_pixels"),Info.TransparentPixels);
    Result->SetBoolField(TEXT("has_visible_subject"),Info.VisiblePixels>0);Result->SetBoolField(TEXT("subject_touches_edge"),Info.bSubjectTouchesEdge);
    Result->SetBoolField(TEXT("overwrite_allowed"),Overwrite);Result->SetBoolField(TEXT("existing_file_observed"),Info.bExistingFileObserved);
    return Result;
}

void Initialize()
{
    if (JobService) return;
    ProfileStore = MakeUnique<FProfileStore>(TEXT("cubeshot"), TEXT("CubeShot"));
    FObject Editor = DefaultOptions(); Editor->SetNumberField(TEXT("resolution"), 1024); Editor->SetStringField(TEXT("projection"), TEXT("perspective"));
    ProfileStore->AddBuiltin(TEXT("editor_default"), TEXT("Editor Default"), Editor);
    FObject Icon = DefaultOptions(); Icon->SetBoolField(TEXT("export_png"), true);
    ProfileStore->AddBuiltin(TEXT("item_icon"), TEXT("Item Icon"), Icon);
    JobService = MakeUnique<FJobService>(TEXT("cubeshot"), TEXT("CubeShot"), *ProfileStore, DefaultOptions(), &Normalize, &Reserve,
        [](const FItemSpec& Spec, const FString&) { return TUniquePtr<IItemRunner>(new FRunner(Spec)); });
    JobService->SetAdmissionValidator([](const TArray<FItemSpec>&,FString& Message)
    {FText Error;const bool Allowed=CubeShot::CanExecute(Error);Message=Error.ToString();return Allowed;});
    JobService->SetRecoveryAcknowledger([](const FObject& Job, FString& Message)
    {
        for (const auto& Value : GetArray(Job, TEXT("items")))
            for (const auto& PackageValue : GetArray(GetObject(Value->AsObject(), TEXT("result")), TEXT("dirty_packages")))
                if (UPackage* Package = FindPackage(nullptr, *PackageValue->AsString()); Package && Package->IsDirty())
                { Message = TEXT("Resolve unsaved output package changes before acknowledging recovery: ") + Package->GetName(); return false; }
        return true;
    });
}
void Shutdown() { if (JobService) JobService->Shutdown(); JobService.Reset(); ProfileStore.Reset(); }
FProfileStore& Profiles() { check(ProfileStore); return *ProfileStore; }
FJobService& Jobs() { check(JobService); return *JobService; }
#if WITH_DEV_AUTOMATION_TESTS
TUniquePtr<IItemRunner> CreateRunnerForAutomation(const FItemSpec& Spec) { return TUniquePtr<IItemRunner>(new FRunner(Spec)); }
#endif
}
