#include "FBX_ExporterService.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	class FRecoveryCheckpointRunner final : public UnrealToolsRuntime::IItemRunner
	{
	public:
		FRecoveryCheckpointRunner(const FString& Journal, const FString& Final)
		{
			Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("recovery_journal"), Journal); Result->SetStringField(TEXT("final_directory"), Final);
		}
		virtual UnrealToolsRuntime::EStepResult Step(bool, bool) override { return UnrealToolsRuntime::EStepResult::Continue; }
		virtual FString GetStage() const override { return TEXT("publish"); }
		virtual bool CanCancel() const override { return false; }
		virtual bool IsCommitPhase() const override { return true; }
		virtual UnrealToolsRuntime::FObject GetResult() const override { return Result; }
		virtual uint64 GetSnapshotRevision() const override { return 0; }
	private:
		UnrealToolsRuntime::FObject Result;
	};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterMultiDomainRecoveryPersistenceTest,
	"FBX_Exporter.Editor.Application.MultiDomainRecoveryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterMultiDomainRecoveryPersistenceTest::RunTest(const FString&)
{
#if !PLATFORM_WINDOWS
	AddWarning(TEXT("Read-only atomic replacement fault requires the supported Win64 filesystem."));
	return true;
#else
	using namespace UnrealToolsRuntime;
	// Each scenario owns a separate core service. No runner, asset load, global
	// service shutdown, or latent tick is needed to exercise the real domain hook.
	auto RunScenario = [this](bool bFailSharedPersist)
	{
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Parent = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/DomainPersistence"));
		const FString Name = TEXT("UnrealTools/V12/DomainPersistence/") + Id + TEXT("/Runtime");
		const FString Root = Parent / Id;
		const FString RecoveryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery"));
		const TArray<FString> Journals = {RecoveryRoot / (Id + TEXT("_A.json")), RecoveryRoot / (Id + TEXT("_B.json"))};
		const FString Label = bFailSharedPersist ? TEXT("Shared persist failure: ") : TEXT("Second domain failure: ");
		FProfileStore Profiles(TEXT("fbx_recovery_test"), Name);
		TUniquePtr<FJobService> LocalJobs;
		auto& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		ON_SCOPE_EXIT
		{
			LocalJobs.Reset();
			for (const FString& Journal : Journals)
			{
				PlatformFile.SetReadOnly(*Journal, false);
				if (FPaths::IsUnderDirectory(Journal, RecoveryRoot) && FPaths::GetCleanFilename(Journal).StartsWith(Id + TEXT("_")))
					IFileManager::Get().Delete(*Journal, false, true);
			}
			if (FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Root), Parent))
				IFileManager::Get().DeleteDirectory(*Root, false, true);
		};
		FFBXExporterOptions Options;
		Options.OutputRoot = Root / TEXT("Outputs"); Options.OutputLayout = TEXT("mesh_name");
		Options.TextureMaps = EFBXExporterTextureMap::None; Options.BakeResolution = 512;
		auto Reload = [&]()
		{
			LocalJobs.Reset();
			LocalJobs = MakeUnique<FJobService>(TEXT("fbx_recovery_test"), Name, Profiles, FBXExporter::OptionsToJson(Options),
				&FBXExporter::NormalizeJobOptions,
				[](const FItemSpec& Spec) { return TArray<FString>{FBXExporter::PlannedDirectory(Spec.MeshPath, FBXExporter::OptionsFromJson(Spec.Options))}; },
				[](const FItemSpec&, const FString&) { return TUniquePtr<IItemRunner>(); });
			LocalJobs->SetRecoveryAcknowledger(&FBXExporter::AcknowledgeJobRecovery);
		};
		Reload();
		auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString());
		Request->SetArrayField(TEXT("mesh_paths"), Strings({TEXT("/Game/DomainRecovery_A.DomainRecovery_A"), TEXT("/Game/DomainRecovery_B.DomainRecovery_B")}));
		const auto Accepted = LocalJobs->Start(Request);
		if (!TestTrue(Label + TEXT("Metadata-only batch accepted"), GetBool(Accepted, TEXT("success")))) return false;
		const FString JobId = GetString(Accepted, TEXT("job_id"));
		const FString JobPath = Root / TEXT("Runtime/Jobs") / (JobId + TEXT(".json"));
		// Only the owned local instance is stopped. Its genuine durable request,
		// hash and terminal fields seed the interrupted publication checkpoint.
		LocalJobs.Reset();
		FString Text, ErrorText;
		if (!TestTrue(Label + TEXT("Local interrupted job readable"), FFileHelper::LoadFileToString(Text, *JobPath))) return false;
		const auto Job = Parse(Text);
		if (!TestTrue(Label + TEXT("Generated job parses"), Job.IsValid())) return false;
		const auto Items = GetArray(Job, TEXT("items"));
		if (!TestEqual(Label + TEXT("Two durable targets"), Items.Num(), 2)) return false;
		Job->SetBoolField(TEXT("recovery_required"), true); Job->SetStringField(TEXT("stage"), TEXT("publish"));
		TArray<FString> ProtectedDirectories, Finals;
		TMap<FString, TArray<uint8>> ProtectedBytes;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const FString Final = FBXExporter::PlannedDirectory(GetString(Items[Index]->AsObject(), TEXT("mesh_path")), Options);
			const FString Stage = Root / FString::Printf(TEXT("Stage%d"), Index), Backup = Stage + TEXT("_Previous");
			Finals.Add(Final);
			for (const FString& Directory : {Final, Stage, Backup})
			{
				ProtectedDirectories.Add(Directory);
				if (!TestTrue(Label + TEXT("Owned sentinel directory created"), IFileManager::Get().MakeDirectory(*Directory, true))) return false;
				const FString File = Directory / TEXT("sentinel.bin");
				const TArray<uint8> Bytes = {0, 255, static_cast<uint8>(Index), 13, 10, 42, 0};
				if (!TestTrue(Label + TEXT("Binary sentinel saved"), FFileHelper::SaveArrayToFile(Bytes, *File))) return false;
				ProtectedBytes.Add(File, Bytes);
			}
			auto Journal = MakeShared<FJsonObject>();
			Journal->SetNumberField(TEXT("version"), 1); Journal->SetStringField(TEXT("provider_id"), TEXT("fbx_exporter"));
			Journal->SetStringField(TEXT("job_id"), JobId); Journal->SetStringField(TEXT("state"), TEXT("publishing"));
			Journal->SetStringField(TEXT("original"), Final); Journal->SetStringField(TEXT("staging"), Stage); Journal->SetStringField(TEXT("backup"), Backup);
			Journal->SetArrayField(TEXT("original_files"), Strings({Final / TEXT("sentinel.bin")}));
			Journal->SetArrayField(TEXT("staged_files"), Strings({Stage / TEXT("sentinel.bin")}));
			if (!TestTrue(Label + TEXT("Valid domain journal saved"), AtomicJson(Journals[Index], Journal, ErrorText))) return false;
			auto Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), false); Result->SetBoolField(TEXT("recovery_required"), true);
			Result->SetStringField(TEXT("error_code"), TEXT("EDITOR_INTERRUPTED"));
			Result->SetStringField(TEXT("recovery_journal"), Journals[Index]); Result->SetStringField(TEXT("final_directory"), Final);
			Result->SetStringField(TEXT("recovery_staging"), Stage); Result->SetStringField(TEXT("recovery_backup"), Backup);
			const auto Item = Items[Index]->AsObject();
			Item->SetStringField(TEXT("stage"), TEXT("publish")); Item->SetBoolField(TEXT("commit_pending"), true); Item->SetObjectField(TEXT("result"), Result);
		}
		if (!TestTrue(Label + TEXT("Interrupted checkpoint saved atomically"), AtomicJson(JobPath, Job, ErrorText))) return false;
		TArray<uint8> JobBefore, SecondBefore;
		if (!TestTrue(Label + TEXT("Durable checkpoint bytes captured"), FFileHelper::LoadFileToArray(JobBefore, *JobPath)) ||
			!TestTrue(Label + TEXT("Second journal bytes captured"), FFileHelper::LoadFileToArray(SecondBefore, *Journals[1]))) return false;
		Reload();
		auto Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("job_id"), JobId);
		auto Ack = Clone(Selector); Ack->SetStringField(TEXT("action"), TEXT("acknowledge_recovery"));
		auto VerifyFiles = [&](const FString& Step)
		{
			TArray<FString> ActualFiles, ExpectedFiles;
			ProtectedBytes.GetKeys(ExpectedFiles);
			for (const FString& Directory : ProtectedDirectories)
			{
				TestTrue(Label + Step + TEXT(" keeps output/staging/backup directories"), IFileManager::Get().DirectoryExists(*Directory));
				IFileManager::Get().FindFilesRecursive(ActualFiles, *Directory, TEXT("*"), true, false, false);
			}
			ActualFiles.Sort(); ExpectedFiles.Sort();
			TestTrue(Label + Step + TEXT(" keeps exact output/staging/backup file set"), ActualFiles == ExpectedFiles);
			for (const auto& Pair : ProtectedBytes)
			{
				TArray<uint8> Bytes;
				TestTrue(Label + Step + TEXT(" keeps every sentinel byte"), FFileHelper::LoadFileToArray(Bytes, *Pair.Key) && Bytes == Pair.Value);
			}
		};
		auto VerifyHeld = [&](const FString& Step)
		{
			const auto Status = LocalJobs->Status(Selector);
			TestTrue(Label + Step + TEXT(" retains recovery in current snapshot"), GetBool(Status, TEXT("success")) && GetBool(Status, TEXT("recovery_required")));
			TestFalse(Label + Step + TEXT(" publishes no premature acknowledgement timestamp"), Status->HasField(TEXT("recovery_acknowledged_at")));
			TArray<uint8> CurrentBytes;
			TestTrue(Label + Step + TEXT(" preserves durable job bytes"), FFileHelper::LoadFileToArray(CurrentBytes, *JobPath) && CurrentBytes == JobBefore);
			for (const FString& Final : Finals)
			{
				TestFalse(Label + Step + TEXT(" retains exact output reservation"), LocalJobs->CheckReservations({Final}, ErrorText));
				TestFalse(Label + Step + TEXT(" retains child output reservation"), LocalJobs->CheckReservations({Final / TEXT("Child")}, ErrorText));
			}
			TestFalse(Label + Step + TEXT(" retains shared ancestor reservation"), LocalJobs->CheckReservations({Options.OutputRoot}, ErrorText));
			TestTrue(Label + Step + TEXT(" allows disjoint output"), LocalJobs->CheckReservations({Root / TEXT("Unrelated")}, ErrorText));
			TestTrue(Label + Step + TEXT(" creates no local record diagnostic"), LocalJobs->GetRecordIssues().IsEmpty());
			VerifyFiles(Step);
		};
		auto JournalState = [&](int32 Index)
		{
			FString Json;
			TestTrue(Label + TEXT("Domain journal remains readable"), FFileHelper::LoadFileToString(Json, *Journals[Index]));
			return GetString(Parse(Json), TEXT("state"));
		};
		VerifyHeld(TEXT("Before acknowledgement"));
		if (bFailSharedPersist)
		{
			LocalJobs->SetPersistGateForTesting([JobId](const FObject& Candidate, FString& Message)
			{
				if (GetString(Candidate, TEXT("job_id")) == JobId && !GetBool(Candidate, TEXT("recovery_required")) && Candidate->HasField(TEXT("recovery_acknowledged_at")))
				{ Message = TEXT("Owned test denied the final shared job persistence."); return false; }
				return true;
			});
		}
		else if (!TestTrue(Label + TEXT("Second domain made read-only"), PlatformFile.SetReadOnly(*Journals[1], true))) return false;
		const auto Failed = LocalJobs->Control(Ack);
		TestFalse(Label + TEXT("Failed acknowledgement reports failure"), GetBool(Failed, TEXT("success")));
		TestEqual(Label + TEXT("Failure retains structured code"), GetString(Failed, TEXT("error_code")), FString(bFailSharedPersist ? TEXT("IO_ERROR") : TEXT("RECOVERY_REQUIRED")));
		TestEqual(Label + TEXT("First domain commit is durable"), JournalState(0), FString(TEXT("acknowledged")));
		TestEqual(Label + TEXT("Second domain state matches failure boundary"), JournalState(1), FString(bFailSharedPersist ? TEXT("acknowledged") : TEXT("publishing")));
		if (!bFailSharedPersist)
		{
			TArray<uint8> After;
			TestTrue(Label + TEXT("Failed domain replacement preserves its original bytes"), FFileHelper::LoadFileToArray(After, *Journals[1]) && After == SecondBefore);
		}
		VerifyHeld(TEXT("After failure"));
		TArray<TArray<uint8>> DomainAfterFailure;
		for (const FString& Journal : Journals)
		{
			TArray<uint8> Bytes;
			TestTrue(Label + TEXT("Domain failure-boundary bytes captured"), FFileHelper::LoadFileToArray(Bytes, *Journal));
			DomainAfterFailure.Add(MoveTemp(Bytes));
		}
		Reload();
		VerifyHeld(TEXT("After local reload"));
		for (int32 Index = 0; Index < Journals.Num(); ++Index)
		{
			TArray<uint8> Bytes;
			TestTrue(Label + TEXT("Local reload preserves domain metadata bytes"), FFileHelper::LoadFileToArray(Bytes, *Journals[Index]) && Bytes == DomainAfterFailure[Index]);
		}
		TestEqual(Label + TEXT("Reload retains first domain acknowledgement"), JournalState(0), FString(TEXT("acknowledged")));
		TestEqual(Label + TEXT("Reload retains second domain boundary"), JournalState(1), FString(bFailSharedPersist ? TEXT("acknowledged") : TEXT("publishing")));
		// Already acknowledged domains must succeed without attempting another
		// metadata write. Leave them read-only during the exact same action retry.
		if (!TestTrue(Label + TEXT("First acknowledged journal made read-only"), PlatformFile.SetReadOnly(*Journals[0], true)) ||
			!TestTrue(Label + TEXT("Second journal retry permissions set"), PlatformFile.SetReadOnly(*Journals[1], bFailSharedPersist))) return false;
		TArray<uint8> FirstAcknowledged, SecondAcknowledged;
		TestTrue(Label + TEXT("Acknowledged first journal bytes captured"), FFileHelper::LoadFileToArray(FirstAcknowledged, *Journals[0]));
		if (bFailSharedPersist) TestTrue(Label + TEXT("Acknowledged second journal bytes captured"), FFileHelper::LoadFileToArray(SecondAcknowledged, *Journals[1]));
		const auto Retried = LocalJobs->Control(Ack);
		TestTrue(Label + TEXT("Same acknowledgement succeeds after retry"), GetBool(Retried, TEXT("success")));
		TestFalse(Label + TEXT("Recovery releases only after shared persistence"), GetBool(Retried, TEXT("recovery_required")));
		TestTrue(Label + TEXT("Successful acknowledgement has timestamp"), Retried->HasField(TEXT("recovery_acknowledged_at")));
		TArray<uint8> FirstAfter, SecondAfter;
		TestTrue(Label + TEXT("Retry never rewrites the first acknowledged journal"), FFileHelper::LoadFileToArray(FirstAfter, *Journals[0]) && FirstAfter == FirstAcknowledged);
		if (bFailSharedPersist) TestTrue(Label + TEXT("Retry never rewrites the second acknowledged journal"), FFileHelper::LoadFileToArray(SecondAfter, *Journals[1]) && SecondAfter == SecondAcknowledged);
		TestEqual(Label + TEXT("Both domains finish acknowledged"), JournalState(1), FString(TEXT("acknowledged")));
		VerifyFiles(TEXT("After retry"));
		Reload();
		const auto Finished = LocalJobs->Status(Selector);
		TestTrue(Label + TEXT("Successful durable job remains readable after reload"), GetBool(Finished, TEXT("success")));
		TestFalse(Label + TEXT("Successful durable job remains released after reload"), GetBool(Finished, TEXT("recovery_required")));
		TestTrue(Label + TEXT("Durable acknowledgement timestamp survives reload"), Finished->HasField(TEXT("recovery_acknowledged_at")));
		TestTrue(Label + TEXT("Exact and ancestor reservations release after durable retry"), LocalJobs->CheckReservations({Finals[0], Finals[1], Options.OutputRoot}, ErrorText));
		VerifyFiles(TEXT("After final local reload"));
		return true;
	};
	const bool bDomain = RunScenario(false);
	const bool bShared = RunScenario(true);
	return bDomain && bShared;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterRecoveryScanPathsTest,
	"FBX_Exporter.Editor.Application.RecoveryScanPathNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterRecoveryScanPathsTest::RunTest(const FString&)
{
	using namespace UnrealToolsRuntime;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Name = TEXT("UnrealTools/V12/RecoveryScan/") + Id;
	const FString Parent = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/RecoveryScan"));
	const FString Root = Parent / Id, RecoveryRoot = Root / TEXT("Recovery");
	const FString JournalPath = RecoveryRoot / TEXT("owned.json"), OrphanPath = RecoveryRoot / TEXT("orphan.json"), LatePath = RecoveryRoot / TEXT("late.json");
	FString RelativeRoot = RecoveryRoot;
	if (!TestTrue(TEXT("Recovery scan root can use the Editor's relative path form"), FPaths::MakePathRelativeTo(RelativeRoot, FPlatformProcess::BaseDir()))) return false;
	TestTrue(TEXT("Filesystem scan receives a relative saved metadata root"), FPaths::IsRelative(RelativeRoot));
	FFBXExporterOptions Options; Options.OutputRoot = Root / TEXT("Outputs"); Options.TextureMaps = EFBXExporterTextureMap::None;
	FProfileStore Profiles(TEXT("fbx_scan_test"), Name);
	TUniquePtr<FJobService> LocalJobs;
	ON_SCOPE_EXIT { LocalJobs.Reset(); if (FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Root), Parent)) IFileManager::Get().DeleteDirectory(*Root, false, true); };
	auto Reload = [&]()
	{
		LocalJobs.Reset();
		LocalJobs = MakeUnique<FJobService>(TEXT("fbx_scan_test"), Name, Profiles, FBXExporter::OptionsToJson(Options), &FBXExporter::NormalizeJobOptions,
			[](const FItemSpec& Spec) { return TArray<FString>{FBXExporter::PlannedDirectory(Spec.MeshPath, FBXExporter::OptionsFromJson(Spec.Options))}; },
			[&LatePath](const FItemSpec& Spec, const FString&) -> TUniquePtr<IItemRunner>
			{ return MakeUnique<FRecoveryCheckpointRunner>(LatePath, FBXExporter::PlannedDirectory(Spec.MeshPath, FBXExporter::OptionsFromJson(Spec.Options))); });
	};
	Reload();
	auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString());
	Request->SetArrayField(TEXT("mesh_paths"), Strings({TEXT("/Game/RecoveryScan.RecoveryScan")}));
	const auto Accepted = LocalJobs->Start(Request);
	if (!TestTrue(TEXT("Independent metadata-only scan fixture accepted"), GetBool(Accepted, TEXT("success")))) return false;
	const FString JobId = GetString(Accepted, TEXT("job_id")), JobPath = Root / TEXT("Jobs") / (JobId + TEXT(".json"));
	LocalJobs.Reset();
	FString Text, ErrorText;
	if (!TestTrue(TEXT("Owned terminal job metadata readable"), FFileHelper::LoadFileToString(Text, *JobPath))) return false;
	const auto Job = Parse(Text);
	if (!TestTrue(TEXT("Owned terminal job metadata parses"), Job.IsValid())) return false;
	const auto Item = GetArray(Job, TEXT("items"))[0]->AsObject();
	auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("recovery_journal"), JournalPath);
	TestFalse(TEXT("Durable job journal reference is absolute"), FPaths::IsRelative(GetString(Result, TEXT("recovery_journal"))));
	Item->SetObjectField(TEXT("result"), Result); Item->SetBoolField(TEXT("commit_pending"), true); Job->SetBoolField(TEXT("recovery_required"), true);
	if (!TestTrue(TEXT("Recovery job checkpoint persisted"), AtomicJson(JobPath, Job, ErrorText))) return false;
	auto Journal = MakeShared<FJsonObject>(); Journal->SetNumberField(TEXT("version"), 1); Journal->SetStringField(TEXT("state"), TEXT("publishing"));
	Journal->SetStringField(TEXT("provider_id"), TEXT("fbx_exporter")); Journal->SetStringField(TEXT("job_id"), JobId);
	Journal->SetStringField(TEXT("original"), Root / TEXT("Outputs/RecoveryScan")); Journal->SetStringField(TEXT("staging"), Root / TEXT("Stage")); Journal->SetStringField(TEXT("backup"), Root / TEXT("Stage_Previous"));
	Journal->SetArrayField(TEXT("original_files"), {}); Journal->SetArrayField(TEXT("staged_files"), {});
	if (!TestTrue(TEXT("Absolute owned publishing journal persisted"), AtomicJson(JournalPath, Journal, ErrorText))) return false;
	TArray<uint8> Before, After;
	TestTrue(TEXT("Owned journal bytes captured"), FFileHelper::LoadFileToArray(Before, *JournalPath));
	Reload();
	FBXExporter::Testing::ScanRecoveryDiagnostics(*LocalJobs, RelativeRoot, [&]()
	{
		auto LateRequest = MakeShared<FJsonObject>(); LateRequest->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString());
		LateRequest->SetArrayField(TEXT("mesh_paths"), Strings({TEXT("/Game/RecoveryScanLate.RecoveryScanLate")}));
		const auto LateJob = LocalJobs->Start(LateRequest);
		if (!TestTrue(TEXT("A new job can arrive after the scan's owner pages"), GetBool(LateJob, TEXT("success")))) return;
		LocalJobs->AdvanceOneStepForTesting();
		auto LateJournal = Clone(Journal); LateJournal->SetStringField(TEXT("job_id"), GetString(LateJob, TEXT("job_id")));
		LateJournal->SetStringField(TEXT("original"), Root / TEXT("Outputs/RecoveryScanLate")); LateJournal->SetStringField(TEXT("staging"), Root / TEXT("LateStage")); LateJournal->SetStringField(TEXT("backup"), Root / TEXT("LateStage_Previous"));
		TestTrue(TEXT("Late journal is created after its durable precommit reference"), AtomicJson(LatePath, LateJournal, ErrorText));
	});
	TestTrue(TEXT("Relative scan and absolute job reference retain the same owner"), LocalJobs->GetRecordIssues().IsEmpty());
	TestTrue(TEXT("Orphan judgement rechecks a job created after owner collection"), IFileManager::Get().FileExists(*LatePath) && LocalJobs->GetRecordIssues().IsEmpty());
	TestTrue(TEXT("Owner scan never changes domain bytes"), FFileHelper::LoadFileToArray(After, *JournalPath) && Before == After);
	TestFalse(TEXT("Owner scan retains the interrupted output reservation"), LocalJobs->CheckReservations({Root / TEXT("Outputs")}, ErrorText));
	if (!TestTrue(TEXT("Genuine unrelated publishing orphan persisted"), AtomicJson(OrphanPath, Journal, ErrorText))) return false;
	FBXExporter::Testing::ScanRecoveryDiagnostics(*LocalJobs, RelativeRoot);
	const auto Issues = LocalJobs->GetRecordIssues();
	TestEqual(TEXT("Normalization still diagnoses exactly the genuine orphan"), Issues.Num(), 1);
	if (!Issues.IsEmpty()) TestEqual(TEXT("Orphan diagnostic points to its absolute source only"), CanonicalPath(GetString(Issues[0], TEXT("source_path"))), CanonicalPath(OrphanPath));
	TestTrue(TEXT("Genuine orphan keeps provider admission blocked"), !LocalJobs->CheckReservations({Root / TEXT("Unrelated")}, ErrorText));
	return true;
}
#endif
