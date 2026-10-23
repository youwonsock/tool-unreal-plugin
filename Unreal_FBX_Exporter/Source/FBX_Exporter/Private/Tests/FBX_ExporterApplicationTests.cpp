#include "FBX_ExporterApplication.h"
#include "AssetCompilingManager.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterBatchPlanTest, "FBX_Exporter.Editor.Application.BatchPlanParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterBatchPlanTest::RunTest(const FString&)
{
	using namespace UnrealToolsRuntime;
	TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	if (!TestNotNull(TEXT("Engine-only fixture"), Cube.Get())) return false;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageRoot = TEXT("/Game/FBXExporterBatch_") + Id;
	TArray<TStrongObjectPtr<UStaticMesh>> Held;
	auto MakeMesh = [&](const FString& Package, const TCHAR* Name)
	{
		auto* Mesh = DuplicateObject<UStaticMesh>(Cube.Get(), CreatePackage(*Package), Name);
		Mesh->ClearFlags(RF_Transient); Mesh->SetFlags(RF_Public | RF_Standalone); Mesh->Build(true);
		Held.Emplace(Mesh); return Mesh;
	};
	UStaticMesh* Parent = MakeMesh(PackageRoot / TEXT("Parent"), TEXT("Parent"));
	UStaticMesh* Child = MakeMesh(PackageRoot / TEXT("Parent/Child"), TEXT("Child"));
	UStaticMesh* Sibling = MakeMesh(PackageRoot / TEXT("ParentExtra"), TEXT("ParentExtra"));
	FAssetCompilingManager::Get().FinishAllCompilation();
	ON_SCOPE_EXIT { for (const auto& Mesh : Held) { Mesh->ClearFlags(RF_Standalone); Mesh->GetOutermost()->SetDirtyFlag(false); } };
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/BatchPlan") / Id);
	FFBXExporterOptions Options; Options.OutputRoot = Root; Options.OutputLayout = TEXT("asset_path"); Options.TextureMaps = EFBXExporterTextureMap::None;
	auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString());
	Request->SetObjectField(TEXT("options"), FBXExporter::OptionsToJson(Options));
	auto SetPaths = [&](const TArray<FString>& Paths) { Request->SetArrayField(TEXT("mesh_paths"), Strings(Paths)); };
	for (const TArray<FString>& Paths : {TArray<FString>{Parent->GetPathName(), Child->GetPathName()}, TArray<FString>{Child->GetPathName(), Parent->GetPathName()}})
	{
		SetPaths(Paths); const auto Plan = FBXExporter::Application::InspectBatch(Request);
		TestEqual(TEXT("Both conflicting targets retain a plan row"), Plan.Items.Num(), 2);
		TestFalse(TEXT("Parent/child output conflict fails in either order"), Plan.CanExport());
		if (Plan.Items.Num() == 2)
		{
			TestFalse(TEXT("Conflict row retains concrete output files"), Plan.Items[1].Plan.PlannedFiles.IsEmpty());
			TestEqual(TEXT("Collision error is structured"), Plan.Items[1].ErrorCode, FString(TEXT("output_collision")));
		}
	}
	SetPaths({Parent->GetPathName(), Sibling->GetPathName()});
	const auto SiblingPlan = FBXExporter::Application::InspectBatch(Request);
	TestTrue(TEXT("Similar path prefix is not a directory overlap"), SiblingPlan.CanExport());
	auto Overrides = MakeShared<FJsonObject>(); auto BadOptions = MakeShared<FJsonObject>(); BadOptions->SetStringField(TEXT("resolution"), TEXT("invalid"));
	Overrides->SetObjectField(Parent->GetPathName(), BadOptions); Request->SetObjectField(TEXT("overrides"), Overrides);
	const auto AllRows = FBXExporter::Application::InspectBatch(Request);
	TestEqual(TEXT("Invalid first row does not truncate inspection"), AllRows.Items.Num(), 2);
	if (AllRows.Items.Num() == 2) TestFalse(TEXT("Later valid row is fully inspected"), AllRows.Items[1].Plan.PlannedFiles.IsEmpty());
	Request->RemoveField(TEXT("overrides"));
	const auto Tool = IModelContextProtocolModule::GetChecked().FindTool(TEXT("fbx_exporter_preflight"));
	if (!TestTrue(TEXT("MCP adapter registered"), Tool.IsValid())) return false;
	auto Wire = MakeShared<FJsonObject>(); Wire->SetObjectField(TEXT("options"), FBXExporter::OptionsToJson(Options)); Wire->SetArrayField(TEXT("mesh_paths"), Strings({Parent->GetPathName(), Sibling->GetPathName()}));
	const auto Response = Tool->Run(Wire).JsonObject->GetObjectField(TEXT("structuredContent"));
	const auto Items = GetArray(Response, TEXT("items"));
	TestEqual(TEXT("GUI-core and MCP keep target count"), Items.Num(), SiblingPlan.Items.Num());
	for (int32 Index = 0; Index < Items.Num() && Index < SiblingPlan.Items.Num(); ++Index)
	{
		const auto Expected = FBXExporter::Application::InspectItemJson(SiblingPlan.Items[Index]);
		for (const TCHAR* Field : {TEXT("planned_files"), TEXT("replacement_files"), TEXT("warnings"), TEXT("errors")})
		{
			const auto ActualValues = GetArray(Items[Index]->AsObject(), Field), ExpectedValues = GetArray(Expected, Field);
			TestEqual(FString(TEXT("Common plan count: ")) + Field, ActualValues.Num(), ExpectedValues.Num());
			for (int32 Entry = 0; Entry < ActualValues.Num() && Entry < ExpectedValues.Num(); ++Entry)
				TestEqual(FString(TEXT("Common plan value: ")) + Field, ActualValues[Entry]->AsString(), ExpectedValues[Entry]->AsString());
		}
		TestEqual(TEXT("Concrete folder parity"), GetString(Items[Index]->AsObject(), TEXT("final_directory")), GetString(Expected, TEXT("final_directory")));
	}
	TestFalse(TEXT("Inspection creates neither files nor output root"), IFileManager::Get().DirectoryExists(*Root));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterRecoveryTypesTest, "FBX_Exporter.Editor.Application.RecoveryJournalTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterRecoveryTypesTest::RunTest(const FString&)
{
	using namespace UnrealToolsRuntime;
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/RecoveryTypes"));
	auto Journal = MakeShared<FJsonObject>(); Journal->SetNumberField(TEXT("version"), 1); Journal->SetStringField(TEXT("state"), TEXT("publishing"));
	Journal->SetStringField(TEXT("original"), Root / TEXT("Final")); Journal->SetStringField(TEXT("staging"), Root / TEXT("Stage")); Journal->SetStringField(TEXT("backup"), Root / TEXT("Stage_Previous"));
	Journal->SetArrayField(TEXT("original_files"), Strings({Root / TEXT("Final/user.txt")})); Journal->SetArrayField(TEXT("staged_files"), Strings({Root / TEXT("Stage/mesh.fbx")}));
	FString Error; TestTrue(TEXT("Typed owned journal is accepted"), FBXExporter::ValidateRecoveryJournal(Journal, Error));
	auto MissingResult = MakeShared<FJsonObject>(); MissingResult->SetStringField(TEXT("recovery_journal"), Root / TEXT("never_created.json"));
	auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("result"), MissingResult);
	auto Interrupted = MakeShared<FJsonObject>(); Interrupted->SetBoolField(TEXT("recovery_required"), true); Interrupted->SetArrayField(TEXT("items"), {MakeShared<FJsonValueObject>(Item)});
	FBXExporter::AnnotateRecoveryStatus(Interrupted);
	TestTrue(TEXT("Prepublish missing journal is explicit in status"), GetBool(MissingResult, TEXT("recovery_journal_missing")));
	TestFalse(TEXT("Missing-journal review guidance is visible"), GetArray(MissingResult, TEXT("warnings")).IsEmpty());
	TestFalse(TEXT("Missing-journal annotation never creates metadata"), IFileManager::Get().FileExists(*(Root / TEXT("never_created.json"))));
	for (const auto& Mutate : {TFunction<void(FObject&)>([](FObject& Data) { Data->SetStringField(TEXT("version"), TEXT("1")); }), TFunction<void(FObject&)>([](FObject& Data) { Data->SetStringField(TEXT("state"), TEXT("unknown")); }), TFunction<void(FObject&)>([Root](FObject& Data) { Data->SetArrayField(TEXT("staged_files"), Strings({Root / TEXT("Final/user.txt")})); })})
	{ auto Bad = Clone(Journal); Mutate(Bad); Error.Empty(); TestFalse(TEXT("Invalid version/state/foreign files cannot release recovery"), FBXExporter::ValidateRecoveryJournal(Bad, Error)); }
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterDomainAckTest, "FBX_Exporter.Editor.Application.DomainRecoveryAcknowledgement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterDomainAckTest::RunTest(const FString&)
{
	using namespace UnrealToolsRuntime;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/DomainAck") / Id);
	const FString JournalRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery"));
	const FString Missing = JournalRoot / (Id + TEXT("_missing.json")), Corrupt = JournalRoot / (Id + TEXT("_corrupt.json")), Orphan = JournalRoot / (Id + TEXT("_orphan.json")), Other = JournalRoot / (Id + TEXT("_other.json"));
	const FString Final = Root / TEXT("Final"), Sentinel = Final / TEXT("user.txt");
	IFileManager::Get().MakeDirectory(*Final, true);
	TestTrue(TEXT("Owned output sentinel prepared"), FFileHelper::SaveStringToFile(TEXT("unchanged output"), *Sentinel));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*Corrupt, false, true); IFileManager::Get().Delete(*Orphan, false, true); IFileManager::Get().Delete(*Other, false, true);
		if (FPaths::IsUnderDirectory(Root, FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/DomainAck")))) IFileManager::Get().DeleteDirectory(*Root, false, true);
	};
	auto MakeJob = [&](const FString& Path)
	{
		auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("recovery_journal"), Path); Result->SetStringField(TEXT("final_directory"), Final);
		auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("result"), Result);
		auto Job = MakeShared<FJsonObject>(); Job->SetStringField(TEXT("job_id"), TEXT("review_") + Id); Job->SetBoolField(TEXT("recovery_required"), true); Job->SetArrayField(TEXT("items"), {MakeShared<FJsonValueObject>(Item)});
		return Job;
	};
	auto AcknowledgeIssue = [&](const FString& Path)
	{
		for (const auto& Issue : FBXExporter::Jobs().GetRecordIssues())
			if (CanonicalPath(GetString(Issue, TEXT("source_path"))) == CanonicalPath(Path))
			{
				auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("job_id"), GetString(Issue, TEXT("job_id"))); Request->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
				return FBXExporter::Jobs().Control(Request);
			}
		return UnrealToolsRuntime::Error(TEXT("Owned diagnostic was not registered."));
	};
	FString ErrorText;
	TestTrue(TEXT("Explicit review can acknowledge a never-created prepublish journal"), FBXExporter::AcknowledgeJobRecovery(MakeJob(Missing), ErrorText));
	TestFalse(TEXT("Missing checkpoint acknowledgement fabricates no journal"), IFileManager::Get().FileExists(*Missing));
	IFileManager::Get().MakeDirectory(*JournalRoot, true); FFileHelper::SaveStringToFile(TEXT("{"), *Corrupt);
	ErrorText.Empty(); TestFalse(TEXT("Present corrupt journal stays blocked"), FBXExporter::AcknowledgeJobRecovery(MakeJob(Corrupt), ErrorText));
	TestTrue(TEXT("Failure preserves corrupt journal bytes"), IFileManager::Get().FileExists(*Corrupt));
	const auto Repaired = AcknowledgeIssue(Corrupt);
	TestTrue(TEXT("Explicit diagnostic review preserves and releases owned damaged metadata"), GetBool(Repaired, TEXT("success")));
	TestTrue(TEXT("Acknowledgement guard proves preserved evidence"), FBXExporter::Jobs().WasRecordIssueAcknowledged(Corrupt));
	ErrorText.Empty(); TestTrue(TEXT("Normal recovery retry can finish after diagnostic review"), FBXExporter::AcknowledgeJobRecovery(MakeJob(Corrupt), ErrorText));
	auto Journal = MakeShared<FJsonObject>(); Journal->SetNumberField(TEXT("version"), 1); Journal->SetStringField(TEXT("state"), TEXT("publishing"));
	Journal->SetStringField(TEXT("original"), Final); Journal->SetStringField(TEXT("staging"), Root / TEXT("Stage")); Journal->SetStringField(TEXT("backup"), Root / TEXT("Stage_Previous"));
	Journal->SetArrayField(TEXT("original_files"), Strings({Sentinel})); Journal->SetArrayField(TEXT("staged_files"), Strings({Root / TEXT("Stage/mesh.fbx")}));
	TestTrue(TEXT("Orphan fixture metadata is persisted"), AtomicJson(Orphan, Journal, ErrorText));
	FBXExporter::RegisterRecoveryDiagnostics(); bool Complete = false;
	for (int32 Step = 0; Step < 2000 && !Complete; ++Step) Complete = FBXExporter::AdvanceRecoveryDiagnostics(8);
	TestTrue(TEXT("Budgeted orphan scan completes"), Complete);
	TArray<uint8> BeforeGate, AfterGate;
	TestTrue(TEXT("Orphan bytes captured before normal acknowledgement"), FFileHelper::LoadFileToArray(BeforeGate, *Orphan));
	ErrorText.Empty(); TestFalse(TEXT("A matching orphan diagnostic must be acknowledged before its normal job"), FBXExporter::AcknowledgeJobRecovery(MakeJob(Orphan), ErrorText));
	TestTrue(TEXT("Diagnostic gate leaves orphan metadata unchanged"), FFileHelper::LoadFileToArray(AfterGate, *Orphan) && BeforeGate == AfterGate);
	TestTrue(TEXT("Valid publishing orphan becomes an addressable recovery diagnostic"), GetBool(AcknowledgeIssue(Orphan), TEXT("success")));
	TestTrue(TEXT("Unrelated owned journal fixture persisted"), AtomicJson(Other, Journal, ErrorText));
	TestTrue(TEXT("Unrelated journal diagnostic registered"), GetBool(FBXExporter::Jobs().RegisterRecordIssue(Other, TEXT("Owned unrelated journal review.")), TEXT("success")));
	ErrorText.Empty(); TestTrue(TEXT("Only this journal's diagnostic gates the normal recovery retry"), FBXExporter::AcknowledgeJobRecovery(MakeJob(Orphan), ErrorText));
	TestTrue(TEXT("Normal acknowledgement preserves an unrelated outstanding diagnostic"), FBXExporter::Jobs().GetRecordIssues().ContainsByPredicate([&Other](const FObject& Issue) { return CanonicalPath(GetString(Issue, TEXT("source_path"))) == CanonicalPath(Other); }));
	TestTrue(TEXT("Owned unrelated diagnostic is explicitly acknowledged for cleanup"), GetBool(AcknowledgeIssue(Other), TEXT("success")));
	FString After; TestTrue(TEXT("Recovery review keeps output sentinel readable"), FFileHelper::LoadFileToString(After, *Sentinel));
	TestEqual(TEXT("All recovery actions leave user output unchanged"), After, FString(TEXT("unchanged output")));
	return true;
}
#endif
