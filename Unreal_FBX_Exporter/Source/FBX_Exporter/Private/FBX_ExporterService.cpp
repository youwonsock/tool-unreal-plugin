#include "FBX_ExporterService.h"
#include "FBX_ExporterFbxMedia.h"

#include "AssetExportTask.h"
#include "AssetRegistry/AssetData.h"
#include "Components/StaticMeshComponent.h"
#include "ContentBrowserMenuContexts.h"
#include "DesktopPlatformModule.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "ImageUtils.h"
#include "Interfaces/IMainFrameModule.h"
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
#include "Materials/MaterialParameters.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentAdapter.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Notifications/NotificationManager.h"

#define LOCTEXT_NAMESPACE "FBX_Exporter"

namespace FBXExporter
{
namespace
{
	constexpr TCHAR ConfigSection[] = TEXT("/Script/FBX_Exporter.FBX_ExporterSettings");
	constexpr TCHAR LegacyHavenConfigSection[] = TEXT("/Script/HavenEditor.HavenStaticMeshTextureExportSettings");
	constexpr TCHAR LegacyAssetImportConfigSection[] = TEXT("/Script/AssetImportProjectEditor.AssetImportProjectStaticMeshTextureExportSettings");
	constexpr TCHAR ConfigLastOutputRoot[] = TEXT("LastOutputRoot");

