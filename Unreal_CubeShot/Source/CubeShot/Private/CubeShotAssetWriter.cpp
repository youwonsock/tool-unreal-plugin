#include "CubeShotAssetWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "CubeShotCaptureService.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Serialization/Archive.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UnrealEdGlobals.h"

#define LOCTEXT_NAMESPACE "CubeShotAssetWriter"

namespace
{
    FString NormalizeLongPackageRoot(FString Root)
    {
        Root.TrimStartAndEndInline();
        Root.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
        {
            Root.LeftChopInline(1, EAllowShrinking::No);
        }
        return Root;
    }

    bool AreOutputsEqual(const TArray<FCubeShotOutputDescriptor>& A, const TArray<FCubeShotOutputDescriptor>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }

        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            if (A[Index].Direction != B[Index].Direction
                || A[Index].LongPackageName != B[Index].LongPackageName
                || A[Index].AssetName != B[Index].AssetName)
            {
                return false;
            }
        }
        return true;
    }

    FString DirectionStagingFilename(const FString& Directory, const int32 Index, const FCubeShotOutputDescriptor& Output)
    {
        return Directory / FString::Printf(
            TEXT("%02d_%s.png"),
            Index,
            CubeShot::GetDirectionInfo(Output.Direction).AssetSuffix);
    }

    bool IsExpectedTextureAsset(const FAssetData& AssetData, const FCubeShotOutputDescriptor& Output, FText& OutError)
    {
        if (AssetData.IsRedirector())
        {
            OutError = FText::Format(
                LOCTEXT("RedirectorCollision", "The output path contains a redirector: {0}"),
                FText::FromString(Output.GetObjectPath()));
            return false;
        }

        if (AssetData.AssetName.ToString() != Output.AssetName)
        {
            OutError = FText::Format(
                LOCTEXT("UnexpectedTopLevelAsset", "Package {0} contains an unexpected top-level asset ({1})."),
                FText::FromString(Output.LongPackageName),
                FText::FromName(AssetData.AssetName));
            return false;
        }

        if (AssetData.AssetClassPath != UTexture2D::StaticClass()->GetClassPathName())
        {
            OutError = FText::Format(
                LOCTEXT("WrongAssetType", "The output path is occupied by a non-Texture2D asset: {0}"),
                FText::FromString(Output.GetObjectPath()));
            return false;
        }

        return true;
    }
}

FCubeShotAssetWriter::FCubeShotAssetWriter(FCubeShotAssetWriterConfig InConfig)
    : Config(MoveTemp(InConfig))
{
    Config.OutputRoot = NormalizeLongPackageRoot(Config.OutputRoot);
}

FString FCubeShotAssetWriter::SanitizeAssetName(const FString& InName)
{
    FString Result = ObjectTools::SanitizeObjectName(InName);
    Result.TrimStartAndEndInline();
    return Result.IsEmpty() ? TEXT("Asset") : Result;
}

TextureMipGenSettings FCubeShotAssetWriter::GetMipPolicy(const int32 Resolution)
{
    return CubeShot::IsPowerOfTwoResolution(Resolution) ? TMGS_FromTextureGroup : TMGS_NoMipmaps;
}

