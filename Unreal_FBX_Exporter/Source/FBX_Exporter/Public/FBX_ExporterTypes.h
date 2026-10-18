#pragma once

#include "CoreMinimal.h"

enum class EFBXExporterTextureMap : uint8
{
	None = 0,
	BaseColor = 1 << 0,
	Normal = 1 << 1,
	Mask = 1 << 2,
	Occlusion = 1 << 3,
	Emission = 1 << 4,
	Opacity = 1 << 5,
	All = 0x3F
};

ENUM_CLASS_FLAGS(EFBXExporterTextureMap)

inline bool IsFBXExporterTextureMapEnabled(const EFBXExporterTextureMap Value, const EFBXExporterTextureMap Map)
{
	return (static_cast<uint8>(Value) & static_cast<uint8>(Map)) != 0;
}

struct FBX_EXPORTER_API FFBXExporterOptions
{
	FString OutputRoot;
	int32 BakeResolution = 2048;
	bool bReuseDirectTextures = true;
	EFBXExporterTextureMap TextureMaps = EFBXExporterTextureMap::All;
	bool bEmbedTexturesInFbx = false;
	// "mesh_name" preserves the original layout; "asset_path" preserves mounts.
	FString OutputLayout = TEXT("mesh_name");
};

struct FBX_EXPORTER_API FFBXExporterResult
{
	bool bSuccess = false;
	FString FinalDirectory;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	FString RecoveryJournal;
	FString RecoveryStaging;
	FString RecoveryBackup;

	bool Succeeded() const
	{
		return bSuccess && Errors.IsEmpty();
	}
};