	FString GetLastOutputRoot()
	{
		FString LastOutputRoot;
		const bool bHasNewValue = GConfig->GetString(ConfigSection, ConfigLastOutputRoot, LastOutputRoot, GEditorPerProjectIni)
			&& !LastOutputRoot.IsEmpty();
		if (!bHasNewValue)
		{
			GConfig->GetString(LegacyHavenConfigSection, ConfigLastOutputRoot, LastOutputRoot, GEditorPerProjectIni);
		}
		if (LastOutputRoot.IsEmpty())
		{
			GConfig->GetString(LegacyAssetImportConfigSection, ConfigLastOutputRoot, LastOutputRoot, GEditorPerProjectIni);
		}
		if (!bHasNewValue && !LastOutputRoot.IsEmpty())
		{
			GConfig->SetString(ConfigSection, ConfigLastOutputRoot, *LastOutputRoot, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
		}
		return LastOutputRoot;
	}

	struct FBakedMaterialData
	{
		TMap<FString, TArray<FColor>> MapPixels;
		TArray<FString> Warnings;
		float EmissionScale = 1.0f;
	};

	struct FExportDialogState
	{
		bool bAccepted = false;
		FString OutputRoot;
		int32 BakeResolution = 2048;
		bool bReuseDirectTextures = true;
		EFBXExporterTextureMap TextureMaps = EFBXExporterTextureMap::All;
		bool bEmbedTexturesInFbx = false;
	};

	struct FBatchExportSummary
	{
		int32 RequestedCount = 0;
		int32 SucceededCount = 0;
		FString OutputRoot;
		TArray<FString> Warnings;
		TArray<FString> Errors;
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

	bool LoadTexturePixels(
		UTexture2D* Texture,
		const int32 TargetWidth,
		const int32 TargetHeight,
		const bool bSourceSRGB,
		TArray<FColor>& OutPixels)
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
		SourceImage.CopyTo(ConvertedImage, ERawImageFormat::BGRA8, EGammaSpace::Linear);

		const TArrayView64<const FColor> ConvertedView = ConvertedImage.AsBGRA8();
		TArray<FColor> ConvertedPixels;
		ConvertedPixels.Append(ConvertedView.GetData(), static_cast<int32>(ConvertedView.Num()));
		return ResizePixels(
			ConvertedPixels,
			ConvertedImage.SizeX,
			ConvertedImage.SizeY,
			false,
			TargetWidth,
			TargetHeight,
			OutPixels);
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

	bool LoadDirectTexturePixels(
		const FDirectTextureBinding& Binding,
		const int32 TargetWidth,
		const int32 TargetHeight,
		const bool bSRGB,
		TArray<FColor>& OutPixels)
	{
		if (!Binding.Texture)
		{
			return false;
		}
		TArray<FColor> SourcePixels;
		// Decode according to the source texture's SRGB flag, then encode the
		// generated PNG in its destination color space below.
		if (!LoadTexturePixels(Binding.Texture, TargetWidth, TargetHeight, Binding.Texture->SRGB, SourcePixels))
		{
			return false;
		}
		if (Binding.Channel == EDirectTextureChannel::RGB || Binding.Channel == EDirectTextureChannel::RGBA)
		{
			if (bSRGB)
			{
				for (FColor& Pixel : SourcePixels)
				{
					// LoadTexturePixels stores a quantized *linear* value.  The
					// FLinearColor(FColor) constructor assumes sRGB input, so use
					// the explicit linear reinterpretation before encoding.
					Pixel = Pixel.ReinterpretAsLinear().ToFColorSRGB();
				}
			}
			OutPixels = MoveTemp(SourcePixels);
			return true;
		}
		for (FColor& Pixel : SourcePixels)
		{
			uint8 Value = Pixel.R;
			switch (Binding.Channel)
			{
			case EDirectTextureChannel::G: Value = Pixel.G; break;
			case EDirectTextureChannel::B: Value = Pixel.B; break;
			case EDirectTextureChannel::A: Value = Pixel.A; break;
			default: break;
			}
			Pixel = FColor(Value, Value, Value, 255);
		}
		if (bSRGB)
		{
			for (FColor& Pixel : SourcePixels)
			{
				const float LinearValue = static_cast<float>(Pixel.R) / 255.0f;
				const FColor Encoded = FLinearColor(LinearValue, LinearValue, LinearValue, 1.0f).ToFColorSRGB();
				Pixel = FColor(Encoded.R, Encoded.G, Encoded.B, 255);
			}
		}
		OutPixels = MoveTemp(SourcePixels);
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
		if (!Material)
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

		IMaterialBakingModule& MaterialBakingModule = FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
		MaterialBakingModule.SetEmissiveHDR(false);
		MaterialBakingModule.SetLinearBake(true);
		if (!MaterialSettings.PropertySizes.IsEmpty())
		{
			MaterialBakingModule.BakeMaterials(MaterialSettingsArray, MeshSettingsArray, BakeOutput);
		}
		// The baking module keeps this as process-wide state.  Restore its
		// ordinary editor default after the synchronous bake has captured its
		// per-property color-space metadata.
		MaterialBakingModule.SetLinearBake(false);
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
			if (!GetProperty(MP_EmissiveColor, FColor::Black, true, Emission))
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
		FString& OutError)
	{
		if (!IFileManager::Get().DirectoryExists(*StagingDirectory))
		{
			OutError = FString::Printf(TEXT("Staging directory does not exist: %s"), *StagingDirectory);
			return false;
		}

		const FString StagingRoot = FPaths::ConvertRelativePathToFull(StagingDirectory);
		const FString FinalRoot = FPaths::ConvertRelativePathToFull(FinalDirectory);
		const FString BackupDirectory = StagingRoot + TEXT("_Previous");
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
					// The output was restored and the temporary backup was removed.
				}
				else
				{
					OutError += FString::Printf(TEXT(" Existing output could not be fully restored. Preserve staging at '%s' and backup at '%s'."), *StagingRoot, *BackupDirectory);
				}
				return false;
			}
		}

