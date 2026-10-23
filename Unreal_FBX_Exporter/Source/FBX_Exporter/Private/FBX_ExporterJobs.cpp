#include "FBX_ExporterService.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"

namespace FBXExporter
{
namespace
{
	TUniquePtr<UnrealToolsRuntime::FProfileStore> ProfileStore;
	TUniquePtr<UnrealToolsRuntime::FJobService> JobService;
	FTSTicker::FDelegateHandle RecoveryScanHandle;
	bool bRecoveryDiagnosticsReady = false;
	bool bRecoveryScanActive = false;
	float RecoveryRefreshElapsed = 0;
	constexpr const TCHAR* JobMapNames[] = {TEXT("BaseColor"), TEXT("Normal"), TEXT("Mask"), TEXT("Occlusion"), TEXT("Emission"), TEXT("Opacity")};
	constexpr EFBXExporterTextureMap JobMapFlags[] = {EFBXExporterTextureMap::BaseColor, EFBXExporterTextureMap::Normal, EFBXExporterTextureMap::Mask, EFBXExporterTextureMap::Occlusion, EFBXExporterTextureMap::Emission, EFBXExporterTextureMap::Opacity};
}

UnrealToolsRuntime::FObject OptionsToJson(const FFBXExporterOptions& Options)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("output_root"), Options.OutputRoot);
	Out->SetStringField(TEXT("output_layout"), Options.OutputLayout);
	Out->SetNumberField(TEXT("resolution"), Options.BakeResolution);
	Out->SetBoolField(TEXT("embed_textures"), Options.bEmbedTexturesInFbx);
	Out->SetBoolField(TEXT("reuse_direct_textures"), Options.bReuseDirectTextures);
	TArray<TSharedPtr<FJsonValue>> Maps;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(JobMapNames); ++Index)
		if (IsFBXExporterTextureMapEnabled(Options.TextureMaps, JobMapFlags[Index])) Maps.Add(MakeShared<FJsonValueString>(JobMapNames[Index]));
	Out->SetArrayField(TEXT("maps"), Maps);
	return Out;
}

FFBXExporterOptions OptionsFromJson(const UnrealToolsRuntime::FObject& Json)
{
	FFBXExporterOptions Options;
	Options.OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Exports"));
	if (!Json.IsValid()) return Options;
	Json->TryGetStringField(TEXT("output_root"), Options.OutputRoot);
	Json->TryGetStringField(TEXT("output_layout"), Options.OutputLayout);
	double Resolution; if (Json->TryGetNumberField(TEXT("resolution"), Resolution)) Options.BakeResolution = static_cast<int32>(Resolution);
	Json->TryGetBoolField(TEXT("embed_textures"), Options.bEmbedTexturesInFbx);
	Json->TryGetBoolField(TEXT("reuse_direct_textures"), Options.bReuseDirectTextures);
	const TArray<TSharedPtr<FJsonValue>>* Maps;
	if (Json->TryGetArrayField(TEXT("maps"), Maps))
	{
		Options.TextureMaps = EFBXExporterTextureMap::None;
		for (const auto& Value : *Maps) for (int32 Index = 0; Index < UE_ARRAY_COUNT(JobMapNames); ++Index)
			if (Value->AsString() == JobMapNames[Index]) Options.TextureMaps |= JobMapFlags[Index];
	}
	return Options;
}

