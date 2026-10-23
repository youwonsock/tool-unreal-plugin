#include "FBX_ExporterApplication.h"
#include "Engine/StaticMesh.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"

namespace FBXExporter::Application
{
using namespace UnrealToolsRuntime;

FBatchPlan InspectBatch(const FObject& Request)
{
	FBatchPlan Out;
	FPreparedBatch Prepared;
	FString Message;
	Jobs().PrepareBatch(Request, Prepared, Message);
	for (int32 Index = 0; Index < Prepared.Specs.Num(); ++Index)
	{
		FInspectedItem& Item = Out.Items.AddDefaulted_GetRef();
		Item.Spec = Prepared.Specs[Index];
		if (!Prepared.ItemResolved.IsValidIndex(Index) || !Prepared.ItemResolved[Index])
		{
			Item.Plan.Errors.Add(Prepared.ItemErrors.IsValidIndex(Index) ? Prepared.ItemErrors[Index] : TEXT("Invalid target."));
			Item.ErrorCode = TEXT("invalid_request");
		}
		else
		{
			TStrongObjectPtr<UStaticMesh> Mesh(LoadObject<UStaticMesh>(nullptr, *Item.Spec.MeshPath));
			Item.Plan = PreflightStaticMesh(Mesh.Get(), OptionsFromJson(Item.Spec.Options));
			if (!Item.Plan.Errors.IsEmpty()) Item.ErrorCode = TEXT("preflight_failed");
			if (!Prepared.ItemErrors[Index].IsEmpty())
			{
				Item.Plan.Errors.Add(Prepared.ItemErrors[Index]);
				Item.ErrorCode = Prepared.ItemErrors[Index].Contains(TEXT("overlapping outputs")) ? TEXT("output_collision") : TEXT("invalid_request");
			}
			if (!Item.Plan.FinalDirectory.IsEmpty())
			{
				for (int32 Previous = 0; Previous < Index; ++Previous)
				{
					const FString& Other = Out.Items[Previous].Plan.FinalDirectory;
					if (!Other.IsEmpty() && PathsOverlap(Other, Item.Plan.FinalDirectory) && Item.ErrorCode != TEXT("output_collision"))
					{ Item.Plan.Errors.Add(TEXT("Output overlaps another target: ") + Other); Item.ErrorCode = TEXT("output_collision"); }
				}
				FString ReservedError;
				if (!Jobs().CheckReservations({Item.Plan.FinalDirectory}, ReservedError))
				{ Item.Plan.Errors.Add(ReservedError); Item.ErrorCode = TEXT("OUTPUT_CONFLICT"); }
			}
		}
		Item.Plan.bCanExport = Item.Plan.Errors.IsEmpty();
		if (!AreRecoveryDiagnosticsReady()) { Item.Plan.Errors.Add(TEXT("FBX recovery records are still being indexed.")); Item.Plan.bCanExport = false; Item.ErrorCode = TEXT("EXECUTION_BLOCKED"); }
		for (const FString& Error : Item.Plan.Errors) Out.Errors.Add(Item.Spec.MeshPath + TEXT(": ") + Error);
	}
	// Envelope/profile failures also matter when all per-row fields happen to parse.
	for (const FString& Error : Prepared.Errors)
		if (!Out.Errors.ContainsByPredicate([&Error](const FString& Existing) { return Existing.Contains(Error); })) Out.Errors.Add(Error);
	if (Out.Items.IsEmpty() && Out.Errors.IsEmpty()) Out.Errors.Add(Message.IsEmpty() ? TEXT("At least one mesh path is required.") : Message);
	return Out;
}

FObject Submit(const FObject& OriginalRequest) { return Jobs().Start(OriginalRequest); }

FObject InspectItemJson(const FInspectedItem& Item)
{
	const FFBXExporterOptions Options = Item.Plan.FinalDirectory.IsEmpty() && Item.ErrorCode == TEXT("invalid_request") ? FFBXExporterOptions() : OptionsFromJson(Item.Spec.Options);
	auto Data = OptionsToJson(Options);
	const auto& Plan = Item.Plan;
	Data->SetStringField(TEXT("mesh_path"), Item.Spec.MeshPath);
	Data->SetBoolField(TEXT("success"), Plan.bCanExport);
	Data->SetBoolField(TEXT("can_export"), Plan.bCanExport);
	Data->SetBoolField(TEXT("requires_overwrite"), Plan.bRequiresOverwrite);
	Data->SetStringField(TEXT("final_directory"), Plan.FinalDirectory);
	Data->SetStringField(TEXT("fbx_file"), Plan.FbxFile);
	Data->SetArrayField(TEXT("planned_files"), Strings(Plan.PlannedFiles));
	Data->SetArrayField(TEXT("replacement_files"), Strings(Plan.ReplacedFiles));
	Data->SetStringField(TEXT("overwrite_scope"), TEXT("entire_mesh_folder"));
	if (Plan.MetadataFile.IsEmpty()) Data->SetField(TEXT("metadata_file"), MakeShared<FJsonValueNull>());
	else Data->SetStringField(TEXT("metadata_file"), Plan.MetadataFile);
	Data->SetArrayField(TEXT("warnings"), Strings(Plan.Warnings));
	Data->SetArrayField(TEXT("errors"), Strings(Plan.Errors));
	if (!Plan.bCanExport)
	{
		Data->SetStringField(TEXT("error_code"), Item.ErrorCode);
		Data->SetStringField(TEXT("error"), Plan.Errors.IsEmpty() ? TEXT("Preflight failed.") : Plan.Errors[0]);
	}
	Data->SetArrayField(TEXT("selected_maps"), GetArray(Data, TEXT("maps")));
	Data->SetBoolField(TEXT("textures_embedded"), Options.bEmbedTexturesInFbx && Options.TextureMaps != EFBXExporterTextureMap::None);
	return Data;
}
}
