#if WITH_DEV_AUTOMATION_TESTS

#include "CubeShotAssetWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "ImageCore.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotOutputPathTest,
    "CubeShot.Editor.AssetWriter.OutputPathMirroring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotOutputPathTest::RunTest(const FString& Parameters)
{
    TArray<FCubeShotOutputDescriptor> Outputs;
    FText Error;
    TestTrue(
        TEXT("Game source path maps successfully"),
        FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
            TEXT("/Game/Props/Furniture/SM_Chair"),
            TEXT("SM_Chair"),
            TEXT("/Game/CubeShot"),
            Outputs,
            Error));
    TestEqual(TEXT("Six descriptors"), Outputs.Num(), 6);
    if (Outputs.Num() == 6)
    {
        TestEqual(
            TEXT("Front mirrors the source hierarchy"),
            Outputs[0].LongPackageName,
            FString(TEXT("/Game/CubeShot/Props/Furniture/SM_Chair/T_SM_Chair_Front")));
        TestEqual(TEXT("Front object name"), Outputs[0].AssetName, FString(TEXT("T_SM_Chair_Front")));
        TestEqual(
            TEXT("Bottom suffix"),
            Outputs[5].GetObjectPath(),
            FString(TEXT("/Game/CubeShot/Props/Furniture/SM_Chair/T_SM_Chair_Bottom.T_SM_Chair_Bottom")));
    }

    Outputs.Reset();
    TestTrue(
        TEXT("Non-Game source path maps successfully"),
        FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
            TEXT("/Engine/BasicShapes/Cube"),
            TEXT("Cube"),
            TEXT("/Game/CubeShot"),
            Outputs,
            Error));
    if (!Outputs.IsEmpty())
    {
        TestEqual(
            TEXT("Other mount is namespaced"),
            Outputs[0].LongPackageName,
            FString(TEXT("/Game/CubeShot/_Mounts/Engine/BasicShapes/Cube/T_Cube_Front")));
    }

    Outputs.Reset();
    TestTrue(
        TEXT("Unsafe source asset name is sanitized"),
        FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
            TEXT("/Game/CubeShotTests/UnsafeSource"),
            TEXT("Mesh Name:Variant"),
            TEXT("/Game/CubeShotTests/Output"),
            Outputs,
            Error));
    for (const FCubeShotOutputDescriptor& Output : Outputs)
    {
        TestTrue(TEXT("Generated object path is valid"), FPackageName::IsValidObjectPath(Output.GetObjectPath()));
        TestFalse(TEXT("Generated object name has no spaces"), Output.AssetName.Contains(TEXT(" ")));
        TestFalse(TEXT("Generated object name has no colon"), Output.AssetName.Contains(TEXT(":")));
    }

    TestEqual(
        TEXT("Power-of-two uses group mip policy"),
        FCubeShotAssetWriter::GetMipPolicy(1024),
        TMGS_FromTextureGroup);
    TestEqual(
        TEXT("Non-power-of-two disables mips"),
        FCubeShotAssetWriter::GetMipPolicy(1000),
        TMGS_NoMipmaps);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotPreflightCollisionTest,
    "CubeShot.Editor.AssetWriter.PreflightCollisions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotPreflightCollisionTest::RunTest(const FString& Parameters)
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh))
    {
        return false;
    }

    FCubeShotOptions Options;
    Options.StaticMesh = CubeMesh;

    const FString UniqueRoot = FString::Printf(
        TEXT("/Game/CubeShotTests/Preflight_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FCubeShotAssetWriterConfig Config;
    Config.OutputRoot = UniqueRoot;
    Config.bPromptForCheckout = false;
    FCubeShotAssetWriter Writer(Config);

    TArray<FCubeShotOutputDescriptor> Outputs;
    FText PathError;
    if (!TestTrue(
        TEXT("Build test paths"),
        FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
            CubeMesh->GetOutermost()->GetName(),
            CubeMesh->GetName(),
            UniqueRoot,
            Outputs,
            PathError)))
    {
        AddError(PathError.ToString());
        return false;
    }

    UPackage* WrongTypePackage = CreatePackage(*Outputs[0].LongPackageName);
    UMaterial* WrongTypeAsset = NewObject<UMaterial>(
        WrongTypePackage,
        FName(*Outputs[0].AssetName),
        RF_Public | RF_Standalone);
    WrongTypePackage->SetDirtyFlag(false);

    FCubeShotPreflightResult Preflight;
    TestFalse(TEXT("Wrong top-level type is rejected"), Writer.BuildPreflight(Options, Preflight));
    TestFalse(TEXT("Rejected preflight cannot proceed"), Preflight.bCanProceed);

    WrongTypeAsset->ClearFlags(RF_Public | RF_Standalone);
    WrongTypeAsset->MarkAsGarbage();
    WrongTypePackage->SetDirtyFlag(false);
    WrongTypePackage->MarkAsGarbage();

    const FString RedirectorRoot = FString::Printf(
        TEXT("/Game/CubeShotTests/Redirector_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Config.OutputRoot = RedirectorRoot;
    FCubeShotAssetWriter RedirectorWriter(Config);
    Outputs.Reset();
    FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
        CubeMesh->GetOutermost()->GetName(),
        CubeMesh->GetName(),
        RedirectorRoot,
        Outputs,
        PathError);

    UPackage* RedirectorPackage = CreatePackage(*Outputs[0].LongPackageName);
    UObjectRedirector* Redirector = NewObject<UObjectRedirector>(
        RedirectorPackage,
        FName(*Outputs[0].AssetName),
        RF_Public | RF_Standalone);
    FAssetRegistryModule::AssetCreated(Redirector);
    RedirectorPackage->SetDirtyFlag(false);

    TestFalse(TEXT("Redirector collision is rejected"), RedirectorWriter.BuildPreflight(Options, Preflight));

    FAssetRegistryModule::AssetDeleted(Redirector);
    Redirector->ClearFlags(RF_Public | RF_Standalone);
    Redirector->MarkAsGarbage();
    RedirectorPackage->SetDirtyFlag(false);
    RedirectorPackage->MarkAsGarbage();

    const FString DirtyRoot = FString::Printf(
        TEXT("/Game/CubeShotTests/Dirty_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Config.OutputRoot = DirtyRoot;
    FCubeShotAssetWriter DirtyWriter(Config);
    Outputs.Reset();
    FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
        CubeMesh->GetOutermost()->GetName(),
        CubeMesh->GetName(),
        DirtyRoot,
        Outputs,
        PathError);

    UPackage* DirtyPackage = CreatePackage(*Outputs[0].LongPackageName);
    UTexture2D* DirtyTexture = NewObject<UTexture2D>(
        DirtyPackage,
        FName(*Outputs[0].AssetName),
        RF_Public | RF_Standalone);
    DirtyPackage->SetDirtyFlag(true);

    TestFalse(TEXT("Already-dirty target package is rejected"), DirtyWriter.BuildPreflight(Options, Preflight));
    TestTrue(TEXT("Dirty collision reports an error"), !Preflight.Error.IsEmpty());

    DirtyTexture->ClearFlags(RF_Public | RF_Standalone);
    DirtyTexture->MarkAsGarbage();
    DirtyPackage->SetDirtyFlag(false);
    DirtyPackage->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCubeShotAssetIdentityTest,
    "CubeShot.Editor.AssetWriter.ObjectIdentityAndAssetCreated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubeShotAssetIdentityTest::RunTest(const FString& Parameters)
{
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh))
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString UniqueRoot = TEXT("/Game/CubeShotTests/Identity_") + Guid;
    const FString StagingRoot = FPaths::ProjectSavedDir() / TEXT("CubeShot/Automation/Identity_") + Guid;

    FCubeShotAssetWriterConfig Config;
    Config.OutputRoot = UniqueRoot;
    Config.StagingRoot = StagingRoot;
    Config.bPromptForCheckout = false;
    Config.bSyncContentBrowserOnSuccess = false;
    FCubeShotAssetWriter Writer(Config);

    FCubeShotOptions Options;
    Options.StaticMesh = CubeMesh;
    Options.Resolution = CubeShot::MinResolution;

    int32 AssetCreatedCount = 0;
    IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
    const FDelegateHandle AssetCreatedHandle = AssetRegistry.OnInMemoryAssetCreated().AddLambda(
        [&AssetCreatedCount, &UniqueRoot](UObject* Asset)
        {
            if (Asset != nullptr && Asset->GetPathName().StartsWith(UniqueRoot))
            {
                ++AssetCreatedCount;
            }
        });

    auto MakeSolidCapture = [](const FColor Color) -> FCubeShotCaptureCallback
    {
        return [Color](
            const FCubeShotOptions& CaptureOptions,
            ECubeShotDirection Direction,
            int32 TargetResolution,
            FCubeShotImage& OutImage,
            FText& OutError)
        {
            OutImage.Width = TargetResolution;
            OutImage.Height = TargetResolution;
            OutImage.Pixels.Init(Color, TargetResolution * TargetResolution);
            OutImage.bHasTransparency = Color.A < 255;
            OutError = FText::GetEmpty();
            return true;
        };
    };

    FCubeShotPreflightResult FirstPreflight;
    FCubeShotGenerationResult FirstResult;
    if (TestTrue(TEXT("First preflight succeeds"), Writer.BuildPreflight(Options, FirstPreflight)))
    {
        Writer.ExecuteWithCaptureCallback(
            Options,
            MakeSolidCapture(FColor(200, 30, 10, 255)),
            FirstPreflight,
            false,
            FCubeShotProgressCallback(),
            FCubeShotCancelCallback(),
            FirstResult);
        TestTrue(TEXT("First generation saves six textures"), FirstResult.bSucceeded);
        TestEqual(TEXT("AssetCreated called for each new texture"), AssetCreatedCount, 6);
    }

    UTexture2D* OriginalFront = nullptr;
    if (!FirstPreflight.Outputs.IsEmpty())
    {
        OriginalFront = LoadObject<UTexture2D>(nullptr, *FirstPreflight.Outputs[0].GetObjectPath());
    }
    TestNotNull(TEXT("Front texture exists after first write"), OriginalFront);
    TArray<TStrongObjectPtr<UTexture2D>> OriginalTextures;
    for(const auto& Output:FirstPreflight.Outputs)OriginalTextures.Emplace(LoadObject<UTexture2D>(nullptr,*Output.GetObjectPath()));

    FCubeShotPreflightResult SecondPreflight;
    FCubeShotGenerationResult SecondResult;
    if (TestTrue(TEXT("Overwrite preflight succeeds"), Writer.BuildPreflight(Options, SecondPreflight)))
    {
        TestTrue(TEXT("Overwrite is reported once for the set"), SecondPreflight.bHasExistingTextures);
        Writer.ExecuteWithCaptureCallback(
            Options,
            MakeSolidCapture(FColor(10, 40, 220, 255)),
            SecondPreflight,
            true,
            FCubeShotProgressCallback(),
            FCubeShotCancelCallback(),
            SecondResult);
        TestTrue(TEXT("Overwrite generation saves six textures"), SecondResult.bSucceeded);
        TestEqual(TEXT("Overwrite does not emit AssetCreated"), AssetCreatedCount, 6);
    }

    UTexture2D* UpdatedFront = nullptr;
    if (!SecondPreflight.Outputs.IsEmpty())
    {
        UpdatedFront = LoadObject<UTexture2D>(nullptr, *SecondPreflight.Outputs[0].GetObjectPath());
    }
    TestTrue(TEXT("Existing Texture2D object identity is preserved"), OriginalFront == UpdatedFront && UpdatedFront != nullptr);
    TestEqual(TEXT("All six original textures strongly held"),OriginalTextures.Num(),6);
    for(int32 Index=0;Index<OriginalTextures.Num();++Index)
    {
        UTexture2D* Updated=LoadObject<UTexture2D>(nullptr,*FirstPreflight.Outputs[Index].GetObjectPath());
        TestTrue(TEXT("Every Texture2D retains UObject identity"),Updated==OriginalTextures[Index].Get()&&Updated!=nullptr);
        if(Updated)
        {
            FImage Source;TestTrue(TEXT("Updated source reads"),Updated->Source.GetMipImage(Source,0));FImage Converted;Source.CopyTo(Converted,ERawImageFormat::BGRA8,EGammaSpace::sRGB);
            if(!Converted.RawData.IsEmpty())TestEqual(TEXT("Source pixels replaced by second capture"),Converted.AsBGRA8()[0],FColor(10,40,220,255));
            TestFalse(TEXT("Successful overwritten package is clean"),Updated->GetOutermost()->IsDirty());
        }
    }
    OriginalTextures.Reset();

    AssetRegistry.OnInMemoryAssetCreated().Remove(AssetCreatedHandle);

    TArray<UObject*> AssetsToDelete;
    const TArray<FCubeShotOutputDescriptor>& CleanupOutputs = SecondPreflight.Outputs.IsEmpty()
        ? FirstPreflight.Outputs
        : SecondPreflight.Outputs;
    for (const FCubeShotOutputDescriptor& Output : CleanupOutputs)
    {
        if (UObject* Asset = FindObject<UObject>(nullptr, *Output.GetObjectPath()))
        {
            AssetsToDelete.Add(Asset);
        }
    }
    if (!AssetsToDelete.IsEmpty())
    {
        TestEqual(
            TEXT("All test textures are deleted"),
            ObjectTools::DeleteObjectsUnchecked(AssetsToDelete),
            AssetsToDelete.Num());
    }

    FString OutputDirectory;
    if (FPackageName::TryConvertLongPackageNameToFilename(UniqueRoot, OutputDirectory))
    {
        IFileManager::Get().DeleteDirectory(*OutputDirectory, false, true);
    }
    IFileManager::Get().DeleteDirectory(*StagingRoot, false, true);
    // Non-recursive removal is intentionally safe if another test happens to be
    // using the shared parent at the same time.
    IFileManager::Get().DeleteDirectory(
        *(FPaths::ProjectContentDir() / TEXT("CubeShotTests")),
        false,
        false);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