		TArray<FString> StagedFiles;
		IFileManager::Get().FindFilesRecursive(StagedFiles, *StagingRoot, TEXT("*"), true, false);
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
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationPath), true) ||
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

		// Anything not present in the staging tree is intentionally removed when
		// the backup is discarded. This makes the mesh folder plugin-owned while
		// retaining a complete backup until all publish steps have succeeded.
		if (IFileManager::Get().DirectoryExists(*BackupDirectory) &&
			!IFileManager::Get().DeleteDirectory(*BackupDirectory, false, true))
		{
			OutError = FString::Printf(TEXT("Failed to remove the previous output backup: %s"), *BackupDirectory);
			RestoreAndCleanupBackup();
			return false;
		}
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

	FFBXExporterResult ExportStaticMesh(UStaticMesh* StaticMesh, const FFBXExporterOptions& Options)
	{
		FFBXExporterResult Result;
		if (!StaticMesh)
		{
			Result.Errors.Add(TEXT("No Static Mesh was selected."));
			return Result;
		}
		if (!StaticMesh->HasValidRenderData() || StaticMesh->GetNumLODs() <= 0)
		{
			Result.Errors.Add(FString::Printf(TEXT("Static Mesh has no valid LOD0 render data: %s"), *StaticMesh->GetPathName()));
			return Result;
		}
		if (Options.OutputRoot.IsEmpty())
		{
			Result.Errors.Add(TEXT("Output folder is empty."));
			return Result;
		}
		if (Options.TextureMaps != EFBXExporterTextureMap::None &&
			(Options.BakeResolution <= 0 || Options.BakeResolution > 16384))
		{
			Result.Errors.Add(TEXT("Bake resolution must be between 1 and 16384 pixels."));
			return Result;
		}
		const uint8 KnownTextureMapBits = static_cast<uint8>(EFBXExporterTextureMap::All);
		if ((static_cast<uint8>(Options.TextureMaps) & static_cast<uint8>(~KnownTextureMapBits)) != 0)
		{
			Result.Errors.Add(TEXT("TextureMaps contains unsupported map bits."));
			return Result;
		}

		const FString MeshName = MakeSafeFileName(StaticMesh->GetName());
		const FString FinalDirectory = FPaths::Combine(Options.OutputRoot, MeshName);
		if (IFileManager::Get().DirectoryExists(*FinalDirectory))
		{
			const EAppReturnType::Type OverwriteChoice = FMessageDialog::Open(
				EAppMsgType::YesNo,
				FText::Format(
					LOCTEXT("OverwriteExistingMeshFolder", "The entire export folder for {0}, including user files, will be replaced. Continue?"),
					FText::FromString(StaticMesh->GetName())));
			if (OverwriteChoice != EAppReturnType::Yes)
			{
				return Result;
			}
		}
		if (!IFileManager::Get().MakeDirectory(*Options.OutputRoot, true))
		{
			Result.Errors.Add(FString::Printf(TEXT("Failed to create output root: %s"), *Options.OutputRoot));
			return Result;
		}

		const FString StagingDirectory = Options.OutputRoot / (TEXT(".FBX_Exporter_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!IFileManager::Get().MakeDirectory(*StagingDirectory, true))
		{
			Result.Errors.Add(FString::Printf(TEXT("Failed to create staging directory: %s"), *StagingDirectory));
			return Result;
		}
		bool bKeepBackup = false;
		const auto CleanupStaging = [&StagingDirectory, &bKeepBackup]()
		{
			if (bKeepBackup)
			{
				return;
			}
			IFileManager::Get().DeleteDirectory(*StagingDirectory, false, true);
			IFileManager::Get().DeleteDirectory(*(StagingDirectory + TEXT("_Previous")), false, true);
		};

		TArray<FFBXExporterSlotData> Slots;
		BuildSlotNames(StaticMesh, Slots);
		const FString StagedFbxPath = StagingDirectory / (MeshName + TEXT(".fbx"));
		const FString FinalFbxPath = FinalDirectory / (MeshName + TEXT(".fbx"));
		FString Error;
		if (!ExportFbx(StaticMesh, StagedFbxPath, Error))
		{
			Result.Errors.Add(Error);
			CleanupStaging();
			return Result;
		}

		TArray<FFBXExporterTextureFileData> TextureFiles;
		if (Options.TextureMaps != EFBXExporterTextureMap::None)
		{
			for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
			{
				UMaterialInterface* Material = StaticMesh->GetMaterial(SlotIndex);
				if (!Material)
				{
					Result.Warnings.Add(FString::Printf(TEXT("Material slot %d has no assigned material; no textures were generated."), SlotIndex));
					continue;
				}
				FBakedMaterialData MaterialData;
				if (!BakeMaterial(StaticMesh, SlotIndex, Material, Options, MaterialData, Error))
				{
					Result.Errors.Add(Error);
					CleanupStaging();
					return Result;
				}
				Slots[SlotIndex].EmissionScale = MaterialData.EmissionScale;
				TArray<FString> SortedMapNames;
				MaterialData.MapPixels.GetKeys(SortedMapNames);
				SortedMapNames.Sort();
				for (const FString& MapName : SortedMapNames)
				{
					const TArray<FColor>* MapPixels = MaterialData.MapPixels.Find(MapName);
					if (!MapPixels)
					{
						Result.Errors.Add(FString::Printf(TEXT("Missing generated pixel data for map %s."), *MapName));
						CleanupStaging();
						return Result;
					}
					const bool bSRGB = MapName == TEXT("BaseColor") || MapName == TEXT("Emission");
					FString RelativePath = FPaths::Combine(TEXT("Textures"), GetMapFileName(Slots[SlotIndex].SlotName, MapName));
					RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
					const FString AbsolutePath = StagingDirectory / RelativePath;
					if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), true) ||
						!SaveImage(AbsolutePath, *MapPixels, Options.BakeResolution, Options.BakeResolution, bSRGB, Error))
					{
						Result.Errors.Add(Error);
						CleanupStaging();
						return Result;
					}
					FFBXExporterTextureFileData TextureFile;
					TextureFile.MapName = MapName;
					TextureFile.RelativePath = RelativePath;
					TextureFile.StagedAbsolutePath = AbsolutePath;
					TextureFile.FinalAbsolutePath = FinalDirectory / RelativePath;
					TextureFile.ColorSpace = bSRGB ? TEXT("sRGB") : TEXT("Linear");
					if (!FFileHelper::LoadFileToArray(TextureFile.FileBytes, *AbsolutePath))
					{
						Result.Errors.Add(FString::Printf(TEXT("Failed to read generated texture: %s"), *AbsolutePath));
						CleanupStaging();
						return Result;
					}
					Slots[SlotIndex].MapRelativePaths.Add(MapName, RelativePath);
					TextureFiles.Add(MoveTemp(TextureFile));
				}
				Result.Warnings.Append(MaterialData.Warnings);
			}
		}

		TArray<FFBXExporterSectionData> Sections;
		const FStaticMeshLODResources& LOD0 = StaticMesh->GetRenderData()->LODResources[0];
		for (const FStaticMeshSection& Section : LOD0.Sections)
		{
			FFBXExporterSectionData SectionData;
			SectionData.SlotIndex = Section.MaterialIndex;
			SectionData.TriangleCount = Section.NumTriangles;
			SectionData.bHasMaterial = Slots.IsValidIndex(Section.MaterialIndex) && Slots[Section.MaterialIndex].bHasMaterial;
			Sections.Add(SectionData);
		}
		TArray<FFBXExporterMaterialBinding> Bindings;
		if (!FBXExporter::PostProcessFbx(StagedFbxPath, FinalFbxPath, Sections, Slots, TextureFiles,
			Options.bEmbedTexturesInFbx && Options.TextureMaps != EFBXExporterTextureMap::None, Bindings, Error))
		{
			Result.Errors.Add(Error);
			CleanupStaging();
			return Result;
		}
		if (Options.TextureMaps != EFBXExporterTextureMap::None)
		{
			const FString JsonPath = StagingDirectory / (MeshName + TEXT("_Materials.json"));
			if (!SaveMetadataJson(JsonPath, StaticMesh->GetPathName(), FinalFbxPath, Options, Slots, Bindings, Error))
			{
				Result.Errors.Add(Error);
				CleanupStaging();
				return Result;
			}
		}

		if (!PublishStagedDirectory(StagingDirectory, FinalDirectory, Error))
		{
			Result.Errors.Add(Error);
			bKeepBackup = IFileManager::Get().DirectoryExists(*(StagingDirectory + TEXT("_Previous")));
			CleanupStaging();
			return Result;
		}
		CleanupStaging();
		Result.bSuccess = true;
		Result.FinalDirectory = FinalDirectory;
		return Result;
	}

	void NotifyBatchExportResult(const FBatchExportSummary& Summary)
	{
		if (!Summary.Errors.IsEmpty())
		{
			FString Message = FString::Printf(
				TEXT("Static Mesh texture export finished: %d/%d mesh(es) exported.\n\n%s"),
				Summary.SucceededCount,
				Summary.RequestedCount,
				*FString::Join(Summary.Errors, TEXT("\n")));
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(Message));
			return;
		}

		FNotificationInfo Notification(
			Summary.Warnings.IsEmpty()
				? FText::Format(
					LOCTEXT("StaticMeshTextureExportBatchSucceeded", "Exported {0} Static Mesh(es) to {1}"),
					FText::AsNumber(Summary.SucceededCount),
					FText::FromString(Summary.OutputRoot))
				: FText::Format(
					LOCTEXT("StaticMeshTextureExportBatchWithWarnings", "Exported {0} Static Mesh(es) with {1} warning(s) to {2}"),
					FText::AsNumber(Summary.SucceededCount),
					FText::AsNumber(Summary.Warnings.Num()),
					FText::FromString(Summary.OutputRoot)));
		Notification.ExpireDuration = 6.0f;
		Notification.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Notification);
	}

	void ExecuteExport(const TArray<FAssetData>& AssetData, const FFBXExporterOptions& Options)
	{
		FBatchExportSummary Summary;
		Summary.RequestedCount = AssetData.Num();
		Summary.OutputRoot = Options.OutputRoot;

		TArray<UStaticMesh*> StaticMeshes;
		TSet<FString> MeshFolderNames;
		for (const FAssetData& Asset : AssetData)
		{
			UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset.GetAsset());
			if (!StaticMesh || !StaticMesh->IsAsset())
			{
				Summary.Errors.Add(FString::Printf(TEXT("Not a loadable Static Mesh: %s"), *Asset.GetObjectPathString()));
				continue;
			}

			const FString MeshFolderName = MakeSafeFileName(StaticMesh->GetName());
			const FString MeshFolderKey = MeshFolderName.ToLower();
			if (MeshFolderNames.Contains(MeshFolderKey))
			{
				Summary.Errors.Add(FString::Printf(
					TEXT("Multiple selected assets resolve to the same output folder '%s'. Rename one asset or export it separately."),
					*MeshFolderName));
				continue;
			}

			MeshFolderNames.Add(MeshFolderKey);
			StaticMeshes.Add(StaticMesh);
		}

		if (!Summary.Errors.IsEmpty())
		{
			NotifyBatchExportResult(Summary);
			return;
		}

		GConfig->SetString(ConfigSection, ConfigLastOutputRoot, *Options.OutputRoot, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);

		for (UStaticMesh* StaticMesh : StaticMeshes)
		{
			const FFBXExporterResult Result = ExportStaticMesh(StaticMesh, Options);
			if (Result.Succeeded())
			{
				++Summary.SucceededCount;
			}
			else if (!Result.Errors.IsEmpty())
			{
				for (const FString& Error : Result.Errors)
				{
					Summary.Errors.Add(FString::Printf(TEXT("%s: %s"), *StaticMesh->GetName(), *Error));
				}
			}
			else
			{
				Summary.Warnings.Add(FString::Printf(TEXT("%s was skipped."), *StaticMesh->GetName()));
			}

			for (const FString& Warning : Result.Warnings)
			{
				Summary.Warnings.Add(FString::Printf(TEXT("%s: %s"), *StaticMesh->GetName(), *Warning));
			}
		}

		NotifyBatchExportResult(Summary);
	}
}

