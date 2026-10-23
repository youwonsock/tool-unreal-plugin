#include "FBX_ExporterMcpTools.h"
#include "FBX_ExporterService.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "ModelContextProtocolSession.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	bool IsError(const FModelContextProtocolToolResult& Result)
	{
		bool bError = false;
		return Result.JsonObject.IsValid() && Result.JsonObject->TryGetBoolField(TEXT("isError"), bError) && bError;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterMcpContractTest, "FBX_Exporter.Editor.MCP.RegistrationAndInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterMcpContractTest::RunTest(const FString& Parameters)
{
	IModelContextProtocolModule& Module = IModelContextProtocolModule::GetChecked();
	const TCHAR* Names[] = {TEXT("fbx_exporter_status"), TEXT("fbx_exporter_find_meshes"), TEXT("fbx_exporter_preflight"), TEXT("fbx_exporter_profiles"), TEXT("fbx_exporter_job_start"), TEXT("fbx_exporter_job_status"), TEXT("fbx_exporter_job_control")};
	for (const TCHAR* Name : Names)
	{
		if (!TestTrue(FString::Printf(TEXT("%s is independently registered"), Name), Module.FindTool(Name).IsValid())) return false;
	}
	TestFalse(TEXT("Retired flat export tool is absent"), Module.FindTool(TEXT("fbx_exporter_export")).IsValid());
	const int32 ToolCount = Module.GetTools().Num();
	FBXExporter::Mcp::Register();
	TestEqual(TEXT("Repeated registration does not duplicate tools"), Module.GetTools().Num(), ToolCount);
	FBXExporter::Mcp::Unregister();
	TestEqual(TEXT("Unregister removes only this plugin's seven tools"), Module.GetTools().Num(), ToolCount - 7);
	FBXExporter::Mcp::Register();
	Module.RefreshTools();
	for (const TCHAR* Name : Names) TestTrue(TEXT("Tools survive an engine refresh"), Module.FindTool(Name).IsValid());
	const TSharedPtr<IModelContextProtocolTool> Preflight = Module.FindTool(TEXT("fbx_exporter_preflight"));
	if (!Preflight.IsValid()) return false;
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	TestTrue(TEXT("Missing mesh rejected"), IsError(Preflight->Run(Args)));
	Args->SetStringField(TEXT("mesh_path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestTrue(TEXT("Flat mesh_path requests are unsupported"), IsError(Preflight->Run(Args)));
	Args->RemoveField(TEXT("mesh_path"));
	Args->SetArrayField(TEXT("mesh_paths"), UnrealToolsRuntime::Strings({TEXT("/Engine/BasicShapes/Cube.Cube")}));
	auto Options = MakeShared<FJsonObject>(); Args->SetObjectField(TEXT("options"), Options);
	Options->SetNumberField(TEXT("resolution"), 512.5);
	TestTrue(TEXT("Fractional resolution rejected"), IsError(Preflight->Run(Args)));
	Options->SetStringField(TEXT("resolution"), TEXT("512"));
	TestTrue(TEXT("String resolution rejected"), IsError(Preflight->Run(Args)));
	Options->RemoveField(TEXT("resolution"));
	Options->SetStringField(TEXT("output_root"), TEXT("../elsewhere"));
	TestTrue(TEXT("Relative output root rejected"), IsError(Preflight->Run(Args)));
	Options->RemoveField(TEXT("output_root"));
	Options->SetArrayField(TEXT("maps"), UnrealToolsRuntime::Strings({TEXT("BaseColor"), TEXT("BaseColor")}));
	TestTrue(TEXT("Duplicate maps rejected"), IsError(Preflight->Run(Args)));
	Options->SetArrayField(TEXT("maps"), UnrealToolsRuntime::Strings({TEXT("Unknown")}));
	TestTrue(TEXT("Unknown maps rejected"), IsError(Preflight->Run(Args)));
	Options->SetArrayField(TEXT("maps"), {});
	Options->SetStringField(TEXT("embed_textures"), TEXT("true"));
	TestTrue(TEXT("String boolean rejected"), IsError(Preflight->Run(Args)));
	Options->RemoveField(TEXT("embed_textures"));
	Args->SetBoolField(TEXT("overwite"), true);
	TestTrue(TEXT("Misspelled arguments rejected"), IsError(Preflight->Run(Args)));
	TSharedRef<FJsonObject> Search = MakeShared<FJsonObject>();
	Search->SetStringField(TEXT("query"), TEXT("Cube"));
	Search->SetNumberField(TEXT("limit"), 1.5);
	TestTrue(TEXT("Fractional search limit rejected"), IsError(Module.FindTool(TEXT("fbx_exporter_find_meshes"))->Run(Search)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterMcpPortableExportTest, "FBX_Exporter.Editor.MCP.PortableGeometryExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterMcpPortableExportTest::RunTest(const FString& Parameters)
{
	if (FBXExporter::Jobs().IsBusy() || !FBXExporter::AreRecoveryDiagnosticsReady())
	{
		AddWarning(TEXT("Portable export requires an idle service with recovery indexing complete."));
		return true;
	}
	// Engine content is the only fixture dependency: this must also run in a
	// fresh project with only FBX_Exporter installed, without Haven or CubeShot.
	const TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	if (!TestTrue(TEXT("Engine cube available"), Cube.IsValid())) return false;
	IModelContextProtocolModule& Module = IModelContextProtocolModule::GetChecked();
	const TSharedPtr<IModelContextProtocolTool> Preflight = Module.FindTool(TEXT("fbx_exporter_preflight"));
	const TSharedPtr<IModelContextProtocolTool> Start = Module.FindTool(TEXT("fbx_exporter_job_start"));
	const TSharedPtr<IModelContextProtocolTool> Status = Module.FindTool(TEXT("fbx_exporter_job_status"));
	if (!TestTrue(TEXT("MCP entry points available"), Preflight.IsValid() && Start.IsValid() && Status.IsValid())) return false;
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_ExporterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetArrayField(TEXT("mesh_paths"), UnrealToolsRuntime::Strings({Cube->GetPathName()}));
	auto Options = MakeShared<FJsonObject>();
	Options->SetStringField(TEXT("output_root"), Root);
	Options->SetArrayField(TEXT("maps"), {});
	Options->SetBoolField(TEXT("embed_textures"), true);
	Args->SetObjectField(TEXT("options"), Options);
	const FModelContextProtocolToolResult Checked = Preflight->Run(Args);
	if (!TestFalse(TEXT("Geometry-only preflight succeeds even with null RHI"), IsError(Checked))) return false;
	TestFalse(TEXT("Preflight does not create output folders"), IFileManager::Get().DirectoryExists(*Root));
	TestFalse(TEXT("No maps means no embedded textures"), UnrealToolsRuntime::GetArray(Checked.JsonObject->GetObjectField(TEXT("structuredContent")), TEXT("items"))[0]->AsObject()->GetBoolField(TEXT("textures_embedded")));
	const FString Final = Root / TEXT("Cube");
	const FString Fbx = Final / TEXT("Cube.fbx");
	const FString Sentinel = Final / TEXT("user.txt");
	auto RunJob = [&]() {
		auto Request = UnrealToolsRuntime::Clone(Args);
		Request->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		auto Accepted = Start->Run(Request);
		if (IsError(Accepted)) return Accepted;
		const FString Id = UnrealToolsRuntime::GetString(Accepted.JsonObject->GetObjectField(TEXT("structuredContent")), TEXT("job_id"));
		for (int32 Step = 0; Step < 64 && FBXExporter::Jobs().IsBusy(); ++Step)
			FBXExporter::Jobs().AdvanceOneStepForTesting(true);
		auto Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("job_id"), Id);
		auto Reply = Status->Run(Selector);
		const auto Data = Reply.JsonObject->GetObjectField(TEXT("structuredContent"));
		Reply.JsonObject->SetBoolField(TEXT("isError"), UnrealToolsRuntime::GetString(Data, TEXT("state")) != TEXT("succeeded"));
		return Reply;
	};
	const FModelContextProtocolToolResult First = RunJob();
	TestFalse(TEXT("MCP executes the real export service"), IsError(First));
	TestTrue(TEXT("FBX file generated"), IFileManager::Get().FileSize(*Fbx) > 0);
	TestFalse(TEXT("Geometry-only creates no texture folder"), IFileManager::Get().DirectoryExists(*(Final / TEXT("Textures"))));
	TestFalse(TEXT("Geometry-only creates no metadata"), IFileManager::Get().FileExists(*(Final / TEXT("Cube_Materials.json"))));
	if (!IsError(First))
	{
		TestTrue(TEXT("Create user-file fixture"), FFileHelper::SaveStringToFile(TEXT("keep me"), *Sentinel));
		TArray<uint8> Before;
		FFileHelper::LoadFileToArray(Before, *Fbx);
		TestTrue(TEXT("Repeat export refuses overwrite without a dialog"), IsError(RunJob()));
		TArray<uint8> After;
		FFileHelper::LoadFileToArray(After, *Fbx);
		TestTrue(TEXT("Refusal preserves original FBX bytes"), Before == After);
		TestTrue(TEXT("Refusal preserves user file"), IFileManager::Get().FileExists(*Sentinel));
		const FModelContextProtocolToolResult Exists = Preflight->Run(Args);
		TestTrue(TEXT("Preflight reports overwrite requirement"), UnrealToolsRuntime::GetArray(Exists.JsonObject->GetObjectField(TEXT("structuredContent")), TEXT("items"))[0]->AsObject()->GetBoolField(TEXT("requires_overwrite")));
		Args->SetBoolField(TEXT("overwrite"), true);
		TestFalse(TEXT("Explicit overwrite succeeds"), IsError(RunJob()));
		TestFalse(TEXT("Explicit overwrite replaces the whole owned folder"), IFileManager::Get().FileExists(*Sentinel));
	}
	TestTrue(TEXT("Remove this test's unique output only"), IFileManager::Get().DeleteDirectory(*Root, false, true));
	return true;
}
namespace FBXPostedRequestTest
{
	struct FState { int32 Callbacks=0; FString ErrorCode; };
	class FGenerationCheck final : public IAutomationLatentCommand
	{
	public:
		explicit FGenerationCheck(FAutomationTestBase* InTest) : Test(InTest), State(MakeShared<FState>()) {}
		virtual bool Update() override
		{
			if (!bPosted)
			{
				const auto Tool=IModelContextProtocolModule::GetChecked().FindTool(TEXT("fbx_exporter_status"));
				if (!Tool) { Test->AddError(TEXT("Generation fixture tool missing.")); return true; }
				FBXExporter::Mcp::Testing::ForcePostRequests(true);
				Tool->RunAsync(FModelContextProtocolToolRequestId(),MakeShared<FJsonObject>(),[Shared=State](const FModelContextProtocolToolResult& Result)
				{
					++Shared->Callbacks; const auto Data=Result.JsonObject->GetObjectField(TEXT("structuredContent")); Data->TryGetStringField(TEXT("error_code"),Shared->ErrorCode);
				});
				FBXExporter::Mcp::Testing::ForcePostRequests(false);
				FBXExporter::Mcp::Unregister(); FBXExporter::Mcp::Register();
				bPosted=true; Started=FPlatformTime::Seconds();
				return false;
			}
			if (State->Callbacks==0 && FPlatformTime::Seconds()-Started<5) return false;
			Test->TestEqual(TEXT("Old pending request completes exactly once"),State->Callbacks,1);
			Test->TestEqual(TEXT("Re-registration cannot revive an old posted request"),State->ErrorCode,FString(TEXT("EDITOR_INTERRUPTED")));
			return true;
		}
	private:
		FAutomationTestBase* Test; TSharedPtr<FState> State; bool bPosted=false; double Started=0;
	};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterPostedGenerationTest,"FBX_Exporter.Editor.MCP.PendingRegistrationGeneration",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterPostedGenerationTest::RunTest(const FString&)
{ ADD_LATENT_AUTOMATION_COMMAND(FBXPostedRequestTest::FGenerationCheck(this)); return true; }
#endif
