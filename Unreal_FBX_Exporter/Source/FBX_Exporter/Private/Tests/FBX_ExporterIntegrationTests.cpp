#include "FBX_ExporterService.h"
#include "FBX_ExporterFbxMedia.h"
#include "AssetCompilingManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "IMaterialBakingModule.h"
#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "ShaderCompiler.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace FBXExporterIntegration
{
	UMaterialExpressionConstant* Scalar(UMaterial* Material, float Value)
	{
		auto* Expression = NewObject<UMaterialExpressionConstant>(Material); Expression->R = Value;
		Material->GetExpressionCollection().AddExpression(Expression); return Expression;
	}
	UMaterialExpressionConstant3Vector* Vector(UMaterial* Material, FLinearColor Value)
	{
		auto* Expression = NewObject<UMaterialExpressionConstant3Vector>(Material); Expression->Constant = Value;
		Material->GetExpressionCollection().AddExpression(Expression); return Expression;
	}
	UMaterial* FixtureMaterial(UPackage* Package, const TCHAR* Name, UTexture2D* Emission, float Red)
	{
		UMaterial* Material = NewObject<UMaterial>(Package, Name, RF_Public | RF_Standalone);
		Material->BlendMode = BLEND_Translucent;
		// Default volumetric translucency deliberately deactivates Metallic and
		// Roughness in UMaterial::IsPropertyActive. Surface lighting is required
		// for this fixture's PBR scalar inputs to participate in material baking.
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Vector(Material, FLinearColor(Red,0.04f,0.08f,1)));
		Material->GetEditorOnlyData()->Normal.Connect(0, Vector(Material,FLinearColor(0,0,1,1)));
		Material->GetEditorOnlyData()->Metallic.Connect(0, Scalar(Material,0.2f));
		Material->GetEditorOnlyData()->Roughness.Connect(0, Scalar(Material,0.3f));
		Material->GetEditorOnlyData()->AmbientOcclusion.Connect(0, Scalar(Material,0.8f));
		Material->GetEditorOnlyData()->Opacity.Connect(0, Scalar(Material,0.75f));
		auto* Sample = NewObject<UMaterialExpressionTextureSample>(Material); Sample->Texture = Emission; Sample->SamplerType = SAMPLERTYPE_LinearColor; Sample->AutomaticViewMipBias = false;
		Material->GetExpressionCollection().AddExpression(Sample); Material->GetEditorOnlyData()->EmissiveColor.Connect(0,Sample);
		Material->PostEditChange(); return Material;
	}
	UStaticMesh* FixtureMesh(TArray<TStrongObjectPtr<UObject>>& Held, FString& OutError)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) { OutError=TEXT("Engine cube missing."); return nullptr; }
		const FString PackageName = TEXT("/Game/FBXExporterAutomation/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		UPackage* Package = CreatePackage(*PackageName);
		UTexture2D* Emission = NewObject<UTexture2D>(Package,TEXT("HDR"),RF_Public|RF_Standalone); Emission->SRGB=false; Emission->CompressionSettings=TC_HDR;
		TArray<FLinearColor> Pixels; Pixels.Init(FLinearColor(8,2,0.25f,1),16);
		Emission->Source.Init(4,4,1,1,TSF_RGBA32F,reinterpret_cast<const uint8*>(Pixels.GetData())); Emission->PostEditChange();
		UMaterial* A = FixtureMaterial(Package,TEXT("A"),Emission,0.02f); UMaterial* B = FixtureMaterial(Package,TEXT("B"),Emission,0.03f);
		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube,Package,TEXT("Mesh")); Mesh->SetFlags(RF_Public|RF_Standalone);
		Mesh->SetStaticMaterials({FStaticMaterial(A,TEXT("A"),TEXT("A")),FStaticMaterial(B,TEXT("B"),TEXT("B"))});
		FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!Description) { OutError=TEXT("Engine cube source MeshDescription missing."); return nullptr; }
		FStaticMeshAttributes Attributes(*Description); auto SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		const FPolygonGroupID GroupA = Description->PolygonGroups().GetElementIDs().begin().operator*();
		const FPolygonGroupID GroupB = Description->CreatePolygonGroup(); SlotNames[GroupA]=TEXT("A"); SlotNames[GroupB]=TEXT("B");
		int32 Index=0; for (const FPolygonID Polygon : Description->Polygons().GetElementIDs()) Description->SetPolygonPolygonGroup(Polygon,Index++ % 2 ? GroupB : GroupA);
		Mesh->CommitMeshDescription(0); Mesh->Build(true);
		FAssetCompilingManager::Get().FinishAllCompilation(); if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		Held.Emplace(Emission); Held.Emplace(A); Held.Emplace(B); Held.Emplace(Mesh); Package->SetDirtyFlag(false); return Mesh;
	}
	bool PixelAtCenter(const FString& Path, FColor& Out)
	{
		FImage Image; if (!FImageUtils::LoadImage(*Path,Image)) return false;
		FImage BGRA; Image.CopyTo(BGRA,ERawImageFormat::BGRA8,Image.GammaSpace);
		const auto Pixels = BGRA.AsBGRA8(); if (Pixels.IsEmpty()) return false;
		Out=Pixels[(BGRA.SizeY/2)*BGRA.SizeX+BGRA.SizeX/2]; return true;
	}
	class FWorkspaceScreenshot final : public IAutomationLatentCommand
	{
	public:
		explicit FWorkspaceScreenshot(FAutomationTestBase* InTest) : Test(InTest) {}
		virtual ~FWorkspaceScreenshot() override { if (Window) Window->RequestDestroyWindow(); }
		virtual bool Update() override
		{
			if (Stage==0)
			{
				UStaticMesh* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
				UStaticMesh* Sphere=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere"));
				Content=FBXExporter::Testing::CreateWorkspaceWidget({FAssetData(Cube),FAssetData(Sphere)});
				Window=SNew(SWindow).Title(FText::FromString(TEXT("FBX Exporter UI verification"))).ClientSize(FVector2D(1280,720)).SupportsMaximize(false).SupportsMinimize(false)[Content.ToSharedRef()];
				FSlateApplication::Get().AddWindow(Window.ToSharedRef());
				Started=FPlatformTime::Seconds(); Stage=1; return false;
			}
			if (FPlatformTime::Seconds()-Started < 1.0) return false;
			TArray<FColor> Pixels; FIntVector Size;
			if (!FSlateApplication::Get().TakeScreenshot(Content.ToSharedRef(),Pixels,Size)) Test->AddError(TEXT("FBX workspace content screenshot failed."));
			else
			{
				Test->TestTrue(TEXT("Screenshot captures complete workspace width"),Size.X >= (Stage==1?1100:1700));
				Test->TestTrue(TEXT("Screenshot captures complete workspace height"),Size.Y >= (Stage==1?600:900));
				const FString Folder=FPaths::ProjectSavedDir()/TEXT("UnrealTools/V12/UI"); IFileManager::Get().MakeDirectory(*Folder,true);
				const FString Path=Folder/(Stage==1?TEXT("FBX_1280x720.png"):TEXT("FBX_1920x1080.png"));
				Test->TestTrue(TEXT("Workspace content PNG saved"),FImageUtils::SaveImageByExtension(*Path,FImageView(Pixels.GetData(),Size.X,Size.Y,EGammaSpace::sRGB)));
			}
			if (Stage==1) { Window->Resize(FVector2D(1920,1080)); Started=FPlatformTime::Seconds(); Stage=2; return false; }
			Window->RequestDestroyWindow(); Window.Reset(); return true;
		}
	private:
		FAutomationTestBase* Test; int32 Stage=0; double Started=0;
		TSharedPtr<SWidget> Content; TSharedPtr<SWindow> Window;
	};

	class FSourceChanged final : public IAutomationLatentCommand
	{
	public:
		explicit FSourceChanged(FAutomationTestBase* InTest) : Test(InTest) {}
		virtual bool Update() override
		{
			if (!Runner)
			{
				UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
				if (!Cube) { Test->AddError(TEXT("Source-change engine fixture missing.")); return true; }
				UPackage* Package = CreatePackage(*(TEXT("/Game/FBXExporterSourceChanged/") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
				Mesh.Reset(DuplicateObject<UStaticMesh>(Cube, Package, TEXT("Mesh"))); Mesh->SetFlags(RF_Public | RF_Standalone);
				Dependency.Reset(NewObject<UMaterialInstanceConstant>(Package, TEXT("MaterialDependency"), RF_Public | RF_Standalone));
				Dependency->SetParentEditorOnly(Cube->GetMaterial(0)); Mesh->SetMaterial(0, Dependency.Get()); Mesh->Build(true);
				FAssetCompilingManager::Get().FinishAllCompilation(); Package->SetDirtyFlag(false);
				FFBXExporterOptions Options; Options.TextureMaps = EFBXExporterTextureMap::None;
				Options.OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/SourceChanged") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
				Final = Options.OutputRoot / TEXT("Mesh");
				UnrealToolsRuntime::FItemSpec Spec; Spec.MeshPath = Mesh->GetPathName(); Spec.Options = FBXExporter::OptionsToJson(Options);
				Runner = FBXExporter::CreateExportRunner(Spec, TEXT("source_changed_automation"));
			}
			// One native stage per Automation/Editor frame, without nested ticks.
			const auto Step = Runner->Step(false, false);
			if (!bModified && Runner->GetStage() == TEXT("files"))
			{
				Dependency->Modify(); Dependency->BasePropertyOverrides.bOverride_TwoSided = true;
				Dependency->BasePropertyOverrides.TwoSided = !Dependency->IsTwoSided(); bModified = true;
			}
			if (Step == UnrealToolsRuntime::EStepResult::Continue) return false;
			Test->TestTrue(TEXT("Held material dependency changed after geometry before publish"), bModified);
			Test->TestTrue(TEXT("Changed dependency fails the real stage runner"), Step == UnrealToolsRuntime::EStepResult::Failed);
			Test->TestEqual(TEXT("Exact source_changed error is preserved"), UnrealToolsRuntime::GetString(Runner->GetResult(), TEXT("error_code")), FString(TEXT("source_changed")));
			Test->TestFalse(TEXT("Changed source publishes no final output"), IFileManager::Get().DirectoryExists(*Final));
			Runner->Shutdown(); Runner.Reset(); Dependency->ClearFlags(RF_Standalone); Mesh->ClearFlags(RF_Standalone); Mesh->GetOutermost()->SetDirtyFlag(false);
			return true;
		}
	private:
		FAutomationTestBase* Test; bool bModified = false; FString Final;
		TStrongObjectPtr<UStaticMesh> Mesh; TStrongObjectPtr<UMaterialInstanceConstant> Dependency;
		TUniquePtr<UnrealToolsRuntime::IItemRunner> Runner;
	};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterGpuRoundTripTest,"FBX_Exporter.Editor.Integration.GPUAndEmbeddedRoundTrip",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterGpuRoundTripTest::RunTest(const FString&)
{
	using namespace FBXExporterIntegration;
	TArray<TStrongObjectPtr<UObject>> Held; FString Error; UStaticMesh* Mesh=FixtureMesh(Held,Error);
	if (!TestNotNull(TEXT("Independent two-slot fixture"),Mesh)) { AddError(Error); return false; }
	for (const auto& Object : Held)
		if (const UMaterial* Material = Cast<UMaterial>(Object.Get()))
		{
			TestTrue(TEXT("Fixture Metallic input is active under surface translucency"), Material->IsPropertyActive(MP_Metallic));
			TestTrue(TEXT("Fixture Roughness input is active under surface translucency"), Material->IsPropertyActive(MP_Roughness));
			TestTrue(TEXT("HDR emission fixture actually exercises direct source reuse"), FBXExporter::Testing::IsDirectTextureReusable(const_cast<UMaterial*>(Material), TEXT("Emission")));
		}
	const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("UnrealTools/V12")/(TEXT("FBX_GPU_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FFBXExporterOptions Options; Options.OutputRoot=Root/TEXT("AllMaps"); Options.BakeResolution=512; Options.bEmbedTexturesInFbx=true;
	const auto Before=FPlatformMemory::GetStats();
	const auto Result=FBXExporter::ExportStaticMeshUnattended(Mesh,Options);
	if (!TestTrue(TEXT("Actual GPU bake and FBX embed completes"),Result.Succeeded())) { for (const auto& Message:Result.Errors) AddError(Message); return false; }
	for (const TCHAR* Slot : {TEXT("A"),TEXT("B")})
	{
		const FString Prefix=Result.FinalDirectory/TEXT("Textures")/Slot; FColor Pixel;
		TestTrue(TEXT("BaseColor PNG reads"),PixelAtCenter(Prefix+TEXT("_BaseColor.png"),Pixel));
		TestTrue(TEXT("BaseColor retains opacity alpha"),FMath::Abs(static_cast<int32>(Pixel.A)-191)<=2);
		TestTrue(TEXT("Tangent normal PNG reads"),PixelAtCenter(Prefix+TEXT("_Normal.png"),Pixel));
		TestTrue(TEXT("Tangent normal is neutral"),FMath::Abs(static_cast<int32>(Pixel.R)-128)<=2&&FMath::Abs(static_cast<int32>(Pixel.G)-128)<=2&&Pixel.B>=253);
		TestTrue(TEXT("Mask PNG reads"),PixelAtCenter(Prefix+TEXT("_Mask.png"),Pixel));
		TestTrue(TEXT("Scalar map channels and smoothness are linear"),FMath::Abs(static_cast<int32>(Pixel.R)-51)<=2&&FMath::Abs(static_cast<int32>(Pixel.G)-204)<=2&&FMath::Abs(static_cast<int32>(Pixel.A)-179)<=2);
		TestTrue(TEXT("Opacity PNG reads"),PixelAtCenter(Prefix+TEXT("_Opacity.png"),Pixel));
		TestTrue(TEXT("Opacity convention is opaque=1 in RGB and alpha"),FMath::Abs(static_cast<int32>(Pixel.R)-191)<=2&&Pixel.R==Pixel.G&&Pixel.R==Pixel.B&&Pixel.R==Pixel.A);
		TestTrue(TEXT("HDR emission PNG reads"),PixelAtCenter(Prefix+TEXT("_Emission.png"),Pixel));
		const FLinearColor Restored=FLinearColor(Pixel)*8.0f;
		TestTrue(TEXT("Actual HDR PNG reconstructs radiance within normalized one linear LSB"),FMath::Abs(Restored.R-8)<=8.0f/255&&FMath::Abs(Restored.G-2)<=8.0f/255&&FMath::Abs(Restored.B-0.25f)<=8.0f/255);
	}
	int32 Textures=0; TestTrue(TEXT("FBX SDK reimport verifies distinct materials, relative links and embedded PNG bytes"),FBXExporter::Testing::VerifyImportedMedia(Result.FinalDirectory/TEXT("Mesh.fbx"),true,2,Textures,Error));
	if (!Error.IsEmpty()) AddError(Error); TestEqual(TEXT("Two slots each have all six texture maps"),Textures,12);
	Options.OutputRoot=Root/TEXT("SelectedNormal"); Options.TextureMaps=EFBXExporterTextureMap::Normal; Options.bEmbedTexturesInFbx=false;
	const auto Selected=FBXExporter::ExportStaticMeshUnattended(Mesh,Options); TestTrue(TEXT("Selected-map export completes"),Selected.Succeeded());
	TArray<FString> Pngs; IFileManager::Get().FindFilesRecursive(Pngs,*Selected.FinalDirectory,TEXT("*.png"),true,false); TestEqual(TEXT("Unselected maps are not generated"),Pngs.Num(),2);
	Error.Empty(); TestTrue(TEXT("External-mode FBX reimports relative PNG references"),FBXExporter::Testing::VerifyImportedMedia(Selected.FinalDirectory/TEXT("Mesh.fbx"),false,2,Textures,Error)); if (!Error.IsEmpty()) AddError(Error);
	Options.OutputRoot=Root/TEXT("Maximum4096"); Options.BakeResolution=4096; Options.TextureMaps=EFBXExporterTextureMap::BaseColor;
	const auto Maximum=FBXExporter::ExportStaticMeshUnattended(Mesh,Options); TestTrue(TEXT("4096 GPU dependency group completes"),Maximum.Succeeded());
	const auto After=FPlatformMemory::GetStats(); auto Metrics=MakeShared<FJsonObject>(); Metrics->SetStringField(TEXT("outputs"),Root);
	Metrics->SetNumberField(TEXT("before_process_physical"),static_cast<double>(Before.UsedPhysical)); Metrics->SetNumberField(TEXT("after_process_physical"),static_cast<double>(After.UsedPhysical)); Metrics->SetNumberField(TEXT("peak_process_physical"),static_cast<double>(After.PeakUsedPhysical));
	Metrics->SetNumberField(TEXT("maximum_resolution"),4096); Metrics->SetNumberField(TEXT("slots"),2);
	UnrealToolsRuntime::AtomicJson(Root/TEXT("metrics.json"),Metrics,Error);
	for (const auto& Object:Held) { Object->ClearFlags(RF_Standalone); Object->GetOutermost()->SetDirtyFlag(false); }
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterBakeStateTest, "FBX_Exporter.Editor.Integration.MaterialBakingStateRestoration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFBXExporterBakeStateTest::RunTest(const FString&)
{
	using namespace FBXExporterIntegration;
	TArray<TStrongObjectPtr<UObject>> Held; FString Error;
	UStaticMesh* Mesh = FixtureMesh(Held, Error);
	if (!TestNotNull(TEXT("Independent PBR material fixture"), Mesh)) { AddError(Error); return false; }
	IMaterialBakingModule& Baking = FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
	const bool OriginalLinear = Baking.IsLinearBake(MP_Metallic);
	FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()); FPaths::NormalizeFilename(Project);
	const bool HdrFlag = FParse::Param(FCommandLine::Get(), TEXT("FBXExporterHdrStateFixture"));
	const bool OwnedHdr = HdrFlag && FParse::Param(FCommandLine::Get(), TEXT("UnrealToolsOwnedHdrFixture")) &&
		(Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/"), ESearchCase::IgnoreCase) || Project.Contains(TEXT("/Saved/UnrealTools/V11/Projects/"), ESearchCase::IgnoreCase));
	ON_SCOPE_EXIT
	{
		Baking.SetLinearBake(OriginalLinear);
		// HDR toggling is opt-in and confined to a process whose module starts with HDR=false.
		if (OwnedHdr) Baking.SetEmissiveHDR(false);
		for (const auto& Object : Held) { Object->ClearFlags(RF_Standalone); Object->GetOutermost()->SetDirtyFlag(false); }
	};
	if (HdrFlag && !OwnedHdr) { AddError(TEXT("HDR state matrix requires an explicitly owned V12/V11 fixture process.")); return false; }
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/BakeState") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	for (int32 HdrCase = 0; HdrCase < (OwnedHdr ? 2 : 1); ++HdrCase)
	{
		if (OwnedHdr) Baking.SetEmissiveHDR(HdrCase != 0);
		for (bool Linear : {false, true})
		{
			Baking.SetLinearBake(Linear); const uint32 Before = Baking.GetCRC();
			FFBXExporterOptions Options; Options.OutputRoot = Root / FString::Printf(TEXT("HDR%d_Linear%d"), HdrCase, Linear ? 1 : 0);
			Options.BakeResolution = 512; Options.bReuseDirectTextures = false;
			Options.TextureMaps = EFBXExporterTextureMap::Mask | EFBXExporterTextureMap::Emission;
			const auto Result = FBXExporter::ExportStaticMeshUnattended(Mesh, Options);
			TestTrue(TEXT("Real graph bake completes"), Result.Succeeded());
			for (const FString& Message : Result.Errors) AddError(Message);
			TestEqual(TEXT("Prior linear mode is restored"), Baking.IsLinearBake(MP_Metallic), Linear);
			TestEqual(TEXT("Shared bake settings CRC is unchanged"), Baking.GetCRC(), Before);
			FColor Pixel;
			if (TestTrue(TEXT("Baked Mask reads"), PixelAtCenter(Result.FinalDirectory / TEXT("Textures/A_Mask.png"), Pixel)))
				TestTrue(TEXT("Linear scalar output stays correct under either prior mode"), FMath::Abs(static_cast<int32>(Pixel.R)-51)<=2 && FMath::Abs(static_cast<int32>(Pixel.G)-204)<=2 && FMath::Abs(static_cast<int32>(Pixel.A)-179)<=2);
			if (TestTrue(TEXT("Baked Emission reads"), PixelAtCenter(Result.FinalDirectory / TEXT("Textures/A_Emission.png"), Pixel)))
			{
				FString Text; FFileHelper::LoadFileToString(Text, *(Result.FinalDirectory / TEXT("Mesh_Materials.json")));
				const auto Metadata = UnrealToolsRuntime::Parse(Text); const auto Materials = UnrealToolsRuntime::GetArray(Metadata, TEXT("materials"));
				if (TestFalse(TEXT("Baked emission scale metadata exists"), Materials.IsEmpty()))
				{
					const float Scale = static_cast<float>(UnrealToolsRuntime::GetNumber(Materials[0]->AsObject(), TEXT("emissionScale"), 1));
					const FLinearColor Restored = FLinearColor(Pixel) * Scale;
					TestTrue(TEXT("Either engine HDR mode retains PNG plus scale radiance"), FMath::Abs(Restored.R-8)<=Scale/255 && FMath::Abs(Restored.G-2)<=Scale/255 && FMath::Abs(Restored.B-0.25f)<=Scale/255);
				}
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterWorkspaceScreenshotTest,"FBX_Exporter.Editor.Integration.WorkspaceScreenshots",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterWorkspaceScreenshotTest::RunTest(const FString&)
{ ADD_LATENT_AUTOMATION_COMMAND(FBXExporterIntegration::FWorkspaceScreenshot(this)); return true; }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterInactiveReuseTest,"FBX_Exporter.Editor.StaticMeshTextureExport.InactiveDirectTexture",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterInactiveReuseTest::RunTest(const FString&)
{
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>());
	TStrongObjectPtr<UTexture2D> Texture(NewObject<UTexture2D>());
	const FColor Pixel(51,51,51,255); Texture->SRGB=false;
	Texture->Source.Init(1,1,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(&Pixel));
	Material->BlendMode=BLEND_Translucent;
	auto* Sample=NewObject<UMaterialExpressionTextureSample>(Material.Get()); Sample->Texture=Texture.Get(); Sample->AutomaticViewMipBias=false; Sample->SamplerType=SAMPLERTYPE_LinearColor;
	Material->GetExpressionCollection().AddExpression(Sample); Material->GetEditorOnlyData()->Metallic.Connect(1,Sample);
	TestFalse(TEXT("Engine default volumetric translucency deactivates Metallic"),Material->IsPropertyActive(MP_Metallic));
	TestFalse(TEXT("Inactive input cannot bypass engine defaults via direct reuse"),FBXExporter::Testing::IsDirectTextureReusable(Material.Get(),TEXT("Metallic")));
	Material->TranslucencyLightingMode=TLM_SurfacePerPixelLighting;
	TestTrue(TEXT("Surface translucency activates the configured Metallic input"),Material->IsPropertyActive(MP_Metallic));
	TestTrue(TEXT("Active exact direct scalar sample remains eligible"),FBXExporter::Testing::IsDirectTextureReusable(Material.Get(),TEXT("Metallic")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterSourceChangedTest,"FBX_Exporter.Editor.Integration.SourceChanged",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterSourceChangedTest::RunTest(const FString&)
{ ADD_LATENT_AUTOMATION_COMMAND(FBXExporterIntegration::FSourceChanged(this)); return true; }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterGuardedCrashTest,"FBX_Exporter.Editor.Integration.GuardedPublishingCrash",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterGuardedCrashTest::RunTest(const FString&)
{
	using namespace UnrealToolsRuntime;
	const bool bOptedIn = FParse::Param(FCommandLine::Get(), TEXT("FBXExporterCrashCheckpoint")) && FParse::Param(FCommandLine::Get(), TEXT("UnrealToolsOwnedCrashFixture"));
	if (!bOptedIn) { AddInfo(TEXT("Guarded process-crash checkpoint skipped: explicit dedicated-fixture flags are absent.")); return true; }
	FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()); FPaths::NormalizeFilename(Project);
	if (!Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/"), ESearchCase::IgnoreCase) && !Project.Contains(TEXT("/Saved/UnrealTools/V11/Projects/"), ESearchCase::IgnoreCase)) { AddError(TEXT("Forced checkpoint refuses a project outside owned V12/V11 Projects.")); return false; }
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealTools/V12/CrashCheckpoint"));
	const FString Root = Folder / FGuid::NewGuid().ToString(EGuidFormats::Digits), Stage = Root / TEXT("Staging"), Final = Root / TEXT("Cube");
	IFileManager::Get().MakeDirectory(*Stage, true); IFileManager::Get().MakeDirectory(*Final, true);
	FFileHelper::SaveStringToFile(TEXT("original-a"), *(Final / TEXT("old_a.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(TEXT("original-b"), *(Final / TEXT("old_b.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(TEXT("replacement"), *(Stage / TEXT("new.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFBXExporterOptions Options; Options.OutputRoot = Root; Options.TextureMaps = EFBXExporterTextureMap::None;
	const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	FObject Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("request_id"), RequestId); Input->SetArrayField(TEXT("mesh_paths"), {MakeShared<FJsonValueString>(TEXT("/Engine/BasicShapes/Cube.Cube"))});
	Input->SetObjectField(TEXT("options"), FBXExporter::OptionsToJson(Options)); Input->SetBoolField(TEXT("overwrite"), true);
	const FObject Accepted = FBXExporter::Jobs().Start(Input);
	if (!GetBool(Accepted, TEXT("success"))) { AddError(GetString(Accepted, TEXT("error"))); return false; }
	const FString JobId = GetString(Accepted, TEXT("job_id")), RecordPath = FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Jobs") / (JobId + TEXT(".json"));
	FString Text, Error;
	if (!FFileHelper::LoadFileToString(Text, *RecordPath)) { AddError(TEXT("Accepted crash fixture job was not durable.")); return false; }
	FObject Record = Parse(Text);
	if (!Record.IsValid()) { AddError(TEXT("Crash fixture record was not valid JSON.")); return false; }
	Record->SetStringField(TEXT("state"), TEXT("running")); Record->SetStringField(TEXT("stage"), TEXT("publish"));
	const FString Journal = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FBX_Exporter/Recovery") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json")));
	for (const auto& Value : GetArray(Record, TEXT("items")))
	{
		FObject Item = Value->AsObject(); Item->SetStringField(TEXT("state"), TEXT("running")); Item->SetBoolField(TEXT("commit_pending"), true);
		FObject Result = UnrealToolsRuntime::Error(TEXT("Explicit publishing crash checkpoint."), TEXT("CHECKPOINT"));
		Result->SetStringField(TEXT("recovery_journal"), Journal); Result->SetStringField(TEXT("recovery_staging"), Stage); Result->SetStringField(TEXT("final_directory"), Final); Item->SetObjectField(TEXT("result"), Result);
	}
	if (!AtomicJson(RecordPath, Record, Error)) { AddError(Error); return false; }
	FObject Ready = MakeShared<FJsonObject>(); Ready->SetStringField(TEXT("checkpoint"), TEXT("prepared")); Ready->SetStringField(TEXT("project_file"), Project); Ready->SetStringField(TEXT("job_id"), JobId); Ready->SetStringField(TEXT("request_id"), RequestId);
	Ready->SetStringField(TEXT("staging"), Stage); Ready->SetStringField(TEXT("final"), Final); Ready->SetStringField(TEXT("backup"), Stage + TEXT("_Previous")); Ready->SetStringField(TEXT("journal"), Journal); Ready->SetObjectField(TEXT("request"), Input);
	if (!AtomicJson(Folder / TEXT("ready.json"), Ready, Error)) { AddError(Error); return false; }
	FBXExporter::Testing::SetPublishFailure(TEXT("process_crash_after_backup"));
	FBXExporter::Testing::PublishWithJournal(Stage, Final, Journal, Error, JobId);
	FBXExporter::Testing::SetPublishFailure(TEXT(""));
	FObject Cancel = MakeShared<FJsonObject>(); Cancel->SetStringField(TEXT("job_id"), JobId); Cancel->SetStringField(TEXT("action"), TEXT("cancel")); FBXExporter::Jobs().Control(Cancel); FBXExporter::Jobs().AdvanceOneStepForTesting();
	AddError(TEXT("Explicit owned crash checkpoint returned without the required forced exit: ") + Error); return false;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFBXExporterWorkspaceStateTest,"FBX_Exporter.Editor.Integration.WorkspaceSelectionState",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FFBXExporterWorkspaceStateTest::RunTest(const FString&)
{
	FString Error; const bool bPassed = FBXExporter::Testing::WorkspaceStateRegression(Error);
	if (!bPassed) AddError(Error); return bPassed;
}
#endif
