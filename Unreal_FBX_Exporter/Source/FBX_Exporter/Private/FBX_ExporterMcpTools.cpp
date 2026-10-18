#include "FBX_ExporterMcpTools.h"

#include "FBX_ExporterService.h"
#include "FBX_ExporterApplication.h"
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
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

namespace FBXExporter::Mcp
{
namespace
{
	FDelegateHandle RefreshHandle;
	TArray<TSharedRef<IModelContextProtocolTool>> RegisteredTools;
	bool bRegistered = false;
	uint64 RegistrationGeneration = 0;
	bool bForcePostRequestsForTesting = false;

	struct FMapOption
	{
		const TCHAR* Name;
		EFBXExporterTextureMap Flag;
	};
	constexpr FMapOption MapOptions[] = {
		{TEXT("BaseColor"), EFBXExporterTextureMap::BaseColor},
		{TEXT("Normal"), EFBXExporterTextureMap::Normal},
		{TEXT("Mask"), EFBXExporterTextureMap::Mask},
		{TEXT("Occlusion"), EFBXExporterTextureMap::Occlusion},
		{TEXT("Emission"), EFBXExporterTextureMap::Emission},
		{TEXT("Opacity"), EFBXExporterTextureMap::Opacity}
	};

	FString DefaultOutputRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Exports"));
	}

	TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const FString& Value : Values) Json.Add(MakeShared<FJsonValueString>(Value));
		return Json;
	}

	TArray<FString> MapNames(const EFBXExporterTextureMap Maps)
	{
		TArray<FString> Names;
		for (const FMapOption& Map : MapOptions)
		{
			if (IsFBXExporterTextureMapEnabled(Maps, Map.Flag)) Names.Add(Map.Name);
		}
		return Names;
	}

	FModelContextProtocolToolResult Result(const TSharedRef<FJsonObject>& Data, const bool bError = false)
	{
		Data->SetBoolField(TEXT("success"), !bError);
		if (bError && !Data->HasField(TEXT("error")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Errors;
			Data->SetStringField(TEXT("error"), Data->TryGetArrayField(TEXT("errors"), Errors) && !Errors->IsEmpty() ? (*Errors)[0]->AsString() : TEXT("FBX operation failed."));
			if (!Data->HasField(TEXT("error_code"))) Data->SetStringField(TEXT("error_code"), TEXT("operation_failed"));
		}
		FString Text;
		FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Text));
		FModelContextProtocolToolResult Out = UE::ModelContextProtocol::MakeTextResult(Text);
		Out.JsonObject->SetObjectField(TEXT("structuredContent"), Data);
		Out.JsonObject->SetBoolField(TEXT("isError"), bError);
		return Out;
	}

	FModelContextProtocolToolResult Error(const FString& Message, const FString& Code = TEXT("invalid_request"))
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("error"), Message);
		Data->SetStringField(TEXT("error_code"), Code);
		return Result(Data, true);
	}

	struct FRequest { UnrealToolsRuntime::FItemSpec Spec; };
	bool ReadRequest(const TSharedPtr<FJsonObject>& Params, FRequest& Request, FString& Error)
	{ return Application::ReadFlatRequest(Params, Request.Spec, Error); }

	void AddOptions(const TSharedRef<FJsonObject>& Data, const FRequest& Request)
	{
		const FFBXExporterOptions Options = OptionsFromJson(Data->HasField(TEXT("maps")) ? Data : Request.Spec.Options);
		Data->SetStringField(TEXT("mesh_path"), Request.Spec.MeshPath);
		if (!Data->HasField(TEXT("output_root"))) Data->SetStringField(TEXT("output_root"), Options.OutputRoot);
		if (!Data->HasField(TEXT("resolution"))) Data->SetNumberField(TEXT("resolution"), Options.BakeResolution);
		Data->SetArrayField(TEXT("selected_maps"), Strings(MapNames(Options.TextureMaps)));
		Data->SetBoolField(TEXT("textures_embedded"), Options.bEmbedTexturesInFbx && Options.TextureMaps != EFBXExporterTextureMap::None);
	}

	FModelContextProtocolToolResult Status(const TSharedPtr<FJsonObject>&)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("plugin"), TEXT("FBX_Exporter"));
		Data->SetStringField(TEXT("plugin_version"), TEXT("1.2.0"));
		Data->SetStringField(TEXT("api_version"), TEXT("1.2.0"));
		Data->SetArrayField(TEXT("capabilities"), Strings({TEXT("profiles"), TEXT("jobs"), TEXT("request_id"), TEXT("batch_preflight"), TEXT("extended_options"), TEXT("asset_path"), TEXT("recovery"), TEXT("linear_direct_textures"), TEXT("history_retention"), TEXT("record_diagnostics")}));
		Data->SetStringField(TEXT("project"), FApp::GetProjectName());
		Data->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Data->SetStringField(TEXT("default_output_root"), DefaultOutputRoot());
		const auto RecordIssues = Jobs().GetRecordIssues();
		const bool bRecordBlocked = !RecordIssues.IsEmpty();
		const bool bEditorReady = GEditor && !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor && !IsExportInProgress() && AreRecoveryDiagnosticsReady() && !bRecordBlocked;
		const bool bRenderingReady = FApp::CanEverRender() && GDynamicRHI && !GUsingNullRHI;
		Data->SetBoolField(TEXT("rendering_available"), bRenderingReady);
		Data->SetStringField(TEXT("texture_blocked_reason"), bRenderingReady ? TEXT("") : TEXT("renderer_unavailable"));
		Data->SetStringField(TEXT("blocked_reason"), !GEditor ? TEXT("editor_unavailable") : (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor) ? TEXT("editor_playing") : IsExportInProgress() ? TEXT("busy") : !AreRecoveryDiagnosticsReady() ? TEXT("record_indexing") : bRecordBlocked ? TEXT("record_review_required") : TEXT(""));
		TArray<TSharedPtr<FJsonValue>> Active;
		for (const auto& Job : Jobs().GetActiveJobs())
		{
			FString State; Job->TryGetStringField(TEXT("state"), State);
			if (State == TEXT("running") || State == TEXT("queued")) Active.Add(MakeShared<FJsonValueObject>(Job));
		}
		Data->SetArrayField(TEXT("active_jobs"), Active);
		Data->SetObjectField(TEXT("retention_policy"), Jobs().GetRetentionPolicy());
		TArray<TSharedPtr<FJsonValue>> Diagnostics; for (const auto& Issue : RecordIssues) Diagnostics.Add(MakeShared<FJsonValueObject>(Issue));
		Data->SetArrayField(TEXT("record_diagnostics"), Diagnostics);
		Data->SetBoolField(TEXT("export_in_progress"), IsExportInProgress());
		Data->SetBoolField(TEXT("geometry_export_available"), bEditorReady);
		Data->SetBoolField(TEXT("texture_export_available"), bEditorReady && bRenderingReady);
		Data->SetArrayField(TEXT("supported_maps"), Strings(MapNames(EFBXExporterTextureMap::All)));
		Data->SetStringField(TEXT("overwrite_policy"), TEXT("Default: refuse. overwrite=true replaces the entire mesh folder, including user files."));
		Data->SetStringField(TEXT("texture_output"), TEXT("Embedding includes PNG bytes in the FBX and also keeps external PNGs. JSON records material metadata; FBX stores the texture connections."));
		return Result(Data);
	}

	FModelContextProtocolToolResult FindMeshes(const TSharedPtr<FJsonObject>& Params)
	{
		FString Query;
		FString Root = TEXT("/Game");
		if (!Params->TryGetStringField(TEXT("query"), Query) || Query.TrimStartAndEnd().IsEmpty())
			return Error(TEXT("query must be a nonempty name fragment."));
		if (Params->HasField(TEXT("root")) && !Params->TryGetStringField(TEXT("root"), Root))
			return Error(TEXT("root must be a package directory such as /Game or /Engine/BasicShapes."));
		if (Root != TEXT("/Game") && Root != TEXT("/Engine") && !FPackageName::IsValidLongPackageName(Root, true))
			return Error(TEXT("root must be a valid package directory."));
		TArray<FString> Mounted; FPackageName::QueryRootContentPaths(Mounted);
		const bool bMounted = Mounted.ContainsByPredicate([&Root](FString Mount) { Mount.RemoveFromEnd(TEXT("/")); return Root == Mount || Root.StartsWith(Mount + TEXT("/")); });
		if (!bMounted) return Error(TEXT("root must belong to a registered content mount."));
		double Limit = 40;
		if (Params->HasField(TEXT("limit")) &&
			(!Params->HasTypedField<EJson::Number>(TEXT("limit")) || !Params->TryGetNumberField(TEXT("limit"), Limit) ||
			 !FMath::IsFinite(Limit) || Limit != FMath::FloorToDouble(Limit) || Limit < 1 || Limit > 200))
			return Error(TEXT("limit must be an integer from 1 to 200."));
		FARFilter Filter;
		Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*Root));
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.GetAssets(Filter, Assets);
		TArray<FString> Matches;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Contains(Query)) Matches.Add(Asset.GetObjectPathString());
		}
		Matches.Sort();
		const int32 Total = Matches.Num();
		if (Matches.Num() > static_cast<int32>(Limit)) Matches.SetNum(static_cast<int32>(Limit));
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("meshes"), Strings(Matches));
		Data->SetNumberField(TEXT("total_matches"), Total);
		Data->SetBoolField(TEXT("truncated"), Total > Limit);
		Data->SetBoolField(TEXT("asset_registry_loading"), Registry.IsLoadingAssets());
		return Result(Data);
	}

	FModelContextProtocolToolResult Preflight(const TSharedPtr<FJsonObject>& Params)
	{
		const bool bBatch = Params->HasField(TEXT("mesh_paths"));
		if (bBatch && Params->HasField(TEXT("mesh_path"))) return Error(TEXT("Use mesh_path or mesh_paths, not both."));
		auto Input = UnrealToolsRuntime::Clone(Params);
		Input->RemoveField(TEXT("mesh_paths")); Input->RemoveField(TEXT("mesh_path"));
		auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Request->SetObjectField(TEXT("options"), Input);
		if (bBatch) Request->SetField(TEXT("mesh_paths"), Params->TryGetField(TEXT("mesh_paths")));
		else
		{
			FString Path;
			if (!Params->TryGetStringField(TEXT("mesh_path"), Path)) return Error(TEXT("mesh_path or mesh_paths is required."));
			Request->SetArrayField(TEXT("mesh_paths"), {MakeShared<FJsonValueString>(Path)});
		}
		const auto Plan = Application::InspectBatch(Request);
		TArray<TSharedPtr<FJsonValue>> Items;
		const bool bBusy = IsExportInProgress();
		for (const auto& Item : Plan.Items)
		{
			auto Data = Application::InspectItemJson(Item);
			Data->SetBoolField(TEXT("export_in_progress"), bBusy);
			if (bBusy) { Data->SetBoolField(TEXT("can_export"), false); Data->SetBoolField(TEXT("success"), false); if (!Data->HasField(TEXT("error"))) { Data->SetStringField(TEXT("error"), TEXT("An FBX export is already running.")); Data->SetStringField(TEXT("error_code"), TEXT("BUSY")); } }
			Items.Add(MakeShared<FJsonValueObject>(Data));
		}
		if (!bBatch && Items.Num() == 1) return Result(Items[0]->AsObject().ToSharedRef(), !Plan.CanExport() || bBusy);
		auto Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("can_export"), Plan.CanExport() && !bBusy);
		Data->SetArrayField(TEXT("items"), Items);
		Data->SetArrayField(TEXT("errors"), Strings(Plan.Errors));
		Data->SetBoolField(TEXT("export_in_progress"), bBusy);
		if (bBusy && Plan.Errors.IsEmpty()) { Data->SetStringField(TEXT("error"), TEXT("An FBX export is already running.")); Data->SetStringField(TEXT("error_code"), TEXT("BUSY")); }
		return Result(Data, !Plan.CanExport() || bBusy);
	}

	FModelContextProtocolToolResult Export(const TSharedPtr<FJsonObject>& Params)
	{
		FRequest Request;
		FString Message;
		if (!ReadRequest(Params, Request, Message)) return Error(Message);
		const auto Data = Jobs().RunInline(Request.Spec, Message);
		if (!Data.IsValid()) return Error(Message);
		AddOptions(Data.ToSharedRef(), Request);
		return Result(Data.ToSharedRef(), !Data->GetBoolField(TEXT("success")));
	}

	FModelContextProtocolToolResult ProfilesTool(const TSharedPtr<FJsonObject>& Params)
	{
		const auto Data = Profiles().Handle(Params); return Result(Data.ToSharedRef(), !Data->GetBoolField(TEXT("success")));
	}
	FModelContextProtocolToolResult JobStart(const TSharedPtr<FJsonObject>& Params) { const auto Data = Application::Submit(Params); return Result(Data.ToSharedRef(), !Data->GetBoolField(TEXT("success"))); }
	FModelContextProtocolToolResult JobStatus(const TSharedPtr<FJsonObject>& Params) { const auto Data = Jobs().Status(Params); AnnotateRecoveryStatus(Data); return Result(Data.ToSharedRef(), !Data->GetBoolField(TEXT("success"))); }
	FModelContextProtocolToolResult JobControl(const TSharedPtr<FJsonObject>& Params)
	{ const auto Data = Jobs().Control(Params); return Result(Data.ToSharedRef(), !Data->GetBoolField(TEXT("success"))); }

	bool ValidateArguments(const UnrealToolsRuntime::FObject& Params, const UnrealToolsRuntime::FObject& Schema, FString& Error)
	{
		if (!Params.IsValid()) { Error = TEXT("Expected an arguments object."); return false; }
		if (!UnrealToolsRuntime::ValidateSchema(Params, Schema, Error)) return false;
		const auto Properties = Schema->GetObjectField(TEXT("properties"));
		for (const auto& Entry : Params->Values)
			if (!Properties->HasField(Entry.Key)) { Error = TEXT("Unknown argument: ") + FString(FStringView(Entry.Key)); return false; }
		return true;
	}

	using FExecute = FModelContextProtocolToolResult (*)(const TSharedPtr<FJsonObject>&);

	struct FTool final : IModelContextProtocolTool
	{
		FString Name;
		FString Description;
		TSharedPtr<FJsonObject> Schema;
		FExecute Execute;
		const uint64 Generation;

		FTool(const TCHAR* InName, const TCHAR* InDescription, const TSharedRef<FJsonObject>& InSchema, FExecute InExecute)
			: Name(InName), Description(InDescription), Schema(InSchema), Execute(InExecute), Generation(RegistrationGeneration) {}
		virtual FString GetName() const override { return Name; }
		virtual FString GetDescription() const override { return Description; }
		virtual TSharedPtr<FJsonObject> GetInputJsonSchema() const override { return Schema; }
		virtual TSharedPtr<FJsonObject> GetOutputJsonSchema() const override { return UnrealToolsRuntime::OutputSchema(); }
		virtual FModelContextProtocolToolResult Run(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!IsInGameThread()) return Error(TEXT("FBX exporter tools require the Editor game thread."));
			if (!bRegistered || Generation != RegistrationGeneration) return Error(TEXT("FBX tools were unregistered or refreshed before this request executed."), TEXT("EDITOR_INTERRUPTED"));
			FString ValidationError;
			if (!ValidateArguments(Params, Schema, ValidationError)) return Error(ValidationError);
			return Execute(Params);
		}
		virtual void RunAsync(const FModelContextProtocolToolRequestId&, const TSharedPtr<FJsonObject>& Params,
			const FResultCallback& OnComplete) override
		{
			const TSharedRef<IModelContextProtocolTool> Self = AsShared();
			const uint64 ExpectedGeneration = Generation;
			auto Work = [Self, Params, OnComplete, ExpectedGeneration]()
			{
				if (!bRegistered || ExpectedGeneration != RegistrationGeneration) { OnComplete(Error(TEXT("FBX tool registration changed while this request waited for the game thread."), TEXT("EDITOR_INTERRUPTED"))); return; }
				if (Self->GetName() != TEXT("fbx_exporter_export")) { OnComplete(Self->Run(Params)); return; }
				FString ValidationError;
				if (!ValidateArguments(Params, Self->GetInputJsonSchema(), ValidationError)) { OnComplete(Error(ValidationError)); return; }
				FRequest Request; FString Message;
				if (!ReadRequest(Params, Request, Message)) { OnComplete(Error(Message)); return; }
				const auto Accepted = Jobs().StartLegacyStructured(Request.Spec, [OnComplete, Request](UnrealToolsRuntime::FObject Data) { AddOptions(Data.ToSharedRef(), Request); OnComplete(Result(Data.ToSharedRef(), !Data->GetBoolField(TEXT("success")))); });
				if (!UnrealToolsRuntime::GetBool(Accepted, TEXT("success"))) OnComplete(Result(Accepted.ToSharedRef(), true));
			};
			if (IsInGameThread() && !bForcePostRequestsForTesting) Work();
			else AsyncTask(ENamedThreads::GameThread, MoveTemp(Work));
		}
	};

	TSharedRef<FJsonObject> ReadSchema(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Schema;
		const bool bValid = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text)), Schema);
		check(bValid && Schema.IsValid());
		return Schema.ToSharedRef();
	}

	void AddTools()
	{
		++RegistrationGeneration;
		IModelContextProtocolModule& Module = IModelContextProtocolModule::GetChecked();
		for (const TSharedRef<IModelContextProtocolTool>& Tool : RegisteredTools) Module.RemoveTool(Tool);
		RegisteredTools.Reset();
		auto Add = [&Module](const TCHAR* Name, const TCHAR* Description, const TSharedRef<FJsonObject>& Schema, FExecute Execute)
		{
			const TSharedRef<IModelContextProtocolTool> Tool = MakeShared<FTool>(Name, Description, Schema, Execute);
			if (Module.AddTool(Tool)) RegisteredTools.Add(Tool);
		};
		Add(TEXT("fbx_exporter_status"), TEXT("Read the connected project, FBX export availability, map choices and default output folder. No files are written."),
			ReadSchema(TEXT(R"({"type":"object","properties":{},"additionalProperties":false})")), Status);
		Add(TEXT("fbx_exporter_find_meshes"), TEXT("Find Static Mesh assets by name in this project or a mounted content root, without loading them. Returns exact object paths. Narrow query/root if truncated; retry if asset_registry_loading is true."),
			ReadSchema(TEXT(R"({"type":"object","properties":{"query":{"type":"string","minLength":1},"root":{"type":"string","default":"/Game"},"limit":{"type":"integer","minimum":1,"maximum":200,"default":40}},"required":["query"],"additionalProperties":false})")), FindMeshes);
		const TCHAR* ExportSchema = TEXT(R"({"type":"object","properties":{"mesh_path":{"type":"string","description":"Exact Static Mesh object path from fbx_exporter_find_meshes."},"output_root":{"type":"string","description":"Absolute output root. Default: the current project's Saved/FBX_Exporter/Exports. A mesh-named subfolder is created."},"resolution":{"type":"integer","enum":[512,1024,2048,4096],"default":2048},"maps":{"type":"array","items":{"type":"string","enum":["BaseColor","Normal","Mask","Occlusion","Emission","Opacity"]},"uniqueItems":true,"description":"Omit for all six maps; [] exports geometry only."},"embed_textures":{"type":"boolean","default":false,"description":"Include PNG bytes inside FBX. External PNGs and material JSON are still produced when maps are selected."},"reuse_direct_textures":{"type":"boolean","default":true},"output_layout":{"type":"string","enum":["mesh_name","asset_path"],"default":"mesh_name"}},"required":["mesh_path"],"additionalProperties":false})");
		Add(TEXT("fbx_exporter_preflight"), TEXT("Check mesh LOD0/UV0, renderer and output paths without writing files or opening dialogs. Reports requires_overwrite; replacement includes user files inside the mesh folder. Filesystem permissions and actual bake success are checked during export."), [&]() { auto Schema = ReadSchema(ExportSchema); Schema->RemoveField(TEXT("required")); Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("mesh_paths"), ReadSchema(TEXT(R"({"type":"array","minItems":1,"items":{"type":"string"},"uniqueItems":true})"))); return Schema; }(), Preflight);
		const TSharedRef<FJsonObject> WriteSchema = ReadSchema(ExportSchema);
		WriteSchema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("overwrite"), ReadSchema(TEXT(R"({"type":"boolean","default":false,"description":"Explicitly replace the entire existing mesh output folder, including user files. Inspect preflight first."})")));
		Add(TEXT("fbx_exporter_export"), TEXT("Export one Static Mesh using FBX_Exporter, with selected PNGs and optional embedded PNG bytes. Requires PIE stopped and a rendering Editor for textures. Run preflight first; existing output is refused unless overwrite=true. Runs to completion on the Editor game thread; allow a long timeout for baking. Returns saved files, warnings and errors. JSON is metadata; FBX stores texture connections."), WriteSchema, Export);
		Add(TEXT("fbx_exporter_profiles"), TEXT("List/get/save/delete project FBX profiles. Builtin pbr and geometry_only are immutable. Profiles never store overwrite approval."),
			ReadSchema(TEXT(R"({"type":"object","properties":{"action":{"type":"string","enum":["list","get","save","delete"]},"id":{"type":"string"},"profile":{"type":"object"},"overwrite":{"type":"boolean","default":false}},"required":["action"],"additionalProperties":false})")), ProfilesTool);
		const auto OptionSchema = ReadSchema(ExportSchema); OptionSchema->RemoveField(TEXT("required")); OptionSchema->GetObjectField(TEXT("properties"))->RemoveField(TEXT("mesh_path"));
		auto JobSchema = ReadSchema(TEXT(R"({"type":"object","properties":{"request_id":{"type":"string","minLength":1},"mesh_paths":{"type":"array","minItems":1,"uniqueItems":true,"items":{"type":"string"}},"profile_id":{"type":"string"},"options":{"type":"object"},"overrides":{"type":"object","additionalProperties":{"type":"object"}},"overwrite":{"type":"boolean","default":false}},"required":["request_id","mesh_paths"],"additionalProperties":false})"));
		JobSchema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("options"), OptionSchema);
		Add(TEXT("fbx_exporter_job_start"), TEXT("Persist and queue ordered exports. Returns a job_id before asset loading. Same request_id+request returns the existing job; changed request errors. First failed target stops later targets."), JobSchema, JobStart);
		Add(TEXT("fbx_exporter_job_status"), TEXT("Query by job_id or request_id. success describes the lookup, state describes the actual job. Native GPU/FBX calls can delay replies."),
			ReadSchema(TEXT(R"({"type":"object","properties":{"job_id":{"type":"string"},"request_id":{"type":"string"}},"oneOf":[{"required":["job_id"]},{"required":["request_id"]}],"additionalProperties":false})")), JobStatus);
		Add(TEXT("fbx_exporter_job_control"), TEXT("Request safe-boundary cancellation or acknowledge inspected recovery without deleting/restoring files. Only the job's own recorded recovery journals can be acknowledged."),
			ReadSchema(TEXT(R"({"type":"object","properties":{"job_id":{"type":"string"},"request_id":{"type":"string"},"action":{"type":"string","enum":["cancel","acknowledge_recovery"]}},"required":["action"],"oneOf":[{"required":["job_id"]},{"required":["request_id"]}],"additionalProperties":false})")), JobControl);

	}
}

void Register()
{
	if (bRegistered) return;
	FModuleManager::LoadModuleChecked<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	bRegistered = true;
	RefreshHandle = IModelContextProtocolModule::GetChecked().OnRefreshTools().AddStatic(&AddTools);
	AddTools();
}

void Unregister()
{
	++RegistrationGeneration;
	bRegistered = false;
	if (IModelContextProtocolModule* Module = IModelContextProtocolModule::Get())
	{
		Module->OnRefreshTools().Remove(RefreshHandle);
		for (const TSharedRef<IModelContextProtocolTool>& Tool : RegisteredTools) Module->RemoveTool(Tool);
	}
	RefreshHandle.Reset();
	RegisteredTools.Reset();
	// The engine server may be serving other plugins. Only our tools belong to us.
}

namespace Testing
{
	void ForcePostRequests(bool bForce) { check(IsInGameThread()); bForcePostRequestsForTesting = bForce; }
}
}
