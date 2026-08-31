#include "FBX_ExporterFbxMedia.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace FBXExporter
{
namespace
{
struct FEmbeddedFileContext
{
	TMap<FString, const TArray<uint8>*> Files;
};

FString NormalizePath(const FString& InPath)
{
	FString Result = InPath;
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
	Result.RemoveFromStart(TEXT("file:///"), ESearchCase::IgnoreCase);
	Result.RemoveFromStart(TEXT("file://"), ESearchCase::IgnoreCase);
	if (Result.Len() >= 3 && Result[0] == TEXT('/') && Result[2] == TEXT(':'))
	{
		Result.RemoveFromStart(TEXT("/"));
	}
	Result = FPaths::ConvertRelativePathToFull(Result);
	FPaths::NormalizeFilename(Result);
	return Result.ToLower();
}

FString NormalizeRelativePath(const FString& InPath)
{
	FString Result = InPath;
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
	Result.RemoveFromStart(TEXT("file:///"), ESearchCase::IgnoreCase);
	Result.RemoveFromStart(TEXT("file://"), ESearchCase::IgnoreCase);
	// Some FBX writers report a URI-style relative name.  A leading slash is
	// not part of the relative Textures/... key unless the path contains a
	// Windows drive prefix (which is handled by NormalizePath instead).
	if (Result.Len() >= 3 && Result[0] == TEXT('/') && Result[2] == TEXT(':'))
	{
		Result.RemoveFromStart(TEXT("/"));
	}
	else if (Result.StartsWith(TEXT("/")) && !FPaths::IsDrive(Result))
	{
		Result.RemoveFromStart(TEXT("/"));
	}
	while (Result.StartsWith(TEXT("./")))
	{
		Result.RightChopInline(2, EAllowShrinking::No);
	}
	return Result.ToLower();
}

FbxCallback::State WriteEmbeddedFile(
	void* UserData,
	FbxClassId DataHint,
	const char* FileName,
	const void** FileBuffer,
	size_t* SizeInBytes)
{
	(void)DataHint;
	if (!UserData || !FileName || !FileBuffer || !SizeInBytes)
	{
		return FbxCallback::eNotHandled;
	}

	FEmbeddedFileContext* Context = static_cast<FEmbeddedFileContext*>(UserData);
	const FString RequestedPath = UTF8_TO_TCHAR(FileName);
	const TArray<uint8>* const* Bytes = Context->Files.Find(NormalizePath(RequestedPath));
	if (!Bytes)
	{
		Bytes = Context->Files.Find(NormalizeRelativePath(RequestedPath));
	}
	if (!Bytes || !*Bytes || (*Bytes)->IsEmpty())
	{
		return FbxCallback::eFailure;
	}

	*FileBuffer = (*Bytes)->GetData();
	*SizeInBytes = static_cast<size_t>((*Bytes)->Num());
	return FbxCallback::eHandled;
}

FbxNode* FindMeshNode(FbxNode* Node)
{
	if (!Node)
	{
		return nullptr;
	}
	if (Node->GetMesh())
	{
		return Node;
	}
	for (int32 ChildIndex = 0; ChildIndex < Node->GetChildCount(); ++ChildIndex)
	{
		if (FbxNode* Result = FindMeshNode(Node->GetChild(ChildIndex)))
		{
			return Result;
		}
	}
	return nullptr;
}

FString FindUv0SetName(const FbxMesh* Mesh)
{
	// Unreal's native FBX exporter names the first UV channel UVmap_0, except
	// when that channel is also the light-map channel (LightMapUV).  Resolve
	// the name from the imported mesh so texture links remain valid in both
	// cases and in files whose importer preserved a custom first-set name.
	FString FirstSetName;
	if (Mesh)
	{
		FbxStringList UvSetNames;
		Mesh->GetUVSetNames(UvSetNames);
		for (int32 Index = 0; Index < UvSetNames.GetCount(); ++Index)
		{
			const FString SetName = UTF8_TO_TCHAR(UvSetNames.GetStringAt(Index));
			if (FirstSetName.IsEmpty())
			{
				FirstSetName = SetName;
			}
			if (SetName.Equals(TEXT("UVmap_0"), ESearchCase::IgnoreCase))
			{
				return SetName;
			}
		}
		for (int32 Index = 0; Index < UvSetNames.GetCount(); ++Index)
		{
			const FString SetName = UTF8_TO_TCHAR(UvSetNames.GetStringAt(Index));
			if (SetName.Equals(TEXT("LightMapUV"), ESearchCase::IgnoreCase))
			{
				return SetName;
			}
		}
	}
	return FirstSetName.IsEmpty() ? TEXT("UVmap_0") : FirstSetName;
}

bool GetPolygonMaterialIndex(const FbxLayerElementMaterial* Element, const int32 PolygonIndex, int32& OutMaterialIndex)
{
	if (!Element)
	{
		OutMaterialIndex = 0;
		return true;
	}

	const FbxLayerElement::EMappingMode MappingMode = Element->GetMappingMode();
	const FbxLayerElement::EReferenceMode ReferenceMode = Element->GetReferenceMode();
	if (ReferenceMode == FbxLayerElement::eDirect)
	{
		return false;
	}
	const FbxLayerElementArrayTemplate<int>& IndexArray = Element->GetIndexArray();
	if (MappingMode == FbxLayerElement::eAllSame)
	{
		if (IndexArray.GetCount() <= 0)
		{
			OutMaterialIndex = 0;
			return true;
		}
		if (IndexArray.GetCount() != 1)
		{
			return false;
		}
		OutMaterialIndex = IndexArray[0];
		return true;
	}
	if (MappingMode == FbxLayerElement::eByPolygon)
	{
		if (PolygonIndex < 0 || PolygonIndex >= IndexArray.GetCount())
		{
			return false;
		}
		OutMaterialIndex = IndexArray[PolygonIndex];
		return true;
	}
	// Material elements produced by the Unreal exporter are eByPolygon. Treating
	// other mappings as unsupported avoids silently assigning a wrong material.
	return false;
}

bool SetStringProperty(FbxObject* Object, const FString& Name, const FString& Value)
{
	if (!Object)
	{
		return false;
	}
	FbxProperty Property = Object->FindProperty(TCHAR_TO_UTF8(*Name));
	if (!Property.IsValid())
	{
		Property = FbxProperty::Create(Object, FbxStringDT, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*Name));
	}
	if (!Property.IsValid())
	{
		return false;
	}
	Property.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
	return Property.Set(FbxString(TCHAR_TO_UTF8(*Value)));
}

bool SetNumberProperty(FbxObject* Object, const FString& Name, const double Value)
{
	if (!Object)
	{
		return false;
	}
	FbxProperty Property = Object->FindProperty(TCHAR_TO_UTF8(*Name));
	if (!Property.IsValid())
	{
		Property = FbxProperty::Create(Object, FbxDoubleDT, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*Name));
	}
	if (!Property.IsValid())
	{
		return false;
	}
	Property.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
	return Property.Set(Value);
}