bool FCubeShotAssetWriter::BuildOutputDescriptorsFromSource(
    const FString& SourceLongPackageName,
    const FString& SourceAssetName,
    const FString& OutputRoot,
    TArray<FCubeShotOutputDescriptor>& OutOutputs,
    FText& OutError)
{
    OutOutputs.Reset();
    OutError = FText::GetEmpty();

    FText ValidationReason;
    if (!FPackageName::IsValidLongPackageName(SourceLongPackageName, true, &ValidationReason))
    {
        OutError = FText::Format(
            LOCTEXT("InvalidSourcePackage", "The mesh package path is invalid: {0}"),
            ValidationReason);
        return false;
    }

    const FString NormalizedRoot = NormalizeLongPackageRoot(OutputRoot);
    if (!NormalizedRoot.StartsWith(TEXT("/Game/"))
        || !FPackageName::IsValidLongPackageName(NormalizedRoot, false, &ValidationReason))
    {
        OutError = FText::Format(
            LOCTEXT("InvalidOutputRoot", "CubeShot output root must be a writable /Game path: {0}"),
            ValidationReason.IsEmpty() ? FText::FromString(NormalizedRoot) : ValidationReason);
        return false;
    }

    const FString MountName = FPackageName::GetPackageMountPoint(SourceLongPackageName).ToString();
    if (MountName.IsEmpty())
    {
        OutError = LOCTEXT("UnknownSourceMount", "The mesh is not stored in a mounted content path.");
        return false;
    }

    const FString MountPrefix = FString::Printf(TEXT("/%s/"), *MountName);
    FString RelativePackage = SourceLongPackageName;
    if (!RelativePackage.RemoveFromStart(MountPrefix) || RelativePackage.IsEmpty())
    {
        OutError = LOCTEXT("InvalidSourceRelativePath", "The mesh package has no path below its mount point.");
        return false;
    }

    FString DestinationFolder = NormalizedRoot;
    if (MountName.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
    {
        DestinationFolder /= RelativePackage;
    }
    else
    {
        DestinationFolder /= TEXT("_Mounts");
        DestinationFolder /= SanitizeAssetName(MountName);
        DestinationFolder /= RelativePackage;
    }

    const FString SafeSourceName = SanitizeAssetName(SourceAssetName);
    for (const FCubeShotDirectionInfo& Direction : CubeShot::GetDirectionInfos())
    {
        FCubeShotOutputDescriptor& Output = OutOutputs.AddDefaulted_GetRef();
        Output.Direction = Direction.Direction;
        Output.AssetName = FString::Printf(TEXT("T_%s_%s"), *SafeSourceName, Direction.AssetSuffix);
        Output.LongPackageName = DestinationFolder / Output.AssetName;

        if (!FPackageName::IsValidObjectPath(Output.GetObjectPath(), &ValidationReason))
        {
            OutError = FText::Format(
                LOCTEXT("InvalidGeneratedPath", "CubeShot generated an invalid asset path ({0}): {1}"),
                FText::FromString(Output.GetObjectPath()),
                ValidationReason);
            OutOutputs.Reset();
            return false;
        }
    }

    return true;
}

bool FCubeShotAssetWriter::ValidateOutputRoot(FText& OutError) const
{
    FText ValidationReason;
    if (!Config.OutputRoot.StartsWith(TEXT("/Game/"))
        || !FPackageName::IsValidLongPackageName(Config.OutputRoot, false, &ValidationReason))
    {
        OutError = FText::Format(
            LOCTEXT("OutputRootNotWritable", "Output root must be a writable path below /Game: {0}"),
            ValidationReason.IsEmpty() ? FText::FromString(Config.OutputRoot) : ValidationReason);
        return false;
    }

    FString ProbeFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(
        Config.OutputRoot / TEXT("CubeShotWriteProbe"),
        ProbeFilename,
        FPackageName::GetAssetPackageExtension()))
    {
        OutError = FText::Format(
            LOCTEXT("OutputRootNotMounted", "Output root is not mapped to a writable content mount: {0}"),
            FText::FromString(Config.OutputRoot));
        return false;
    }

    // Path conversion alone does not prove that the mounted Content directory is
    // writable. Probe the closest existing parent before any capture or UObject
    // mutation so a read-only workspace fails during preflight.
    FString ProbeDirectory = FPaths::GetPath(ProbeFilename);
    while (!ProbeDirectory.IsEmpty() && !IFileManager::Get().DirectoryExists(*ProbeDirectory))
    {
        const FString ParentDirectory = FPaths::GetPath(ProbeDirectory);
        if (ParentDirectory == ProbeDirectory)
        {
            ProbeDirectory.Reset();
            break;
        }
        ProbeDirectory = ParentDirectory;
    }

    if (ProbeDirectory.IsEmpty())
    {
        OutError = FText::Format(
            LOCTEXT("OutputRootDirectoryMissing", "CubeShot could not resolve a writable directory for output root: {0}"),
            FText::FromString(Config.OutputRoot));
        return false;
    }

    const FString WriteProbePath = ProbeDirectory / FString::Printf(
        TEXT(".cubeshot_write_probe_%s.tmp"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TUniquePtr<FArchive> WriteProbe(IFileManager::Get().CreateFileWriter(*WriteProbePath));
    if (!WriteProbe.IsValid())
    {
        OutError = FText::Format(
            LOCTEXT("OutputRootWriteProbeFailed", "Output root is not writable: {0}"),
            FText::FromString(Config.OutputRoot));
        return false;
    }
    WriteProbe->Close();
    WriteProbe.Reset();

    if (!IFileManager::Get().Delete(*WriteProbePath, false, true, true))
    {
        OutError = FText::Format(
            LOCTEXT("OutputRootWriteProbeCleanupFailed", "CubeShot could not clean up its write probe below: {0}"),
            FText::FromString(Config.OutputRoot));
        return false;
    }

    OutError = FText::GetEmpty();
    return true;
}

bool FCubeShotAssetWriter::InspectOutput(
    const FCubeShotOutputDescriptor& Output,
    bool& bOutExists,
    FText& OutError) const
{
    bOutExists = false;
    OutError = FText::GetEmpty();

    FText ValidationReason;
    if (!FPackageName::IsValidLongPackageName(Output.LongPackageName, false, &ValidationReason)
        || !FPackageName::IsValidObjectPath(Output.GetObjectPath(), &ValidationReason))
    {
        OutError = FText::Format(
            LOCTEXT("InvalidOutputPath", "Invalid output path {0}: {1}"),
            FText::FromString(Output.GetObjectPath()),
            ValidationReason);
        return false;
    }

    FString PackageFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(
        Output.LongPackageName,
        PackageFilename,
        FPackageName::GetAssetPackageExtension()))
    {
        OutError = FText::Format(
            LOCTEXT("UnwritableOutputMount", "Output package is not in a writable mounted path: {0}"),
            FText::FromString(Output.LongPackageName));
        return false;
    }

    UPackage* Package = FindPackage(nullptr, *Output.LongPackageName);
    if (Package != nullptr && Package->IsDirty())
    {
        OutError = FText::Format(
            LOCTEXT("DirtyOutputPackage", "The target package already has unsaved changes: {0}"),
            FText::FromString(Output.LongPackageName));
        return false;
    }

    IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
    TArray<FAssetData> PackageAssets;
    AssetRegistry.GetAssetsByPackageName(FName(*Output.LongPackageName), PackageAssets, false);

    if (PackageAssets.Num() > 1)
    {
        OutError = FText::Format(
            LOCTEXT("MultipleTopLevelAssets", "Package {0} contains more than one top-level asset."),
            FText::FromString(Output.LongPackageName));
        return false;
    }
    if (PackageAssets.Num() == 1 && !IsExpectedTextureAsset(PackageAssets[0], Output, OutError))
    {
        return false;
    }

    const bool bPackageFileExists = FPackageName::DoesPackageExist(Output.LongPackageName);
    if (Package == nullptr && (bPackageFileExists || PackageAssets.Num() > 0))
    {
        Package = LoadPackage(nullptr, *Output.LongPackageName, LOAD_None);
        if (Package == nullptr)
        {
            OutError = FText::Format(
                LOCTEXT("TargetLoadFailed", "Existing target package could not be loaded: {0}"),
                FText::FromString(Output.LongPackageName));
            return false;
        }
    }

    if (Package != nullptr)
    {
        if (Package->IsDirty())
        {
            OutError = FText::Format(
                LOCTEXT("DirtyAfterLoad", "The target package has unsaved changes: {0}"),
                FText::FromString(Output.LongPackageName));
            return false;
        }

        TArray<UObject*> TopLevelObjects;
        GetObjectsWithPackage(Package, TopLevelObjects, EGetObjectsFlags::None, RF_Transient);
        TopLevelObjects.RemoveAll([](const UObject* Object)
        {
            return Object == nullptr || !Object->IsAsset();
        });

        if (TopLevelObjects.Num() > 1)
        {
            OutError = FText::Format(
                LOCTEXT("UnexpectedLoadedAssets", "Package {0} contains unexpected top-level assets."),
                FText::FromString(Output.LongPackageName));
            return false;
        }

        if (TopLevelObjects.Num() == 1)
        {
            UObject* ExistingObject = TopLevelObjects[0];
            if (ExistingObject->GetName() != Output.AssetName || !ExistingObject->IsA<UTexture2D>())
            {
                OutError = FText::Format(
                    LOCTEXT("UnexpectedLoadedObject", "Output package is occupied by an unexpected asset: {0}"),
                    FText::FromString(ExistingObject->GetPathName()));
                return false;
            }
            bOutExists = true;
        }
        else if (bPackageFileExists || PackageAssets.Num() > 0)
        {
            OutError = FText::Format(
                LOCTEXT("ExistingPackageWithoutAsset", "Existing output package does not contain the expected Texture2D: {0}"),
                FText::FromString(Output.GetObjectPath()));
            return false;
        }

        if (GUnrealEd != nullptr && !GUnrealEd->CanSavePackage(Package))
        {
            OutError = FText::Format(
                LOCTEXT("CannotSaveTarget", "The editor reports that this target package cannot be saved: {0}"),
                FText::FromString(Output.LongPackageName));
            return false;
        }
    }

    bOutExists |= PackageAssets.Num() == 1 || bPackageFileExists;
    if (bOutExists && IFileManager::Get().FileExists(*PackageFilename) && IFileManager::Get().IsReadOnly(*PackageFilename))
    {
        // A read-only source-controlled file is allowed only when checkout prompts are enabled.
        if (!Config.bPromptForCheckout)
        {
            OutError = FText::Format(
                LOCTEXT("ReadOnlyTarget", "The existing target package is read-only: {0}"),
                FText::FromString(PackageFilename));
            return false;
        }
    }

    return true;
}

bool FCubeShotAssetWriter::BuildPreflight(
    const FCubeShotOptions& Options,
    FCubeShotPreflightResult& OutResult) const
{
    OutResult = FCubeShotPreflightResult();

    FText OptionsWarning;
    if (!CubeShot::ValidateOptions(Options, OutResult.Error, OptionsWarning))
    {
        return false;
    }
    if (!ValidateOutputRoot(OutResult.Error))
    {
        return false;
    }

    if (!BuildOutputDescriptorsFromSource(
        Options.StaticMesh->GetOutermost()->GetName(),
        Options.StaticMesh->GetName(),
        Config.OutputRoot,
        OutResult.Outputs,
        OutResult.Error))
    {
        return false;
    }

    for (const FCubeShotOutputDescriptor& Output : OutResult.Outputs)
    {
        bool bExists = false;
        if (!InspectOutput(Output, bExists, OutResult.Error))
        {
            OutResult.Outputs.Reset();
            return false;
        }
        OutResult.bHasExistingTextures |= bExists;
    }

    OutResult.Warning = OptionsWarning;
    if (OutResult.bHasExistingTextures)
    {
        const FText OverwriteWarning = LOCTEXT(
            "OverwriteWarning",
            "One or more CubeShot textures already exist. Their UObject identity will be preserved and their source pixels will be replaced.");
        OutResult.Warning = OutResult.Warning.IsEmpty()
            ? OverwriteWarning
            : FText::Format(LOCTEXT("CombinedWarnings", "{0}\n{1}"), OutResult.Warning, OverwriteWarning);
    }

    OutResult.bCanProceed = true;
    return true;
}

FString FCubeShotAssetWriter::GetResolvedStagingRoot() const
{
    FString Root = Config.StagingRoot.IsEmpty()
        ? FPaths::ProjectSavedDir() / TEXT("CubeShot/Staging")
        : Config.StagingRoot;
    return FPaths::ConvertRelativePathToFull(Root);
}

FString FCubeShotAssetWriter::CreateStagingDirectory(FText& OutError) const
{
    const FString Root = GetResolvedStagingRoot();
    const FString Directory = Root / FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    if (!IFileManager::Get().MakeDirectory(*Directory, true))
    {
        OutError = FText::Format(
            LOCTEXT("CreateStagingFailed", "Could not create CubeShot staging directory: {0}"),
            FText::FromString(Directory));
        return FString();
    }
    return Directory;
}

bool FCubeShotAssetWriter::DeleteStagingDirectory(const FString& StagingDirectory) const
{
    const FString Root = GetResolvedStagingRoot();
    const FString FullDirectory = FPaths::ConvertRelativePathToFull(StagingDirectory);
    if (FullDirectory.IsEmpty() || FullDirectory == Root || !FPaths::IsUnderDirectory(FullDirectory, Root))
    {
        return false;
    }
    return IFileManager::Get().DeleteDirectory(*FullDirectory, false, true);
}

bool FCubeShotAssetWriter::StageCaptures(
    const FCubeShotOptions& Options,
    const FCubeShotCaptureCallback& Capture,
    const TArray<FCubeShotOutputDescriptor>& Outputs,
    const FString& StagingDirectory,
    const FCubeShotProgressCallback& Progress,
    const FCubeShotCancelCallback& ShouldCancel,
    TArray<FString>& OutStagedFilenames,
    FCubeShotGenerationResult& OutResult) const
{
    OutStagedFilenames.Reset();
    OutStagedFilenames.Reserve(Outputs.Num());

    if (Progress)
    {
        Progress(0, Outputs.Num(), LOCTEXT("StartingCapture", "Preparing six CubeShot captures..."));
    }

    for (int32 Index = 0; Index < Outputs.Num(); ++Index)
    {
        if (ShouldCancel && ShouldCancel())
        {
            OutResult.bCancelled = true;
            OutResult.Message = LOCTEXT("CaptureCancelled", "CubeShot generation was cancelled before any asset was changed.");
            return false;
        }

        const FCubeShotOutputDescriptor& Output = Outputs[Index];
        FCubeShotImage CapturedImage;
        FText CaptureError;
        if (!Capture || !Capture(
            Options,
            Output.Direction,
            Options.Resolution,
            CapturedImage,
            CaptureError)
            || !CapturedImage.IsValid())
        {
            OutResult.Message = CaptureError.IsEmpty()
                ? LOCTEXT("InvalidCapturedImage", "CubeShot returned an invalid captured image.")
                : CaptureError;
            return false;
        }

        FImage StagingImage;
        StagingImage.Init(CapturedImage.Width, CapturedImage.Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        FMemory::Memcpy(
            StagingImage.RawData.GetData(),
            CapturedImage.Pixels.GetData(),
            CapturedImage.Pixels.Num() * sizeof(FColor));
        CapturedImage.Reset();

        const FString Filename = DirectionStagingFilename(StagingDirectory, Index, Output);
        if (!FImageUtils::SaveImageByExtension(*Filename, StagingImage, 100))
        {
            OutResult.Message = FText::Format(
                LOCTEXT("StageWriteFailed", "Failed to write lossless staging image: {0}"),
                FText::FromString(Filename));
            return false;
        }
        StagingImage.RawData.Reset();

        // Read back once before the mutation boundary so a corrupt staging file cannot cause a partial update later.
        FImage ValidationImage;
        if (!FImageUtils::LoadImage(*Filename, ValidationImage)
            || ValidationImage.SizeX != Options.Resolution
            || ValidationImage.SizeY != Options.Resolution
            || ValidationImage.NumSlices != 1)
        {
            OutResult.Message = FText::Format(
                LOCTEXT("StageValidationFailed", "Staging image could not be validated: {0}"),
                FText::FromString(Filename));
            return false;
        }
        ValidationImage.RawData.Reset();
        OutStagedFilenames.Add(Filename);

        if (Progress)
        {
            Progress(
                Index + 1,
                Outputs.Num(),
                FText::Format(
                    LOCTEXT("CapturedDirection", "Captured {0} ({1}/{2})"),
                    FText::FromString(CubeShot::GetDirectionInfo(Output.Direction).Label),
                    Index + 1,
                    Outputs.Num()));
        }
    }

    if (ShouldCancel && ShouldCancel())
    {
        OutResult.bCancelled = true;
        OutResult.Message = LOCTEXT("CancelledAfterCapture", "CubeShot generation was cancelled before any asset was changed.");
        return false;
    }

    return true;
}

bool FCubeShotAssetWriter::CheckoutExistingPackages(
    const TArray<FCubeShotOutputDescriptor>& Outputs,
    FText& OutError) const
{
    TArray<UPackage*> ExistingPackages;
    for (const FCubeShotOutputDescriptor& Output : Outputs)
    {
        if (!FPackageName::DoesPackageExist(Output.LongPackageName))
        {
            continue;
        }

        UPackage* Package = FindPackage(nullptr, *Output.LongPackageName);
        if (Package == nullptr)
        {
            Package = LoadPackage(nullptr, *Output.LongPackageName, LOAD_None);
        }
        if (Package == nullptr)
        {
            OutError = FText::Format(
                LOCTEXT("CheckoutLoadFailed", "Could not load existing package for checkout: {0}"),
                FText::FromString(Output.LongPackageName));
            return false;
        }
        if (Package->IsDirty())
        {
            OutError = FText::Format(
                LOCTEXT("DirtyBeforeCheckout", "Target package became dirty during capture: {0}"),
                FText::FromString(Output.LongPackageName));
            return false;
        }
        ExistingPackages.Add(Package);
    }

    if (ExistingPackages.IsEmpty())
    {
        return true;
    }

    if (Config.bPromptForCheckout)
    {
        TArray<UPackage*> CheckedOutOrWritable;
        TArray<UPackage*> NotNeedingCheckout;
        if (!FEditorFileUtils::PromptToCheckoutPackages(
            false,
            ExistingPackages,
            &CheckedOutOrWritable,
            &NotNeedingCheckout,
            false,
            false))
        {
            OutError = LOCTEXT("CheckoutCancelled", "Checkout was cancelled or one of the existing textures could not be made writable.");
            return false;
        }

        // PromptToCheckoutPackages reports whether the dialog itself was accepted. Users may
        // leave individual packages unchecked, so success alone is not a safe mutation boundary.
        TSet<UPackage*> ReadyPackages;
        for (UPackage* Package : CheckedOutOrWritable)
        {
            if (Package != nullptr)
            {
                ReadyPackages.Add(Package);
            }
        }
        for (UPackage* Package : NotNeedingCheckout)
        {
            if (Package != nullptr)
            {
                ReadyPackages.Add(Package);
            }
        }

        for (UPackage* Package : ExistingPackages)
        {
            const FString Filename = FPackageName::LongPackageNameToFilename(
                Package->GetName(),
                FPackageName::GetAssetPackageExtension());
            if (!ReadyPackages.Contains(Package)
                || (IFileManager::Get().FileExists(*Filename) && IFileManager::Get().IsReadOnly(*Filename)))
            {
                OutError = FText::Format(
                    LOCTEXT(
                        "CheckoutIncomplete",
                        "Every existing CubeShot texture must be checked out or writable before generation. Not ready: {0}"),
                    FText::FromString(Package->GetName()));
                return false;
            }
        }
        return true;
    }

    for (UPackage* Package : ExistingPackages)
    {
        const FString Filename = FPackageName::LongPackageNameToFilename(
            Package->GetName(),
            FPackageName::GetAssetPackageExtension());
        if (IFileManager::Get().IsReadOnly(*Filename))
        {
            OutError = FText::Format(
                LOCTEXT("UnattendedReadOnly", "Target package is read-only and checkout prompts are disabled: {0}"),
                FText::FromString(Filename));
            return false;
        }
    }
    return true;
}

bool FCubeShotAssetWriter::ApplyImageToTexture(
    UTexture2D& Texture,
    const FImage& Image,
    const int32 Resolution,
    FText& OutError)
{
    if (Image.SizeX != Resolution
        || Image.SizeY != Resolution
        || Image.NumSlices != 1
        || Image.Format != ERawImageFormat::BGRA8)
    {
        OutError = LOCTEXT("InvalidTextureSourceImage", "Staged CubeShot image has invalid dimensions or pixel format.");
        return false;
    }

    Texture.Modify();
    Texture.PreEditChange(nullptr);
    Texture.Source.Init(Image);
    Texture.SRGB = true;
    Texture.CompressionSettings = TC_Default;
    Texture.CompressionNoAlpha = false;
    Texture.LODGroup = TEXTUREGROUP_World;
    Texture.AddressX = TA_Clamp;
    Texture.AddressY = TA_Clamp;
    Texture.VirtualTextureStreaming = false;
    Texture.MipGenSettings = GetMipPolicy(Resolution);
    Texture.SetModernSettingsForNewOrChangedTexture();
    Texture.PostEditChange();
    Texture.MarkPackageDirty();
    OutError = FText::GetEmpty();
    return true;
}

bool FCubeShotAssetWriter::PrepareAsset(
    const FCubeShotOutputDescriptor& Output,
    const FString& StagedFilename,
    FPreparedAsset& OutPrepared,
    FText& OutError) const
{
    FImage LoadedImage;
    if (!FImageUtils::LoadImage(*StagedFilename, LoadedImage))
    {
        OutError = FText::Format(
            LOCTEXT("LoadStagedImageFailed", "Failed to load staging image: {0}"),
            FText::FromString(StagedFilename));
        return false;
    }

    if (LoadedImage.Format != ERawImageFormat::BGRA8 || LoadedImage.GammaSpace != EGammaSpace::sRGB)
    {
        FImage ConvertedImage;
        LoadedImage.CopyTo(ConvertedImage, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        LoadedImage = MoveTemp(ConvertedImage);
    }

    UPackage* Package = FindPackage(nullptr, *Output.LongPackageName);
    if (Package == nullptr)
    {
        Package = CreatePackage(*Output.LongPackageName);
    }
    if (Package == nullptr)
    {
        OutError = FText::Format(
            LOCTEXT("CreatePackageFailed", "Failed to create output package: {0}"),
            FText::FromString(Output.LongPackageName));
        return false;
    }

    UObject* ExistingObject = StaticFindObjectFast(UObject::StaticClass(), Package, FName(*Output.AssetName));
    UTexture2D* Texture = Cast<UTexture2D>(ExistingObject);
    if (ExistingObject != nullptr && Texture == nullptr)
    {
        OutError = FText::Format(
            LOCTEXT("LateTypeCollision", "Output path became occupied by another asset during generation: {0}"),
            FText::FromString(Output.GetObjectPath()));
        return false;
    }

    const bool bCreated = Texture == nullptr;
    if (bCreated)
    {
        Texture = NewObject<UTexture2D>(
            Package,
            FName(*Output.AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
    }
    if (Texture == nullptr || !ApplyImageToTexture(*Texture, LoadedImage, LoadedImage.SizeX, OutError))
    {
        return false;
    }

    if (bCreated)
    {
        FAssetRegistryModule::AssetCreated(Texture);
    }

    OutPrepared.Descriptor = Output;
    OutPrepared.StagedFilename = StagedFilename;
    OutPrepared.Package = Package;
    OutPrepared.Texture = Texture;
    OutPrepared.bWasCreated = bCreated;
    return true;
}

void FCubeShotAssetWriter::SavePreparedAssets(
    TArray<FPreparedAsset>& PreparedAssets,
    FCubeShotGenerationResult& OutResult) const
{
    for (FPreparedAsset& Prepared : PreparedAssets)
    {
        if (Prepared.Package == nullptr || Prepared.Texture == nullptr)
        {
            OutResult.FailedObjectPaths.Add(Prepared.Descriptor.GetObjectPath());
            continue;
        }

        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            Prepared.Descriptor.LongPackageName,
            FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        SaveArgs.Error = GError;

        if (UPackage::SavePackage(Prepared.Package, Prepared.Texture, *PackageFilename, SaveArgs))
        {
            OutResult.SavedObjectPaths.Add(Prepared.Descriptor.GetObjectPath());
        }
        else
        {
            // The package intentionally remains dirty and staging is retained for manual recovery.
            Prepared.Package->SetDirtyFlag(true);
            OutResult.FailedObjectPaths.Add(Prepared.Descriptor.GetObjectPath());
        }
    }
}

void FCubeShotAssetWriter::SyncContentBrowser(const TArray<FPreparedAsset>& PreparedAssets) const
{
    TArray<UObject*> Assets;
    Assets.Reserve(PreparedAssets.Num());
    for (const FPreparedAsset& Prepared : PreparedAssets)
    {
        if (Prepared.Texture != nullptr)
        {
            Assets.Add(Prepared.Texture);
        }
    }

    if (!Assets.IsEmpty())
    {
        FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
        ContentBrowser.Get().SyncBrowserToAssets(Assets, false, true);
    }
}

void FCubeShotAssetWriter::Execute(
    const FCubeShotOptions& Options,
    FCubeShotCaptureService& CaptureService,
    const FCubeShotPreflightResult& Preflight,
    const bool bOverwriteConfirmed,
    const FCubeShotProgressCallback& Progress,
    const FCubeShotCancelCallback& ShouldCancel,
    FCubeShotGenerationResult& OutResult) const
{
    const FCubeShotCaptureCallback Capture = [&CaptureService](
        const FCubeShotOptions& CaptureOptions,
        const ECubeShotDirection Direction,
        const int32 TargetResolution,
        FCubeShotImage& OutImage,
        FText& OutError)
    {
        return CaptureService.CaptureAndComposite(CaptureOptions, Direction, TargetResolution, OutImage, OutError);
    };
    ExecuteInternal(Options, Capture, Preflight, bOverwriteConfirmed, Progress, ShouldCancel, OutResult);
}

void FCubeShotAssetWriter::ExecuteWithCaptureCallback(
    const FCubeShotOptions& Options,
    const FCubeShotCaptureCallback& Capture,
    const FCubeShotPreflightResult& Preflight,
    const bool bOverwriteConfirmed,
    const FCubeShotProgressCallback& Progress,
    const FCubeShotCancelCallback& ShouldCancel,
    FCubeShotGenerationResult& OutResult) const
{
    ExecuteInternal(Options, Capture, Preflight, bOverwriteConfirmed, Progress, ShouldCancel, OutResult);
}

void FCubeShotAssetWriter::ExecuteInternal(
    const FCubeShotOptions& Options,
    const FCubeShotCaptureCallback& Capture,
    const FCubeShotPreflightResult& Preflight,
    const bool bOverwriteConfirmed,
    const FCubeShotProgressCallback& Progress,
    const FCubeShotCancelCallback& ShouldCancel,
    FCubeShotGenerationResult& OutResult) const
{
    OutResult = FCubeShotGenerationResult();

    if (!Preflight.bCanProceed || Preflight.Outputs.Num() != static_cast<int32>(ECubeShotDirection::Count))
    {
        OutResult.Message = LOCTEXT("InvalidPreflight", "CubeShot preflight is missing or no longer valid.");
        return;
    }
    if (Preflight.bHasExistingTextures && !bOverwriteConfirmed)
    {
        OutResult.Message = LOCTEXT("OverwriteNotConfirmed", "Existing CubeShot textures were not approved for overwrite.");
        return;
    }

    FCubeShotPreflightResult FreshPreflight;
    if (!BuildPreflight(Options, FreshPreflight)
        || !FreshPreflight.bCanProceed
        || !AreOutputsEqual(Preflight.Outputs, FreshPreflight.Outputs))
    {
        OutResult.Message = FreshPreflight.Error.IsEmpty()
            ? LOCTEXT("InputsChanged", "CubeShot inputs or output paths changed after preflight.")
            : FreshPreflight.Error;
        return;
    }
    if (FreshPreflight.bHasExistingTextures && !Preflight.bHasExistingTextures)
    {
        OutResult.Message = LOCTEXT("LateOverwriteCollision", "A CubeShot output appeared after preflight. Run Generate again to review the overwrite warning.");
        return;
    }

    FText StagingError;
    OutResult.StagingDirectory = CreateStagingDirectory(StagingError);
    if (OutResult.StagingDirectory.IsEmpty())
    {
        OutResult.Message = StagingError;
        return;
    }

    TArray<FString> StagedFilenames;
    if (!StageCaptures(
        Options,
        Capture,
        FreshPreflight.Outputs,
        OutResult.StagingDirectory,
        Progress,
        ShouldCancel,
        StagedFilenames,
        OutResult))
    {
        DeleteStagingDirectory(OutResult.StagingDirectory);
        OutResult.StagingDirectory.Reset();
        return;
    }

    // Close the time-of-check/time-of-use gap before the UObject mutation boundary.
    FCubeShotPreflightResult PostCapturePreflight;
    if (!BuildPreflight(Options, PostCapturePreflight)
        || !AreOutputsEqual(FreshPreflight.Outputs, PostCapturePreflight.Outputs)
        || (PostCapturePreflight.bHasExistingTextures && !FreshPreflight.bHasExistingTextures))
    {
        OutResult.Message = PostCapturePreflight.Error.IsEmpty()
            ? LOCTEXT("CollisionDuringCapture", "An output package changed while CubeShot was capturing.")
            : PostCapturePreflight.Error;
        DeleteStagingDirectory(OutResult.StagingDirectory);
        OutResult.StagingDirectory.Reset();
        return;
    }

    FText CheckoutError;
    if (!CheckoutExistingPackages(PostCapturePreflight.Outputs, CheckoutError))
    {
        OutResult.Message = CheckoutError;
        DeleteStagingDirectory(OutResult.StagingDirectory);
        OutResult.StagingDirectory.Reset();
        return;
    }

    TArray<FPreparedAsset> PreparedAssets;
    PreparedAssets.Reserve(PostCapturePreflight.Outputs.Num());
    for (int32 Index = 0; Index < PostCapturePreflight.Outputs.Num(); ++Index)
    {
        FPreparedAsset Prepared;
        FText PrepareError;
        if (PrepareAsset(PostCapturePreflight.Outputs[Index], StagedFilenames[Index], Prepared, PrepareError))
        {
            PreparedAssets.Add(MoveTemp(Prepared));
        }
        else
        {
            OutResult.FailedObjectPaths.Add(PostCapturePreflight.Outputs[Index].GetObjectPath());
            if (OutResult.Message.IsEmpty())
            {
                OutResult.Message = PrepareError;
            }
        }
    }

    SavePreparedAssets(PreparedAssets, OutResult);
    OutResult.bSucceeded = OutResult.FailedObjectPaths.IsEmpty()
        && OutResult.SavedObjectPaths.Num() == PostCapturePreflight.Outputs.Num();

    if (OutResult.bSucceeded)
    {
        if (Config.bSyncContentBrowserOnSuccess)
        {
            SyncContentBrowser(PreparedAssets);
        }
        if (DeleteStagingDirectory(OutResult.StagingDirectory))
        {
            OutResult.StagingDirectory.Reset();
            OutResult.Message = LOCTEXT("GenerationSucceeded", "Generated and saved all six CubeShot textures.");
        }
        else
        {
            OutResult.Message = FText::Format(
                LOCTEXT("GenerationSucceededCleanupFailed", "Generated all six textures, but the staging directory could not be removed: {0}"),
                FText::FromString(OutResult.StagingDirectory));
        }
    }
    else
    {
        OutResult.Message = FText::Format(
            LOCTEXT(
                "PartialSaveFailure",
                "CubeShot saved {0} texture(s) and failed {1}. Failed packages remain dirty and staging was kept at {2}."),
            OutResult.SavedObjectPaths.Num(),
            OutResult.FailedObjectPaths.Num(),
            FText::FromString(OutResult.StagingDirectory));
    }
}

#undef LOCTEXT_NAMESPACE
