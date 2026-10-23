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

FBatchPlan InspectBatch(const UnrealToolsRuntime::FObject& Request);
UnrealToolsRuntime::FObject Submit(const UnrealToolsRuntime::FObject& OriginalRequest);
UnrealToolsRuntime::FObject InspectItemJson(const FInspectedItem& Item);
}
