#include "FBX_ExporterService.h"
#include "FBX_ExporterFbxMedia.h"

#include "AssetExportTask.h"
#include "Async/Async.h"
#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "ContentBrowserMenuContexts.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "ImageUtils.h"
#include "IMaterialBakingModule.h"
#include "MaterialBakingStructures.h"
#include "MaterialExpressionIO.h"
#include "MaterialShared.h"
#include "MaterialUtilities.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialParameters.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentAdapter.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"

#define LOCTEXT_NAMESPACE "FBX_Exporter"

namespace FBXExporter
{
namespace
{
	FString PublishFailurePoint;
	bool WriteJournal(const FString& Path, const TSharedRef<FJsonObject>& Data)
	{
		FString Error;
		return UnrealToolsRuntime::AtomicJson(Path, Data, Error);
	}

	TSharedPtr<FJsonObject> ReadJournal(const FString& Path)
	{
		FString Text; TSharedPtr<FJsonObject> Data;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Data)) return nullptr;
		return Data;
	}

	struct FBakedMaterialData
	{
		TMap<FString, TArray<FColor>> MapPixels;
		TArray<FString> Warnings;
		float EmissionScale = 1.0f;
	};


	FString GetMapFileName(const FString& MaterialName, const FString& MapName)
	{
		return FString::Printf(TEXT("%s_%s.png"), *MakeSafeFileName(MaterialName), *MapName);
	}

	const TArray<FString>& GetTextureMapNames()
	{
		static const TArray<FString> Names = {
			TEXT("BaseColor"), TEXT("Normal"), TEXT("Mask"), TEXT("Occlusion"), TEXT("Emission"), TEXT("Opacity")};
		return Names;
	}

	FString GetOpacityMapName(const UMaterialInterface* Material)
	{
		return Material && Material->GetBlendMode() == BLEND_Masked ? TEXT("OpacityMask") : TEXT("Opacity");
	}

	FString GetBlendModeName(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return TEXT("Opaque");
		}
		switch (Material->GetBlendMode())
		{
		case BLEND_Masked: return TEXT("Masked");
		case BLEND_Translucent: return TEXT("Translucent");
		case BLEND_Additive: return TEXT("Additive");
		case BLEND_Modulate: return TEXT("Modulate");
		case BLEND_AlphaComposite: return TEXT("AlphaComposite");
		case BLEND_AlphaHoldout: return TEXT("AlphaHoldout");
		default: return TEXT("Opaque");
		}
	}

	EFBXExporterTextureMap GetRequiredBakeMaps(const EFBXExporterTextureMap SelectedMaps)
	{
		EFBXExporterTextureMap Required = EFBXExporterTextureMap::None;
		if (IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::BaseColor))
		{
			Required |= EFBXExporterTextureMap::BaseColor;
			// BaseColor always keeps alpha semantics, even when standalone opacity
			// output was not selected.
			Required |= EFBXExporterTextureMap::Opacity;
		}
		if (IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Opacity))
		{
			Required |= EFBXExporterTextureMap::Opacity;
		}
		if (IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Mask))
		{
			Required |= EFBXExporterTextureMap::Mask;
			Required |= EFBXExporterTextureMap::Occlusion;
		}
		if (IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Occlusion))
		{
			Required |= EFBXExporterTextureMap::Occlusion;
		}
		if (IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Normal))
		{
			Required |= EFBXExporterTextureMap::Normal;
		}
		if (IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Emission))
		{
			Required |= EFBXExporterTextureMap::Emission;
		}
		return Required;
	}

	bool IsMapSelected(const EFBXExporterTextureMap SelectedMaps, const FString& MapName)
	{
		if (MapName == TEXT("BaseColor")) return IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::BaseColor);
		if (MapName == TEXT("Normal")) return IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Normal);
		if (MapName == TEXT("Mask")) return IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Mask);
		if (MapName == TEXT("Occlusion")) return IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Occlusion);
		if (MapName == TEXT("Emission")) return IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Emission);
		return MapName == TEXT("Opacity") || MapName == TEXT("OpacityMask")
			? IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Opacity)
			: false;
	}

	bool SaveImage(const FString& FilePath, const TArray<FColor>& Pixels, const int32 Width, const int32 Height, const bool bSRGB, FString& OutError)
	{
		if (Pixels.Num() != Width * Height || Width <= 0 || Height <= 0)
		{
			OutError = FString::Printf(TEXT("Invalid image data for %s."), *FilePath);
			return false;
		}

		const FImageView ImageView(Pixels.GetData(), Width, Height, bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
		if (!FImageUtils::SaveImageByExtension(*FilePath, ImageView))
		{
			OutError = FString::Printf(TEXT("Failed to save image: %s"), *FilePath);
			return false;
		}

		return true;
	}

	bool ResizePixels(
		const TArray<FColor>& SourcePixels,
		const int32 SourceWidth,
		const int32 SourceHeight,
		const bool bSRGB,
		const int32 TargetWidth,
		const int32 TargetHeight,
		TArray<FColor>& OutPixels)
	{
		if (SourcePixels.Num() == 1)
		{
			OutPixels.Init(SourcePixels[0], TargetWidth * TargetHeight);
			return true;
		}

		if (SourceWidth <= 0 || SourceHeight <= 0 || SourcePixels.Num() != SourceWidth * SourceHeight)
		{
			return false;
		}

		if (SourceWidth == TargetWidth && SourceHeight == TargetHeight)
		{
			OutPixels = SourcePixels;
			return true;
		}

		FImage SourceImage(SourceWidth, SourceHeight, ERawImageFormat::BGRA8, bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
		FMemory::Memcpy(SourceImage.RawData.GetData(), SourcePixels.GetData(), SourcePixels.Num() * sizeof(FColor));

		FImage ResizedImage(TargetWidth, TargetHeight, ERawImageFormat::BGRA8, bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
		SourceImage.ResizeTo(
			ResizedImage,
			TargetWidth,
			TargetHeight,
			ERawImageFormat::BGRA8,
			bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);

		const TArrayView64<const FColor> ResizedView = ResizedImage.AsBGRA8();
		OutPixels.Append(ResizedView.GetData(), static_cast<int32>(ResizedView.Num()));
		return true;
	}

	bool LoadTextureLinearPixels(
		UTexture2D* Texture,
		const int32 TargetWidth,
		const int32 TargetHeight,
		const bool bSourceSRGB,
		TArray<FLinearColor>& OutPixels)
	{
		if (!Texture)
		{
			return false;
		}

		FImage SourceImage;
		if (!FImageUtils::GetTexture2DSourceImage(Texture, SourceImage))
		{
			return false;
		}

		// Texture sampling uses the texture asset's SRGB flag, not merely the
		// gamma metadata stored in the source file. Decode to linear first so the
		// selected channel and any resize have the same meaning as the material
		// sample. FImage formats without gamma support ignore this field.
		SourceImage.GammaSpace = bSourceSRGB
			? (Texture->bUseLegacyGamma ? EGammaSpace::Pow22 : EGammaSpace::sRGB)
			: EGammaSpace::Linear;
		FImage ConvertedImage;
		SourceImage.CopyTo(ConvertedImage, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		if (ConvertedImage.SizeX != TargetWidth || ConvertedImage.SizeY != TargetHeight)
		{
			FImage ResizedImage;
			ConvertedImage.ResizeTo(ResizedImage, TargetWidth, TargetHeight, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
			ConvertedImage = MoveTemp(ResizedImage);
		}
		const TArrayView64<const FLinearColor> View = ConvertedImage.AsRGBA32F();
		OutPixels.Reset();
		OutPixels.Append(View.GetData(), static_cast<int32>(View.Num()));
		return true;
	}

	enum class EDirectTextureChannel : uint8
	{
		RGB,
		RGBA,
		R,
		G,
		B,
		A
	};

	struct FDirectTextureBinding
	{
		UTexture2D* Texture = nullptr;
		EDirectTextureChannel Channel = EDirectTextureChannel::RGB;
	};

	bool LoadDirectTextureLinearPixels(
		const FDirectTextureBinding& Binding,
		const int32 TargetWidth,
		const int32 TargetHeight,
		TArray<FLinearColor>& OutPixels)
	{
		if (!Binding.Texture)
		{
			return false;
		}
		TArray<FLinearColor> SourcePixels;
		// Decode according to the source texture's SRGB flag, then encode the
		// generated PNG in its destination color space below.
		if (!LoadTextureLinearPixels(Binding.Texture, TargetWidth, TargetHeight, Binding.Texture->SRGB, SourcePixels))
		{
			return false;
		}
		if (Binding.Channel == EDirectTextureChannel::RGB || Binding.Channel == EDirectTextureChannel::RGBA)
		{
			OutPixels = MoveTemp(SourcePixels);
			return true;
		}
		for (FLinearColor& Pixel : SourcePixels)
		{
			float Value = Pixel.R;
			switch (Binding.Channel)
			{
			case EDirectTextureChannel::G: Value = Pixel.G; break;
			case EDirectTextureChannel::B: Value = Pixel.B; break;
			case EDirectTextureChannel::A: Value = Pixel.A; break;
			default: break;
			}
			Pixel = FLinearColor(Value, Value, Value, 1.0f);
		}
		OutPixels = MoveTemp(SourcePixels);
		return true;
	}

	// The only float -> 8-bit conversion on the direct-texture path.
	bool LoadDirectTexturePixels(const FDirectTextureBinding& Binding, int32 Width, int32 Height,
		bool bSRGB, TArray<FColor>& OutPixels, float* OutEmissionScale = nullptr)
	{
		TArray<FLinearColor> Linear;
		if (!LoadDirectTextureLinearPixels(Binding, Width, Height, Linear)) return false;
		float Scale = 1.0f;
		if (OutEmissionScale)
		{
			for (const FLinearColor& Pixel : Linear)
				Scale = FMath::Max(Scale, FMath::Max3(Pixel.R, Pixel.G, Pixel.B));
			*OutEmissionScale = Scale;
		}
		OutPixels.Reset(Linear.Num());
		for (const FLinearColor& Pixel : Linear)
		{
			const FLinearColor Normalized(Pixel.R / Scale, Pixel.G / Scale, Pixel.B / Scale, Pixel.A);
			OutPixels.Add(Normalized.ToFColor(bSRGB));
		}
		return true;
	}

	bool GetBakedPropertyPixels(
		const FBakeOutput& BakeOutput,
		const EMaterialProperty Property,
		const int32 TargetWidth,
		const int32 TargetHeight,
		const FColor DefaultColor,
		TArray<FColor>& OutPixels)
	{
		const TArray<FColor>* SourcePixels = BakeOutput.PropertyData.Find(Property);
		if (!SourcePixels || SourcePixels->IsEmpty())
		{
			OutPixels.Init(DefaultColor, TargetWidth * TargetHeight);
			return true;
		}

		const FIntPoint* SourceSize = BakeOutput.PropertySizes.Find(Property);
		if (!SourceSize)
		{
			if (SourcePixels->Num() == 1)
			{
				OutPixels.Init((*SourcePixels)[0], TargetWidth * TargetHeight);
				return true;
			}
			return false;
		}

		return ResizePixels(
			*SourcePixels,
			SourceSize->X,
			SourceSize->Y,
			BakeOutput.PropertyIsLinearColor.FindRef(Property) ? false : true,
			TargetWidth,
			TargetHeight,
			OutPixels);
	}

	bool IsTextureAdjustmentDefault(const UTexture2D* Texture)
	{
		if (!Texture)
		{
			return false;
		}
		return FMath::IsNearlyEqual(Texture->AdjustBrightness, 1.0f) &&
			FMath::IsNearlyEqual(Texture->AdjustBrightnessCurve, 1.0f) &&
			FMath::IsNearlyZero(Texture->AdjustVibrance) &&
			FMath::IsNearlyEqual(Texture->AdjustSaturation, 1.0f) &&
			FMath::IsNearlyEqual(Texture->AdjustRGBCurve, 1.0f) &&
			FMath::IsNearlyZero(Texture->AdjustHue) &&
			FMath::IsNearlyZero(Texture->AdjustMinAlpha) &&
			FMath::IsNearlyEqual(Texture->AdjustMaxAlpha, 1.0f) &&
			!Texture->bFlipGreenChannel &&
			!Texture->bChromaKeyTexture &&
			Texture->PowerOfTwoMode == ETexturePowerOfTwoSetting::None;
	}

	EDirectTextureChannel GetDirectTextureChannel(const FExpressionInput& Input, const bool bColorProperty, bool& bValid)
	{
		bValid = true;
		const int32 ExplicitMask = Input.MaskR + Input.MaskG + Input.MaskB + Input.MaskA;
		if (Input.Mask)
		{
			if (ExplicitMask == 0)
			{
				bValid = false;
				return EDirectTextureChannel::R;
			}
			// TextureSample's RGB/RGBA outputs carry a multi-channel mask.  They
			// are valid for color properties; scalar properties must select exactly
			// one channel so that a vector-to-scalar implicit conversion cannot
			// silently change the exported value.
			const bool bRGB = Input.MaskR && Input.MaskG && Input.MaskB && !Input.MaskA;
			const bool bRGBA = Input.MaskR && Input.MaskG && Input.MaskB && Input.MaskA;
			if (bColorProperty && bRGB)
			{
				return EDirectTextureChannel::RGB;
			}
			if (bColorProperty && bRGBA)
			{
				return EDirectTextureChannel::RGBA;
			}
			if (!bColorProperty && ExplicitMask != 1)
			{
				bValid = false;
				return EDirectTextureChannel::R;
			}
			if (bColorProperty && ExplicitMask > 1)
			{
				bValid = false;
				return EDirectTextureChannel::RGB;
			}
		}
		if (Input.MaskA || Input.OutputIndex == 4)
		{
			return EDirectTextureChannel::A;
		}
		if (Input.MaskG || Input.OutputIndex == 2)
		{
			return EDirectTextureChannel::G;
		}
		if (Input.MaskB || Input.OutputIndex == 3)
		{
			return EDirectTextureChannel::B;
		}
		if (Input.MaskR || Input.OutputIndex == 1 || !bColorProperty)
		{
			if (!bColorProperty && !Input.Mask && Input.OutputIndex != 1 && Input.OutputIndex != 2 && Input.OutputIndex != 3 && Input.OutputIndex != 4)
			{
				bValid = false;
			}
			return EDirectTextureChannel::R;
		}
		if (Input.OutputIndex == 5)
		{
			return EDirectTextureChannel::RGBA;
		}
		if (Input.OutputIndex != 0)
		{
			bValid = false;
		}
		return EDirectTextureChannel::RGB;
	}

	bool FindDirectTexture(UMaterialInterface* Material, const EMaterialProperty Property, FDirectTextureBinding& OutBinding)
	{
		OutBinding = FDirectTextureBinding();
		if (!Material || !Material->IsPropertyActive(Property))
		{
			return false;
		}
		UMaterial* BaseMaterial = Material->GetMaterial();
		if (!BaseMaterial)
		{
			return false;
		}
		FExpressionInput* PropertyInput = BaseMaterial->GetExpressionInputForProperty(Property);
		if (!PropertyInput)
		{
			return false;
		}
		const FExpressionInput TracedInput = PropertyInput->GetTracedInput();
		UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(TracedInput.Expression);
		if (!TextureSample || TextureSample->TextureObject.Expression ||
			TextureSample->MipValue.Expression || TextureSample->CoordinatesDX.Expression || TextureSample->CoordinatesDY.Expression ||
			TextureSample->AutomaticViewMipBiasValue.Expression ||
			TextureSample->AutomaticViewMipBias || TextureSample->GatherMode != TGM_None ||
			TextureSample->MipValueMode != TMVM_None || TextureSample->SamplerSource != SSM_FromTextureAsset ||
			TextureSample->ConstCoordinate != 0)
		{
			return false;
		}
		const FExpressionInput CoordinateInput = TextureSample->Coordinates.GetTracedInput();
		if (CoordinateInput.Mask || CoordinateInput.OutputIndex != 0)
		{
			return false;
		}
		if (CoordinateInput.Expression)
		{
			const UMaterialExpressionTextureCoordinate* TextureCoordinate = Cast<UMaterialExpressionTextureCoordinate>(CoordinateInput.Expression);
			if (!TextureCoordinate || TextureCoordinate->CoordinateIndex != 0 ||
				!FMath::IsNearlyEqual(TextureCoordinate->UTiling, 1.0f) ||
				!FMath::IsNearlyEqual(TextureCoordinate->VTiling, 1.0f) ||
				TextureCoordinate->UnMirrorU || TextureCoordinate->UnMirrorV)
			{
				return false;
			}
		}
		UTexture2D* Texture = Cast<UTexture2D>(TextureSample->Texture);
		if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(TextureSample))
		{
			UTexture* ParameterTexture = nullptr;
			if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Parameter->ParameterName), ParameterTexture) || !ParameterTexture)
			{
				// A material instance may leave a parameter at the base material's
				// default.  In that case the expression's Texture field is still the
				// exact sampled source and remains eligible for direct reuse.
				ParameterTexture = TextureSample->Texture;
			}
			Texture = Cast<UTexture2D>(ParameterTexture);
		}
		if (!Texture || !IsTextureAdjustmentDefault(Texture))
		{
			return false;
		}
		if (Texture->VirtualTextureStreaming || IsVirtualSamplerType(TextureSample->SamplerType))
		{
			return false;
		}
		switch (TextureSample->SamplerType)
		{
		case SAMPLERTYPE_Color:
		case SAMPLERTYPE_Grayscale:
		case SAMPLERTYPE_Alpha:
		case SAMPLERTYPE_Normal:
		case SAMPLERTYPE_Masks:
		case SAMPLERTYPE_LinearColor:
		case SAMPLERTYPE_LinearGrayscale:
		case SAMPLERTYPE_Data:
			break;
		default:
			return false;
		}
		const bool bColorProperty = Property == MP_BaseColor || Property == MP_Normal || Property == MP_EmissiveColor;
		bool bChannelValid = false;
		OutBinding.Texture = Texture;
		OutBinding.Channel = GetDirectTextureChannel(TracedInput, bColorProperty, bChannelValid);
		return bChannelValid;
	}

	bool ExportFbx(UStaticMesh* StaticMesh, const FString& FilePath, FString& OutError)
	{
		UAssetExportTask* ExportTask = NewObject<UAssetExportTask>(GetTransientPackage());
		ExportTask->Object = StaticMesh;
		ExportTask->Filename = FilePath;
		ExportTask->bSelected = false;
		ExportTask->bReplaceIdentical = true;
		ExportTask->bPrompt = false;
		ExportTask->bAutomated = true;
		ExportTask->bUseFileArchive = false;
		ExportTask->bWriteEmptyFiles = false;

		UFbxExportOption* ExportOptions = NewObject<UFbxExportOption>(ExportTask);
		ExportOptions->FbxExportCompatibility = EFbxExportCompatibility::FBX_2013;
		ExportOptions->bASCII = false;
		ExportOptions->LevelOfDetail = false;
		ExportOptions->Collision = false;
		ExportOptions->bExportSourceMesh = false;
		ExportOptions->VertexColor = true;
		ExportOptions->BakeMaterialInputs = EFbxMaterialBakeMode::Disabled;
		ExportTask->Options = ExportOptions;
		ExportTask->Exporter = UExporter::FindExporter(StaticMesh, TEXT("FBX"));

		if (!ExportTask->Exporter)
		{
			OutError = TEXT("UE could not find the Static Mesh FBX exporter.");
			return false;
		}

		if (!UExporter::RunAssetExportTask(ExportTask))
		{
			OutError = ExportTask->Errors.IsEmpty()
				? TEXT("Static Mesh FBX export failed.")
				: FString::Join(ExportTask->Errors, TEXT("\n"));
			return false;
		}

		return IFileManager::Get().FileExists(*FilePath);
	}

	bool BakeMaterial(
		UStaticMesh* StaticMesh,
		const int32 SlotIndex,
		UMaterialInterface* Material,
		const FFBXExporterOptions& Options,
		FBakedMaterialData& OutMaterialData,
		FString& OutError)
	{
		OutMaterialData = FBakedMaterialData();
		const EFBXExporterTextureMap SelectedMaps = Options.TextureMaps;
		if (SelectedMaps == EFBXExporterTextureMap::None)
		{
			return true;
		}

		UStaticMeshComponent* TransientComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
		TransientComponent->SetStaticMesh(StaticMesh);
		FStaticMeshComponentAdapter Adapter(TransientComponent);

		FMeshDescription MeshDescription;
		FStaticMeshAttributes(MeshDescription).Register();
		Adapter.RetrieveRawMeshData(0, MeshDescription, true);
		if (MeshDescription.GetNumUVElementChannels() <= 0)
		{
			OutError = FString::Printf(TEXT("Material bake requires UV0, but Static Mesh slot %d has no UV channel."), SlotIndex);
			return false;
		}

		TArray<FSectionInfo> Sections;
		Adapter.RetrieveMeshSections(0, Sections);
		TArray<int32> MaterialSectionIndices;
		for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
		{
			if (Sections[SectionIndex].MaterialIndex == SlotIndex)
			{
				MaterialSectionIndices.Add(SectionIndex);
			}
		}

		if (MaterialSectionIndices.IsEmpty())
		{
			OutMaterialData.Warnings.Add(TEXT("Material slot is not used by LOD0; no textures were generated."));
			return true;
		}

		FMeshData MeshSettings;
		MeshSettings.MeshDescription = &MeshDescription;
		MeshSettings.Mesh = StaticMesh;
		MeshSettings.MaterialIndices = MaterialSectionIndices;
		MeshSettings.TextureCoordinateIndex = 0;
		MeshSettings.TextureCoordinateBox = FBox2D(FVector2D::ZeroVector, FVector2D(1.0f, 1.0f));
		MeshSettings.PrimitiveData = FPrimitiveData(StaticMesh);
		Adapter.ApplySettings(0, MeshSettings);

		FMaterialData MaterialSettings;
		MaterialSettings.Material = Material;
		MaterialSettings.BlendMode = Material->GetBlendMode();
		MaterialSettings.bTangentSpaceNormal = true;
		MaterialSettings.bPerformBorderSmear = true;
		MaterialSettings.bPerformShrinking = false;
		if (Material->WritesToRuntimeVirtualTexture())
		{
			OutMaterialData.Warnings.Add(TEXT("Material writes to a Runtime Virtual Texture; Unity material setup must be handled manually."));
		}
		if (UMaterial* BaseMaterial = Material->GetMaterial())
		{
			if (BaseMaterial->HasSubstrateFrontMaterialConnected())
			{
				OutMaterialData.Warnings.Add(TEXT("Material uses a Substrate front material; only the baked PBR textures are exported."));
			}
			if (BaseMaterial->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionCustom>())
			{
				OutMaterialData.Warnings.Add(TEXT("Material contains Custom HLSL; only the baked PBR textures are exported."));
			}
		}

		const EMaterialProperty OpacityProperty = Material->GetBlendMode() == BLEND_Masked ? MP_OpacityMask : MP_Opacity;
		const EFBXExporterTextureMap RequiredMaps = GetRequiredBakeMaps(SelectedMaps);
		TArray<EMaterialProperty> Properties;
		auto AddProperty = [&Properties](const EMaterialProperty Property)
		{
			if (!Properties.Contains(Property))
			{
				Properties.Add(Property);
			}
		};
		if (IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::BaseColor)) AddProperty(MP_BaseColor);
		if (IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::Normal)) AddProperty(MP_Normal);
		if (IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::Mask))
		{
			AddProperty(MP_Metallic);
			AddProperty(MP_AmbientOcclusion);
			AddProperty(MP_Roughness);
		}
		else if (IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::Occlusion))
		{
			AddProperty(MP_AmbientOcclusion);
		}
		if (IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::Emission)) AddProperty(MP_EmissiveColor);
		if (IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::Opacity)) AddProperty(OpacityProperty);

		TMap<EMaterialProperty, FDirectTextureBinding> DirectTextures;
		if (Options.bReuseDirectTextures)
		{
			for (const EMaterialProperty Property : Properties)
			{
				FDirectTextureBinding Binding;
				if (FindDirectTexture(Material, Property, Binding))
				{
					DirectTextures.Add(Property, Binding);
				}
			}
		}

		const FIntPoint BakeSize(Options.BakeResolution, Options.BakeResolution);
		// A direct binding is only a fast path.  Validate that its source pixels
		// can actually be read before omitting the corresponding bake property;
		// otherwise a missing source image would silently turn into a default
		// color instead of being baked from the material graph.
		for (auto DirectIt = DirectTextures.CreateIterator(); DirectIt; ++DirectIt)
		{
			const EMaterialProperty Property = DirectIt.Key();
			const bool bSRGB = Property == MP_BaseColor || Property == MP_EmissiveColor;
			TArray<FColor> ProbePixels;
			if (!LoadDirectTexturePixels(DirectIt.Value(), Options.BakeResolution, Options.BakeResolution, bSRGB, ProbePixels))
			{
				OutMaterialData.Warnings.Add(FString::Printf(
					TEXT("Direct texture could not be read for material property %d; baking the material graph instead."),
					static_cast<int32>(Property)));
				DirectIt.RemoveCurrent();
			}
			ProbePixels.Reset();
		}
		for (const EMaterialProperty Property : Properties)
		{
			if (!DirectTextures.Contains(Property))
			{
				MaterialSettings.PropertySizes.Add(Property, BakeSize);
			}
		}

		TArray<FMaterialData*> MaterialSettingsArray;
		MaterialSettingsArray.Add(&MaterialSettings);
		TArray<FMeshData*> MeshSettingsArray;
		MeshSettingsArray.Add(&MeshSettings);
		FBakeOutput BakeOutput;

		if (!MaterialSettings.PropertySizes.IsEmpty())
		{
			IMaterialBakingModule& MaterialBakingModule = FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
			const bool bPreviousLinearBake = MaterialBakingModule.IsLinearBake(MP_Metallic);
			ON_SCOPE_EXIT { MaterialBakingModule.SetLinearBake(bPreviousLinearBake); };
			MaterialBakingModule.SetLinearBake(true);
			// Both engine HDR modes provide normalized PropertyData and EmissiveScale.
			// Leave the shared HDR setting untouched; optional float data is released below.
			MaterialBakingModule.BakeMaterials(MaterialSettingsArray, MeshSettingsArray, BakeOutput);
		}

		if (BakeOutput.PropertyData.IsEmpty() && DirectTextures.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Material bake returned no property data for slot %d (%s)."), SlotIndex, *Material->GetName());
			return false;
		}

		auto GetProperty = [&](const EMaterialProperty Property, const FColor DefaultColor, const bool bSRGB, TArray<FColor>& OutPixels)
		{
			if (const FDirectTextureBinding* DirectTexture = DirectTextures.Find(Property))
			{
				if (LoadDirectTexturePixels(*DirectTexture, Options.BakeResolution, Options.BakeResolution, bSRGB, OutPixels))
				{
					return true;
				}
				OutMaterialData.Warnings.Add(FString::Printf(
					TEXT("Direct texture could not be read for material property %d; baked output was used."),
					static_cast<int32>(Property)));
			}

			return GetBakedPropertyPixels(
				BakeOutput,
				Property,
				Options.BakeResolution,
				Options.BakeResolution,
				DefaultColor,
				OutPixels);
		};
		auto ReleaseBakedProperty = [&BakeOutput](const EMaterialProperty Property)
		{
			BakeOutput.PropertyData.Remove(Property);
			BakeOutput.PropertySizes.Remove(Property);
			BakeOutput.HDRPropertyData.Remove(Property);
			BakeOutput.PropertyIsLinearColor.Remove(Property);
		};

		const bool bNeedBaseColor = IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::BaseColor);
		const bool bNeedNormal = IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Normal);
		const bool bNeedEmission = IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Emission);
		const bool bNeedOpacity = IsFBXExporterTextureMapEnabled(RequiredMaps, EFBXExporterTextureMap::Opacity);
		auto SaveMap = [&](const FString& MapName, TArray<FColor>&& Pixels)
		{
			OutMaterialData.MapPixels.Add(MapName, MoveTemp(Pixels));
		};

		// BaseColor and opacity are evaluated together so BaseColor alpha always
		// carries the material's opacity convention.  Release both buffers before
		// evaluating any of the scalar maps.
		TArray<FColor> BaseColor;
		TArray<FColor> Opacity;
		if ((bNeedBaseColor && !GetProperty(MP_BaseColor, FColor::White, true, BaseColor)) ||
			(bNeedOpacity && !GetProperty(OpacityProperty, FColor::White, false, Opacity)))
		{
			OutError = FString::Printf(TEXT("Material bake failed for slot %d (%s)."), SlotIndex, *Material->GetName());
			return false;
		}
		if (bNeedBaseColor && bNeedOpacity)
		{
			for (int32 PixelIndex = 0; PixelIndex < BaseColor.Num() && PixelIndex < Opacity.Num(); ++PixelIndex)
			{
				BaseColor[PixelIndex].A = Opacity[PixelIndex].R;
			}
		}
		if (bNeedBaseColor)
		{
			SaveMap(TEXT("BaseColor"), MoveTemp(BaseColor));
		}
		BaseColor.Reset();
		if (IsMapSelected(SelectedMaps, TEXT("Opacity")))
		{
			for (FColor& Pixel : Opacity)
			{
				Pixel.G = Pixel.R;
				Pixel.B = Pixel.R;
				Pixel.A = Pixel.R;
			}
			SaveMap(GetOpacityMapName(Material), MoveTemp(Opacity));
		}
		Opacity.Reset();
		ReleaseBakedProperty(MP_BaseColor);
		ReleaseBakedProperty(OpacityProperty);

		TArray<FColor> Metallic;
		TArray<FColor> AmbientOcclusion;
		TArray<FColor> Roughness;
		const bool bNeedMask = IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Mask);
		const bool bNeedOcclusion = IsFBXExporterTextureMapEnabled(SelectedMaps, EFBXExporterTextureMap::Occlusion);
		if ((bNeedMask && !GetProperty(MP_Metallic, FColor::Black, false, Metallic)) ||
			((bNeedMask || bNeedOcclusion) && !GetProperty(MP_AmbientOcclusion, FColor::White, false, AmbientOcclusion)) ||
			(bNeedMask && !GetProperty(MP_Roughness, FColor(128, 128, 128, 255), false, Roughness)))
		{
			OutError = FString::Printf(TEXT("Scalar material bake failed for slot %d (%s)."), SlotIndex, *Material->GetName());
			return false;
		}
		TArray<FColor> Mask;
		if (bNeedMask)
		{
			BuildMaskPixels(Metallic, AmbientOcclusion, Roughness, Options.BakeResolution, Options.BakeResolution, Mask);
		}
		if (bNeedMask)
		{
			SaveMap(TEXT("Mask"), MoveTemp(Mask));
		}
		if (bNeedOcclusion)
		{
			SaveMap(TEXT("Occlusion"), MoveTemp(AmbientOcclusion));
		}
		Mask.Reset();
		AmbientOcclusion.Reset();
		Metallic.Reset();
		Roughness.Reset();
		ReleaseBakedProperty(MP_Metallic);
		ReleaseBakedProperty(MP_AmbientOcclusion);
		ReleaseBakedProperty(MP_Roughness);

		// Normal is independent of the color/scalar groups and can be discarded as
		// soon as its selected output has been captured.
		if (bNeedNormal)
		{
			TArray<FColor> Normal;
			if (!GetProperty(MP_Normal, FColor(128, 128, 255, 255), false, Normal))
			{
				OutError = FString::Printf(TEXT("Normal material bake failed for slot %d (%s)."), SlotIndex, *Material->GetName());
				return false;
			}
			SaveMap(TEXT("Normal"), MoveTemp(Normal));
			Normal.Reset();
			ReleaseBakedProperty(MP_Normal);
		}

		// Emission is baked in linear space and explicitly encoded as sRGB before
		// the PNG is staged.  Keep its scale alongside the material metadata.
		if (bNeedEmission)
		{
			TArray<FColor> Emission;
			const FDirectTextureBinding* DirectEmission = DirectTextures.Find(MP_EmissiveColor);
			const bool bReadEmission = DirectEmission
				? LoadDirectTexturePixels(*DirectEmission, Options.BakeResolution, Options.BakeResolution, true, Emission, &OutMaterialData.EmissionScale)
				: GetProperty(MP_EmissiveColor, FColor::Black, true, Emission);
			if (!bReadEmission)
			{
				OutError = FString::Printf(TEXT("Emission material bake failed for slot %d (%s)."), SlotIndex, *Material->GetName());
				return false;
			}
			if (!DirectTextures.Contains(MP_EmissiveColor))
			{
				OutMaterialData.EmissionScale = BakeOutput.EmissiveScale > 0.0f ? BakeOutput.EmissiveScale : 1.0f;
				for (FColor& Pixel : Emission)
				{
					const FLinearColor LinearColor(
						static_cast<float>(Pixel.R) / 255.0f,
						static_cast<float>(Pixel.G) / 255.0f,
						static_cast<float>(Pixel.B) / 255.0f,
						static_cast<float>(Pixel.A) / 255.0f);
					Pixel = LinearColor.ToFColorSRGB();
				}
			}
			SaveMap(TEXT("Emission"), MoveTemp(Emission));
			Emission.Reset();
		}
		ReleaseBakedProperty(MP_EmissiveColor);

		return true;
	}

	bool RestorePreviousFiles(const FString& BackupDirectory, const FString& FinalDirectory)
	{
		if (PublishFailurePoint == TEXT("publish_restore")) return false;
		TArray<FString> BackupFiles;
		IFileManager::Get().FindFilesRecursive(BackupFiles, *BackupDirectory, TEXT("*"), true, false);
		bool bSuccess = true;
		for (const FString& BackupFile : BackupFiles)
		{
			FString RelativePath = BackupFile.Mid(BackupDirectory.Len());
			RelativePath.RemoveFromStart(TEXT("/"));
			RelativePath.RemoveFromStart(TEXT("\\"));
			const FString DestinationPath = FinalDirectory / RelativePath;
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationPath), true) ||
				!IFileManager::Get().Move(*DestinationPath, *BackupFile, true, true))
			{
				bSuccess = false;
			}
		}
		return bSuccess;
	}

	bool PublishStagedDirectory(
		const FString& StagingDirectory,
		const FString& FinalDirectory,
		FString& OutError,
		const FString& ExistingJournalPath = FString(),
		const FString& JobId = FString())
	{
		if (!IFileManager::Get().DirectoryExists(*StagingDirectory))
		{
			OutError = FString::Printf(TEXT("Staging directory does not exist: %s"), *StagingDirectory);
			return false;
		}

		const FString StagingRoot = FPaths::ConvertRelativePathToFull(StagingDirectory);
		const FString FinalRoot = FPaths::ConvertRelativePathToFull(FinalDirectory);
		const FString BackupDirectory = StagingRoot + TEXT("_Previous");
		FString BlockingJournal;
		if (IsRecoveryBlocked(FinalRoot, BlockingJournal)) { OutError = TEXT("Output requires inspected recovery acknowledgement: ") + BlockingJournal; return false; }
		const bool bHadFinalDirectory = IFileManager::Get().DirectoryExists(*FinalRoot);
		if (IFileManager::Get().DirectoryExists(*BackupDirectory) &&
			!IFileManager::Get().DeleteDirectory(*BackupDirectory, false, true))
		{
			OutError = FString::Printf(TEXT("Could not remove the previous staging backup: %s"), *BackupDirectory);
			return false;
		}

		auto NormalizeDirectory = [](FString Path)
		{
			Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeDirectoryName(Path);
			return Path.ToLower();
		};
		auto RelativeTo = [](FString Path, const FString& Root)
		{
			Path = FPaths::ConvertRelativePathToFull(Path);
			// FPaths::MakePathRelativeTo interprets its second argument as a file
			// path and strips that file's leaf.  Pass a synthetic child so Root is
			// treated as the directory itself; otherwise every relative path would
			// retain the final-directory name (e.g. Final/new.txt).
			FPaths::MakePathRelativeTo(Path, *(Root / TEXT("_FBXExporterRoot")));
			Path.RemoveFromStart(TEXT("./"));
			Path.RemoveFromStart(TEXT(".\\"));
			return Path;
		};

		TArray<FString> ExistingDirectories;
		TSet<FString> OriginalDirectoryNames;
		if (bHadFinalDirectory)
		{
			IFileManager::Get().FindFilesRecursive(ExistingDirectories, *FinalRoot, TEXT("*"), false, true);
			for (const FString& ExistingDirectory : ExistingDirectories)
			{
				OriginalDirectoryNames.Add(NormalizeDirectory(ExistingDirectory));
			}
		}
		TArray<FString> ExistingFiles;
		if (bHadFinalDirectory)
		{
			IFileManager::Get().FindFilesRecursive(ExistingFiles, *FinalRoot, TEXT("*"), true, false);
		}
		TArray<FString> StagedFiles;
		IFileManager::Get().FindFilesRecursive(StagedFiles, *StagingRoot, TEXT("*"), true, false);
		if (StagedFiles.IsEmpty()) { OutError = TEXT("Staging is empty; previous output was preserved."); return false; }
		const FString JournalPath = ExistingJournalPath.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"))) : ExistingJournalPath;
		TSharedRef<FJsonObject> Journal = MakeShared<FJsonObject>();
		Journal->SetNumberField(TEXT("version"), 1);
		Journal->SetStringField(TEXT("provider_id"), TEXT("fbx_exporter"));
		if (!JobId.IsEmpty()) Journal->SetStringField(TEXT("job_id"), JobId);
		Journal->SetStringField(TEXT("state"), TEXT("publishing"));
		Journal->SetStringField(TEXT("original"), FinalRoot);
		Journal->SetStringField(TEXT("staging"), StagingRoot);
		Journal->SetStringField(TEXT("backup"), BackupDirectory);
		TArray<TSharedPtr<FJsonValue>> Originals, Generated;
		for (const FString& File : ExistingFiles) Originals.Add(MakeShared<FJsonValueString>(File));
		for (const FString& File : StagedFiles) Generated.Add(MakeShared<FJsonValueString>(File));
		Journal->SetArrayField(TEXT("original_files"), Originals);
		Journal->SetArrayField(TEXT("staged_files"), Generated);
		if (!WriteJournal(JournalPath, Journal)) { OutError = TEXT("Cannot persist publish journal; existing output was preserved."); return false; }
		auto MarkRestored = [&]() { Journal->SetStringField(TEXT("state"), TEXT("restored")); WriteJournal(JournalPath, Journal); };
		for (const FString& ExistingFile : ExistingFiles)
		{
			const FString RelativePath = RelativeTo(ExistingFile, FinalRoot);
			if (RelativePath.IsEmpty() || RelativePath == TEXT("..") || RelativePath.StartsWith(TEXT("../")) || RelativePath.StartsWith(TEXT("..\\")))
			{
				OutError = FString::Printf(TEXT("Existing output path is outside the final directory: %s"), *ExistingFile);
				return false;
			}
			const FString BackupPath = BackupDirectory / RelativePath;
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(BackupPath), true) ||
				!IFileManager::Get().Move(*BackupPath, *ExistingFile, true, true))
			{
				OutError = FString::Printf(TEXT("Failed to back up the existing output file: %s"), *ExistingFile);
				const bool bRestored = RestorePreviousFiles(BackupDirectory, FinalRoot);
				if (bRestored && (!IFileManager::Get().DirectoryExists(*BackupDirectory) ||
					IFileManager::Get().DeleteDirectory(*BackupDirectory, false, true)))
				{
					MarkRestored();
				}
				else
				{
					OutError += FString::Printf(TEXT(" Existing output could not be fully restored. Preserve staging at '%s' and backup at '%s'."), *StagingRoot, *BackupDirectory);
				}
				return false;
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (PublishFailurePoint == TEXT("process_crash_after_backup"))
			{
				FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()); FPaths::NormalizeFilename(Project);
				const bool bOwnedFixture = Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/"), ESearchCase::IgnoreCase) || Project.Contains(TEXT("/Saved/UnrealTools/V11/Projects/"), ESearchCase::IgnoreCase);
				const bool bExplicitFlags = FParse::Param(FCommandLine::Get(), TEXT("FBXExporterCrashCheckpoint")) && FParse::Param(FCommandLine::Get(), TEXT("UnrealToolsOwnedCrashFixture"));
				if (bOwnedFixture && bExplicitFlags)
				{
					const FString Marker = FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/CrashCheckpoint/ready.json");
					const auto Ready = ReadJournal(Marker);
					if (!Ready.IsValid()) { OutError = TEXT("Owned crash fixture marker was not prepared; process exit was refused."); return false; }
					Ready->SetStringField(TEXT("checkpoint"), TEXT("after_first_backup_move")); Ready->SetStringField(TEXT("journal"), JournalPath);
					Ready->SetStringField(TEXT("moved_original"), ExistingFile); Ready->SetStringField(TEXT("moved_backup"), BackupPath);
					if (!WriteJournal(Marker, Ready.ToSharedRef())) { OutError = TEXT("Crash checkpoint marker could not be persisted; process exit was refused."); return false; }
					// Deliberately bypasses normal module shutdown only in an explicitly
					// opted-in Automation fixture. Never active in a regular Editor.
					FPlatformMisc::RequestExitWithStatus(true, 86, TEXT("FBXExporterOwnedCrashAutomation"));
				}
			}
