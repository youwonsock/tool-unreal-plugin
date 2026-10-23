#pragma once

#include "AssetRegistry/AssetData.h"
#include "FBX_ExporterTypes.h"
#include "UnrealToolsRuntime.h"

class FFBXExporterWorkspaceController;
class UToolMenu;
class UStaticMesh;
class UTexture2D;
class UMaterialInterface;
class SWidget;

namespace FBXExporter
{
	struct FPreflightResult
	{
		bool bCanExport = false;
		bool bRequiresOverwrite = false;
		FString FinalDirectory;
		FString FbxFile;
		FString MetadataFile;
		TArray<FString> Warnings;
		TArray<FString> Errors;
		TArray<FString> PlannedFiles;
		TArray<FString> ReplacedFiles;
	};

	// Game-thread services shared by the UI and the plugin's MCP tools.
	bool IsExportInProgress();
	bool IsRecoveryBlocked(const FString& FinalDirectory, FString& OutJournal);
	bool AcknowledgeRecovery(const FString& JournalPath, FString& OutError);
	bool AcknowledgeJobRecovery(const UnrealToolsRuntime::FObject& Job, FString& OutError);
	bool ValidateRecoveryJournal(const UnrealToolsRuntime::FObject& Data, FString& OutError);
	void AnnotateRecoveryStatus(const UnrealToolsRuntime::FObject& Job);
	void RegisterRecoveryDiagnostics();
	bool AdvanceRecoveryDiagnostics(int32 Budget = 8);
	void CancelRecoveryDiagnostics();
	bool AreRecoveryDiagnosticsReady();
	void RefreshRecoveryDiagnostics();
	FPreflightResult PreflightStaticMesh(UStaticMesh* StaticMesh, const FFBXExporterOptions& Options);
	FFBXExporterResult ExportStaticMeshUnattended(UStaticMesh* StaticMesh, const FFBXExporterOptions& Options, bool bOverwrite = false);
	TUniquePtr<UnrealToolsRuntime::IItemRunner> CreateExportRunner(const UnrealToolsRuntime::FItemSpec& Spec, const FString& JobId);
	void InitializeJobs();
	void ShutdownJobs();
	UnrealToolsRuntime::FProfileStore& Profiles();
	UnrealToolsRuntime::FJobService& Jobs();
	bool NormalizeJobOptions(UnrealToolsRuntime::FObject& Options, FString& OutError);
	FFBXExporterOptions OptionsFromJson(const UnrealToolsRuntime::FObject& Json);
	UnrealToolsRuntime::FObject OptionsToJson(const FFBXExporterOptions& Options);
	FString PlannedDirectory(const FString& MeshPath, const FFBXExporterOptions& Options);
	void RegisterTab(TSharedRef<FFBXExporterWorkspaceController> Workspace);
	void UnregisterTab();
	void AddAssetsToTab(const TArray<FAssetData>& Assets);
	FString LastOutputRoot();

	void PopulateContentBrowserMenu(UToolMenu* Menu);

	FString MakeSafeFileName(const FString& InName);
	EFBXExporterTextureMap GetRequiredTextureMapsForBake(EFBXExporterTextureMap SelectedMaps);
	void BuildMaskPixels(
		const TArray<FColor>& Metallic,
		const TArray<FColor>& AmbientOcclusion,
		const TArray<FColor>& Roughness,
		int32 Width,
		int32 Height,
		TArray<FColor>& OutPixels);

	namespace Testing
	{
		bool PublishStagedFiles(const FString& StagingDirectory, const FString& FinalDirectory, FString& OutError);
		bool PublishWithJournal(const FString& StagingDirectory, const FString& FinalDirectory, const FString& JournalPath, FString& OutError, const FString& JobId = FString());
		void ScanRecoveryDiagnostics(UnrealToolsRuntime::FJobService& Service, const FString& RecoveryRoot, const TFunction<void()>& BeforeFileScan = {});
		void SetPublishFailure(const FString& FailurePoint);
		bool ReadLinearTexture(UTexture2D* Texture, int32 Width, int32 Height, TArray<FLinearColor>& Pixels);
		bool ReadDirectEmission(UTexture2D* Texture, int32 Width, int32 Height, TArray<FColor>& Pixels, float& Scale);
		bool IsDirectTextureReusable(UMaterialInterface* Material, const FString& Property);
		TSharedRef<SWidget> CreateWorkspaceWidget(const TArray<FAssetData>& Assets);
		bool WorkspaceStateRegression(FString& OutError);
	}
}
