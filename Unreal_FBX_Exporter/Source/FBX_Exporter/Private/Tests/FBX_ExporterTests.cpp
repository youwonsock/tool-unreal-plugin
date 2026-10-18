#include "FBX_ExporterService.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "UObject/StrongObjectPtr.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFBXExporterSafeFileNameTest,
	"FBX_Exporter.Editor.StaticMeshTextureExport.SafeFileName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterSafeFileNameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Invalid path characters are replaced"), FBXExporter::MakeSafeFileName(TEXT("Mesh:01/LOD*0")), TEXT("Mesh_01_LOD_0"));
	TestEqual(TEXT("Empty names receive a stable fallback"), FBXExporter::MakeSafeFileName(TEXT("   ")), TEXT("Unnamed"));
	TestEqual(TEXT("Path traversal components receive a stable fallback"), FBXExporter::MakeSafeFileName(TEXT("..")), TEXT("Unnamed"));
	TestEqual(TEXT("Reserved Windows names are prefixed"), FBXExporter::MakeSafeFileName(TEXT("CON")), TEXT("_CON"));
	TestEqual(TEXT("Trailing dots and spaces cannot alias another output"), FBXExporter::MakeSafeFileName(TEXT("Cube. ")), TEXT("Cube"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFBXExporterMaskPackingTest,
	"FBX_Exporter.Editor.StaticMeshTextureExport.MaskPacking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterMaskPackingTest::RunTest(const FString& Parameters)
{
	const TArray<FColor> Metallic = {FColor(10, 0, 0, 255)};
	const TArray<FColor> AmbientOcclusion = {FColor(20, 0, 0, 255)};
	const TArray<FColor> Roughness = {FColor(30, 0, 0, 255)};
	TArray<FColor> Mask;

	FBXExporter::BuildMaskPixels(Metallic, AmbientOcclusion, Roughness, 2, 2, Mask);
	TestEqual(TEXT("Mask has requested pixel count"), Mask.Num(), 4);
	for (const FColor& Pixel : Mask)
	{
		TestEqual(TEXT("Metallic is packed in R"), Pixel.R, static_cast<uint8>(10));
		TestEqual(TEXT("Ambient Occlusion is packed in G"), Pixel.G, static_cast<uint8>(20));
		TestEqual(TEXT("Mask reserves B with opaque value"), Pixel.B, static_cast<uint8>(255));
		TestEqual(TEXT("Smoothness is inverse roughness in A"), Pixel.A, static_cast<uint8>(225));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFBXExporterTextureMapOptionsTest,
	"FBX_Exporter.Editor.StaticMeshTextureExport.TextureMapOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterTextureMapOptionsTest::RunTest(const FString& Parameters)
{
	const FFBXExporterOptions Defaults;
	TestEqual(TEXT("Texture maps default to All"), Defaults.TextureMaps, EFBXExporterTextureMap::All);
	TestFalse(TEXT("Embedding defaults to disabled"), Defaults.bEmbedTexturesInFbx);

	const EFBXExporterTextureMap BaseColorOnly = EFBXExporterTextureMap::BaseColor;
	const EFBXExporterTextureMap BaseColorDependencies = FBXExporter::GetRequiredTextureMapsForBake(BaseColorOnly);
	TestTrue(TEXT("BaseColor dependency includes opacity"), IsFBXExporterTextureMapEnabled(BaseColorDependencies, EFBXExporterTextureMap::Opacity));
	TestFalse(TEXT("BaseColor dependency does not add unrelated normal"), IsFBXExporterTextureMapEnabled(BaseColorDependencies, EFBXExporterTextureMap::Normal));

	const EFBXExporterTextureMap MaskDependencies = FBXExporter::GetRequiredTextureMapsForBake(EFBXExporterTextureMap::Mask);
	TestTrue(TEXT("Mask dependency includes occlusion"), IsFBXExporterTextureMapEnabled(MaskDependencies, EFBXExporterTextureMap::Occlusion));
	TestTrue(TEXT("Mask dependency includes the mask output"), IsFBXExporterTextureMapEnabled(MaskDependencies, EFBXExporterTextureMap::Mask));

	TestEqual(TEXT("No selected maps has no bake dependency"),
		FBXExporter::GetRequiredTextureMapsForBake(EFBXExporterTextureMap::None),
		EFBXExporterTextureMap::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFBXExporterConfigMigrationTest,
	"FBX_Exporter.Editor.StaticMeshTextureExport.ConfigMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterConfigMigrationTest::RunTest(const FString& Parameters)
{
	const TCHAR* ConfigSection = TEXT("/Script/FBX_Exporter.FBX_ExporterSettings");
	const TCHAR* HavenLegacySection = TEXT("/Script/HavenEditor.HavenStaticMeshTextureExportSettings");
	const TCHAR* AssetImportLegacySection = TEXT("/Script/AssetImportProjectEditor.AssetImportProjectStaticMeshTextureExportSettings");
	const TCHAR* ConfigKey = TEXT("LastOutputRoot");
	const FString IniFile = GEditorPerProjectIni;

	struct FConfigValue
	{
		bool bExists = false;
		FString Value;
	};

	auto ReadValue = [ConfigKey, &IniFile](const TCHAR* Section)
	{
		FConfigValue Result;
		Result.bExists = GConfig->GetString(Section, ConfigKey, Result.Value, IniFile);
		return Result;
	};
	auto RestoreValue = [ConfigKey, &IniFile](const TCHAR* Section, const FConfigValue& Value)
	{
		if (Value.bExists)
		{
			GConfig->SetString(Section, ConfigKey, *Value.Value, IniFile);
		}
		else
		{
			GConfig->RemoveKey(Section, ConfigKey, IniFile);
		}
	};

	const FConfigValue OriginalNew = ReadValue(ConfigSection);
	const FConfigValue OriginalHaven = ReadValue(HavenLegacySection);
	const FConfigValue OriginalAssetImport = ReadValue(AssetImportLegacySection);

	GConfig->RemoveKey(ConfigSection, ConfigKey, IniFile);
	GConfig->SetString(HavenLegacySection, ConfigKey, TEXT("C:/Legacy/Haven"), IniFile);
	GConfig->SetString(AssetImportLegacySection, ConfigKey, TEXT("C:/Legacy/AssetImportProject"), IniFile);
	GConfig->Flush(false, IniFile);

	const FString MigratedPath = FBXExporter::Testing::ReadAndMigrateLastOutputRoot();
	TestEqual(TEXT("Haven legacy section is preferred"), MigratedPath, TEXT("C:/Legacy/Haven"));

	FString NewPath;
	TestTrue(TEXT("Migrated value is written to the new section"), GConfig->GetString(ConfigSection, ConfigKey, NewPath, IniFile));
	TestEqual(TEXT("New section stores the migrated path"), NewPath, TEXT("C:/Legacy/Haven"));

	GConfig->RemoveKey(ConfigSection, ConfigKey, IniFile);
	GConfig->RemoveKey(HavenLegacySection, ConfigKey, IniFile);
	GConfig->SetString(AssetImportLegacySection, ConfigKey, TEXT("C:/Legacy/AssetImportProject"), IniFile);
	GConfig->Flush(false, IniFile);

	const FString AssetImportMigratedPath = FBXExporter::Testing::ReadAndMigrateLastOutputRoot();
	TestEqual(TEXT("AssetImportProject legacy section is used when Haven has no value"), AssetImportMigratedPath, TEXT("C:/Legacy/AssetImportProject"));

	NewPath.Reset();
	TestTrue(TEXT("AssetImportProject value is written to the new section"), GConfig->GetString(ConfigSection, ConfigKey, NewPath, IniFile));
	TestEqual(TEXT("New section stores the AssetImportProject path"), NewPath, TEXT("C:/Legacy/AssetImportProject"));

	RestoreValue(ConfigSection, OriginalNew);
	RestoreValue(HavenLegacySection, OriginalHaven);
	RestoreValue(AssetImportLegacySection, OriginalAssetImport);
	GConfig->Flush(false, IniFile);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFBXExporterStagingRollbackTest,
	"FBX_Exporter.Editor.StaticMeshTextureExport.StagingRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterStagingRollbackTest::RunTest(const FString& Parameters)
{
	const FString RootDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FBX_ExporterTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString StagingDirectory = RootDirectory / TEXT("Staging");
	const FString FinalDirectory = RootDirectory / TEXT("Final");
	IFileManager::Get().MakeDirectory(*StagingDirectory, true);
	IFileManager::Get().MakeDirectory(*FinalDirectory, true);

	const FString ExistingFile = FinalDirectory / TEXT("A.txt");
	const FString BlockingDirectory = FinalDirectory / TEXT("B.txt");
	const FString StagedFile = StagingDirectory / TEXT("A.txt");
	const FString StagedFailureFile = StagingDirectory / TEXT("B.txt");
	FFileHelper::SaveStringToFile(TEXT("previous"), *ExistingFile);
	IFileManager::Get().MakeDirectory(*BlockingDirectory, true);
	FFileHelper::SaveStringToFile(TEXT("replacement"), *StagedFile);
	FFileHelper::SaveStringToFile(TEXT("must fail"), *StagedFailureFile);

	FString Error;
	const bool bPublished = FBXExporter::Testing::PublishStagedFiles(StagingDirectory, FinalDirectory, Error);
	TestFalse(TEXT("Publish fails when a staged destination is blocked"), bPublished);

	FString RestoredContents;
	TestTrue(TEXT("The original file is restored after publish failure"), FFileHelper::LoadFileToString(RestoredContents, *ExistingFile));
	TestEqual(TEXT("Restored file keeps the original contents"), RestoredContents, TEXT("previous"));
	TestTrue(TEXT("The blocking destination remains intact"), IFileManager::Get().DirectoryExists(*BlockingDirectory));
	TestFalse(TEXT("The backup directory is cleaned up"), IFileManager::Get().DirectoryExists(*(StagingDirectory + TEXT("_Previous"))));

	IFileManager::Get().DeleteDirectory(*RootDirectory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFBXExporterStagingReplacesFolderTest,
	"FBX_Exporter.Editor.StaticMeshTextureExport.StagingReplacesFolder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFBXExporterStagingReplacesFolderTest::RunTest(const FString& Parameters)
{
	const FString RootDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FBX_ExporterTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString StagingDirectory = RootDirectory / TEXT("Staging");
	const FString FinalDirectory = RootDirectory / TEXT("Final");
	IFileManager::Get().MakeDirectory(*StagingDirectory, true);
	IFileManager::Get().MakeDirectory(*FinalDirectory, true);
	FFileHelper::SaveStringToFile(TEXT("user file"), *(FinalDirectory / TEXT("user.txt")));
	FFileHelper::SaveStringToFile(TEXT("fresh"), *(StagingDirectory / TEXT("new.txt")));

	FString Error;
	TestTrue(TEXT("Folder publish succeeds"), FBXExporter::Testing::PublishStagedFiles(StagingDirectory, FinalDirectory, Error));
	TestFalse(TEXT("Stale user files are removed with the old folder"), IFileManager::Get().FileExists(*(FinalDirectory / TEXT("user.txt"))));
	FString Contents;
	TestTrue(TEXT("The staged file is published"), FFileHelper::LoadFileToString(Contents, *(FinalDirectory / TEXT("new.txt"))));
	TestEqual(TEXT("Published content is unchanged"), Contents, TEXT("fresh"));
	TestFalse(TEXT("The backup directory is removed after success"), IFileManager::Get().DirectoryExists(*(StagingDirectory + TEXT("_Previous"))));

	IFileManager::Get().DeleteDirectory(*RootDirectory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterCommittedCleanupTest, "FBX_Exporter.Editor.StaticMeshTextureExport.CommittedPartialCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterCommittedCleanupTest::RunTest(const FString&)
{
	const FString Root = FPaths::ProjectSavedDir() / TEXT("FBX_ExporterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Stage = Root / TEXT("Staging"), Final = Root / TEXT("Final");
	IFileManager::Get().MakeDirectory(*Stage, true); IFileManager::Get().MakeDirectory(*Final, true);
	FFileHelper::SaveStringToFile(TEXT("oldA"), *(Final / TEXT("oldA.txt"))); FFileHelper::SaveStringToFile(TEXT("oldB"), *(Final / TEXT("oldB.txt")));
	FFileHelper::SaveStringToFile(TEXT("new verified output"), *(Stage / TEXT("new.txt")));
	FBXExporter::Testing::SetPublishFailure(TEXT("cleanup_partial"));
	FString Error;
	TestTrue(TEXT("Cleanup failure after commit is successful publication"), FBXExporter::Testing::PublishStagedFiles(Stage, Final, Error));
	FBXExporter::Testing::SetPublishFailure(TEXT(""));
	FString Text; FFileHelper::LoadFileToString(Text, *(Final / TEXT("new.txt")));
	TestEqual(TEXT("New output survives partial backup deletion"), Text, TEXT("new verified output"));
	TestTrue(TEXT("Cleanup warning includes retained recovery paths"), Error.Contains(TEXT("journal")) && Error.Contains(TEXT("backup")));
	TestTrue(TEXT("Remaining backup is preserved"), IFileManager::Get().DirectoryExists(*(Stage + TEXT("_Previous"))));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterUnrestoredJournalTest, "FBX_Exporter.Editor.StaticMeshTextureExport.UnrestoredRecoveryJournal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterUnrestoredJournalTest::RunTest(const FString&)
{
	const FString Root = FPaths::ProjectSavedDir() / TEXT("FBX_ExporterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Stage = Root / TEXT("Staging"), Final = Root / TEXT("Final");
	IFileManager::Get().MakeDirectory(*Stage, true); IFileManager::Get().MakeDirectory(*Final, true);
	FFileHelper::SaveStringToFile(TEXT("old"), *(Final / TEXT("old.txt"))); FFileHelper::SaveStringToFile(TEXT("new"), *(Stage / TEXT("new.txt")));
	FBXExporter::Testing::SetPublishFailure(TEXT("publish_restore")); FString Error;
	TestFalse(TEXT("Publish and restore failure is reported"), FBXExporter::Testing::PublishStagedFiles(Stage, Final, Error)); FBXExporter::Testing::SetPublishFailure(TEXT(""));
	FString Journal; TestTrue(TEXT("Unfinished publish blocks this exact output"), FBXExporter::IsRecoveryBlocked(Final, Journal));
	const FString Old = Stage + TEXT("_Previous/old.txt"); TestTrue(TEXT("Old backup and stage remain available"), IFileManager::Get().FileExists(*Old) && IFileManager::Get().FileExists(*(Stage / TEXT("new.txt"))));
	TestTrue(TEXT("Explicit inspection acknowledgement is durable"), FBXExporter::AcknowledgeRecovery(Journal, Error));
	TestTrue(TEXT("Acknowledgement leaves backup bytes unchanged"), IFileManager::Get().FileExists(*Old));
	TestFalse(TEXT("Acknowledgement only releases this output block"), FBXExporter::IsRecoveryBlocked(Final, Journal));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterFloatTextureTest, "FBX_Exporter.Editor.StaticMeshTextureExport.FloatTextureSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterFloatTextureTest::RunTest(const FString&)
{
	TStrongObjectPtr<UTexture2D> Texture(NewObject<UTexture2D>()); Texture->SRGB = true;
	TArray<FColor> Ramp; for (int32 Value=0; Value<256; ++Value) Ramp.Add(FColor(Value,Value,Value,255));
	Texture->Source.Init(256,1,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(Ramp.GetData()));
	TArray<FLinearColor> Linear; TestTrue(TEXT("sRGB source remains float until PNG"), FBXExporter::Testing::ReadLinearTexture(Texture.Get(),256,1,Linear));
	if (Linear.Num()==Ramp.Num()) for (int32 Index=0; Index<Linear.Num(); ++Index) TestTrue(TEXT("Dark sRGB round trip is within one code value"), FMath::Abs(static_cast<int32>(Linear[Index].ToFColorSRGB().R)-Index)<=1);
	const FColor Dark[] = {FColor(17,17,17,255), FColor(17,17,17,255)};
	Texture->Source.Init(2,1,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(Dark));
	TestTrue(TEXT("Resize keeps dark values in float linear space"), FBXExporter::Testing::ReadLinearTexture(Texture.Get(),64,64,Linear));
	for (const FLinearColor& Pixel : Linear) TestTrue(TEXT("Independent constant-field resize reference is within one final PNG code"), FMath::Abs(static_cast<int32>(Pixel.ToFColorSRGB().R)-17)<=1);
	// Two source texels reduced to their centered one-texel footprint have
	// equal area weights. Expected values below use the analytic transfer
	// function and 1/2 weights, independently of ImageCore's resize code.
	auto DecodeSrgb = [](double Value) { return Value <= 0.04045 ? Value / 12.92 : FMath::Pow((Value + 0.055) / 1.055, 2.4); };
	auto EncodeSrgb = [](double Value) { const double Encoded = Value <= 0.0031308 ? 12.92 * Value : 1.055 * FMath::Pow(Value, 1.0 / 2.4) - 0.055; return FMath::RoundToInt(Encoded * 255.0); };
	auto CheckPng = [this, &EncodeSrgb](const FLinearColor& Actual, const FLinearColor& Expected, const FString& Label)
	{
		const FString Folder = FPaths::ProjectSavedDir() / TEXT("FBX_ExporterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
		IFileManager::Get().MakeDirectory(*Folder, true); const FString Path = Folder / TEXT("resize.png");
		const FColor Pixel = Actual.ToFColorSRGB();
		TestTrue(Label + TEXT(" PNG stores the final quantized pixel"), FImageUtils::SaveImageByExtension(*Path, FImageView(&Pixel, 1, 1, EGammaSpace::sRGB)));
		FImage Image; if (!TestTrue(Label + TEXT(" PNG decodes"), FImageUtils::LoadImage(*Path, Image))) return;
		FImage BGRA; Image.CopyTo(BGRA, ERawImageFormat::BGRA8, Image.GammaSpace); const FColor Read = BGRA.AsBGRA8()[0];
		TestTrue(Label + TEXT(" independent analytic RGB reference <= 1 PNG LSB"), FMath::Abs(static_cast<int32>(Read.R)-EncodeSrgb(Expected.R))<=1 && FMath::Abs(static_cast<int32>(Read.G)-EncodeSrgb(Expected.G))<=1 && FMath::Abs(static_cast<int32>(Read.B)-EncodeSrgb(Expected.B))<=1);
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	const FColor NonuniformSrgb[] = {FColor(17,61,233,255), FColor(29,201,35,255)};
	Texture->bUseLegacyGamma=false; Texture->Source.Init(2,1,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(NonuniformSrgb));
	TestTrue(TEXT("Nonuniform sRGB downsample completes in linear space"), FBXExporter::Testing::ReadLinearTexture(Texture.Get(),1,1,Linear));
	if (Linear.Num()==1) CheckPng(Linear[0], FLinearColor(static_cast<float>((DecodeSrgb(17.0/255)+DecodeSrgb(29.0/255))/2), static_cast<float>((DecodeSrgb(61.0/255)+DecodeSrgb(201.0/255))/2), static_cast<float>((DecodeSrgb(233.0/255)+DecodeSrgb(35.0/255))/2), 1), TEXT("sRGB area average"));
	const FLinearColor NonuniformFloat[] = {FLinearColor(0.0005f,0.8f,0.15f,1), FLinearColor(0.0019f,0.05f,0.95f,1)};
	Texture->SRGB=false; Texture->Source.Init(2,1,1,1,TSF_RGBA32F,reinterpret_cast<const uint8*>(NonuniformFloat));
	TestTrue(TEXT("Nonuniform float-linear downsample completes"), FBXExporter::Testing::ReadLinearTexture(Texture.Get(),1,1,Linear));
	if (Linear.Num()==1) CheckPng(Linear[0], FLinearColor(0.0012f,0.425f,0.55f,1), TEXT("Float area average"));
	Texture->SRGB=false; const FLinearColor HDR[] = {FLinearColor(8.0f,2.0f,0.25f,1.0f)};
	Texture->Source.Init(1,1,1,1,TSF_RGBA32F,reinterpret_cast<const uint8*>(HDR));
	TestTrue(TEXT("Float HDR source can be read"), FBXExporter::Testing::ReadLinearTexture(Texture.Get(),1,1,Linear));
	if (!Linear.IsEmpty()) TestEqual(TEXT("HDR radiance is not clamped to PNG range during source decode"), Linear[0].R,8.0f);
	TArray<FColor> Encoded; float Scale = 0;
	TestTrue(TEXT("HDR direct emission uses final PNG normalization"), FBXExporter::Testing::ReadDirectEmission(Texture.Get(),1,1,Encoded,Scale));
	TestEqual(TEXT("HDR maximum is retained as emission scale"), Scale,8.0f);
	if (!Encoded.IsEmpty())
	{
		const FLinearColor Restored = FLinearColor(Encoded[0]) * Scale;
		TestTrue(TEXT("HDR normalized reconstruction stays within one linear LSB for fixture"), FMath::Abs(Restored.R-HDR[0].R)<=Scale/255.0f && FMath::Abs(Restored.G-HDR[0].G)<=Scale/255.0f && FMath::Abs(Restored.B-HDR[0].B)<=Scale/255.0f);
	}
	return true;
}