#endif
		}

		TArray<FString> PublishedFiles;
		auto CleanupUnexpectedDirectories = [&]()
		{
			if (!IFileManager::Get().DirectoryExists(*FinalRoot))
			{
				return true;
			}
			TArray<FString> CurrentDirectories;
			IFileManager::Get().FindFilesRecursive(CurrentDirectories, *FinalRoot, TEXT("*"), false, true);
			CurrentDirectories.Sort([](const FString& A, const FString& B)
			{
				return A.Len() > B.Len();
			});
			bool bSuccess = true;
			for (const FString& CurrentDirectory : CurrentDirectories)
			{
				if (!OriginalDirectoryNames.Contains(NormalizeDirectory(CurrentDirectory)) &&
					!IFileManager::Get().DeleteDirectory(*CurrentDirectory, false, false))
				{
					bSuccess = false;
				}
			}
			return bSuccess;
		};
		auto RestoreAndCleanupBackup = [&]()
		{
			bool bDeletedPublishedFiles = true;
			for (const FString& PublishedFile : PublishedFiles)
			{
				if (IFileManager::Get().FileExists(*PublishedFile) &&
					!IFileManager::Get().Delete(*PublishedFile, false, true, true))
				{
					bDeletedPublishedFiles = false;
				}
			}
			const bool bCleanedDirectories = CleanupUnexpectedDirectories();
			const bool bRestored = RestorePreviousFiles(BackupDirectory, FinalRoot);
			bool bRecreatedDirectories = true;
			if (bRestored && bCleanedDirectories)
			{
				for (const FString& ExistingDirectory : ExistingDirectories)
				{
					if (!IFileManager::Get().MakeDirectory(*ExistingDirectory, true))
					{
						bRecreatedDirectories = false;
					}
				}
				if (!bHadFinalDirectory)
				{
					bRecreatedDirectories = IFileManager::Get().DeleteDirectory(*FinalRoot, false, true) && bRecreatedDirectories;
				}
			}
			if (!bRestored || !bCleanedDirectories || !bDeletedPublishedFiles || !bRecreatedDirectories)
			{
				OutError += FString::Printf(TEXT(" Existing output could not be fully restored%s%s%s. Preserve staging at '%s' and backup at '%s'."),
					bDeletedPublishedFiles ? TEXT("") : TEXT(" or generated files could not be removed"),
					bCleanedDirectories ? TEXT("") : TEXT(" or stale directories could not be removed"),
					bRecreatedDirectories ? TEXT("") : TEXT(" or original directories could not be recreated"),
					*StagingRoot, *BackupDirectory);
				return false;
			}
			if (IFileManager::Get().DirectoryExists(*BackupDirectory) &&
				!IFileManager::Get().DeleteDirectory(*BackupDirectory, false, true))
			{
				OutError += FString::Printf(TEXT(" Previous output was restored, but backup cleanup failed. Preserve staging at '%s' and backup at '%s'."), *StagingRoot, *BackupDirectory);
				return false;
			}
			MarkRestored();
			return true;
		};

		for (const FString& StagedFile : StagedFiles)
		{
			const FString RelativePath = RelativeTo(StagedFile, StagingRoot);
			if (RelativePath.IsEmpty() || RelativePath == TEXT("..") || RelativePath.StartsWith(TEXT("../")) || RelativePath.StartsWith(TEXT("..\\")))
			{
				OutError = FString::Printf(TEXT("Staged output path is outside the staging directory: %s"), *StagedFile);
				RestoreAndCleanupBackup();
				return false;
			}
			const FString DestinationPath = FinalRoot / RelativePath;
			PublishedFiles.Add(DestinationPath);
			if (PublishFailurePoint == TEXT("publish_restore") || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationPath), true) ||
				IFileManager::Get().Copy(*DestinationPath, *StagedFile, true, true) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("Failed to publish generated file: %s"), *DestinationPath);
				RestoreAndCleanupBackup();
				return false;
			}
		}

		TSet<FString> RequiredDirectories;
		RequiredDirectories.Add(NormalizeDirectory(FinalRoot));
		for (const FString& StagedFile : StagedFiles)
		{
			const FString RelativePath = RelativeTo(StagedFile, StagingRoot);
			FString RequiredDirectory = FPaths::GetPath(FinalRoot / RelativePath);
			while (!RequiredDirectory.IsEmpty())
			{
				RequiredDirectories.Add(NormalizeDirectory(RequiredDirectory));
				if (NormalizeDirectory(RequiredDirectory) == NormalizeDirectory(FinalRoot))
				{
					break;
				}
				const FString ParentDirectory = FPaths::GetPath(RequiredDirectory);
				if (ParentDirectory == RequiredDirectory)
				{
					break;
				}
				RequiredDirectory = ParentDirectory;
			}
		}
		TArray<FString> CurrentDirectories;
		IFileManager::Get().FindFilesRecursive(CurrentDirectories, *FinalRoot, TEXT("*"), false, true);
		CurrentDirectories.Sort([](const FString& A, const FString& B)
		{
			return A.Len() > B.Len();
		});
		for (const FString& CurrentDirectory : CurrentDirectories)
		{
			if (!RequiredDirectories.Contains(NormalizeDirectory(CurrentDirectory)) &&
				!IFileManager::Get().DeleteDirectory(*CurrentDirectory, false, false))
			{
				OutError = FString::Printf(TEXT("Failed to remove stale output directory: %s"), *CurrentDirectory);
				RestoreAndCleanupBackup();
				return false;
			}
		}

		// Validate every new file before commit while a complete old backup exists.
		for (const FString& StagedFile : StagedFiles)
		{
			TArray<uint8> Expected, Actual;
			const FString Destination = FinalRoot / RelativeTo(StagedFile, StagingRoot);
			if (!FFileHelper::LoadFileToArray(Expected, *StagedFile) || !FFileHelper::LoadFileToArray(Actual, *Destination) || Expected != Actual)
			{
				OutError = FString::Printf(TEXT("Published file verification failed: %s"), *Destination);
				RestoreAndCleanupBackup(); return false;
			}
		}
		Journal->SetStringField(TEXT("state"), TEXT("committed"));
		if (!WriteJournal(JournalPath, Journal))
		{
			OutError = TEXT("Cannot persist publish commit; restoring the complete previous backup.");
			RestoreAndCleanupBackup(); return false;
		}
		// Commit is durable now. Cleanup may delete only part of the backup, so
		// it must NEVER trigger rollback or discard the newly verified output.
		if (PublishFailurePoint == TEXT("cleanup_partial"))
		{
			TArray<FString> Previous; IFileManager::Get().FindFilesRecursive(Previous, *BackupDirectory, TEXT("*"), true, false);
			if (!Previous.IsEmpty()) IFileManager::Get().Delete(*Previous[0], false, true, true);
		}
		if (IFileManager::Get().DirectoryExists(*BackupDirectory) &&
			(PublishFailurePoint == TEXT("cleanup") || PublishFailurePoint == TEXT("cleanup_partial") || !IFileManager::Get().DeleteDirectory(*BackupDirectory, false, true)))
		{
			OutError = FString::Printf(TEXT("Export committed successfully; previous backup cleanup failed. Preserve backup '%s' and journal '%s'."), *BackupDirectory, *JournalPath);
			Journal->SetStringField(TEXT("cleanup_warning"), OutError); WriteJournal(JournalPath, Journal);
			return true;
		}
		Journal->SetStringField(TEXT("state"), TEXT("complete")); WriteJournal(JournalPath, Journal);
		return true;
	}

	bool SaveMetadataJson(
		const FString& JsonPath,
		const FString& SourceStaticMesh,
		const FString& FinalFbxPath,
		const FFBXExporterOptions& Options,
		const TArray<FFBXExporterSlotData>& Slots,
		const TArray<FFBXExporterMaterialBinding>& Bindings,
		FString& OutError)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schemaVersion"), 1);
		Root->SetStringField(TEXT("sourceStaticMesh"), SourceStaticMesh);
		Root->SetStringField(TEXT("fbxFile"), FPaths::ConvertRelativePathToFull(FinalFbxPath));
		Root->SetNumberField(TEXT("bakeResolution"), Options.BakeResolution);
		TArray<TSharedPtr<FJsonValue>> SelectedMapValues;
		for (const FString& Name : GetTextureMapNames())
		{
			if (IsMapSelected(Options.TextureMaps, Name))
			{
				SelectedMapValues.Add(MakeShared<FJsonValueString>(Name));
			}
		}
		Root->SetArrayField(TEXT("selectedMaps"), MoveTemp(SelectedMapValues));
		Root->SetBoolField(TEXT("texturesEmbedded"), Options.bEmbedTexturesInFbx && Options.TextureMaps != EFBXExporterTextureMap::None);
		Root->SetStringField(TEXT("maskLayout"), TEXT("R=Metallic;G=AO;B=Reserved;A=Smoothness(1-Roughness)"));
		Root->SetStringField(TEXT("opacityConvention"), TEXT("1=Opaque"));

		TArray<TSharedPtr<FJsonValue>> MaterialValues;
		for (const FFBXExporterMaterialBinding& Binding : Bindings)
		{
			TSharedRef<FJsonObject> MaterialObject = MakeShared<FJsonObject>();
			MaterialObject->SetNumberField(TEXT("fbxMaterialIndex"), Binding.FbxMaterialIndex);
			TArray<TSharedPtr<FJsonValue>> SourceSlots;
			for (const int32 SlotIndex : Binding.SourceSlotIndices)
			{
				if (!Slots.IsValidIndex(SlotIndex)) continue;
				TSharedRef<FJsonObject> SlotObject = MakeShared<FJsonObject>();
				SlotObject->SetNumberField(TEXT("slotIndex"), SlotIndex);
				SlotObject->SetStringField(TEXT("slotName"), Slots[SlotIndex].SlotName);
				SourceSlots.Add(MakeShared<FJsonValueObject>(SlotObject));
			}
			MaterialObject->SetArrayField(TEXT("sourceSlots"), MoveTemp(SourceSlots));
			if (Binding.MaterialPath.IsEmpty())
			{
				MaterialObject->SetField(TEXT("materialPath"), MakeShared<FJsonValueNull>());
			}
			else
			{
				MaterialObject->SetStringField(TEXT("materialPath"), Binding.MaterialPath);
			}
			MaterialObject->SetStringField(TEXT("blendMode"), Binding.BlendMode);
			MaterialObject->SetStringField(TEXT("baseColorAlpha"), Binding.BaseColorAlpha);
			MaterialObject->SetNumberField(TEXT("emissionScale"), Binding.EmissionScale);
			TArray<TSharedPtr<FJsonValue>> Maps;
			TArray<FString> SortedMapNames;
			Binding.MapRelativePaths.GetKeys(SortedMapNames);
			SortedMapNames.Sort();
			for (const FString& MapName : SortedMapNames)
			{
				const FString& RelativePath = Binding.MapRelativePaths[MapName];
				TSharedRef<FJsonObject> MapObject = MakeShared<FJsonObject>();
				MapObject->SetStringField(TEXT("map"), MapName);
				MapObject->SetStringField(TEXT("file"), RelativePath);
				const bool bSRGB = MapName == TEXT("BaseColor") || MapName == TEXT("Emission");
				MapObject->SetStringField(TEXT("colorSpace"), bSRGB ? TEXT("sRGB") : TEXT("Linear"));
				Maps.Add(MakeShared<FJsonValueObject>(MapObject));
			}
			MaterialObject->SetArrayField(TEXT("maps"), MoveTemp(Maps));
			MaterialValues.Add(MakeShared<FJsonValueObject>(MaterialObject));
		}
		Root->SetArrayField(TEXT("materials"), MoveTemp(MaterialValues));

		FString JsonString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		if (!FJsonSerializer::Serialize(Root, Writer) || !FFileHelper::SaveStringToFile(JsonString, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write material metadata: %s"), *JsonPath);
			return false;
		}
		return true;
	}

	void BuildSlotNames(UStaticMesh* StaticMesh, TArray<FFBXExporterSlotData>& OutSlots)
	{
		OutSlots.Reset();
		const TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
		TSet<FString> UsedNames;
		TArray<FString> CandidateNames;
		for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
		{
			const FString RawName = StaticMaterials[SlotIndex].MaterialSlotName.IsNone()
				? FString::Printf(TEXT("Slot_%d"), SlotIndex)
				: StaticMaterials[SlotIndex].MaterialSlotName.ToString();
			CandidateNames.Add(MakeSafeFileName(RawName));
		}
		for (int32 SlotIndex = 0; SlotIndex < CandidateNames.Num(); ++SlotIndex)
		{
			FString SafeName = CandidateNames[SlotIndex];
			FString LowerName = SafeName.ToLower();
			if (UsedNames.Contains(LowerName) || CandidateNames.FilterByPredicate([&SafeName](const FString& Other)
			{
				return Other.Equals(SafeName, ESearchCase::IgnoreCase);
			}).Num() > 1)
			{
				SafeName += FString::Printf(TEXT("_Slot%d"), SlotIndex);
				int32 Suffix = 0;
				while (UsedNames.Contains(SafeName.ToLower()))
				{
					SafeName = FString::Printf(TEXT("%s_%d"), *SafeName, ++Suffix);
				}
			}
			UsedNames.Add(SafeName.ToLower());
			FFBXExporterSlotData Slot;
			Slot.SlotIndex = SlotIndex;
			Slot.SlotName = SafeName;
			Slot.bHasMaterial = StaticMesh->GetMaterial(SlotIndex) != nullptr;
			if (UMaterialInterface* Material = StaticMesh->GetMaterial(SlotIndex))
			{
				Slot.MaterialPath = Material->GetPathName();
				Slot.BlendMode = GetBlendModeName(Material);
				Slot.BaseColorAlpha = GetOpacityMapName(Material);
			}
			OutSlots.Add(MoveTemp(Slot));
		}
	}

	class FExportRunner final : public UnrealToolsRuntime::IItemRunner
	{
	public:
		FExportRunner(const UnrealToolsRuntime::FItemSpec& InSpec, const FString& InJobId)
			: Spec(InSpec), Options(OptionsFromJson(InSpec.Options)), JobId(InJobId) {}
		virtual ~FExportRunner() override { ReleaseReferences(); }
		virtual FString GetStage() const override
		{
			return Stage == TEXT("materials") || Stage == TEXT("png") ? FString::Printf(TEXT("%s.slot_%d.group_%d"), *Stage, SlotIndex, GroupIndex) : Stage;
		}
		virtual bool CanCancel() const override { return !IsCommitPhase() && (!PngTask.IsValid() || PngTask.IsReady()); }
		virtual bool IsCommitPhase() const override { return Stage == TEXT("publish") || Stage == TEXT("cleanup"); }
		virtual uint64 GetSnapshotRevision() const override { return SnapshotRevision; }
		virtual UnrealToolsRuntime::FObject GetResult() const override
		{
			using namespace UnrealToolsRuntime;
			if (CachedResult.IsValid() && CachedRevision == SnapshotRevision) return Clone(CachedResult);
			FObject Data = OptionsToJson(Options);
			Data->SetStringField(TEXT("mesh_path"), Spec.MeshPath);
			Data->SetBoolField(TEXT("success"), Result.Succeeded());
			Data->SetStringField(TEXT("stage"), GetStage());
			Data->SetStringField(TEXT("final_directory"), FinalDirectory);
			if (!StagingDirectory.IsEmpty()) Data->SetStringField(TEXT("staging_directory"), StagingDirectory);
			Data->SetStringField(TEXT("fbx_file"), FinalFbxPath);
			Data->SetStringField(TEXT("error_code"), ErrorCode);
			if (Options.TextureMaps == EFBXExporterTextureMap::None) Data->SetField(TEXT("metadata_file"), MakeShared<FJsonValueNull>());
			else Data->SetStringField(TEXT("metadata_file"), FinalDirectory / (MeshName + TEXT("_Materials.json")));
			auto Values = [](const TArray<FString>& Array) { TArray<TSharedPtr<FJsonValue>> Out; for (const FString& Item : Array) Out.Add(MakeShared<FJsonValueString>(Item)); return Out; };
			Data->SetArrayField(TEXT("warnings"), Values(Result.Warnings));
			Data->SetArrayField(TEXT("errors"), Values(Result.Errors));
			if (!Result.Succeeded()) Data->SetStringField(TEXT("error"), Result.Errors.IsEmpty() ? TEXT("Export has not finished.") : Result.Errors[0]);
			Data->SetArrayField(TEXT("files"), Values(PublishedFiles));
			if (bKeepStaging) Data->SetStringField(TEXT("recovery_staging"), StagingDirectory);
			if (bRecoveryRequired) { Data->SetStringField(TEXT("recovery_journal"), JournalPath); Data->SetBoolField(TEXT("recovery_required"), true); }
			else if ((IsCommitPhase() || bKeepStaging) && !JournalPath.IsEmpty()) Data->SetStringField(TEXT("recovery_journal"), JournalPath);
			if (!RecoveryBackup.IsEmpty()) Data->SetStringField(TEXT("recovery_backup"), RecoveryBackup);
			CachedRevision = SnapshotRevision; CachedResult = Data;
			return Clone(CachedResult);
		}

		virtual void Shutdown() override { if (PngTask.IsValid()) { PngTask.Wait(); PngTask.Get(); } bKeepStaging = true; PendingPixels.Empty(); ReleaseReferences(); ++SnapshotRevision; }
		virtual UnrealToolsRuntime::EStepResult Step(bool bCancelRequested, bool bInlineMode) override
		{
			using namespace UnrealToolsRuntime;
			check(IsInGameThread());
			if (Stage == TEXT("done")) return Result.Succeeded() ? EStepResult::Succeeded : EStepResult::Failed;
			const FString PreviousStage = GetStage();
			const int32 PreviousWarnings = Result.Warnings.Num(), PreviousErrors = Result.Errors.Num();
			const bool bPreviousSuccess = Result.bSuccess;
			ON_SCOPE_EXIT
			{
				if (PreviousStage != GetStage() || PreviousWarnings != Result.Warnings.Num() || PreviousErrors != Result.Errors.Num() || bPreviousSuccess != Result.bSuccess) ++SnapshotRevision;
			};
			if ((bCancelRequested || (GEditor && (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor))) && CanCancel())
			{
				Result.Errors.Add(bCancelRequested ? TEXT("Cancelled at a safe export boundary.") : TEXT("PIE/SIE started during export."));
				ErrorCode = bCancelRequested ? TEXT("cancelled") : TEXT("editor_playing"); Cleanup(); Stage = TEXT("done");
				return bCancelRequested ? EStepResult::Cancelled : EStepResult::Failed;
			}
			FString Error;
			if (Stage == TEXT("validate"))
			{
				Mesh.Reset(LoadObject<UStaticMesh>(nullptr, *Spec.MeshPath));
				const FPreflightResult Check = PreflightStaticMesh(Mesh.Get(), Options);
				Result.Warnings.Append(Check.Warnings);
				if (!Check.bCanExport) return Fail(FString::Join(Check.Errors, TEXT("\n")), TEXT("preflight_failed"));
				if (Check.bRequiresOverwrite && !Spec.bOverwrite) return Fail(TEXT("Output exists; explicitly approve overwrite after preflight."), TEXT("overwrite_required"));
				MeshName = MakeSafeFileName(Mesh->GetName()); FinalDirectory = Check.FinalDirectory;
				FinalFbxPath = Check.FbxFile;
				StagingDirectory = Options.OutputRoot / (TEXT(".FBX_Exporter_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
				if (!IFileManager::Get().MakeDirectory(*StagingDirectory, true)) return Fail(TEXT("Cannot create staging directory."), TEXT("staging_failed"));
				StagedFbxPath = StagingDirectory / (MeshName + TEXT(".fbx"));
				BuildSlotNames(Mesh.Get(), Slots);
				WatchPackage(Mesh.Get());
				for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
				{
					if (UMaterialInterface* Material = Slot.MaterialInterface)
					{
						HeldObjects.Emplace(Material); WatchPackage(Material); WatchPackage(Material->GetMaterial());
						TArray<UMaterialFunctionInterface*> Functions; Material->GetDependentFunctions(Functions);
						for (UMaterialFunctionInterface* Function : Functions) { HeldObjects.Emplace(Function); WatchPackage(Function); }
						for (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material); Instance; Instance = Cast<UMaterialInstance>(Instance->Parent))
							if (Instance->Parent) { HeldObjects.Emplace(Instance->Parent.Get()); WatchPackage(Instance->Parent); }
						TArray<UTexture*> Textures; Material->GetUsedTextures(Textures);
						for (UTexture* Texture : Textures) { HeldObjects.Emplace(Texture); WatchPackage(Texture); }
					}
				}
				ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object) { if (Object && WatchedPackages.Contains(Object->GetOutermost())) bSourceChanged = true; });
				PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* Object, FPropertyChangedEvent&) { if (Object && WatchedPackages.Contains(Object->GetOutermost())) bSourceChanged = true; });
				// Each tick handles one dependency group, never all maps/all slots.
				const EFBXExporterTextureMap Selected = Options.TextureMaps;
				if (IsFBXExporterTextureMapEnabled(Selected, EFBXExporterTextureMap::BaseColor)) Groups.Add(EFBXExporterTextureMap::BaseColor | (Selected & EFBXExporterTextureMap::Opacity));
				else if (IsFBXExporterTextureMapEnabled(Selected, EFBXExporterTextureMap::Opacity)) Groups.Add(EFBXExporterTextureMap::Opacity);
				if (IsFBXExporterTextureMapEnabled(Selected, EFBXExporterTextureMap::Mask)) Groups.Add(EFBXExporterTextureMap::Mask | (Selected & EFBXExporterTextureMap::Occlusion));
				else if (IsFBXExporterTextureMapEnabled(Selected, EFBXExporterTextureMap::Occlusion)) Groups.Add(EFBXExporterTextureMap::Occlusion);
				if (IsFBXExporterTextureMapEnabled(Selected, EFBXExporterTextureMap::Normal)) Groups.Add(EFBXExporterTextureMap::Normal);
				if (IsFBXExporterTextureMapEnabled(Selected, EFBXExporterTextureMap::Emission)) Groups.Add(EFBXExporterTextureMap::Emission);
				Stage = TEXT("geometry");
			}
			else if (Stage == TEXT("geometry"))
			{
				if (!ExportFbx(Mesh.Get(), StagedFbxPath, Error)) return Fail(Error, TEXT("geometry_failed"));
				Stage = Groups.IsEmpty() ? TEXT("files") : TEXT("materials");
			}
			else if (Stage == TEXT("materials"))
			{
				if (SlotIndex >= Slots.Num()) { Stage = TEXT("files"); return EStepResult::Continue; }
				UMaterialInterface* Material = Mesh->GetMaterial(SlotIndex);
				if (!Material) { Result.Warnings.Add(FString::Printf(TEXT("Material slot %d has no assigned material."), SlotIndex)); ++SlotIndex; GroupIndex = 0; return EStepResult::Continue; }
				FFBXExporterOptions GroupOptions = Options; GroupOptions.TextureMaps = Groups[GroupIndex];
				FBakedMaterialData Data;
				if (!BakeMaterial(Mesh.Get(), SlotIndex, Material, GroupOptions, Data, Error)) return Fail(Error, TEXT("bake_failed"));
				if (IsFBXExporterTextureMapEnabled(GroupOptions.TextureMaps, EFBXExporterTextureMap::Emission)) Slots[SlotIndex].EmissionScale = Data.EmissionScale;
				Result.Warnings.Append(Data.Warnings);
				PendingPixels = MoveTemp(Data.MapPixels); PendingPixels.GetKeys(PendingMapNames); PendingMapNames.Sort(); PendingMapIndex = 0;
				Stage = TEXT("png");
			}
			else if (Stage == TEXT("png"))
			{
				if (PendingMapIndex >= PendingMapNames.Num())
				{
					PendingPixels.Empty(); PendingMapNames.Empty(); ++GroupIndex;
					if (GroupIndex >= Groups.Num()) { GroupIndex = 0; ++SlotIndex; }
					Stage = TEXT("materials"); return EStepResult::Continue;
				}
				const FString MapName = PendingMapNames[PendingMapIndex];
				const bool bSRGB = MapName == TEXT("BaseColor") || MapName == TEXT("Emission");
				const FString RelativePath = TEXT("Textures/") + GetMapFileName(Slots[SlotIndex].SlotName, MapName);
				const FString Path = StagingDirectory / RelativePath;
				FPngWrite Write;
				if (!PngTask.IsValid())
				{
					TArray<FColor> Pixels = MoveTemp(PendingPixels.FindChecked(MapName)); PendingPixels.Remove(MapName);
					const int32 Resolution = Options.BakeResolution;
					// Load codecs on the game thread; worker owns only plain pixels/paths.
					FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));
					auto Work = [Pixels = MoveTemp(Pixels), Path, Resolution, bSRGB]() mutable
					{
						FPngWrite Outcome;
						Outcome.bSuccess = IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true) && SaveImage(Path, Pixels, Resolution, Resolution, bSRGB, Outcome.Error);
						if (!Outcome.bSuccess && Outcome.Error.IsEmpty()) Outcome.Error = TEXT("Cannot create PNG directory: ") + Path;
						Pixels.Empty(); return Outcome;
					};
					if (bInlineMode) Write = Work();
					else { PngTask = Async(EAsyncExecution::ThreadPool, MoveTemp(Work)); return EStepResult::Continue; }
				}
				else
				{
					if (!PngTask.IsReady()) return EStepResult::Continue;
					Write = PngTask.Get(); PngTask = TFuture<FPngWrite>();
				}
				if (!Write.bSuccess) return Fail(Write.Error, TEXT("png_failed"));
				FFBXExporterTextureFileData TextureFile;
				TextureFile.MapName = MapName; TextureFile.RelativePath = RelativePath; TextureFile.StagedAbsolutePath = Path;
				TextureFile.FinalAbsolutePath = FinalDirectory / RelativePath; TextureFile.ColorSpace = bSRGB ? TEXT("sRGB") : TEXT("Linear");
				if (Options.bEmbedTexturesInFbx && !FFileHelper::LoadFileToArray(TextureFile.FileBytes, *Path)) return Fail(TEXT("Cannot read generated PNG for embedding."), TEXT("png_failed"));
				Slots[SlotIndex].MapRelativePaths.Add(MapName, RelativePath); TextureFiles.Add(MoveTemp(TextureFile)); ++PendingMapIndex;
			}

			else if (Stage == TEXT("files"))
			{
				if (bSourceChanged) return Fail(TEXT("Source mesh/material/texture was modified during this export."), TEXT("source_changed"));
				TArray<FFBXExporterSectionData> Sections;
				for (const FStaticMeshSection& Section : Mesh->GetRenderData()->LODResources[0].Sections)
				{
					FFBXExporterSectionData Data; Data.SlotIndex = Section.MaterialIndex; Data.TriangleCount = Section.NumTriangles;
					Data.bHasMaterial = Slots.IsValidIndex(Section.MaterialIndex) && Slots[Section.MaterialIndex].bHasMaterial; Sections.Add(Data);
				}
				TArray<FFBXExporterMaterialBinding> Bindings;
				if (!PostProcessFbx(StagedFbxPath, FinalFbxPath, Sections, Slots, TextureFiles, Options.bEmbedTexturesInFbx && !Groups.IsEmpty(), Bindings, Error)) return Fail(Error, TEXT("fbx_media_failed"));
				TextureFiles.Empty();
				if (!Groups.IsEmpty() && !SaveMetadataJson(StagingDirectory / (MeshName + TEXT("_Materials.json")), Spec.MeshPath, FinalFbxPath, Options, Slots, Bindings, Error)) return Fail(Error, TEXT("metadata_failed"));
				Stage = TEXT("verify");
			}
			else if (Stage == TEXT("verify"))
			{
				if (bSourceChanged) return Fail(TEXT("Source changed before publish."), TEXT("source_changed"));
				const FPreflightResult Check = PreflightStaticMesh(Mesh.Get(), Options);
				if (!Check.bCanExport) return Fail(FString::Join(Check.Errors, TEXT("\n")), TEXT("preflight_failed"));
				if (Check.bRequiresOverwrite && !Spec.bOverwrite) return Fail(TEXT("Output appeared during export; previous output was preserved."), TEXT("overwrite_required"));
				if (IFileManager::Get().FileSize(*StagedFbxPath) <= 0) return Fail(TEXT("FBX staging validation failed."), TEXT("verification_failed"));
				JournalPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json")));
				Stage = TEXT("publish");
			}
			else if (Stage == TEXT("publish"))
			{
				if (bSourceChanged) return Fail(TEXT("Source changed before publish."), TEXT("source_changed"));
				if (!PublishStagedDirectory(StagingDirectory, FinalDirectory, Error, JournalPath, JobId)) { bKeepStaging = true; FString BlockingJournal; bRecoveryRequired = IsRecoveryBlocked(FinalDirectory, BlockingJournal); RecoveryBackup = IFileManager::Get().DirectoryExists(*(StagingDirectory + TEXT("_Previous"))) ? StagingDirectory + TEXT("_Previous") : FString(); return Fail(Error, TEXT("publish_failed")); }
				if (!Error.IsEmpty()) { Result.Warnings.Add(Error); bKeepStaging = true; }
				IFileManager::Get().FindFilesRecursive(PublishedFiles, *FinalDirectory, TEXT("*"), true, false); PublishedFiles.Sort();
				RecoveryBackup = IFileManager::Get().DirectoryExists(*(StagingDirectory + TEXT("_Previous"))) ? StagingDirectory + TEXT("_Previous") : FString();
				Result.bSuccess = true; Result.FinalDirectory = FinalDirectory; Stage = TEXT("cleanup");
			}
			else if (Stage == TEXT("cleanup")) { Cleanup(); Stage = TEXT("done"); return EStepResult::Succeeded; }
			return EStepResult::Continue;
		}
	private:
		struct FPngWrite { bool bSuccess = false; FString Error; };
		TFuture<FPngWrite> PngTask; TMap<FString, TArray<FColor>> PendingPixels; TArray<FString> PendingMapNames; int32 PendingMapIndex = 0;
		UnrealToolsRuntime::EStepResult Fail(const FString& Error, const FString& Code)
		{ Result.Errors.Add(Error); ErrorCode = Code; Cleanup(); Stage = TEXT("done"); return UnrealToolsRuntime::EStepResult::Failed; }
		void WatchPackage(UObject* Object) { if (Object) WatchedPackages.Add(Object->GetOutermost()); }
		void ReleaseReferences()
		{
			if (ModifiedHandle.IsValid()) { FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle); ModifiedHandle.Reset(); }
			if (PropertyHandle.IsValid()) { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle); PropertyHandle.Reset(); }
			HeldObjects.Empty(); Mesh.Reset();
		}
		void Cleanup()
		{
			if (!bKeepStaging && !StagingDirectory.IsEmpty() && IFileManager::Get().DirectoryExists(*StagingDirectory) && !IFileManager::Get().DeleteDirectory(*StagingDirectory, false, true))
			{ Result.Warnings.Add(TEXT("Staging cleanup failed: ") + StagingDirectory); bKeepStaging = true; }
			PendingPixels.Empty(); PendingMapNames.Empty(); ReleaseReferences();
		}
		UnrealToolsRuntime::FItemSpec Spec; FFBXExporterOptions Options; FString JobId;
		TStrongObjectPtr<UStaticMesh> Mesh; TArray<TStrongObjectPtr<UObject>> HeldObjects; TSet<UPackage*> WatchedPackages; FDelegateHandle ModifiedHandle, PropertyHandle;
		FFBXExporterResult Result; FString Stage = TEXT("validate"), ErrorCode, MeshName, StagingDirectory, StagedFbxPath, FinalFbxPath, FinalDirectory, JournalPath;
		bool bSourceChanged = false, bKeepStaging = false, bRecoveryRequired = false;
		uint64 SnapshotRevision = 1; mutable uint64 CachedRevision = 0; mutable UnrealToolsRuntime::FObject CachedResult;
		TArray<FString> PublishedFiles; FString RecoveryBackup;
		int32 SlotIndex = 0, GroupIndex = 0;
		TArray<EFBXExporterTextureMap> Groups; TArray<FFBXExporterSlotData> Slots; TArray<FFBXExporterTextureFileData> TextureFiles;
	};


}