bool ConnectTextureProperty(FbxSurfaceMaterial* Material, FbxFileTexture* Texture, const char* PropertyName)
{
	if (!Material || !Texture)
	{
		return false;
	}
	const FbxProperty Property = Material->FindProperty(PropertyName);
	return Property.IsValid() && Texture->ConnectDstProperty(Property);
}

int32 Utf8Length(const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	return Utf8.Length();
}

FString MakeUniqueExportDirectory(
	const FString& ParentDirectory,
	const FString& FinalDirectory,
	FString& OutError)
{
	const FString FinalLeaf = FPaths::GetCleanFilename(FinalDirectory);
	const int32 RequiredUtf8Bytes = Utf8Length(FinalLeaf);
	if (FinalLeaf.IsEmpty() || RequiredUtf8Bytes <= 0)
	{
		OutError = FString::Printf(TEXT("Could not derive the final FBX directory name from %s."), *FinalDirectory);
		return FString();
	}

	// The FBX SDK rewrites FileName/RelativeFileName relative to the directory
	// containing the output file.  Export into a sibling whose UTF-8 byte length
	// matches the final directory leaf, then replace that byte sequence in the
	// serialized FBX.  Equal byte lengths let us do this without changing any
	// FBX string offsets or binary layout.
	const FString Seed = TEXT("FBXTmp_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	for (int32 Attempt = 0; Attempt < 256; ++Attempt)
	{
		FString CandidateLeaf;
		if (RequiredUtf8Bytes <= 1)
		{
			CandidateLeaf = TEXT("T");
		}
		else
		{
			CandidateLeaf = Seed;
			if (CandidateLeaf.Len() < RequiredUtf8Bytes)
			{
				CandidateLeaf.Append(FString::ChrN(RequiredUtf8Bytes - CandidateLeaf.Len(), TEXT('T')));
			}
			else
			{
				CandidateLeaf.LeftInline(RequiredUtf8Bytes, EAllowShrinking::No);
			}
		}

		// Preserve the requested byte length while making retries deterministic.
		if (Attempt > 0 && !CandidateLeaf.IsEmpty())
		{
			const TCHAR Suffix = TEXT('A') + static_cast<TCHAR>(Attempt % 26);
			CandidateLeaf[CandidateLeaf.Len() - 1] = Suffix;
		}
		if (Utf8Length(CandidateLeaf) != RequiredUtf8Bytes || CandidateLeaf.Equals(FinalLeaf, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString CandidateDirectory = FPaths::Combine(ParentDirectory, CandidateLeaf);
		if (!IFileManager::Get().DirectoryExists(*CandidateDirectory) &&
			!IFileManager::Get().FileExists(*CandidateDirectory))
		{
			return CandidateDirectory;
		}
	}

	OutError = FString::Printf(TEXT("Could not allocate a unique temporary FBX directory beside %s."), *FinalDirectory);
	return FString();
}

int32 ReplaceUtf8Bytes(TArray<uint8>& Bytes, const FString& From, const FString& To)
{
	FTCHARToUTF8 FromUtf8(*From);
	FTCHARToUTF8 ToUtf8(*To);
	if (FromUtf8.Length() <= 0 || FromUtf8.Length() != ToUtf8.Length())
	{
		return INDEX_NONE;
	}

	const int32 PatternLength = FromUtf8.Length();
	int32 ReplacementCount = 0;
	for (int32 Offset = 0; Offset + PatternLength <= Bytes.Num(); ++Offset)
	{
		if (FMemory::Memcmp(Bytes.GetData() + Offset, FromUtf8.Get(), PatternLength) == 0)
		{
			FMemory::Memcpy(Bytes.GetData() + Offset, ToUtf8.Get(), PatternLength);
			++ReplacementCount;
			Offset += PatternLength - 1;
		}
	}
	return ReplacementCount;
}

bool ContainsUtf8Bytes(const TArray<uint8>& Bytes, const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	if (Utf8.Length() <= 0 || Utf8.Length() > Bytes.Num())
	{
		return false;
	}

	for (int32 Offset = 0; Offset + Utf8.Length() <= Bytes.Num(); ++Offset)
	{
		if (FMemory::Memcmp(Bytes.GetData() + Offset, Utf8.Get(), Utf8.Length()) == 0)
		{
			return true;
		}
	}
	return false;
}

bool PatchExportDirectory(
	TArray<uint8>& SerializedFbx,
	const FString& TemporaryDirectory,
	const FString& FinalDirectory,
	FString& OutError)
{
	const FString TemporaryFull = FPaths::ConvertRelativePathToFull(TemporaryDirectory);
	const FString FinalFull = FPaths::ConvertRelativePathToFull(FinalDirectory);
	if (Utf8Length(TemporaryFull) != Utf8Length(FinalFull))
	{
		OutError = TEXT("Temporary and final FBX directory paths have different UTF-8 lengths.");
		return false;
	}

	TArray<TPair<FString, FString>> PathPairs;
	PathPairs.Add(TPair<FString, FString>(TemporaryFull, FinalFull));
	FString TemporaryForward = TemporaryFull;
	FString FinalForward = FinalFull;
	TemporaryForward.ReplaceInline(TEXT("\\"), TEXT("/"));
	FinalForward.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!TemporaryForward.Equals(TemporaryFull, ESearchCase::CaseSensitive) ||
		!FinalForward.Equals(FinalFull, ESearchCase::CaseSensitive))
	{
		PathPairs.Add(TPair<FString, FString>(TemporaryForward, FinalForward));
	}
	FString TemporaryBackslash = TemporaryFull;
	FString FinalBackslash = FinalFull;
	TemporaryBackslash.ReplaceInline(TEXT("/"), TEXT("\\"));
	FinalBackslash.ReplaceInline(TEXT("/"), TEXT("\\"));
	if (!TemporaryBackslash.Equals(TemporaryFull, ESearchCase::CaseSensitive) ||
		!FinalBackslash.Equals(FinalFull, ESearchCase::CaseSensitive))
	{
		PathPairs.Add(TPair<FString, FString>(TemporaryBackslash, FinalBackslash));
	}

	for (const TPair<FString, FString>& Pair : PathPairs)
	{
		const int32 Count = ReplaceUtf8Bytes(SerializedFbx, Pair.Key, Pair.Value);
		if (Count == INDEX_NONE)
		{
			OutError = TEXT("Could not patch temporary FBX directory paths in the serialized file.");
			return false;
		}
		if (ContainsUtf8Bytes(SerializedFbx, Pair.Key))
		{
			OutError = FString::Printf(TEXT("Temporary FBX directory path remained after patching: %s"), *Pair.Key);
			return false;
		}
	}

	// A geometry-only scene may not contain an absolute URL at all, so zero
	// replacements is valid.  Any paths that were emitted by the SDK are patched
	// above; all replacements are byte-preserving by construction.
	return true;
}

bool AddTexturePair(
	FbxScene* Scene,
	FbxSurfaceMaterial* Material,
	const FString& MaterialName,
	const FFBXExporterTextureFileData& FileData,
	const FString& Uv0SetName,
	const FString& ExportDirectory,
	const bool bEmbed,
	FEmbeddedFileContext& EmbeddedContext,
	FString& OutError)
{
	if (!Scene || !Material || FileData.FileBytes.IsEmpty())
	{
		OutError = TEXT("Invalid FBX texture data.");
		return false;
	}

	const FString SafeMapName = FileData.MapName.IsEmpty() ? TEXT("Map") : FileData.MapName;
	FString NormalizedRelativePath = FileData.RelativePath;
	NormalizedRelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (NormalizedRelativePath.StartsWith(TEXT("Textures/"), ESearchCase::IgnoreCase))
	{
		NormalizedRelativePath = TEXT("Textures/") + NormalizedRelativePath.Mid(9);
	}
	TArray<FString> RelativePathParts;
	NormalizedRelativePath.ParseIntoArray(RelativePathParts, TEXT("/"), false);
	bool bHasUnsafePathPart = false;
	for (const FString& Part : RelativePathParts)
	{
		if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT(".."))
		{
			bHasUnsafePathPart = true;
			break;
		}
	}
	if (NormalizedRelativePath.IsEmpty() ||
		!NormalizedRelativePath.StartsWith(TEXT("Textures/"), ESearchCase::IgnoreCase) ||
		RelativePathParts.Num() < 2 ||
		bHasUnsafePathPart ||
		NormalizedRelativePath.Contains(TEXT(":")) ||
		NormalizedRelativePath.StartsWith(TEXT("/")) ||
		FileData.FinalAbsolutePath.IsEmpty())
	{
		OutError = FString::Printf(TEXT("FBX texture path must be relative to Textures/: %s"), *FileData.RelativePath);
		return false;
	}
	const FString ObjectName = MaterialName + TEXT("_") + SafeMapName;
	FbxFileTexture* Texture = FbxFileTexture::Create(Scene, TCHAR_TO_UTF8(*ObjectName));
	FbxVideo* Video = FbxVideo::Create(Scene, TCHAR_TO_UTF8(*(ObjectName + TEXT("_Video"))));
	if (!Texture || !Video)
	{
		OutError = FString::Printf(TEXT("Failed to create FBX media objects for %s."), *FileData.RelativePath);
		return false;
	}

	const FString ExportAbsolutePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ExportDirectory, NormalizedRelativePath));
	const FString FinalAbsolutePath = FPaths::ConvertRelativePathToFull(FileData.FinalAbsolutePath);
	if (!Texture->SetFileName(TCHAR_TO_UTF8(*ExportAbsolutePath)) ||
		!Texture->SetRelativeFileName(TCHAR_TO_UTF8(*NormalizedRelativePath)) ||
		!Video->SetFileName(TCHAR_TO_UTF8(*ExportAbsolutePath)) ||
		!Video->SetRelativeFileName(TCHAR_TO_UTF8(*NormalizedRelativePath)))
	{
		OutError = FString::Printf(TEXT("Failed to set FBX media paths for %s."), *FileData.RelativePath);
		return false;
	}
	Texture->SetTextureUse(FbxTexture::eStandard);
	Texture->SetMaterialUse(FbxFileTexture::eModelMaterial);
	Texture->SetMappingType(FbxTexture::eUV);
	Texture->UVSet.Set(FbxString(TCHAR_TO_UTF8(*Uv0SetName)));
	Texture->SetAlphaSource(
		(FileData.MapName == TEXT("BaseColor") || FileData.MapName == TEXT("Opacity") || FileData.MapName == TEXT("OpacityMask"))
			? FbxTexture::eBlack
			: FbxTexture::eNone);

	Video->SetFileName(TCHAR_TO_UTF8(*ExportAbsolutePath));
	Video->SetRelativeFileName(TCHAR_TO_UTF8(*NormalizedRelativePath));
	Texture->SetMediaName(TCHAR_TO_UTF8(*NormalizedRelativePath));
	if (!Texture->ConnectSrcObject(Video))
	{
		OutError = FString::Printf(TEXT("Failed to connect FBX media objects for %s."), *FileData.RelativePath);
		return false;
	}
	// Create(Scene, ...) already places objects in the scene's object graph on
	// current SDK builds. Add* is intentionally idempotent here so the same code
	// also works with SDK builds where the container connection is deferred.
	Scene->AddVideo(Video);
	Scene->AddTexture(Texture);
	if (!bEmbed)
	{
		// The scene still owns the pair, but no callback is registered and the
		// normal exporter writes an external relative file reference.
	}
	else
	{
		EmbeddedContext.Files.Add(NormalizePath(FinalAbsolutePath), &FileData.FileBytes);
		EmbeddedContext.Files.Add(NormalizePath(ExportAbsolutePath), &FileData.FileBytes);
		if (!FileData.StagedAbsolutePath.IsEmpty())
		{
			EmbeddedContext.Files.Add(NormalizePath(FileData.StagedAbsolutePath), &FileData.FileBytes);
		}
		EmbeddedContext.Files.Add(NormalizeRelativePath(NormalizedRelativePath), &FileData.FileBytes);
		EmbeddedContext.Files.Add(NormalizeRelativePath(FPaths::GetCleanFilename(NormalizedRelativePath)), &FileData.FileBytes);
	}

	bool bStandardConnected = true;
	if (FileData.MapName == TEXT("BaseColor"))
	{
		bStandardConnected = ConnectTextureProperty(Material, Texture, FbxSurfaceMaterial::sDiffuse);
	}
	else if (FileData.MapName == TEXT("Normal"))
	{
		bStandardConnected = ConnectTextureProperty(Material, Texture, FbxSurfaceMaterial::sNormalMap);
	}
	else if (FileData.MapName == TEXT("Emission"))
	{
		bStandardConnected = ConnectTextureProperty(Material, Texture, FbxSurfaceMaterial::sEmissive);
	}
	else if (FileData.MapName == TEXT("Opacity"))
	{
		bStandardConnected = ConnectTextureProperty(Material, Texture, FbxSurfaceMaterial::sTransparentColor);
	}
	else if (FileData.MapName == TEXT("OpacityMask"))
	{
		bStandardConnected = ConnectTextureProperty(Material, Texture, FbxSurfaceMaterial::sTransparencyFactor);
	}
	if (!bStandardConnected)
	{
		OutError = FString::Printf(TEXT("Failed to connect %s texture to the FBX material standard property."), *FileData.MapName);
		return false;
	}

	const FString CustomPropertyName = TEXT("UE_PBR_") + FileData.MapName;
	FbxProperty CustomProperty = Material->FindProperty(TCHAR_TO_UTF8(*CustomPropertyName));
	if (!CustomProperty.IsValid())
	{
		// A color/vector data type is required for a texture connection. The
		// property itself carries the map semantics through its UE_PBR_* name.
		CustomProperty = FbxProperty::Create(Material, FbxDouble3DT, TCHAR_TO_UTF8(*CustomPropertyName), TCHAR_TO_UTF8(*CustomPropertyName));
		if (CustomProperty.IsValid())
		{
			CustomProperty.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
		}
	}
	if (!CustomProperty.IsValid() || !Texture->ConnectDstProperty(CustomProperty))
	{
		OutError = FString::Printf(TEXT("Failed to connect %s texture to UE_PBR_%s."), *FileData.RelativePath, *FileData.MapName);
		return false;
	}
	return true;
}

