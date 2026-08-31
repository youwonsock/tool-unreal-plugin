#pragma once

#include "CoreMinimal.h"

struct FFBXExporterTextureFileData
{
	FString MapName;
	FString RelativePath;
	/** Absolute path inside the staging tree. Used only as a callback lookup key. */
	FString StagedAbsolutePath;
	FString FinalAbsolutePath;
	FString ColorSpace;
	TArray<uint8> FileBytes;
};

struct FFBXExporterSectionData
{
	int32 SlotIndex = INDEX_NONE;
	int32 TriangleCount = 0;
	bool bHasMaterial = false;
};

struct FFBXExporterSlotData
{
	int32 SlotIndex = INDEX_NONE;
	FString SlotName;
	FString MaterialPath;
	FString BlendMode;
	FString BaseColorAlpha;
	float EmissionScale = 1.0f;
	bool bHasMaterial = false;
	TMap<FString, FString> MapRelativePaths;
};

struct FFBXExporterMaterialBinding
{
	int32 FbxMaterialIndex = INDEX_NONE;
	TArray<int32> SourceSlotIndices;
	FString MaterialPath;
	FString BlendMode;
	FString BaseColorAlpha;
	float EmissionScale = 1.0f;
	TMap<FString, FString> MapRelativePaths;
};

namespace FBXExporter
{
	bool PostProcessFbx(
		const FString& StagedFbxPath,
		const FString& FinalFbxPath,
		const TArray<FFBXExporterSectionData>& Sections,
		const TArray<FFBXExporterSlotData>& Slots,
		const TArray<FFBXExporterTextureFileData>& TextureFiles,
		bool bEmbedTextures,
		TArray<FFBXExporterMaterialBinding>& OutMaterialBindings,
		FString& OutError);
}