bool IsExportInProgress()
{
	return Jobs().IsBusy();
}

TUniquePtr<UnrealToolsRuntime::IItemRunner> CreateExportRunner(const UnrealToolsRuntime::FItemSpec& Spec, const FString& JobId)
{ return MakeUnique<FExportRunner>(Spec, JobId); }

bool IsRecoveryBlocked(const FString& FinalDirectory, FString& OutJournal)
{
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery")), TEXT("*.json"), true, false);
	FString Wanted = FPaths::ConvertRelativePathToFull(FinalDirectory); FPaths::NormalizeDirectoryName(Wanted);
	for (const FString& File : Files)
	{
		const TSharedPtr<FJsonObject> Data = ReadJournal(File);
		FString Original, State;
		FString ValidationError;
		if (!ValidateRecoveryJournal(Data, ValidationError))
		{
			OutJournal = File; return true;
		}
		Data->TryGetStringField(TEXT("original"), Original); Data->TryGetStringField(TEXT("state"), State);
		FPaths::NormalizeDirectoryName(Original);
		if (UnrealToolsRuntime::PathsOverlap(Original, Wanted) && State == TEXT("publishing")) { OutJournal = File; return true; }
	}
	return false;
}

bool AcknowledgeRecovery(const FString& JournalPath, FString& OutError)
{
	const FString Path = FPaths::ConvertRelativePathToFull(JournalPath);
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery"));
	if (!FPaths::IsUnderDirectory(Path, Root)) { OutError = TEXT("Recovery journal must belong to this project's FBX Exporter."); return false; }
	const TSharedPtr<FJsonObject> Data = ReadJournal(Path);
	if (!ValidateRecoveryJournal(Data, OutError)) return false;
	if (UnrealToolsRuntime::GetString(Data, TEXT("state")) == TEXT("acknowledged")) return true;
	Data->SetStringField(TEXT("state"), TEXT("acknowledged"));
	Data->SetBoolField(TEXT("files_unchanged"), true);
	if (!WriteJournal(Path, Data.ToSharedRef())) { OutError = TEXT("Could not persist recovery acknowledgement."); return false; }
	return true;
}