bool NormalizeJobOptions(UnrealToolsRuntime::FObject& Json, FString& Error)
{
	if (!Json.IsValid()) { Error = TEXT("options must be an object."); return false; }
	const TSet<FString> Allowed = {TEXT("output_root"), TEXT("output_layout"), TEXT("resolution"), TEXT("embed_textures"), TEXT("reuse_direct_textures"), TEXT("maps")};
	for (const auto& Entry : Json->Values)
		if (!Allowed.Contains(FString(FStringView(Entry.Key)))) { Error = TEXT("Unknown option: ") + FString(FStringView(Entry.Key)); return false; }
	for (const TCHAR* Name : {TEXT("embed_textures"), TEXT("reuse_direct_textures")})
		if (Json->HasField(Name) && !Json->HasTypedField<EJson::Boolean>(Name)) { Error = FString(Name) + TEXT(" must be boolean."); return false; }
	if (Json->HasField(TEXT("resolution")))
	{
		double Value = 0;
		if (!Json->HasTypedField<EJson::Number>(TEXT("resolution")) || !Json->TryGetNumberField(TEXT("resolution"), Value) || (Value != 512 && Value != 1024 && Value != 2048 && Value != 4096))
		{ Error = TEXT("resolution must be 512, 1024, 2048 or 4096."); return false; }
	}
	FString Root;
	if (Json->HasField(TEXT("output_root")) && (!Json->TryGetStringField(TEXT("output_root"), Root) || Root.TrimStartAndEnd().IsEmpty() || FPaths::IsRelative(Root)))
	{ Error = TEXT("output_root must be an absolute directory."); return false; }
	FString Layout;
	if (Json->HasField(TEXT("output_layout")) && (!Json->TryGetStringField(TEXT("output_layout"), Layout) || (Layout != TEXT("mesh_name") && Layout != TEXT("asset_path"))))
	{ Error = TEXT("output_layout must be mesh_name or asset_path."); return false; }
	const TArray<TSharedPtr<FJsonValue>>* Maps;
	if (Json->HasField(TEXT("maps")))
	{
		if (!Json->TryGetArrayField(TEXT("maps"), Maps)) { Error = TEXT("maps must be an array."); return false; }
		TSet<FString> Unique;
		for (const auto& Value : *Maps)
		{
			FString Name; bool bKnown = false;
			if (Value.IsValid() && Value->Type == EJson::String && Value->TryGetString(Name))
				for (const TCHAR* Known : JobMapNames) bKnown |= Name == Known;
			if (!bKnown || Unique.Contains(Name)) { Error = TEXT("maps must contain unique known map names."); return false; }
			Unique.Add(Name);
		}
	}
	FFBXExporterOptions Options = OptionsFromJson(Json);
	FPaths::NormalizeDirectoryName(Options.OutputRoot);
	if (!FPaths::CollapseRelativeDirectories(Options.OutputRoot)) { Error = TEXT("Cannot normalize output_root."); return false; }
	Json = OptionsToJson(Options);
	return true;
}

FString PlannedDirectory(const FString& MeshPath, const FFBXExporterOptions& Options)
{
	if (Options.OutputLayout == TEXT("asset_path"))
	{
		FString Package = FPackageName::ObjectPathToPackageName(MeshPath); Package.RemoveFromStart(TEXT("/"));
		TArray<FString> Parts; Package.ParseIntoArray(Parts, TEXT("/"), true); FString Relative;
		for (const FString& Part : Parts) Relative /= MakeSafeFileName(Part);
		return Options.OutputRoot / Relative;
	}
	return Options.OutputRoot / MakeSafeFileName(FPackageName::ObjectPathToObjectName(MeshPath));
}