bool BuildMaterialBindings(
	FbxNode* MeshNode,
	const TArray<FFBXExporterSectionData>& Sections,
	const TArray<FFBXExporterSlotData>& Slots,
	TArray<FFBXExporterMaterialBinding>& OutBindings,
	FString& OutError)
{
	FbxMesh* Mesh = MeshNode ? MeshNode->GetMesh() : nullptr;
	if (!Mesh)
	{
		OutError = TEXT("The exported FBX does not contain a mesh node.");
		return false;
	}
	int32 ExpectedPolygonCount = 0;
	for (const FFBXExporterSectionData& Section : Sections)
	{
		ExpectedPolygonCount += Section.TriangleCount;
	}
	if (Mesh->GetPolygonCount() != ExpectedPolygonCount)
	{
		OutError = FString::Printf(TEXT("FBX polygon count (%d) does not match LOD0 section triangles (%d)."), Mesh->GetPolygonCount(), ExpectedPolygonCount);
		return false;
	}
	if (MeshNode->GetMaterialCount() == 0)
	{
		for (const FFBXExporterSectionData& Section : Sections)
		{
			if (Section.bHasMaterial && Section.TriangleCount > 0)
			{
				OutError = TEXT("LOD0 contains an assigned material but the FBX has no material objects.");
				return false;
			}
		}
		return true;
	}

	const FbxLayerElementMaterial* MaterialElement = nullptr;
	for (int32 LayerIndex = 0; LayerIndex < Mesh->GetLayerCount(); ++LayerIndex)
	{
		if (FbxLayer* Layer = Mesh->GetLayer(LayerIndex))
		{
			MaterialElement = Layer->GetMaterials();
			if (MaterialElement)
			{
				break;
			}
		}
	}
	if (MaterialElement && MaterialElement->GetMappingMode() == FbxLayerElement::eByPolygon &&
		MaterialElement->GetIndexArray().GetCount() != Mesh->GetPolygonCount())
	{
		OutError = TEXT("FBX polygon material mapping does not contain exactly one index per polygon.");
		return false;
	}
	TMap<int32, TArray<int32>> FbxToSlots;
	TMap<int32, int32> FbxNonEmptySlot;
	TMap<int32, int32> SlotToFbx;
	int32 PolygonOffset = 0;
	for (const FFBXExporterSectionData& Section : Sections)
	{
		if (Section.SlotIndex < 0 || Section.SlotIndex >= Slots.Num())
		{
			OutError = FString::Printf(TEXT("LOD0 section references invalid material slot %d."), Section.SlotIndex);
			return false;
		}
		if (Section.TriangleCount < 0)
		{
			OutError = FString::Printf(TEXT("LOD0 section %d has a negative triangle count."), Section.SlotIndex);
			return false;
		}
		if (Section.TriangleCount == 0)
		{
			continue;
		}
		int32 SectionMaterial = INDEX_NONE;
		for (int32 PolygonIndex = 0; PolygonIndex < Section.TriangleCount; ++PolygonIndex)
		{
			int32 PolygonMaterial = 0;
			if (!GetPolygonMaterialIndex(MaterialElement, PolygonOffset + PolygonIndex, PolygonMaterial))
			{
				OutError = FString::Printf(TEXT("Unsupported or malformed FBX material mapping at polygon %d."), PolygonOffset + PolygonIndex);
				return false;
			}
			if (SectionMaterial == INDEX_NONE)
			{
				SectionMaterial = PolygonMaterial;
			}
			else if (SectionMaterial != PolygonMaterial)
			{
				OutError = FString::Printf(TEXT("LOD0 section %d spans more than one FBX material index."), Section.SlotIndex);
				return false;
			}
		}
		PolygonOffset += Section.TriangleCount;
		if (SectionMaterial == INDEX_NONE)
		{
			SectionMaterial = 0;
		}
		if (SectionMaterial < 0 || SectionMaterial >= MeshNode->GetMaterialCount())
		{
			OutError = FString::Printf(TEXT("FBX material index %d is out of range."), SectionMaterial);
			return false;
		}

		const bool bNonEmptySlot = Section.bHasMaterial;
		if (!bNonEmptySlot && MeshNode->GetMaterial(SectionMaterial) &&
			!FString(UTF8_TO_TCHAR(MeshNode->GetMaterial(SectionMaterial)->GetName())).Contains(TEXT("default"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Empty source material slot %d is assigned to non-default FBX material %d."), Section.SlotIndex, SectionMaterial);
			return false;
		}
		if (const int32* ExistingFbx = SlotToFbx.Find(Section.SlotIndex))
		{
			if (*ExistingFbx != SectionMaterial)
			{
				OutError = FString::Printf(TEXT("Source material slot %d maps to multiple FBX material indices (%d and %d)."), Section.SlotIndex, *ExistingFbx, SectionMaterial);
				return false;
			}
		}
		else
		{
			SlotToFbx.Add(Section.SlotIndex, SectionMaterial);
		}
		TArray<int32>& SourceSlots = FbxToSlots.FindOrAdd(SectionMaterial);
		const bool bAlreadyContains = SourceSlots.Contains(Section.SlotIndex);
		if (!bAlreadyContains)
		{
			if (bNonEmptySlot)
			{
				if (const int32* ExistingNonEmptySlot = FbxNonEmptySlot.Find(SectionMaterial))
				{
					OutError = FString::Printf(TEXT("FBX material %d represents multiple non-empty source slots (%d and %d)."), SectionMaterial, *ExistingNonEmptySlot, Section.SlotIndex);
					return false;
				}
				FbxNonEmptySlot.Add(SectionMaterial, Section.SlotIndex);
			}
			SourceSlots.Add(Section.SlotIndex);
		}
	}

	for (const TPair<int32, TArray<int32>>& Pair : FbxToSlots)
	{
		// A material index may be shared by repeated sections of the same source
		// slot.  Sharing across distinct slots is only valid for the exporter’s
		// default material when every represented source slot is empty.
		if (FbxNonEmptySlot.Contains(Pair.Key) && Pair.Value.Num() > 1)
		{
			OutError = FString::Printf(TEXT("FBX material %d is shared by a non-empty source slot and another source slot."), Pair.Key);
			return false;
		}
	}

	OutBindings.Reset();
	for (const TPair<int32, TArray<int32>>& Pair : FbxToSlots)
	{
		FFBXExporterMaterialBinding Binding;
		Binding.FbxMaterialIndex = Pair.Key;
		Binding.SourceSlotIndices = Pair.Value;
		Binding.SourceSlotIndices.Sort();
		int32 PrimarySlotIndex = Binding.SourceSlotIndices[0];
		for (const int32 SlotIndex : Binding.SourceSlotIndices)
		{
			if (Slots.IsValidIndex(SlotIndex) && Slots[SlotIndex].bHasMaterial)
			{
				PrimarySlotIndex = SlotIndex;
				break;
			}
		}
		const FFBXExporterSlotData& Slot = Slots[PrimarySlotIndex];
		Binding.MaterialPath = Slot.MaterialPath;
		Binding.BlendMode = Slot.BlendMode;
		Binding.BaseColorAlpha = Slot.BaseColorAlpha;
		Binding.EmissionScale = Slot.EmissionScale;
		Binding.MapRelativePaths = Slot.MapRelativePaths;
		OutBindings.Add(MoveTemp(Binding));
	}
	OutBindings.Sort([](const FFBXExporterMaterialBinding& A, const FFBXExporterMaterialBinding& B)
	{
		return A.FbxMaterialIndex < B.FbxMaterialIndex;
	});
	return true;
}

void RemoveSceneMedia(FbxScene* Scene)
{
	while (Scene && Scene->GetSrcObjectCount<FbxTexture>() > 0)
	{
		if (FbxTexture* Texture = Scene->GetSrcObject<FbxTexture>(0))
		{
			Texture->Destroy();
		}
		else
		{
			break;
		}
	}
	while (Scene && Scene->GetSrcObjectCount<FbxVideo>() > 0)
	{
		if (FbxVideo* Video = Scene->GetSrcObject<FbxVideo>(0))
		{
			Video->Destroy();
		}
		else
		{
			break;
		}
	}
}
}

bool PostProcessFbx(
	const FString& StagedFbxPath,
	const FString& FinalFbxPath,
	const TArray<FFBXExporterSectionData>& Sections,
	const TArray<FFBXExporterSlotData>& Slots,
	const TArray<FFBXExporterTextureFileData>& TextureFiles,
	const bool bEmbedTextures,
	TArray<FFBXExporterMaterialBinding>& OutMaterialBindings,
	FString& OutError)
{
	OutError.Reset();
	OutMaterialBindings.Reset();
	if (!IFileManager::Get().FileExists(*StagedFbxPath))
	{
		OutError = FString::Printf(TEXT("Staged FBX does not exist: %s"), *StagedFbxPath);
		return false;
	}

	FString TemporaryDirectory;
	const auto CleanupTemporaryDirectory = [&]() -> bool
	{
		if (TemporaryDirectory.IsEmpty())
		{
			return true;
		}
		const bool bDeleted = IFileManager::Get().DeleteDirectory(*TemporaryDirectory, false, true);
		if (bDeleted && !IFileManager::Get().DirectoryExists(*TemporaryDirectory))
		{
			TemporaryDirectory.Reset();
			return true;
		}
		return false;
	};
	const auto CleanupTemporaryDirectoryAndReport = [&]()
	{
		if (!CleanupTemporaryDirectory() && !TemporaryDirectory.IsEmpty())
		{
			if (!OutError.IsEmpty())
			{
				OutError += TEXT(" ");
			}
			OutError += FString::Printf(TEXT("Temporary FBX directory was retained: %s"), *TemporaryDirectory);
		}
	};

	FbxManager* Manager = FbxManager::Create();
	if (!Manager)
	{
		OutError = TEXT("FBX SDK manager creation failed.");
		return false;
	}
	FbxIOSettings* IOSettings = FbxIOSettings::Create(Manager, IOSROOT);
	if (!IOSettings)
	{
		OutError = TEXT("FBX SDK IO settings creation failed.");
		Manager->Destroy();
		return false;
	}
	Manager->SetIOSettings(IOSettings);
	IOSettings->SetBoolProp(IMP_FBX_MATERIAL, true);
	IOSettings->SetBoolProp(IMP_FBX_TEXTURE, true);
	IOSettings->SetBoolProp(IMP_FBX_LINK, true);
	IOSettings->SetBoolProp(EXP_FBX_MATERIAL, true);
	IOSettings->SetBoolProp(EXP_FBX_TEXTURE, true);
	IOSettings->SetBoolProp(EXP_FBX_EMBEDDED, bEmbedTextures);
	IOSettings->SetBoolProp(EXP_ASCIIFBX, false);
	FbxScene* Scene = FbxScene::Create(Manager, "FBXExporterScene");
	FbxImporter* Importer = FbxImporter::Create(Manager, "FBXExporterImporter");
	if (!Scene || !Importer || !Importer->Initialize(TCHAR_TO_UTF8(*StagedFbxPath), -1, IOSettings) || !Importer->Import(Scene))
	{
		OutError = Importer ? UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString()) : TEXT("FBX import failed.");
		if (OutError.IsEmpty())
		{
			OutError = TEXT("FBX SDK could not import the staged FBX.");
		}
		if (Importer)
		{
			Importer->Destroy();
		}
		Manager->Destroy();
		return false;
	}
	Importer->Destroy();
	if (FbxDocumentInfo* SceneInfo = Scene->GetSceneInfo())
	{
		const FbxString FinalUrl(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(FinalFbxPath)));
		SceneInfo->Original_FileName.Set(FinalUrl);
		SceneInfo->LastSavedUrl.Set(FinalUrl);
		SceneInfo->Url.Set(FinalUrl);
		SceneInfo->EmbeddedUrl.Set(FbxString(""));
	}

	FbxNode* MeshNode = FindMeshNode(Scene->GetRootNode());
	if (!BuildMaterialBindings(MeshNode, Sections, Slots, OutMaterialBindings, OutError))
	{
		Manager->Destroy();
		return false;
	}

	const FString FinalDirectory = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FinalFbxPath));
	const FString ParentDirectory = FPaths::GetPath(FinalDirectory);
	TemporaryDirectory = MakeUniqueExportDirectory(ParentDirectory, FinalDirectory, OutError);
	if (TemporaryDirectory.IsEmpty() || !IFileManager::Get().MakeDirectory(*TemporaryDirectory, true))
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Could not create a temporary FBX directory beside %s."), *FinalDirectory);
		}
		CleanupTemporaryDirectoryAndReport();
		Manager->Destroy();
		return false;
	}

	RemoveSceneMedia(Scene);
	if (Scene->GetSrcObjectCount<FbxTexture>() > 0 || Scene->GetSrcObjectCount<FbxVideo>() > 0)
	{
		OutError = TEXT("The staged FBX retained source texture or video media after cleanup.");
		CleanupTemporaryDirectoryAndReport();
		Manager->Destroy();
		return false;
	}
	const FString Uv0SetName = FindUv0SetName(MeshNode->GetMesh());
	FEmbeddedFileContext EmbeddedContext;
	for (FFBXExporterMaterialBinding& Binding : OutMaterialBindings)
	{
		FbxSurfaceMaterial* Material = MeshNode->GetMaterial(Binding.FbxMaterialIndex);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("FBX material %d is missing."), Binding.FbxMaterialIndex);
			CleanupTemporaryDirectoryAndReport();
			Manager->Destroy();
			return false;
		}
		if (!SetStringProperty(Material, TEXT("UE_PBR_OpacityConvention"), TEXT("1=Opaque")) ||
			!SetStringProperty(Material, TEXT("UE_PBR_MaskLayout"), TEXT("R=Metallic;G=AO;B=Reserved;A=Smoothness(1-Roughness)")) ||
			!SetStringProperty(Material, TEXT("UE_PBR_BlendMode"), Binding.BlendMode) ||
			!SetStringProperty(Material, TEXT("UE_PBR_BaseColorAlpha"), Binding.BaseColorAlpha) ||
			!SetNumberProperty(Material, TEXT("UE_PBR_EmissionScale"), Binding.EmissionScale))
		{
			OutError = FString::Printf(TEXT("Failed to write UE_PBR metadata for FBX material %d."), Binding.FbxMaterialIndex);
			CleanupTemporaryDirectoryAndReport();
			Manager->Destroy();
			return false;
		}
		if (FbxSurfaceLambert* Lambert = FbxCast<FbxSurfaceLambert>(Material))
		{
			Lambert->EmissiveFactor.Set(Binding.EmissionScale);
		}

		TArray<FString> SortedMapNames;
		Binding.MapRelativePaths.GetKeys(SortedMapNames);
		SortedMapNames.Sort();
		for (const FString& MapName : SortedMapNames)
		{
			const FString& RelativePath = Binding.MapRelativePaths[MapName];
			const FFBXExporterTextureFileData* FileData = TextureFiles.FindByPredicate([&MapName, &RelativePath](const FFBXExporterTextureFileData& Candidate)
			{
				return Candidate.MapName == MapName && Candidate.RelativePath == RelativePath;
			});
			if (!FileData)
			{
				OutError = FString::Printf(TEXT("Missing staged texture data for %s."), *RelativePath);
				CleanupTemporaryDirectoryAndReport();
				Manager->Destroy();
				return false;
			}
			if (!AddTexturePair(Scene, Material, FString::Printf(TEXT("Material_%d"), Binding.FbxMaterialIndex), *FileData, Uv0SetName, TemporaryDirectory, bEmbedTextures, EmbeddedContext, OutError))
			{
				CleanupTemporaryDirectoryAndReport();
				Manager->Destroy();
				return false;
			}
		}
	}

	const FString PostprocessedPath = StagedFbxPath + TEXT(".postprocessed.fbx");
	IFileManager::Get().Delete(*PostprocessedPath, false, true, true);
	FbxExporter* Exporter = FbxExporter::Create(Manager, "FBXExporterExporter");
	const int32 WriterId = Manager->GetIOPluginRegistry()->FindWriterIDByExtension("fbx");
	if (!Exporter || WriterId < 0)
	{
		OutError = Exporter ? UTF8_TO_TCHAR(Exporter->GetStatus().GetErrorString()) : TEXT("FBX exporter creation failed.");
		if (Exporter)
		{
			Exporter->Destroy();
		}
		CleanupTemporaryDirectoryAndReport();
		Manager->Destroy();
		return false;
	}
	if (!Exporter->SetFileExportVersion(FBX_2013_00_COMPATIBLE, FbxSceneRenamer::eNone))
	{
		OutError = TEXT("FBX SDK could not select the FBX 2013 file format.");
		Exporter->Destroy();
		CleanupTemporaryDirectoryAndReport();
		Manager->Destroy();
		return false;
	}
	const FString TemporaryOutputPath = FPaths::Combine(TemporaryDirectory, FPaths::GetCleanFilename(FinalFbxPath));
	if (!Exporter->Initialize(TCHAR_TO_UTF8(*TemporaryOutputPath), WriterId, IOSettings))
	{
		OutError = UTF8_TO_TCHAR(Exporter->GetStatus().GetErrorString());
		if (OutError.IsEmpty())
		{
			OutError = TEXT("FBX exporter initialization failed.");
		}
		Exporter->Destroy();
		CleanupTemporaryDirectoryAndReport();
		Manager->Destroy();
		return false;
	}
	if (bEmbedTextures)
	{
		FbxEmbeddedFileCallback* Callback = FbxEmbeddedFileCallback::Create(Manager, "FBXExporterEmbeddedFileCallback");
		if (!Callback || Callback->RegisterWriteFunction(WriteEmbeddedFile, &EmbeddedContext) < 0)
		{
			OutError = TEXT("Could not register the FBX embedded-media callback.");
			if (Callback)
			{
				Callback->Destroy();
			}
			Exporter->Destroy();
			CleanupTemporaryDirectoryAndReport();
			Manager->Destroy();
			return false;
		}
		Exporter->SetEmbeddedFileWriteCallback(Callback);
	}
	const bool bExported = Exporter->Export(Scene);
	if (!bExported)
	{
		OutError = UTF8_TO_TCHAR(Exporter->GetStatus().GetErrorString());
		if (OutError.IsEmpty())
		{
			OutError = TEXT("FBX SDK post-processing export failed.");
		}
		Exporter->Destroy();
		Manager->Destroy();
		CleanupTemporaryDirectoryAndReport();
		return false;
	}
	Exporter->Destroy();
	Manager->Destroy();

	TArray<uint8> SerializedFbx;
	if (!FFileHelper::LoadFileToArray(SerializedFbx, *TemporaryOutputPath))
	{
		OutError = FString::Printf(TEXT("Failed to read the temporary post-processed FBX: %s"), *TemporaryOutputPath);
		CleanupTemporaryDirectoryAndReport();
		return false;
	}
	if (!PatchExportDirectory(SerializedFbx, TemporaryDirectory, FinalDirectory, OutError))
	{
		CleanupTemporaryDirectoryAndReport();
		return false;
	}
	if (!FFileHelper::SaveArrayToFile(SerializedFbx, *PostprocessedPath))
	{
		OutError = FString::Printf(TEXT("Failed to stage the patched post-processed FBX: %s"), *PostprocessedPath);
		CleanupTemporaryDirectoryAndReport();
		return false;
	}
	if (!CleanupTemporaryDirectory())
	{
		OutError = FString::Printf(TEXT("Temporary FBX directory was retained: %s"), *TemporaryDirectory);
		IFileManager::Get().Delete(*PostprocessedPath, false, true, true);
		return false;
	}

	if (!IFileManager::Get().FileExists(*PostprocessedPath) ||
		!IFileManager::Get().Move(*StagedFbxPath, *PostprocessedPath, true, true))
	{
		OutError = FString::Printf(TEXT("Failed to replace the staged FBX with the post-processed file: %s"), *StagedFbxPath);
		return false;
	}
	return true;
}
}
