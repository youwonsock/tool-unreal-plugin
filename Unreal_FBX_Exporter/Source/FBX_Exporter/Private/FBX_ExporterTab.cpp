#include "FBX_ExporterService.h"
#include "FBX_ExporterApplication.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "IContentBrowserSingleton.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Engine/StaticMesh.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FBXExporterTab"
namespace FBXExporter
{
namespace
{
	using namespace UnrealToolsRuntime;
	const FName TabId(TEXT("FBX_Exporter.Workspace"));
	struct FTarget { FString Path; FObject Overrides = MakeShared<FJsonObject>(); FString Error; TOptional<FString> InvalidOverrideDraft; bool bSelected = false; };
	struct FStatusCache { uint64 Revision = 0; FObject Snapshot; FString Summary, Json; };
	struct FWorkspace
	{
		TArray<TSharedPtr<FTarget>> Targets; TSharedPtr<FTarget> EditingTarget;
		FObject Options;
		FString ProfileId = TEXT("pbr"), ProfileName = TEXT("My FBX Profile"), UserProfileId = TEXT("my_fbx_profile");
		FString JobId, PreviewReport, PreviewSummary, InputError;
		bool bOverwrite = false;
		TMap<FString, FStatusCache> StatusCache; TArray<FString> CacheOrder;
		explicit FWorkspace(bool bReadSettings = true)
		{
			FFBXExporterOptions Initial;
			if (bReadSettings) Initial.OutputRoot = LastOutputRoot();
			if (Initial.OutputRoot.IsEmpty()) Initial.OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Exports/StaticMeshTextures"));
			Options = OptionsToJson(Initial);
		}
	};
	TSharedPtr<FWorkspace> Workspace;
	TWeakPtr<class SExporterWorkspace> LiveWidget;
	FWorkspace& State() { if (!Workspace) Workspace = MakeShared<FWorkspace>(); return *Workspace; }
	bool ReconcileProfileSelection(FWorkspace& Model, const TArray<FString>& Available)
	{
		if (Available.Contains(Model.ProfileId)) return false;
		// The editable options are explicit values. Losing a profile identity
		// must not silently replace those values or an already queued snapshot.
		Model.ProfileId = TEXT("pbr"); return true;
	}
	void AddTargetPath(FWorkspace& Model, const FString& Path)
	{
		if (Model.Targets.ContainsByPredicate([&Path](const auto& Target) { return Target->Path == Path; })) return;
		auto Target = MakeShared<FTarget>(); Target->Path = Path; Model.Targets.Add(Target);
	}
	void RemoveSelected(FWorkspace& Model)
	{
		if (Model.EditingTarget && Model.EditingTarget->bSelected) Model.EditingTarget.Reset();
		Model.Targets.RemoveAll([](const auto& Target) { return Target->bSelected; });
	}
	void ApplyGlobalToSelected(FWorkspace& Model)
	{
		for (const auto& Target : Model.Targets)
			if (Target->bSelected) { Target->Overrides = Clone(Model.Options); Target->Error.Empty(); Target->InvalidOverrideDraft.Reset(); }
	}
	FObject ResolveTarget(const FWorkspace& Model, const TSharedPtr<FTarget>& Target, FString& Error)
	{
		if (!Target->Error.IsEmpty()) { Error = Target->Error; return nullptr; }
		auto Options = Merge(Model.Options, Target->Overrides);
		return NormalizeJobOptions(Options, Error) ? Options : nullptr;
	}
	FObject ActiveOptions() { return State().EditingTarget ? State().EditingTarget->Overrides : State().Options; }
	void SetActiveOptions(const FObject& Options) { if (State().EditingTarget) State().EditingTarget->Overrides = Options; else State().Options = Options; }
	FString JsonText(const FObject& Json)
	{ FString Text; if (Json) FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text)); return Text; }
	TSharedRef<SEditableTextBox> CreateOverrideEditor(const TSharedPtr<FTarget>& Target)
	{
		return SNew(SEditableTextBox)
			.HintText(LOCTEXT("Override", "Per-target option overrides as JSON, e.g. {\"maps\":[]}"))
			.Text_Lambda([Target]() { return FText::FromString(!Target->Error.IsEmpty() && Target->InvalidOverrideDraft.IsSet() ? Target->InvalidOverrideDraft.GetValue() : JsonText(Target->Overrides)); })
			.OnTextCommitted_Lambda([Target](const FText& Text, ETextCommit::Type)
			{
				auto Object = Parse(Text.ToString());
				if (!Object) { Target->InvalidOverrideDraft = Text.ToString(); Target->Error = TEXT("Invalid override JSON."); return; }
				Target->Overrides = Object; Target->InvalidOverrideDraft.Reset(); Target->Error.Empty();
			});
	}
	FObject CurrentJob()
	{
		const FStatusCache* Cached = State().StatusCache.Find(State().JobId);
		return Cached ? Cached->Snapshot : nullptr;
	}
	FString CompleteResultText()
	{
		const FStatusCache* Cached = State().StatusCache.Find(State().JobId);
		return Cached ? Cached->Json : FString();
	}

	FString ResultDirectory()
	{
		for (const auto& Item : GetArray(CurrentJob(), TEXT("items")))
		{
			const FString Directory = GetString(GetObject(Item->AsObject(), TEXT("result")), TEXT("final_directory"));
			if (!Directory.IsEmpty()) return Directory;
		}
		return GetString(State().Options, TEXT("output_root"));
	}
	FString BuildResultSummary(const FObject& Job)
	{
		if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic"))
		{
			FString Raw = GetString(Job, TEXT("raw_path"));
			if (Raw.IsEmpty()) Raw = GetString(Job, TEXT("phase")) == TEXT("issue")
				? TEXT("Not created yet. Acknowledgement first preserves and verifies source metadata bytes.")
				: TEXT("Missing raw evidence. Inspect the complete JSON before acknowledgement.");
			return FString::Printf(TEXT("Diagnostic ID: %s\nPhase: %s\nSource metadata: %s\nRaw evidence: %s\nError: %s · %s\n"),
				*GetString(Job, TEXT("job_id")), *GetString(Job, TEXT("phase")), *GetString(Job, TEXT("source_path")), *Raw,
				*GetString(Job, TEXT("error_code")), *GetString(Job, TEXT("error")));
		}
		FString Text;
		for (const auto& Value : GetArray(Job, TEXT("items")))
		{
			const auto Item = Value->AsObject(), Result = GetObject(Item, TEXT("result"));
			Text += GetString(Item, TEXT("state")) + TEXT(" · ") + GetString(Item, TEXT("mesh_path")) + TEXT("\n");
			for (const TCHAR* Field : {TEXT("files"), TEXT("warnings"), TEXT("errors")})
				for (const auto& Entry : GetArray(Result, Field)) Text += FString(TEXT("  ")) + Field + TEXT(": ") + Entry->AsString() + TEXT("\n");
			for (const TCHAR* Field : {TEXT("recovery_journal"), TEXT("recovery_staging"), TEXT("recovery_backup"), TEXT("staging_directory")})
				if (Result.IsValid() && Result->HasField(Field)) Text += FString(TEXT("  ")) + Field + TEXT(": ") + GetString(Result, Field) + TEXT("\n");
		}
		if (GetArray(Job, TEXT("items")).IsEmpty()) Text += JsonText(Job);
		if (!GetString(Job, TEXT("error")).IsEmpty()) Text += TEXT("Job error: ") + GetString(Job, TEXT("error_code")) + TEXT(" · ") + GetString(Job, TEXT("error")) + TEXT("\n");
		return Text;
	}
	FString ResultSummary()
	{
		const FStatusCache* Cached = State().StatusCache.Find(State().JobId);
		return Cached ? Cached->Summary : FString();
	}
	FString JobStateLabel(const FObject& Job)
	{
		if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic"))
			return GetString(Job, TEXT("phase")) == TEXT("acknowledged") ? TEXT("Recovery acknowledged") : TEXT("Recovery review required");
		const FString State = GetString(Job, TEXT("state"));
		if (State == TEXT("succeeded")) return TEXT("Completed");
		if (State == TEXT("failed")) return TEXT("Failed");
		if (State == TEXT("cancelled")) return TEXT("Cancelled");
		if (State == TEXT("interrupted")) return TEXT("Interrupted");
		if (State == TEXT("running")) return TEXT("Running");
		if (State == TEXT("queued")) return TEXT("Queued");
		return State;
	}
	FText RecoveryTooltip(const FObject& Job)
	{
		return GetString(Job, TEXT("record_kind")) == TEXT("diagnostic")
			? LOCTEXT("DiagnosticRecoveryTooltip", "Review the source and raw evidence paths first. Acknowledgement preserves and verifies metadata bytes, then quarantines damaged metadata or keeps a repaired valid record. Output, staging and backup files are untouched.")
			: LOCTEXT("RecoveryTooltip", "Only after inspecting retained output, staging and backup. This releases recovery without restoring or deleting those output files. A related unresolved diagnostic must be acknowledged first.");
	}

	class SExporterWorkspace final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SExporterWorkspace) {} SLATE_END_ARGS()
		virtual ~SExporterWorkspace() override { if (AreJobsInitialized()) { Jobs().OnChanged.Remove(ChangeHandle); if (!PinnedJobId.IsEmpty()) Jobs().UnpinJob(PinnedJobId); } }
		void Construct(const FArguments&)
		{
			ChangeHandle = Jobs().OnChanged.AddLambda([this]() { bDataDirty = true; });
			ProgressStyle = FAppStyle::Get().GetWidgetStyle<FProgressBarStyle>(TEXT("ProgressBar"));
			ProgressStyle.SetEnableFillAnimation(false);
			for (int32 Value : {512, 1024, 2048, 4096}) Resolutions.Add(MakeShared<int32>(Value));
			RefreshProfiles(); RefreshJobs();
			ChildSlot
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot().Padding(12)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8) [ SNew(STextBlock).Text(LOCTEXT("Title", "FBX Exporter — Targets, profiles and output")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4))
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Add", "Add selected meshes")).OnClicked_Lambda([this]() { TArray<FAssetData> Assets; FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Assets); Add(Assets); return FReply::Handled(); }) ]
						+ SWrapBox::Slot() [ SNew(SButton).Text(LOCTEXT("Clear", "Clear targets")).OnClicked_Lambda([this]() { State().Targets.Empty(); State().EditingTarget.Reset(); RebuildTargets(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().FillEmptySpace(true).Padding(12,0) [ SNew(STextBlock).Text(LOCTEXT("Drop", "Drop Static Meshes here. Each row can override profile options.")) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("SelectAllTargets", "Select all targets")).OnClicked_Lambda([]() { for (const auto& Target : State().Targets) Target->bSelected = true; return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("ClearTargetSelection", "Clear target selection")).OnClicked_Lambda([]() { for (const auto& Target : State().Targets) Target->bSelected = false; return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("ApplySelected", "Apply global options to selected")).OnClicked_Lambda([]() { ApplyGlobalToSelected(State()); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("RemoveSelected", "Remove selected targets")).OnClicked_Lambda([this]() { RemoveSelected(State()); RebuildTargets(); return FReply::Handled(); }) ]

					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8) [ SNew(SBox).MaxDesiredHeight(210) [ SNew(SScrollBox) + SScrollBox::Slot() [ SAssignNew(TargetBox, SVerticalBox) ] ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8) [ SNew(STextBlock).Text_Lambda([]() { return State().EditingTarget ? FText::FromString(TEXT("Options for ") + State().EditingTarget->Path) : LOCTEXT("Profile", "Global profile and options"); }) ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SButton).Text(LOCTEXT("EditGlobal", "Edit global options")).Visibility_Lambda([]() { return State().EditingTarget ? EVisibility::Visible : EVisibility::Collapsed; }).OnClicked_Lambda([]() { State().EditingTarget.Reset(); return FReply::Handled(); }) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4))
						+ SWrapBox::Slot().FillEmptySpace(true).Padding(0,0,8,0)
						[ SAssignNew(ProfileCombo, SComboBox<TSharedPtr<FString>>).OptionsSource(&ProfileIds).OnGenerateWidget_Lambda([](TSharedPtr<FString> Id) { const auto Profile = Profiles().Get(*Id); return SNew(STextBlock).Text(FText::FromString(GetString(Profile, TEXT("display_name"), *Id))); }).OnSelectionChanged_Lambda([this](TSharedPtr<FString> Id, ESelectInfo::Type) { if (!Id) return; FString Error; FFBXExporterOptions Defaults; Defaults.OutputRoot = GetString(ActiveOptions(), TEXT("output_root")); auto Options = Profiles().Resolve(*Id, OptionsToJson(Defaults), Error); if (!Options) { State().InputError = Error; return; } const FString Root = GetString(ActiveOptions(), TEXT("output_root")); Options->SetStringField(TEXT("output_root"), Root); SetActiveOptions(Options); State().ProfileId = *Id; }).Content() [ SNew(STextBlock).Text_Lambda([]() { const auto Profile = Profiles().Get(State().ProfileId); return FText::FromString(GetString(Profile, TEXT("display_name"), State().ProfileId)); }) ] ]
						+ SWrapBox::Slot() [ SNew(SButton).Text(LOCTEXT("Import", "Import profile JSON")).OnClicked_Lambda([this]() { return ImportProfile(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("Export", "Export profile JSON")).OnClicked_Lambda([this]() { return ExportProfile(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8)
					[
						SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4))
						+ SWrapBox::Slot().FillEmptySpace(true).Padding(0,0,8,0) [ SNew(SEditableTextBox).HintText(LOCTEXT("ProfileId", "User profile ID")).Text_Lambda([]() { return FText::FromString(State().UserProfileId); }).OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type) { State().UserProfileId = Text.ToString(); }) ]
						+ SWrapBox::Slot().FillEmptySpace(true).Padding(0,0,8,0) [ SNew(SEditableTextBox).HintText(LOCTEXT("ProfileName", "Display name")).Text_Lambda([]() { return FText::FromString(State().ProfileName); }).OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type) { State().ProfileName = Text.ToString(); }) ]
						+ SWrapBox::Slot() [ SNew(SButton).Text(LOCTEXT("SaveProfile", "Save user profile")).OnClicked_Lambda([this]() { auto Profile = MakeShared<FJsonObject>(); Profile->SetNumberField(TEXT("schema_version"), 1); Profile->SetStringField(TEXT("provider_id"), TEXT("fbx_exporter")); Profile->SetStringField(TEXT("id"), State().UserProfileId); Profile->SetStringField(TEXT("display_name"), State().ProfileName); Profile->SetObjectField(TEXT("options"), Clone(ActiveOptions())); auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("action"), TEXT("save")); Request->SetObjectField(TEXT("profile"), Profile); if (!ConfirmProfileReplacement(Request, State().UserProfileId)) return FReply::Handled(); const auto Result = Profiles().Handle(Request); State().InputError = GetString(Result, TEXT("error")); if (GetBool(Result, TEXT("success"))) State().ProfileId = State().UserProfileId; RefreshProfiles(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("DeleteProfile", "Delete user profile")).OnClicked_Lambda([this]() { auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("action"), TEXT("delete")); Request->SetStringField(TEXT("id"), State().ProfileId); State().InputError = GetString(Profiles().Handle(Request), TEXT("error")); RefreshProfiles(); return FReply::Handled(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,4) [ SNew(STextBlock).Text(LOCTEXT("OutputRoot", "Output folder (replaces the complete mesh folder only when explicitly approved)")) ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SEditableTextBox).Text_Lambda([]() { return FText::FromString(GetString(ActiveOptions(), TEXT("output_root"))); }).OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type) { ActiveOptions()->SetStringField(TEXT("output_root"), Text.ToString()); }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8)
					[
						SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4))
						+ SWrapBox::Slot().Padding(0,0,12,0) [ SNew(SCheckBox).IsChecked_Lambda([]() { return GetString(ActiveOptions(), TEXT("output_layout")) == TEXT("asset_path") ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([](ECheckBoxState Value) { ActiveOptions()->SetStringField(TEXT("output_layout"), Value == ECheckBoxState::Checked ? TEXT("asset_path") : TEXT("mesh_name")); }) [ SNew(STextBlock).Text(LOCTEXT("PathLayout", "Preserve asset path")) ] ]
						+ SWrapBox::Slot().Padding(0,0,12,0) [ SNew(SComboBox<TSharedPtr<int32>>).OptionsSource(&Resolutions).IsEnabled_Lambda([]() { return !GetArray(ActiveOptions(), TEXT("maps")).IsEmpty(); }).OnGenerateWidget_Lambda([](TSharedPtr<int32> Value) { return SNew(STextBlock).Text(FText::AsNumber(*Value)); }).OnSelectionChanged_Lambda([](TSharedPtr<int32> Value, ESelectInfo::Type) { if (Value) ActiveOptions()->SetNumberField(TEXT("resolution"), *Value); }).Content() [ SNew(STextBlock).Text_Lambda([]() { return FText::Format(LOCTEXT("Resolution", "Resolution: {0}"), FText::AsNumber(static_cast<int32>(GetNumber(ActiveOptions(), TEXT("resolution"), 2048)))); }) ] ]
						+ SWrapBox::Slot().Padding(0,0,12,0) [ SNew(SCheckBox).IsEnabled_Lambda([]() { return !GetArray(ActiveOptions(), TEXT("maps")).IsEmpty(); }).IsChecked_Lambda([]() { return GetBool(ActiveOptions(), TEXT("reuse_direct_textures")) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([](ECheckBoxState Value) { ActiveOptions()->SetBoolField(TEXT("reuse_direct_textures"), Value == ECheckBoxState::Checked); }) [ SNew(STextBlock).Text(LOCTEXT("Reuse", "Reuse direct textures")) ] ]
						+ SWrapBox::Slot() [ SNew(SCheckBox).IsEnabled_Lambda([]() { return !GetArray(ActiveOptions(), TEXT("maps")).IsEmpty(); }).IsChecked_Lambda([]() { return GetBool(ActiveOptions(), TEXT("embed_textures")) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([](ECheckBoxState Value) { ActiveOptions()->SetBoolField(TEXT("embed_textures"), Value == ECheckBoxState::Checked); }) [ SNew(STextBlock).Text(LOCTEXT("Embed", "Embed PNG bytes")) ] ]
					]
					+ SVerticalBox::Slot().AutoHeight() [ SAssignNew(MapBox, SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4)) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8) [ SNew(SCheckBox).IsChecked_Lambda([]() { return State().bOverwrite ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([](ECheckBoxState Value) { State().bOverwrite = Value == ECheckBoxState::Checked; }) [ SNew(STextBlock).Text(LOCTEXT("Overwrite", "Approve replacing complete existing target folders, including user files")) ] ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FLinearColor(1,0.5f,0.2f)).Text_Lambda([]() { return FText::FromString(State().InputError); }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8)
					[
						SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4))
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Preflight", "Inspect output plan")).OnClicked_Lambda([this]() { Inspect(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Queue", "Queue export")).OnClicked_Lambda([this]() { StartTargets(State().Targets); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Cancel", "Request cancel")).OnClicked_Lambda([]() { auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("job_id"), State().JobId); Request->SetStringField(TEXT("action"), TEXT("cancel")); State().InputError = GetString(Jobs().Control(Request), TEXT("error")); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Retry", "Retry failed")).OnClicked_Lambda([this]() { Retry(TEXT("failed")); return FReply::Handled(); }) ]
						+ SWrapBox::Slot() [ SNew(SButton).Text(LOCTEXT("Skipped", "Run skipped")).OnClicked_Lambda([this]() { Retry(TEXT("skipped")); return FReply::Handled(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([]() { return FText::FromString(State().PreviewSummary); }) ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SExpandableArea).InitiallyCollapsed(true).HeaderContent() [ SNew(STextBlock).Text(LOCTEXT("PlanDetails", "Output plan: files, replacement scope and errors")) ].BodyContent() [ SNew(SBox).MaxDesiredHeight(260) [ SNew(SScrollBox) + SScrollBox::Slot() [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([]() { return FText::FromString(State().PreviewReport); }) ] ] ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8) [ SNew(STextBlock).Text(LOCTEXT("Result", "Job progress and preserved results")) ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SWrapBox).UseAllottedSize(true)
						+ SWrapBox::Slot() [ SNew(SButton).Text(LOCTEXT("Recent", "Recent")).OnClicked_Lambda([this]() { bArchive = false; bDiagnostics = false; HistoryOffset = 0; RefreshJobs(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("Archive", "Archive")).OnClicked_Lambda([this]() { bArchive = true; bDiagnostics = false; HistoryOffset = 0; RefreshJobs(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("Diagnostics", "Recovery diagnostics")).OnClicked_Lambda([this]() { bDiagnostics = true; HistoryOffset = 0; RefreshRecoveryDiagnostics(); RefreshJobs(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("PreviousPage", "Previous 50")).OnClicked_Lambda([this]() { HistoryOffset = FMath::Max(0, HistoryOffset - 50); RefreshJobs(); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(8,0) [ SNew(SButton).Text(LOCTEXT("NextPage", "Next 50")).IsEnabled_Lambda([this]() { return JobIds.Num() == 50; }).OnClicked_Lambda([this]() { HistoryOffset += 50; RefreshJobs(); return FReply::Handled(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight() [ SAssignNew(JobCombo, SComboBox<TSharedPtr<FString>>).OptionsSource(&JobIds).OnGenerateWidget_Lambda([this](TSharedPtr<FString> Id) { const auto* Summary = JobSummaries.Find(*Id); return SNew(STextBlock).Text(FText::FromString((Summary ? GetString(*Summary, TEXT("state")) : FString()) + TEXT(" · ") + *Id)); }).OnSelectionChanged_Lambda([this](TSharedPtr<FString> Id, ESelectInfo::Type) { if (Id) { State().JobId = *Id; RefreshSelected(true); } }).Content() [ SNew(STextBlock).Text_Lambda([]() { return FText::FromString(State().JobId.IsEmpty() ? TEXT("Select a preserved job") : State().JobId); }) ] ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SProgressBar).Style(&ProgressStyle).Percent_Lambda([]() -> TOptional<float> { auto Job = CurrentJob(); const auto Progress = GetObject(Job, TEXT("progress")); const double Total = GetNumber(Progress, TEXT("total"), 1); return Total > 0 ? static_cast<float>(GetNumber(Progress, TEXT("completed")) / Total) : 0.0f; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8) [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([]() { const auto Job = CurrentJob(); if (!Job) return LOCTEXT("NoJob", "No job submitted. Native FBX and GPU steps may delay progress updates. Cancellation applies at safe boundaries."); return FText::FromString(JobStateLabel(Job) + TEXT(" · ") + GetString(Job, TEXT("stage")) + TEXT(" · ") + GetString(Job, TEXT("job_id"))); }) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0,4))
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Open", "Open output")).OnClicked_Lambda([]() { FPlatformProcess::ExploreFolder(*ResultDirectory()); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("CopyPath", "Copy output path")).OnClicked_Lambda([]() { FPlatformApplicationMisc::ClipboardCopy(*ResultDirectory()); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Copy", "Copy result JSON")).OnClicked_Lambda([]() { FPlatformApplicationMisc::ClipboardCopy(*JsonText(CurrentJob())); return FReply::Handled(); }) ]
						+ SWrapBox::Slot().Padding(0,0,8,0) [ SNew(SButton).Text(LOCTEXT("Recovery", "Acknowledge inspected recovery")).ToolTipText_Lambda([]() { return RecoveryTooltip(CurrentJob()); }).OnClicked_Lambda([]() { auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("job_id"), State().JobId); Request->SetStringField(TEXT("action"), TEXT("acknowledge_recovery")); State().InputError = GetString(Jobs().Control(Request), TEXT("error")); return FReply::Handled(); }) ]
						+ SWrapBox::Slot() [ SNew(SButton).Text(LOCTEXT("ShowPng", "Preview first PNG")).OnClicked_Lambda([this]() { ShowPng(); return FReply::Handled(); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8) [ SNew(SBox).HeightOverride(120).Visibility_Lambda([this]() { return PngBrush.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; }) [ SNew(SImage).Image_Lambda([this]() -> const FSlateBrush* { return PngBrush.Get(); }) ] ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([]() { return FText::FromString(ResultSummary()); }) ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SExpandableArea).InitiallyCollapsed(true).HeaderContent() [ SNew(STextBlock).Text(LOCTEXT("AdvancedResult", "Advanced: complete job JSON")) ].BodyContent() [ SNew(SBox).MaxDesiredHeight(260) [ SNew(SMultiLineEditableTextBox).IsReadOnly(true).Text_Lambda([]() { return FText::FromString(CompleteResultText()); }) ] ] ]
				]
			];
			for (const TCHAR* Name : {TEXT("BaseColor"), TEXT("Normal"), TEXT("Mask"), TEXT("Occlusion"), TEXT("Emission"), TEXT("Opacity")})
			{
				const FString Map(Name);
				MapBox->AddSlot().Padding(0,0,12,0)
				[ SNew(SCheckBox).IsChecked_Lambda([Map]() { for (const auto& Value : GetArray(ActiveOptions(), TEXT("maps"))) if (Value->AsString() == Map) return ECheckBoxState::Checked; return ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([Map](ECheckBoxState Value) { auto Maps = GetArray(ActiveOptions(), TEXT("maps")); Maps.RemoveAll([Map](const auto& Entry) { return Entry->AsString() == Map; }); if (Value == ECheckBoxState::Checked) Maps.Add(MakeShared<FJsonValueString>(Map)); ActiveOptions()->SetArrayField(TEXT("maps"), Maps); }) [ SNew(STextBlock).Text(FText::FromString(Map)) ] ];
			}
			RebuildTargets();
		}
		void Add(const TArray<FAssetData>& Assets)
		{
			for (const auto& Asset : Assets)
			{
				if (Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()) continue;
				AddTargetPath(State(), Asset.GetObjectPathString());
			}
			RebuildTargets();
		}
		void PreparePreviewForTesting() { Inspect(); }
		virtual FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
		{ const auto Op = Event.GetOperationAs<FAssetDragDropOp>(); if (!Op) return FReply::Unhandled(); Add(Op->GetAssets()); return FReply::Handled(); }
		virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override { SCompoundWidget::Tick(Geometry, Time, Delta); if (!AreJobsInitialized()) return; RefreshElapsed += Delta; if (RefreshElapsed >= 0.25f) { RefreshElapsed = 0; if (bDataDirty || LastServiceRevision != Jobs().GetRevision()) RefreshJobs(); else RefreshSelected(); } }
		virtual void OnDragEnter(const FGeometry&, const FDragDropEvent&) override {}
	private:
		void RefreshSelected(bool bForce = false)
		{
			const FString Id = State().JobId;
			if (PinnedJobId != Id) { if (!PinnedJobId.IsEmpty()) Jobs().UnpinJob(PinnedJobId); PinnedJobId = Id; if (!Id.IsEmpty()) Jobs().PinJob(Id); }
			if (Id.IsEmpty()) return;
			const uint64 Revision = Jobs().GetJobRevision(Id);
			const auto* Existing = State().StatusCache.Find(Id);
			if (!bForce && Existing && Existing->Revision == Revision) return;
			auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("job_id"), Id);
			const auto Snapshot = Jobs().Status(Request); AnnotateRecoveryStatus(Snapshot);
			FStatusCache Cached; Cached.Revision = Revision; Cached.Snapshot = Snapshot; Cached.Summary = BuildResultSummary(Snapshot); Cached.Json = JsonText(Snapshot);
			State().StatusCache.Add(Id, MoveTemp(Cached)); State().CacheOrder.Remove(Id); State().CacheOrder.Add(Id);
			while (State().CacheOrder.Num() > 20) { State().StatusCache.Remove(State().CacheOrder[0]); State().CacheOrder.RemoveAt(0); }
		}
		void RefreshJobs()
		{
			const auto All = bDiagnostics ? Jobs().GetDiagnosticSummaries(HistoryOffset, 50) : Jobs().GetJobSummaries(HistoryOffset, 50, bArchive);
			JobIds.Empty(); JobSummaries.Empty();
			for (const auto& Job : All) { const FString Id = GetString(Job, TEXT("job_id")); JobIds.Add(MakeShared<FString>(Id)); JobSummaries.Add(Id, Job); }
			if (State().JobId.IsEmpty() && !JobIds.IsEmpty()) State().JobId = *JobIds[0];
			if (JobCombo) JobCombo->RefreshOptions();
			LastServiceRevision = Jobs().GetRevision(); bDataDirty = false; RefreshSelected();
		}

		void RefreshProfiles()
		{
			ProfileIds.Empty(); TArray<FString> Available;
			for (const auto& Profile : Profiles().GetAll()) { const FString Id = GetString(Profile, TEXT("id")); Available.Add(Id); ProfileIds.Add(MakeShared<FString>(Id)); }
			const bool bFallback = ReconcileProfileSelection(State(), Available);
			if (ProfileCombo) { if (bFallback) ProfileCombo->ClearSelection(); ProfileCombo->RefreshOptions(); }
		}
		void RebuildTargets()
		{
			if (!TargetBox) return; TargetBox->ClearChildren();
			for (const auto& Target : State().Targets)
			{
				TargetBox->AddSlot().AutoHeight().Padding(0,4)
				[ SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0) [ SNew(SCheckBox).ToolTipText(LOCTEXT("SelectTarget", "Select this target for bulk editing or removal")).IsChecked_Lambda([Target]() { return Target->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }).OnCheckStateChanged_Lambda([Target](ECheckBoxState Value) { Target->bSelected = Value == ECheckBoxState::Checked; }) ]
						+ SHorizontalBox::Slot().FillWidth(1) [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Target->Path)) ]
						+ SHorizontalBox::Slot().AutoWidth() [ SNew(SButton).Text(LOCTEXT("Up", "Up")).OnClicked_Lambda([this, Target]() { const int32 Index = State().Targets.IndexOfByKey(Target); if (Index > 0) State().Targets.Swap(Index, Index-1); RebuildTargets(); return FReply::Handled(); }) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4,0) [ SNew(SButton).Text(LOCTEXT("Down", "Down")).OnClicked_Lambda([this, Target]() { const int32 Index = State().Targets.IndexOfByKey(Target); if (Index >= 0 && Index+1 < State().Targets.Num()) State().Targets.Swap(Index, Index+1); RebuildTargets(); return FReply::Handled(); }) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4,0) [ SNew(SButton).Text(LOCTEXT("EditTarget", "Edit options")).OnClicked_Lambda([Target]() { Target->Overrides = Merge(State().Options, Target->Overrides); State().EditingTarget = Target; return FReply::Handled(); }) ]
						+ SHorizontalBox::Slot().AutoWidth() [ SNew(SButton).Text(LOCTEXT("Remove", "Remove")).OnClicked_Lambda([this, Target]() { State().Targets.Remove(Target); if (State().EditingTarget == Target) State().EditingTarget.Reset(); RebuildTargets(); return FReply::Handled(); }) ] ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(SExpandableArea).InitiallyCollapsed(true).HeaderContent() [ SNew(STextBlock).Text(LOCTEXT("AdvancedTarget", "Advanced: JSON overrides")) ].BodyContent() [ CreateOverrideEditor(Target) ] ]
					+ SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text_Lambda([Target]() { return FText::FromString(Target->Error); }) ] ];
			}
		}
		FObject BuildTargetsRequest(const TArray<TSharedPtr<FTarget>>& Targets)
		{
			auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			TArray<TSharedPtr<FJsonValue>> Paths; auto Overrides = MakeShared<FJsonObject>();
			for (const auto& Target : Targets) { Paths.Add(MakeShared<FJsonValueString>(Target->Path)); Overrides->SetObjectField(Target->Path, Clone(Target->Overrides)); }
			Request->SetArrayField(TEXT("mesh_paths"), Paths); Request->SetObjectField(TEXT("options"), Clone(State().Options)); Request->SetObjectField(TEXT("overrides"), Overrides); Request->SetBoolField(TEXT("overwrite"), State().bOverwrite);
			return Request;
		}
		bool EvaluateTargets(const TArray<TSharedPtr<FTarget>>& Targets, bool bForExecution, FObject Request = nullptr)
		{
			State().PreviewReport.Empty(); State().InputError.Empty();
			const auto Batch = Application::InspectBatch(Request ? Request : BuildTargetsRequest(Targets));
			TArray<FString> Errors = Batch.Errors; int32 PlannedCount = 0, ReplacementCount = 0;
			for (int32 Index = 0; Index < Batch.Items.Num(); ++Index)
			{
				const auto& Item = Batch.Items[Index]; const auto& Plan = Item.Plan;
				State().PreviewReport += Item.Spec.MeshPath + TEXT("\n  Folder: ") + Plan.FinalDirectory + TEXT("\n");
				if (Targets.IsValidIndex(Index) && !Targets[Index]->Error.IsEmpty()) { Errors.Add(Item.Spec.MeshPath + TEXT(": ") + Targets[Index]->Error); State().PreviewReport += TEXT("  ERROR: ") + Targets[Index]->Error + TEXT("\n"); }
				for (const auto& Error : Plan.Errors) State().PreviewReport += TEXT("  ERROR: ") + Error + TEXT("\n");
				for (const auto& Warning : Plan.Warnings) State().PreviewReport += TEXT("  WARNING: ") + Warning + TEXT("\n");
				for (const FString& File : Plan.PlannedFiles) State().PreviewReport += TEXT("  Planned file: ") + File + TEXT("\n");
				PlannedCount += Plan.PlannedFiles.Num(); ReplacementCount += Plan.ReplacedFiles.Num();
				if (Plan.bRequiresOverwrite)
				{
					State().PreviewReport += TEXT("  REPLACEMENT SCOPE: complete folder ") + Plan.FinalDirectory + TEXT(" (including user files and subdirectories)\n");
					for (const FString& File : Plan.ReplacedFiles) State().PreviewReport += TEXT("  Affected existing file: ") + File + TEXT("\n");
					if (bForExecution && !State().bOverwrite) { const FString Error = TEXT("Explicit whole-folder overwrite approval is required."); Errors.Add(Item.Spec.MeshPath + TEXT(": ") + Error); State().PreviewReport += TEXT("  ERROR: ") + Error + TEXT("\n"); }
				}
			}
			State().PreviewSummary = FString::Printf(TEXT("Output plan: %d targets, %d planned files, %d existing files affected, %d errors. Expand details for exact filenames and replacement scope."), Targets.Num(), PlannedCount, ReplacementCount, Errors.Num());
			State().InputError = FString::Join(Errors, TEXT("\n")); return Errors.IsEmpty();
		}
		void Inspect() { EvaluateTargets(State().Targets, false); }
		void StartTargets(const TArray<TSharedPtr<FTarget>>& Targets)
		{
			const auto Request = BuildTargetsRequest(Targets);
			if (!EvaluateTargets(Targets, true, Request)) return;
			const auto Result = Application::Submit(Request); State().InputError = GetString(Result, TEXT("error"));
			if (GetBool(Result, TEXT("success"))) { State().JobId = GetString(Result, TEXT("job_id")); GConfig->SetString(TEXT("/Script/FBX_Exporter.FBX_ExporterSettings"), TEXT("LastOutputRoot"), *GetString(State().Options, TEXT("output_root")), GEditorPerProjectIni); GConfig->Flush(false, GEditorPerProjectIni); State().bOverwrite = false; RefreshSelected(true); }
		}

		void Retry(const FString& Wanted)
		{
			const auto Job = CurrentJob(); TArray<TSharedPtr<FTarget>> Targets; TSharedPtr<FTarget> EditingTarget;
			for (const auto& Value : GetArray(Job, TEXT("items")))
			{
				const auto Item = Value->AsObject(); if (GetString(Item, TEXT("state")) != Wanted) continue;
				auto Target = MakeShared<FTarget>(); Target->Path = GetString(Item, TEXT("mesh_path")); Target->Overrides = Clone(GetObject(Item, TEXT("options"))); Targets.Add(Target);
			}
			StartTargets(Targets);
		}
		bool ConfirmProfileReplacement(const FObject& Request, const FString& Id)
		{
			const auto Existing = Profiles().Get(Id);
			if (!Existing || GetBool(Existing, TEXT("builtin"))) return true;
			if (FMessageDialog::Open(EAppMsgType::YesNo, LOCTEXT("ReplaceProfile", "Replace the existing user profile with these options?")) != EAppReturnType::Yes) return false;
			Request->SetBoolField(TEXT("overwrite"), true); return true;
		}
		FReply ImportProfile()
		{
			IDesktopPlatform* Desktop = FDesktopPlatformModule::Get(); TArray<FString> Files;
			if (!Desktop || !Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()), TEXT("Import FBX profile"), FPaths::ProjectSavedDir(), TEXT(""), TEXT("JSON profile (*.json)|*.json"), EFileDialogFlags::None, Files) || Files.IsEmpty()) return FReply::Handled();
			FString Text; if (!FFileHelper::LoadFileToString(Text, *Files[0])) { State().InputError = TEXT("Cannot read profile file."); return FReply::Handled(); }
			auto Profile = Parse(Text); if (!Profile) { State().InputError = TEXT("Invalid profile JSON."); return FReply::Handled(); }
			Profile->RemoveField(TEXT("builtin")); Profile->RemoveField(TEXT("success"));
			auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("action"), TEXT("save")); Request->SetObjectField(TEXT("profile"), Profile);
			if (!ConfirmProfileReplacement(Request, GetString(Profile, TEXT("id")))) return FReply::Handled();
			const auto Result = Profiles().Handle(Request); State().InputError = GetString(Result, TEXT("error")); RefreshProfiles(); return FReply::Handled();
		}
		FReply ExportProfile()
		{
			auto Profile = Profiles().Get(State().ProfileId); if (!Profile) { State().InputError = TEXT("Profile not found."); return FReply::Handled(); }
			Profile->RemoveField(TEXT("builtin")); Profile->RemoveField(TEXT("success"));
			IDesktopPlatform* Desktop = FDesktopPlatformModule::Get(); TArray<FString> Files;
			if (!Desktop || !Desktop->SaveFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()), TEXT("Export FBX profile"), FPaths::ProjectSavedDir(), State().ProfileId + TEXT(".json"), TEXT("JSON profile (*.json)|*.json"), EFileDialogFlags::None, Files) || Files.IsEmpty()) return FReply::Handled();
			State().InputError = FFileHelper::SaveStringToFile(JsonText(Profile), *Files[0], FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? TEXT("") : TEXT("Cannot export profile file."); return FReply::Handled();
		}

		void ShowPng()
		{
			const auto Job = CurrentJob();
			for (const auto& Item : GetArray(Job, TEXT("items"))) for (const auto& File : GetArray(GetObject(Item->AsObject(), TEXT("result")), TEXT("files")))
				if (File->AsString().EndsWith(TEXT(".png"))) { PngBrush = MakeShared<FSlateDynamicImageBrush>(FName(*File->AsString()), FVector2D(120,120)); return; }
		}
		TSharedPtr<SVerticalBox> TargetBox; TSharedPtr<SWrapBox> MapBox; TArray<TSharedPtr<int32>> Resolutions;
		TArray<TSharedPtr<FString>> ProfileIds; TSharedPtr<SComboBox<TSharedPtr<FString>>> ProfileCombo; TSharedPtr<FSlateDynamicImageBrush> PngBrush; TArray<TSharedPtr<FString>> JobIds; TSharedPtr<SComboBox<TSharedPtr<FString>>> JobCombo; float RefreshElapsed = 0;
		FDelegateHandle ChangeHandle; FString PinnedJobId; bool bDataDirty = true, bArchive = false, bDiagnostics = false; int32 HistoryOffset = 0; uint64 LastServiceRevision = 0; TMap<FString, FObject> JobSummaries;
		FProgressBarStyle ProgressStyle;
	};
}
void RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) { auto Widget = SNew(SExporterWorkspace); LiveWidget = Widget; return SNew(SDockTab).TabRole(ETabRole::NomadTab)[Widget]; }))
		.SetDisplayName(LOCTEXT("Tab", "FBX Exporter")).SetTooltipText(LOCTEXT("TabTooltip", "Export mesh batches, PBR textures and recover interrupted outputs."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}
void UnregisterTab() { if (const auto Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabId)) Tab->RequestCloseTab(); FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId); LiveWidget.Reset(); Workspace.Reset(); }
void AddAssetsToTab(const TArray<FAssetData>& Assets)
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
	if (auto Widget = LiveWidget.Pin()) Widget->Add(Assets);
}

namespace Testing
{
	bool WorkspaceStateRegression(FString& Error)
	{
		FWorkspace Model(false); bool bPassed = true;
		auto Check = [&Error, &bPassed](bool bCondition, const TCHAR* Message) { if (!bCondition) { Error += FString(Message) + TEXT("\n"); bPassed = false; } };
		const FString ExplicitOptions = JsonText(Model.Options);
		Model.ProfileId = TEXT("deleted_user_profile");
		Check(ReconcileProfileSelection(Model,{TEXT("pbr"),TEXT("geometry_only")}) && Model.ProfileId==TEXT("pbr"),TEXT("Deleted profile identity must fall back to PBR on refresh or reopen."));
		Check(JsonText(Model.Options)==ExplicitOptions,TEXT("Profile identity fallback must preserve explicit options and output path."));
		Model.ProfileId=TEXT("geometry_only");
		Check(!ReconcileProfileSelection(Model,{TEXT("pbr"),TEXT("geometry_only")}) && Model.ProfileId==TEXT("geometry_only"),TEXT("Refresh must preserve a surviving selected profile."));
		const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere"), C = TEXT("/Engine/BasicShapes/Plane.Plane");
		AddTargetPath(Model,A); AddTargetPath(Model,B); AddTargetPath(Model,A); AddTargetPath(Model,C);
		Check(Model.Targets.Num()==3 && Model.Targets[0]->Path==A && Model.Targets[1]->Path==B && Model.Targets[2]->Path==C,TEXT("Duplicate addition must retain first occurrence and target order."));
		Model.Targets[0]->bSelected=true; Model.Targets[2]->bSelected=true; Model.EditingTarget=Model.Targets[0];
		Model.Options->SetNumberField(TEXT("resolution"),512); ApplyGlobalToSelected(Model);
		Model.Options->SetNumberField(TEXT("resolution"),1024);
		Check(GetNumber(Model.Targets[0]->Overrides,TEXT("resolution"))==512 && GetNumber(Model.Targets[2]->Overrides,TEXT("resolution"))==512,TEXT("Bulk options must snapshot each selected target independently."));
		Check(!Model.Targets[1]->Overrides->HasField(TEXT("resolution")),TEXT("Bulk editing must leave unselected overrides untouched."));
		RemoveSelected(Model);
		Check(Model.Targets.Num()==1 && Model.Targets[0]->Path==B && !Model.EditingTarget,TEXT("Removing selection must preserve remaining order and clear a removed active target."));
		Model.Targets[0]->bSelected=true; AddTargetPath(Model,B);
		Check(Model.Targets.Num()==1 && Model.Targets[0]->bSelected,TEXT("Duplicate drop must preserve existing selection."));
		Model.Targets[0]->Error=TEXT("Invalid override JSON."); AddTargetPath(Model,C); Model.Targets[1]->Overrides->SetStringField(TEXT("resolution"),TEXT("bad"));
		int32 ErrorCount=0;
		for (const auto& Target:Model.Targets) { FString Message; if (!ResolveTarget(Model,Target,Message)) ++ErrorCount; }
		Check(ErrorCount==2,TEXT("Target option validation must report all malformed rows."));
		// Exercise the production text attribute and Slate focus-loss commit path, not just model values.
		FWorkspace EditorModel(false); AddTargetPath(EditorModel, A);
		const auto EditorTarget = EditorModel.Targets[0]; EditorTarget->bSelected = true;
		const auto OverrideEditor = CreateOverrideEditor(EditorTarget);
		TFunction<TSharedPtr<SWidget>(const TSharedRef<SWidget>&)> FindEditable;
		FindEditable = [&FindEditable](const TSharedRef<SWidget>& Widget) -> TSharedPtr<SWidget>
		{
			if (Widget->GetType() == FName(TEXT("SEditableText"))) return Widget;
			FChildren* Children = Widget->GetChildren();
			for (int32 Index = 0; Index < Children->Num(); ++Index)
				if (auto Found = FindEditable(Children->GetChildAt(Index))) return Found;
			return nullptr;
		};
		const auto Editable = FindEditable(OverrideEditor);
		Check(Editable.IsValid(), TEXT("The production override textbox must contain a real editable text widget."));
		if (Editable)
		{
			auto RefreshEditor = [&]() { Editable->Invalidate(EInvalidateWidgetReason::Layout); OverrideEditor->Invalidate(EInvalidateWidgetReason::Layout); OverrideEditor->SlatePrepass(1.0f); };
			auto LoseFocus = [&]() { Editable->OnFocusLost(FFocusEvent(EFocusCause::Mouse, 0)); RefreshEditor(); };
			auto TypeRawJson = [&](const FString& Text)
			{
				OverrideEditor->SelectAllText();
				for (const TCHAR Character : Text) Editable->OnKeyChar(FGeometry(), FCharacterEvent(Character, FModifierKeysState(), 0, false));
			};
			RefreshEditor();
			Check(OverrideEditor->GetText().ToString() == JsonText(EditorTarget->Overrides), TEXT("A new row displays its current override model."));
			EditorModel.Options->SetNumberField(TEXT("resolution"), 2048); EditorModel.Options->SetStringField(TEXT("output_root"), TEXT("C:/Owned/NewBulkOutput"));
			ApplyGlobalToSelected(EditorModel); RefreshEditor();
			Check(OverrideEditor->GetText().ToString() == JsonText(EditorTarget->Overrides) && OverrideEditor->GetText().ToString().Contains(TEXT("NewBulkOutput")), TEXT("Bulk replacement updates the existing textbox without rebuilding its row."));
			LoseFocus();
			Check(GetNumber(EditorTarget->Overrides, TEXT("resolution")) == 2048 && GetString(EditorTarget->Overrides, TEXT("output_root")) == TEXT("C:/Owned/NewBulkOutput"), TEXT("Unchanged focus-loss commit must not restore stale pre-bulk overrides."));
			EditorTarget->Overrides = Clone(EditorModel.Options); EditorTarget->Overrides->SetNumberField(TEXT("resolution"), 4096); RefreshEditor();
			Check(OverrideEditor->GetText().ToString() == JsonText(EditorTarget->Overrides) && OverrideEditor->GetText().ToString().Contains(TEXT("4096")), TEXT("Per-target option replacement also updates the same textbox."));
			const FString ValidBeforeTyping = JsonText(EditorTarget->Overrides), InvalidJson = TEXT("{\"resolution\":");
			TypeRawJson(InvalidJson);
			Check(JsonText(EditorTarget->Overrides) == ValidBeforeTyping && EditorTarget->Error.IsEmpty(), TEXT("Raw typing does not write or normalize the override model before commit."));
			LoseFocus();
			FString InvalidError;
			Check(JsonText(EditorTarget->Overrides) == ValidBeforeTyping && EditorTarget->InvalidOverrideDraft.IsSet() && OverrideEditor->GetText().ToString() == InvalidJson && !ResolveTarget(EditorModel, EditorTarget, InvalidError), TEXT("Invalid raw JSON remains visible, preserves the last valid model, and blocks validation."));
			TypeRawJson(TEXT("{\"resolution\":1024,\"maps\":[]}")); LoseFocus();
			Check(EditorTarget->Error.IsEmpty() && !EditorTarget->InvalidOverrideDraft.IsSet() && GetNumber(EditorTarget->Overrides, TEXT("resolution")) == 1024 && OverrideEditor->GetText().ToString() == JsonText(EditorTarget->Overrides), TEXT("A valid raw JSON correction updates the model and clears the invalid draft."));
			TypeRawJson(InvalidJson); LoseFocus(); ApplyGlobalToSelected(EditorModel); RefreshEditor();
			Check(EditorTarget->Error.IsEmpty() && !EditorTarget->InvalidOverrideDraft.IsSet() && OverrideEditor->GetText().ToString() == JsonText(EditorTarget->Overrides), TEXT("Applying global options clears both invalid error and draft in an existing row."));
		}
		auto Diagnostic = MakeShared<FJsonObject>(); Diagnostic->SetStringField(TEXT("record_kind"), TEXT("diagnostic"));
		Diagnostic->SetStringField(TEXT("job_id"), TEXT("fbx_exporter-diagnostic-owned")); Diagnostic->SetStringField(TEXT("phase"), TEXT("issue"));
		Diagnostic->SetStringField(TEXT("source_path"), TEXT("C:/Owned/Recovery/invalid.json")); Diagnostic->SetStringField(TEXT("error_code"), TEXT("JOURNAL_INVALID")); Diagnostic->SetStringField(TEXT("error"), TEXT("Review owned metadata."));
		const FString IssueSummary = BuildResultSummary(Diagnostic);
		Check(IssueSummary.Contains(TEXT("Diagnostic ID: fbx_exporter-diagnostic-owned")) && IssueSummary.Contains(TEXT("Phase: issue")) && IssueSummary.Contains(TEXT("Source metadata: C:/Owned/Recovery/invalid.json")) && IssueSummary.Contains(TEXT("Raw evidence: Not created yet.")) && IssueSummary.Contains(TEXT("Error: JOURNAL_INVALID")), TEXT("Issue diagnostics expose concise identity, phase, source, pending raw evidence and error."));
		Diagnostic->SetStringField(TEXT("phase"), TEXT("prepared")); Diagnostic->SetStringField(TEXT("raw_path"), TEXT("C:/Owned/Quarantine/invalid.raw"));
		const FString PreparedSummary = BuildResultSummary(Diagnostic);
		Check(PreparedSummary.Contains(TEXT("Phase: prepared")) && PreparedSummary.Contains(TEXT("Raw evidence: C:/Owned/Quarantine/invalid.raw")), TEXT("Prepared diagnostics show the preserved raw path."));
		Check(RecoveryTooltip(Diagnostic).ToString().Contains(TEXT("quarantines damaged metadata")) && RecoveryTooltip(Diagnostic).ToString().Contains(TEXT("files are untouched")), TEXT("Diagnostic acknowledgement explains metadata handling and output preservation."));
		auto Normal = MakeShared<FJsonObject>(); auto NormalItem = MakeShared<FJsonObject>(); NormalItem->SetStringField(TEXT("state"), TEXT("failed")); NormalItem->SetStringField(TEXT("mesh_path"), A);
		auto NormalResult = MakeShared<FJsonObject>(); NormalResult->SetArrayField(TEXT("warnings"), Strings({TEXT("Original material warning")})); NormalItem->SetObjectField(TEXT("result"), NormalResult); Normal->SetArrayField(TEXT("items"), {MakeShared<FJsonValueObject>(NormalItem)});
		Check(BuildResultSummary(Normal).Contains(TEXT("warnings: Original material warning")) && !BuildResultSummary(Normal).Contains(TEXT("Diagnostic ID:")), TEXT("Ordinary result fields remain visible without diagnostic formatting."));
		Check(RecoveryTooltip(Normal).ToString().Contains(TEXT("unresolved diagnostic")), TEXT("Normal acknowledgement explains the matching diagnostic prerequisite."));
		return bPassed;
	}
	TSharedRef<SWidget> CreateWorkspaceWidget(const TArray<FAssetData>& Assets)
	{
		State().Targets.Empty(); State().EditingTarget.Reset(); State().InputError.Empty();
		for (const auto& Job : Jobs().GetJobSummaries(0, 50, false))
			if (GetString(Job, TEXT("state")) == TEXT("succeeded")) { State().JobId = GetString(Job, TEXT("job_id")); break; }
		auto Widget = SNew(SExporterWorkspace); Widget->Add(Assets); Widget->PreparePreviewForTesting();
		return Widget;
	}
}
}
#undef LOCTEXT_NAMESPACE