void InitializeJobs()
{
	if (JobService) return;
	ProfileStore = MakeUnique<UnrealToolsRuntime::FProfileStore>(TEXT("fbx_exporter"), TEXT("FBX_Exporter"));
	FFBXExporterOptions Defaults;
	Defaults.OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Exports"));
	ProfileStore->AddBuiltin(TEXT("pbr"), TEXT("PBR"), OptionsToJson(Defaults));
	FFBXExporterOptions Geometry = Defaults; Geometry.TextureMaps = EFBXExporterTextureMap::None;
	ProfileStore->AddBuiltin(TEXT("geometry_only"), TEXT("Geometry Only"), OptionsToJson(Geometry));
	JobService = MakeUnique<UnrealToolsRuntime::FJobService>(TEXT("fbx_exporter"), TEXT("FBX_Exporter"), *ProfileStore, OptionsToJson(Defaults),
		&NormalizeJobOptions,
		[](const UnrealToolsRuntime::FItemSpec& Item) { return TArray<FString>{PlannedDirectory(Item.MeshPath, OptionsFromJson(Item.Options))}; },
		&CreateExportRunner);
	JobService->SetAdmissionValidator([](const TArray<UnrealToolsRuntime::FItemSpec>& Items, FString& Error)
	{
		if (!bRecoveryDiagnosticsReady) { Error = TEXT("FBX recovery records are still being indexed."); return false; }
		if (!GEditor || GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
		{ Error = TEXT("FBX jobs require an Editor with PIE/SIE stopped."); return false; }
		for (const auto& Item : Items)
			if (OptionsFromJson(Item.Options).TextureMaps != EFBXExporterTextureMap::None &&
				(!FApp::CanEverRender() || !GDynamicRHI || GUsingNullRHI))
			{ Error = TEXT("Texture jobs require a rendering Editor (no -nullrhi). Geometry-only jobs remain available."); return false; }
		return true;
	});
	JobService->SetRecoveryAcknowledger(&AcknowledgeJobRecovery);
	RegisterRecoveryDiagnostics();
	bRecoveryScanActive = true;
	RecoveryScanHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float Delta)
	{
		if (!JobService) return false;
		if (!JobService->IsInitialScanComplete()) return true;
		if (bRecoveryScanActive)
		{
			if (AdvanceRecoveryDiagnostics()) { bRecoveryScanActive = false; bRecoveryDiagnosticsReady = true; RecoveryRefreshElapsed = 0; JobService->OnChanged.Broadcast(); }
		}
		else if ((RecoveryRefreshElapsed += Delta) >= 30) { RegisterRecoveryDiagnostics(); bRecoveryScanActive = true; }
		return true;
	}));
}
bool AcknowledgeJobRecovery(const UnrealToolsRuntime::FObject& Job, FString& Error)
{
		TArray<FString> Journals;
		for (const auto& Value : UnrealToolsRuntime::GetArray(Job, TEXT("items")))
		{
			const auto Result = UnrealToolsRuntime::GetObject(Value->AsObject(), TEXT("result"));
			const FString Journal = UnrealToolsRuntime::GetString(Result, TEXT("recovery_journal"));
			if (Journal.IsEmpty()) continue;
			const FString RecoveryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery"));
			if (!FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Journal), RecoveryRoot)) { Error = TEXT("Recorded recovery journal belongs to another metadata root."); return false; }
			const FString Source = UnrealToolsRuntime::CanonicalPath(FPaths::ConvertRelativePathToFull(Journal));
			if (JobService)
				for (const auto& Issue : JobService->GetRecordIssues())
					if (UnrealToolsRuntime::CanonicalPath(FPaths::ConvertRelativePathToFull(UnrealToolsRuntime::GetString(Issue, TEXT("source_path")))) == Source)
					{ Error = TEXT("Acknowledge this journal's diagnostic before releasing its job recovery: ") + UnrealToolsRuntime::GetString(Issue, TEXT("job_id")); return false; }
			// A durable pre-publish checkpoint can precede journal creation. This
			// callback runs only after the user's explicit inspected-recovery action.
			if (!IFileManager::Get().FileExists(*Journal)) continue;
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Journal) || !ValidateRecoveryJournal(UnrealToolsRuntime::Parse(Text), Error))
			{ if (Error.IsEmpty()) Error = TEXT("Recorded FBX recovery journal is unreadable: ") + Journal; Jobs().RegisterRecordIssue(Journal, Error); return false; }
			const auto Data = UnrealToolsRuntime::Parse(Text);
			const FString Final = UnrealToolsRuntime::GetString(Result, TEXT("final_directory"));
			if (Final.IsEmpty() || UnrealToolsRuntime::CanonicalPath(Final) != UnrealToolsRuntime::CanonicalPath(UnrealToolsRuntime::GetString(Data, TEXT("original"))) ||
				(Data->HasField(TEXT("job_id")) && UnrealToolsRuntime::GetString(Data, TEXT("job_id")) != UnrealToolsRuntime::GetString(Job, TEXT("job_id"))))
			{ Error = TEXT("Recovery journal does not belong to this job target."); return false; }
			Journals.AddUnique(Journal);
		}
		for (const FString& Journal : Journals) if (!AcknowledgeRecovery(Journal, Error)) return false;
		return true;
}

void ShutdownJobs() { if (RecoveryScanHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(RecoveryScanHandle); RecoveryScanHandle.Reset(); CancelRecoveryDiagnostics(); bRecoveryDiagnosticsReady = false; bRecoveryScanActive = false; RecoveryRefreshElapsed = 0; if (JobService) JobService->Shutdown(); JobService.Reset(); ProfileStore.Reset(); }
bool AreRecoveryDiagnosticsReady() { return bRecoveryDiagnosticsReady; }
void RefreshRecoveryDiagnostics() { InitializeJobs(); RegisterRecoveryDiagnostics(); bRecoveryScanActive = true; bRecoveryDiagnosticsReady = false; JobService->OnChanged.Broadcast(); }
UnrealToolsRuntime::FProfileStore& Profiles() { InitializeJobs(); return *ProfileStore; }
UnrealToolsRuntime::FJobService& Jobs() { InitializeJobs(); return *JobService; }
}
