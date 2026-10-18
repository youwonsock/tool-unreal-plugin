#pragma once

#include "FBX_ExporterService.h"

namespace FBXExporter::Application
{
struct FInspectedItem
{
	UnrealToolsRuntime::FItemSpec Spec;
	FPreflightResult Plan;
	FString ErrorCode;
};
struct FBatchPlan
{
	TArray<FInspectedItem> Items;
	TArray<FString> Errors;
	bool CanExport() const { return !Items.IsEmpty() && Errors.IsEmpty(); }
};

// Adapter-only extraction. Option validation/defaults belong to PrepareBatch.
bool ReadFlatRequest(const UnrealToolsRuntime::FObject& Input, UnrealToolsRuntime::FItemSpec& Spec, FString& Error);
UnrealToolsRuntime::FObject SingleRequest(const UnrealToolsRuntime::FItemSpec& Spec);
FBatchPlan InspectBatch(const UnrealToolsRuntime::FObject& Request);
UnrealToolsRuntime::FObject Submit(const UnrealToolsRuntime::FObject& OriginalRequest);
UnrealToolsRuntime::FObject InspectItemJson(const FInspectedItem& Item);
}