bool ValidateRecoveryJournal(const UnrealToolsRuntime::FObject& Data, FString& OutError)
{
	using namespace UnrealToolsRuntime;
	if (!Data.IsValid() || !Data->HasTypedField<EJson::Number>(TEXT("version")) || GetNumber(Data, TEXT("version")) != 1)
	{ OutError = TEXT("Unreadable or unsupported FBX recovery journal version."); return false; }
	const FString State = GetString(Data, TEXT("state"));
	if ((Data->HasField(TEXT("provider_id")) && GetString(Data, TEXT("provider_id")) != TEXT("fbx_exporter")) ||
		(Data->HasField(TEXT("job_id")) && (!Data->HasTypedField<EJson::String>(TEXT("job_id")) || GetString(Data, TEXT("job_id")).IsEmpty())))
	{ OutError = TEXT("Foreign or invalid FBX recovery journal owner."); return false; }
	if (!TSet<FString>{TEXT("publishing"), TEXT("committed"), TEXT("complete"), TEXT("restored"), TEXT("acknowledged")}.Contains(State))
	{ OutError = TEXT("Unsupported FBX recovery journal state."); return false; }
	FString Original, Staging, Backup;
	if (!Data->TryGetStringField(TEXT("original"), Original) || !Data->TryGetStringField(TEXT("staging"), Staging) || !Data->TryGetStringField(TEXT("backup"), Backup) ||
		Original.IsEmpty() || Staging.IsEmpty() || Backup.IsEmpty() || FPaths::IsRelative(Original) || FPaths::IsRelative(Staging) || FPaths::IsRelative(Backup) ||
		CanonicalPath(Backup) != CanonicalPath(Staging + TEXT("_Previous")) || PathsOverlap(Original, Staging) || PathsOverlap(Original, Backup))
	{ OutError = TEXT("Invalid FBX recovery original/staging/backup paths."); return false; }
	for (const auto& Pair : {TPair<const TCHAR*, FString>(TEXT("original_files"), Original), TPair<const TCHAR*, FString>(TEXT("staged_files"), Staging)})
	{
		const TArray<TSharedPtr<FJsonValue>>* Files;
		if (!Data->TryGetArrayField(Pair.Key, Files)) { OutError = TEXT("FBX recovery file list must be an array."); return false; }
		for (const auto& Value : *Files)
		{
			FString File;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(File) || FPaths::IsRelative(File) || !FPaths::IsUnderDirectory(File, Pair.Value))
			{ OutError = TEXT("FBX recovery file list contains an invalid or foreign path."); return false; }
		}
	}
	return true;
}