namespace Testing
{
	FString ReadAndMigrateLastOutputRoot()
	{
		return GetLastOutputRoot();
	}

	bool PublishStagedFiles(const FString& StagingDirectory, const FString& FinalDirectory, FString& OutError)
	{
		return PublishStagedDirectory(StagingDirectory, FinalDirectory, OutError);
	}
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
			ShowExportDialog(SelectedAssets);
		})));
}

void ShowExportDialog(const TArray<FAssetData>& SelectedAssets)
{
	if (SelectedAssets.IsEmpty())
	{
		return;
	}

	FString LastOutputRoot = GetLastOutputRoot();
	if (LastOutputRoot.IsEmpty())
	{
		LastOutputRoot = FPaths::ProjectDir() / TEXT("Exports/StaticMeshTextures");
	}

	TSharedRef<FExportDialogState> State = MakeShared<FExportDialogState>();
	State->OutputRoot = LastOutputRoot;

	TArray<TSharedPtr<int32>> ResolutionOptions;
	for (const int32 Resolution : {512, 1024, 2048, 4096})
	{
		ResolutionOptions.Add(MakeShared<int32>(Resolution));
	}
	TSharedPtr<int32> SelectedResolution = ResolutionOptions[2];
	TSharedPtr<SEditableTextBox> OutputTextBox;
	TSharedPtr<STextBlock> ResolutionText;
	TSharedPtr<SWindow> Window;

	SAssignNew(Window, SWindow)
		.Title(LOCTEXT("FBXExporterWindowTitle", "FBX_Exporter"))
		.ClientSize(FVector2D(720.0f, 500.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	Window->SetContent(
		SNew(SBorder)
		.Padding(16.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock).Text(FText::Format(
					LOCTEXT("SourceStaticMeshes", "Selected Static Meshes: {0}\nEach mesh is exported to its own folder below the output root."),
					FText::AsNumber(SelectedAssets.Num())))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(OutputTextBox, SEditableTextBox)
					.Text(FText::FromString(State->OutputRoot))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("BrowseOutputFolder", "Browse..."))
					.OnClicked_Lambda([OutputTextBox]()
					{
						if (IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get())
						{
							FString SelectedFolder;
							const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
							if (DesktopPlatform->OpenDirectoryDialog(
								ParentWindowHandle,
								LOCTEXT("SelectStaticMeshTextureOutputFolder", "Select Static Mesh Texture Export Folder").ToString(),
								OutputTextBox->GetText().ToString(),
								SelectedFolder))
							{
								OutputTextBox->SetText(FText::FromString(SelectedFolder));
							}
						}
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("BakeResolutionLabel", "PBR Bake Resolution"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(16.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SComboBox<TSharedPtr<int32>>)
					.OptionsSource(&ResolutionOptions)
					.InitiallySelectedItem(SelectedResolution)
					.OnGenerateWidget_Lambda([](TSharedPtr<int32> Value)
					{
						return SNew(STextBlock).Text(FText::AsNumber(Value.IsValid() ? *Value : 2048));
					})
					.OnSelectionChanged_Lambda([State, &SelectedResolution, &ResolutionText](TSharedPtr<int32> Value, ESelectInfo::Type)
					{
						if (Value.IsValid())
						{
							SelectedResolution = Value;
							State->BakeResolution = *Value;
							if (ResolutionText.IsValid())
							{
								ResolutionText->SetText(FText::AsNumber(*Value));
							}
						}
					})
					[
						SAssignNew(ResolutionText, STextBlock).Text(FText::AsNumber(*SelectedResolution))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SCheckBox)
				.IsChecked(ECheckBoxState::Checked)
				.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
				{
					State->bReuseDirectTextures = NewState == ECheckBoxState::Checked;
				})
				[
					SNew(STextBlock).Text(LOCTEXT("ReuseDirectTextures", "Reuse directly connected source textures when available"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("TextureMapsLabel", "Texture maps"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SUniformGridPanel)
				.SlotPadding(FMargin(4.0f, 2.0f))
				+ SUniformGridPanel::Slot(0, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return IsFBXExporterTextureMapEnabled(State->TextureMaps, EFBXExporterTextureMap::BaseColor) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked) State->TextureMaps |= EFBXExporterTextureMap::BaseColor;
						else State->TextureMaps &= static_cast<EFBXExporterTextureMap>(~static_cast<uint8>(EFBXExporterTextureMap::BaseColor));
					})
					[
						SNew(STextBlock).Text(LOCTEXT("BaseColorMap", "Base Color"))
					]
				]
				+ SUniformGridPanel::Slot(1, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return IsFBXExporterTextureMapEnabled(State->TextureMaps, EFBXExporterTextureMap::Normal) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked) State->TextureMaps |= EFBXExporterTextureMap::Normal;
						else State->TextureMaps &= static_cast<EFBXExporterTextureMap>(~static_cast<uint8>(EFBXExporterTextureMap::Normal));
					})
					[
						SNew(STextBlock).Text(LOCTEXT("NormalMap", "Normal"))
					]
				]
				+ SUniformGridPanel::Slot(2, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return IsFBXExporterTextureMapEnabled(State->TextureMaps, EFBXExporterTextureMap::Mask) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked) State->TextureMaps |= EFBXExporterTextureMap::Mask;
						else State->TextureMaps &= static_cast<EFBXExporterTextureMap>(~static_cast<uint8>(EFBXExporterTextureMap::Mask));
					})
					[
						SNew(STextBlock).Text(LOCTEXT("MaskMap", "Mask"))
					]
				]
				+ SUniformGridPanel::Slot(3, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return IsFBXExporterTextureMapEnabled(State->TextureMaps, EFBXExporterTextureMap::Occlusion) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked) State->TextureMaps |= EFBXExporterTextureMap::Occlusion;
						else State->TextureMaps &= static_cast<EFBXExporterTextureMap>(~static_cast<uint8>(EFBXExporterTextureMap::Occlusion));
					})
					[
						SNew(STextBlock).Text(LOCTEXT("OcclusionMap", "Occlusion"))
					]
				]
				+ SUniformGridPanel::Slot(0, 1)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return IsFBXExporterTextureMapEnabled(State->TextureMaps, EFBXExporterTextureMap::Emission) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked) State->TextureMaps |= EFBXExporterTextureMap::Emission;
						else State->TextureMaps &= static_cast<EFBXExporterTextureMap>(~static_cast<uint8>(EFBXExporterTextureMap::Emission));
					})
					[
						SNew(STextBlock).Text(LOCTEXT("EmissionMap", "Emission"))
					]
				]
				+ SUniformGridPanel::Slot(1, 1)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return IsFBXExporterTextureMapEnabled(State->TextureMaps, EFBXExporterTextureMap::Opacity) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked) State->TextureMaps |= EFBXExporterTextureMap::Opacity;
						else State->TextureMaps &= static_cast<EFBXExporterTextureMap>(~static_cast<uint8>(EFBXExporterTextureMap::Opacity));
					})
					[
						SNew(STextBlock).Text(LOCTEXT("OpacityMap", "Opacity / Opacity Mask"))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("SelectAllTextureMaps", "Select All"))
					.OnClicked_Lambda([State]()
					{
						State->TextureMaps = EFBXExporterTextureMap::All;
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ClearAllTextureMaps", "Clear All"))
					.OnClicked_Lambda([State]()
					{
						State->TextureMaps = EFBXExporterTextureMap::None;
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([State]() { return State->bEmbedTexturesInFbx ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.IsEnabled_Lambda([State]() { return State->TextureMaps != EFBXExporterTextureMap::None; })
				.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState)
				{
					State->bEmbedTexturesInFbx = NewState == ECheckBoxState::Checked;
				})
				[
					SNew(STextBlock).Text(LOCTEXT("EmbedTexturesInFbx", "Embed selected textures in FBX"))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(STextBlock).Text(LOCTEXT(
					"StaticMeshTextureExportDescription",
					"Selected maps are baked or directly reused from UV0. The export creates an FBX, selected PNGs under Textures, and a versioned material JSON file. The Unreal asset is not modified."))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SUniformGridPanel)
				.SlotPadding(FMargin(8.0f, 0.0f))
				+ SUniformGridPanel::Slot(0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelStaticMeshTextureExport", "Cancel"))
					.OnClicked_Lambda([Window]()
					{
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
				+ SUniformGridPanel::Slot(1, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("RunStaticMeshTextureExport", "Export"))
					.OnClicked_Lambda([Window, State, OutputTextBox]()
					{
						State->OutputRoot = OutputTextBox->GetText().ToString().TrimStartAndEnd();
						if (State->OutputRoot.IsEmpty())
						{
							FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MissingUnityOutputFolder", "Select an output folder first."));
							return FReply::Handled();
						}
						if (State->BakeResolution <= 0)
						{
							State->BakeResolution = 2048;
						}
						State->bAccepted = true;
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
			]
		]);

	TSharedPtr<SWindow> ParentWindow;
	if (FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")))
	{
		ParentWindow = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame")).GetParentWindow();
	}
	FSlateApplication::Get().AddModalWindow(Window.ToSharedRef(), ParentWindow, false);
	if (!State->bAccepted)
	{
		return;
	}

	FFBXExporterOptions Options;
	Options.OutputRoot = State->OutputRoot;
	Options.BakeResolution = State->BakeResolution;
	Options.bReuseDirectTextures = State->bReuseDirectTextures;
	Options.TextureMaps = State->TextureMaps;
	Options.bEmbedTexturesInFbx = State->bEmbedTexturesInFbx;
	ExecuteExport(SelectedAssets, Options);
}
}

#undef LOCTEXT_NAMESPACE