void AnnotateRecoveryStatus(const UnrealToolsRuntime::FObject& Job)
{
	using namespace UnrealToolsRuntime;
	for (const auto& Item : GetArray(Job, TEXT("items")))
	{
		const auto Result = GetObject(Item->AsObject(), TEXT("result"));
		const FString Journal = GetString(Result, TEXT("recovery_journal"));
		if (Journal.IsEmpty()) continue;
		const bool bMissing = !IFileManager::Get().FileExists(*Journal);
		Result->SetBoolField(TEXT("recovery_journal_missing"), bMissing);
		if (bMissing && GetBool(Job, TEXT("recovery_required")))
		{
			const FString Warning = TEXT("Recorded recovery journal is missing; publishing may not have started. Inspect output, staging and backup before explicit acknowledgement: ") + Journal;
			auto Warnings = GetArray(Result, TEXT("warnings"));
			if (!Warnings.ContainsByPredicate([&Warning](const auto& Value) { return Value->AsString() == Warning; })) Warnings.Add(MakeShared<FJsonValueString>(Warning));
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
	}
}

namespace
{
	struct FRecoveryScan
	{
		explicit FRecoveryScan(const FString& InRoot) : Root(FPaths::ConvertRelativePathToFull(InRoot)) {}
		FString Root;
		TSet<FString> Recorded; TArray<UnrealToolsRuntime::FObject> Page; TArray<FString> Files;
		int32 Offset = 0, PageIndex = 0, FileIndex = 0; bool bArchive = false, bJobsDone = false, bFilesListed = false;
	};
	TUniquePtr<FRecoveryScan> RecoveryScan;
	bool HasCurrentRecoveryReference(UnrealToolsRuntime::FJobService& Service, const UnrealToolsRuntime::FObject& Data, const FString& File)
	{
		using namespace UnrealToolsRuntime;
		const FString Source = CanonicalPath(FPaths::ConvertRelativePathToFull(File));
		auto References = [&Source](const FObject& Job)
		{
			const FString State = GetString(Job, TEXT("state"));
			if (State != TEXT("queued") && State != TEXT("running") && !GetBool(Job, TEXT("recovery_required"))) return false;
			for (const auto& Item : GetArray(Job, TEXT("items")))
			{
				const FString Journal = GetString(GetObject(Item->AsObject(), TEXT("result")), TEXT("recovery_journal"));
				if (!Journal.IsEmpty() && CanonicalPath(FPaths::ConvertRelativePathToFull(Journal)) == Source) return true;
			}
			return false;
		};
		const FString Owner = GetString(Data, TEXT("job_id"));
		if (!Owner.IsEmpty())
		{
			auto Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("job_id"), Owner);
			return References(Service.Status(Selector));
		}
		// Older domain journals can omit job_id. Consult current active and
		// protected recovery references, never infer ownership from output paths.
		for (const auto& Job : Service.GetActiveJobs()) if (References(Job)) return true;
		for (bool Archived : {false, true})
			for (int32 Offset = 0; ; Offset += 50)
			{
				const auto Page = Service.GetJobSummaries(Offset, 50, Archived);
				if (Page.IsEmpty()) break;
				for (const auto& Summary : Page)
					if (GetBool(Summary, TEXT("recovery_required")))
						for (const auto& Journal : GetArray(Summary, TEXT("recovery_journals")))
							if (CanonicalPath(FPaths::ConvertRelativePathToFull(Journal->AsString())) == Source) return true;
			}
		return false;
	}
	bool AdvanceRecoveryScan(FRecoveryScan& Scan, UnrealToolsRuntime::FJobService& Service, int32 Budget)
{
	using namespace UnrealToolsRuntime;
	for (int32 Work = 0; Work < Budget; ++Work)
	{
		if (!Scan.bJobsDone)
		{
			if (Scan.PageIndex >= Scan.Page.Num())
			{
				Scan.Page = Service.GetJobSummaries(Scan.Offset, 50, Scan.bArchive); Scan.PageIndex = 0;
				if (Scan.Page.IsEmpty()) { if (!Scan.bArchive) { Scan.bArchive = true; Scan.Offset = 0; continue; } Scan.bJobsDone = true; continue; }
				Scan.Offset += Scan.Page.Num();
			}
			const auto Summary = Scan.Page[Scan.PageIndex++];
			const FString State = GetString(Summary, TEXT("state"));
			if (!GetBool(Summary, TEXT("recovery_required")) && State != TEXT("queued") && State != TEXT("running")) continue;
			auto Selector = MakeShared<FJsonObject>(); Selector->SetStringField(TEXT("job_id"), GetString(Summary, TEXT("job_id")));
			for (const auto& Item : GetArray(Service.Status(Selector), TEXT("items")))
			{
				const FString Journal = GetString(GetObject(Item->AsObject(), TEXT("result")), TEXT("recovery_journal"));
				if (!Journal.IsEmpty()) Scan.Recorded.Add(CanonicalPath(FPaths::ConvertRelativePathToFull(Journal)));
			}
			continue;
		}
		if (!Scan.bFilesListed)
		{
			IFileManager::Get().FindFilesRecursive(Scan.Files, *Scan.Root, TEXT("*.json"), true, false);
			Scan.bFilesListed = true;
		}
		if (Scan.FileIndex >= Scan.Files.Num()) return true;
		const FString File = FPaths::ConvertRelativePathToFull(Scan.Files[Scan.FileIndex++]); const auto Data = ReadJournal(File); FString Error;
		if (!ValidateRecoveryJournal(Data, Error)) Service.RegisterRecordIssue(File, Error);
		else if (GetString(Data, TEXT("state")) == TEXT("publishing") && !Scan.Recorded.Contains(CanonicalPath(File)) && !HasCurrentRecoveryReference(Service, Data, File))
			Service.RegisterRecordIssue(File, TEXT("FBX publishing journal has no owning job record."));
	}
	return false;
}
}
void RegisterRecoveryDiagnostics() { RecoveryScan = MakeUnique<FRecoveryScan>(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery")); }
void CancelRecoveryDiagnostics() { RecoveryScan.Reset(); }
bool AdvanceRecoveryDiagnostics(int32 Budget)
{
	if (!RecoveryScan) return true;
	if (!AdvanceRecoveryScan(*RecoveryScan, Jobs(), Budget)) return false;
	RecoveryScan.Reset();
	return true;
}

FPreflightResult PreflightStaticMesh(UStaticMesh* StaticMesh, const FFBXExporterOptions& Options)
{
	FPreflightResult Result;
	if (!IsInGameThread())
	{
		Result.Errors.Add(TEXT("FBX export requires the Editor game thread."));
		return Result;
	}
	if (!StaticMesh || !StaticMesh->IsAsset())
	{
		Result.Errors.Add(TEXT("A Static Mesh asset is required."));
		return Result;
	}
	if (!GEditor || GEditor->PlayWorld || GEditor->bIsSimulatingInEditor)
	{
		Result.Errors.Add(TEXT("Stop PIE before exporting Static Meshes."));
	}
	if (!StaticMesh->HasValidRenderData() || StaticMesh->GetNumLODs() <= 0)
	{
		Result.Errors.Add(TEXT("Static Mesh has no valid LOD0 render data."));
	}
	else if (Options.TextureMaps != EFBXExporterTextureMap::None &&
		StaticMesh->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		Result.Errors.Add(TEXT("Selected texture maps require UV0."));
	}
	if (Options.TextureMaps != EFBXExporterTextureMap::None)
	{
		if (Options.BakeResolution <= 0 || Options.BakeResolution > 16384)
		{
			Result.Errors.Add(TEXT("Bake resolution must be between 1 and 16384 pixels."));
		}
		if (!FApp::CanEverRender() || !GDynamicRHI || GUsingNullRHI)
		{
			Result.Errors.Add(TEXT("Texture export requires a rendering Editor (no -nullrhi)."));
		}
	}
	if ((static_cast<uint8>(Options.TextureMaps) & ~static_cast<uint8>(EFBXExporterTextureMap::All)) != 0)
	{
		Result.Errors.Add(TEXT("TextureMaps contains unsupported map bits."));
	}
	if (Options.OutputRoot.TrimStartAndEnd().IsEmpty())
	{
		Result.Errors.Add(TEXT("Output folder is empty."));
		return Result;
	}
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(Options.OutputRoot);
	if (IFileManager::Get().FileExists(*OutputRoot))
	{
		Result.Errors.Add(TEXT("Output root is a file, not a directory."));
	}
	const FString MeshName = MakeSafeFileName(StaticMesh->GetName());
	Result.FinalDirectory = OutputRoot / MeshName;
	if (Options.OutputLayout == TEXT("asset_path"))
	{
		FString Package = StaticMesh->GetOutermost()->GetName(); Package.RemoveFromStart(TEXT("/"));
		TArray<FString> Components; Package.ParseIntoArray(Components, TEXT("/"), true);
		FString Relative;
		for (const FString& Component : Components) Relative /= MakeSafeFileName(Component);
		Result.FinalDirectory = OutputRoot / Relative;
	}
	else if (Options.OutputLayout != TEXT("mesh_name")) Result.Errors.Add(TEXT("output_layout must be mesh_name or asset_path."));
	FPaths::NormalizeDirectoryName(Result.FinalDirectory);
	FPaths::CollapseRelativeDirectories(Result.FinalDirectory);
	FString Journal;
	if (IsRecoveryBlocked(Result.FinalDirectory, Journal)) Result.Errors.Add(FString::Printf(TEXT("Output requires recovery acknowledgement before reuse: %s"), *Journal));
	Result.FbxFile = Result.FinalDirectory / (MeshName + TEXT(".fbx"));
	if (Options.TextureMaps != EFBXExporterTextureMap::None)
	{
		Result.MetadataFile = Result.FinalDirectory / (MeshName + TEXT("_Materials.json"));
	}
	if (IFileManager::Get().FileExists(*Result.FinalDirectory))
	{
		Result.Errors.Add(TEXT("The mesh output directory is blocked by a file."));
	}
	Result.bRequiresOverwrite = IFileManager::Get().DirectoryExists(*Result.FinalDirectory);
	if (Result.bRequiresOverwrite)
	{
		Result.Warnings.Add(TEXT("Overwrite replaces the entire mesh output folder, including user files."));
		IFileManager::Get().FindFilesRecursive(Result.ReplacedFiles, *Result.FinalDirectory, TEXT("*"), true, false);
		Result.ReplacedFiles.Sort();
	}
	Result.PlannedFiles.Add(Result.FbxFile);
	if (!Result.MetadataFile.IsEmpty()) Result.PlannedFiles.Add(Result.MetadataFile);
	if (Options.TextureMaps != EFBXExporterTextureMap::None && StaticMesh->HasValidRenderData() && StaticMesh->GetNumLODs() > 0)
	{
		TArray<FFBXExporterSlotData> Slots; BuildSlotNames(StaticMesh, Slots);
		for (const auto& Slot : Slots)
		{
			if (!Slot.bHasMaterial || !StaticMesh->GetRenderData()->LODResources[0].Sections.ContainsByPredicate([&Slot](const FStaticMeshSection& Section) { return Section.MaterialIndex == Slot.SlotIndex; })) continue;
			for (const FString& Map : GetTextureMapNames())
				if (IsMapSelected(Options.TextureMaps, Map))
				{
					const FString PhysicalMap = Map == TEXT("Opacity") ? GetOpacityMapName(StaticMesh->GetMaterial(Slot.SlotIndex)) : Map;
					Result.PlannedFiles.Add(Result.FinalDirectory / TEXT("Textures") / GetMapFileName(Slot.SlotName, PhysicalMap));
				}
		}
	}
	Result.PlannedFiles.Sort();
	Result.bCanExport = Result.Errors.IsEmpty();
	return Result;
}

FFBXExporterResult ExportStaticMeshUnattended(UStaticMesh* StaticMesh, const FFBXExporterOptions& Options, const bool bOverwrite)
{
	FFBXExporterResult Result;
	if (!StaticMesh) { Result.Errors.Add(TEXT("A Static Mesh asset is required.")); return Result; }
	UnrealToolsRuntime::FItemSpec Spec; Spec.MeshPath = StaticMesh->GetPathName(); Spec.Options = OptionsToJson(Options); Spec.bOverwrite = bOverwrite;
	FString Error;
	const auto Data = Jobs().RunInline(Spec, Error);
	if (!Data.IsValid()) { Result.Errors.Add(Error); return Result; }
	Result.bSuccess = UnrealToolsRuntime::GetBool(Data, TEXT("success")); Result.FinalDirectory = UnrealToolsRuntime::GetString(Data, TEXT("final_directory"));
	for (const auto& Value : UnrealToolsRuntime::GetArray(Data, TEXT("errors"))) Result.Errors.Add(Value->AsString());
	for (const auto& Value : UnrealToolsRuntime::GetArray(Data, TEXT("warnings"))) Result.Warnings.Add(Value->AsString());
	if (!Result.bSuccess && Result.Errors.IsEmpty()) Result.Errors.Add(UnrealToolsRuntime::GetString(Data, TEXT("error"), Error));
	Result.RecoveryJournal = UnrealToolsRuntime::GetString(Data, TEXT("recovery_journal")); Result.RecoveryStaging = UnrealToolsRuntime::GetString(Data, TEXT("recovery_staging")); Result.RecoveryBackup = UnrealToolsRuntime::GetString(Data, TEXT("recovery_backup"));
	return Result;
}

namespace Testing
{
	bool PublishStagedFiles(const FString& StagingDirectory, const FString& FinalDirectory, FString& OutError)
	{
		return PublishStagedDirectory(StagingDirectory, FinalDirectory, OutError);
	}
	bool PublishWithJournal(const FString& Stage, const FString& Final, const FString& Journal, FString& Error, const FString& JobId)
	{ return PublishStagedDirectory(Stage, Final, Error, Journal, JobId); }
	void ScanRecoveryDiagnostics(UnrealToolsRuntime::FJobService& Service, const FString& RecoveryRoot, const TFunction<void()>& BeforeFileScan)
	{
		FRecoveryScan Scan(RecoveryRoot);
		while (!Scan.bJobsDone) if (AdvanceRecoveryScan(Scan, Service, 1)) return;
		if (BeforeFileScan) BeforeFileScan();
		while (!AdvanceRecoveryScan(Scan, Service, 8)) {}
	}
	void SetPublishFailure(const FString& FailurePoint) { PublishFailurePoint = FailurePoint; }
	bool ReadLinearTexture(UTexture2D* Texture, int32 Width, int32 Height, TArray<FLinearColor>& Pixels)
	{ return LoadTextureLinearPixels(Texture, Width, Height, Texture && Texture->SRGB, Pixels); }
	bool ReadDirectEmission(UTexture2D* Texture, int32 Width, int32 Height, TArray<FColor>& Pixels, float& Scale)
	{ FDirectTextureBinding Binding; Binding.Texture = Texture; return LoadDirectTexturePixels(Binding, Width, Height, true, Pixels, &Scale); }
	bool IsDirectTextureReusable(UMaterialInterface* Material, const FString& Property)
	{ FDirectTextureBinding Binding; return FindDirectTexture(Material, Property == TEXT("Metallic") ? MP_Metallic : MP_EmissiveColor, Binding); }
}

FString MakeSafeFileName(const FString& InName)
{
	FString Result = InName.TrimStartAndEnd();
	const FString InvalidCharacters = TEXT("<>:\"/\\|?*");
	for (int32 Index = 0; Index < Result.Len(); ++Index)
	{
		int32 InvalidCharacterIndex = INDEX_NONE;
		if (InvalidCharacters.FindChar(Result[Index], InvalidCharacterIndex))
		{
			Result[Index] = TEXT('_');
		}
	}
	while (Result.EndsWith(TEXT(".")) || Result.EndsWith(TEXT(" "))) Result.LeftChopInline(1);
	const FString Base = FPaths::GetBaseFilename(Result).ToUpper();
	const bool bReserved = Base == TEXT("CON") || Base == TEXT("PRN") || Base == TEXT("AUX") || Base == TEXT("NUL")
		|| (Base.Len() == 4 && (Base.StartsWith(TEXT("COM")) || Base.StartsWith(TEXT("LPT"))) && Base[3] >= TEXT('1') && Base[3] <= TEXT('9'));
	if (bReserved) Result = TEXT("_") + Result;
	return Result.IsEmpty() || Result == TEXT(".") || Result == TEXT("..") ? TEXT("Unnamed") : Result;
}

EFBXExporterTextureMap GetRequiredTextureMapsForBake(const EFBXExporterTextureMap SelectedMaps)
{
	return GetRequiredBakeMaps(SelectedMaps);
}

void BuildMaskPixels(
	const TArray<FColor>& Metallic,
	const TArray<FColor>& AmbientOcclusion,
	const TArray<FColor>& Roughness,
	const int32 Width,
	const int32 Height,
	TArray<FColor>& OutPixels)
{
	if (Width <= 0 || Height <= 0)
	{
		OutPixels.Reset();
		return;
	}
	const int32 PixelCount = Width * Height;
	OutPixels.SetNum(PixelCount);

	auto ReadChannel = [](const TArray<FColor>& Pixels, const int32 Index, const uint8 DefaultValue) -> uint8
	{
		if (Pixels.Num() == 1)
		{
			return Pixels[0].R;
		}
		return Pixels.IsValidIndex(Index) ? Pixels[Index].R : DefaultValue;
	};

	for (int32 Index = 0; Index < PixelCount; ++Index)
	{
		const uint8 MetallicValue = ReadChannel(Metallic, Index, 0);
		const uint8 OcclusionValue = ReadChannel(AmbientOcclusion, Index, 255);
		const uint8 RoughnessValue = ReadChannel(Roughness, Index, 128);
		OutPixels[Index] = FColor(MetallicValue, OcclusionValue, 255, 255 - RoughnessValue);
	}
}

void PopulateContentBrowserMenu(UToolMenu* Menu)
{
	if (!Menu)
	{
		return;
	}

	const UContentBrowserAssetContextMenuContext* Context =
		Menu->FindContext<UContentBrowserAssetContextMenuContext>();
	if (!Context || Context->SelectedAssets.IsEmpty())
	{
		return;
	}

	TArray<FAssetData> SelectedAssets;
	SelectedAssets.Reserve(Context->SelectedAssets.Num());
	for (const FAssetData& AssetData : Context->SelectedAssets)
	{
		if (!AssetData.IsInstanceOf(UStaticMesh::StaticClass()) || !Cast<UStaticMesh>(AssetData.GetAsset()))
		{
			return;
		}
		SelectedAssets.Add(AssetData);
	}
	FToolMenuSection& Section = Menu->AddSection(
		TEXT("FBX_Exporter"),
		LOCTEXT("FBXExporterHeading", "FBX_Exporter"));
	Section.AddMenuEntry(
		TEXT("FBX_Exporter.ExportStaticMesh"),
		LOCTEXT("ExportStaticMeshesWithTextures", "Export Static Meshes + Textures"),
		LOCTEXT("ExportStaticMeshesWithTexturesTooltip", "Export all selected Static Meshes as separate FBX files and texture folders."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
		{
			AddAssetsToTab(SelectedAssets);
		})));
}

FString LastOutputRoot()
{
	FString Value;
	GConfig->GetString(TEXT("/Script/FBX_Exporter.FBX_ExporterSettings"), TEXT("LastOutputRoot"), Value, GEditorPerProjectIni);
	return Value;
}

}

#undef LOCTEXT_NAMESPACE
