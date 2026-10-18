#include "CubeShotAssetWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
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

    OutError = FText::GetEmpty();
    return true;
}

bool FCubeShotAssetWriter::ValidateOutputPermissions(FText& OutError) const
{
    if (!ValidateOutputRoot(OutError)) return false;
    FString ProbeFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Config.OutputRoot / TEXT("CubeShotWriteProbe"),
        ProbeFilename,FPackageName::GetAssetPackageExtension())) return false;
    // Path conversion alone does not prove that the mounted Content directory is
    // writable. Probe the closest existing parent before any capture or UObject
    // mutation so a read-only workspace fails during execution preparation.
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
    if (Config.ShouldFailOperation && Config.ShouldFailOperation(TEXT("write_probe"),WriteProbePath))
    { OutError=FText::FromString(TEXT("Output directory permission probe failed: ")+ProbeDirectory);return false; }
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
    if (Config.ShouldFailOperation && Config.ShouldFailOperation(TEXT("stage_cleanup"), StagingDirectory)) return false;
    const FString Root = GetResolvedStagingRoot();
    const FString FullDirectory = FPaths::ConvertRelativePathToFull(StagingDirectory);
    if (FullDirectory.IsEmpty() || FullDirectory == Root || !FPaths::IsUnderDirectory(FullDirectory, Root))
    {
        return false;
    }
    return IFileManager::Get().DeleteDirectory(*FullDirectory, false, true);
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
        ExistingPackages.AddUnique(Package);
    }

    if (ExistingPackages.IsEmpty())
    {
        return true;
    }

    if (Config.bPromptForCheckout)
    {
        if (Config.CheckoutCallback)
        {
            if (!Config.CheckoutCallback(ExistingPackages, OutError))
            { if (OutError.IsEmpty()) OutError=LOCTEXT("InjectedCheckoutCancelled","Checkout was cancelled."); return false; }
            for (UPackage* Package : ExistingPackages)
            {
                const FString Filename=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
                if (IFileManager::Get().IsReadOnly(*Filename))
                {OutError=FText::FromString(TEXT("Checkout incomplete: ")+Package->GetName());return false;}
            }
            return true;
        }
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

bool FCubeShotAssetWriter::CheckoutForInteractiveSubmission(const TArray<FCubeShotOutputDescriptor>& Outputs, FText& Error) const
{
    check(IsInGameThread());
    return CheckoutExistingPackages(Outputs, Error);
}

void FCubeShotAssetWriter::FinalizeStaging(FCubeShotGenerationResult& Result) const
{
    if (Result.StagingDirectory.IsEmpty()) return;
    if (DeleteStagingDirectory(Result.StagingDirectory))
    {
        Result.StagingDirectory.Reset();
        Result.StagedFilenames.Reset();
    }
    else Result.Warnings.AddUnique(TEXT("Staging cleanup failed; recovery images remain at ") + Result.StagingDirectory);
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

        if (!(Config.ShouldFailOperation && Config.ShouldFailOperation(TEXT("package_save"), PackageFilename))
            && UPackage::SavePackage(Prepared.Package, Prepared.Texture, *PackageFilename, SaveArgs))
        {
            OutResult.SavedObjectPaths.Add(Prepared.Descriptor.GetObjectPath());
        }
        else
        {
            // The package intentionally remains dirty and staging is retained for manual recovery.
            Prepared.Package->SetDirtyFlag(true);
            OutResult.FailedObjectPaths.Add(Prepared.Descriptor.GetObjectPath());
            OutResult.Errors.Add(TEXT("Package save failed: ") + PackageFilename);
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
    TUniquePtr<FExecution> Execution=BeginPreparedExecution(Options,[&CaptureService](const FCubeShotOptions& InOptions,ECubeShotDirection Direction,int32 Resolution,FCubeShotPixelWork& Work,FText& Error)
    {
        return CaptureService.PrepareCaptureWork(InOptions,Direction,Resolution,Work,Error);
    },Preflight,bOverwriteConfirmed,Progress,ShouldCancel);
    while(!Execution->Step(true)){}
    OutResult=Execution->GetResult();
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

namespace
{
    struct FStagePixelsResult { bool bSucceeded=false; FString Filename; FString Error; };
    FStagePixelsResult WriteOwnedPixels(FCubeShotPixelWork Work, FString Filename, int32 Resolution,
        TFunction<bool(const FString&,const FString&)> ShouldFail)
    {
        FStagePixelsResult Result; Result.Filename=Filename; FCubeShotImage Captured; FText Error;
        if (!Work || !Work(Captured,Error) || !Captured.IsValid())
        { Result.Error=Error.IsEmpty()?TEXT("Captured pixels are invalid."):Error.ToString();return Result; }
        FImage Image;Image.Init(Captured.Width,Captured.Height,ERawImageFormat::BGRA8,EGammaSpace::sRGB);
        FMemory::Memcpy(Image.RawData.GetData(),Captured.Pixels.GetData(),Captured.Pixels.Num()*sizeof(FColor));Captured.Reset();
        if ((ShouldFail&&ShouldFail(TEXT("stage_write"),Filename)) || !FImageUtils::SaveImageByExtension(*Filename,Image,100))
        { Result.Error=TEXT("Staging PNG write failed: ")+Filename;return Result; }
        Image.RawData.Reset();FImage Validation;
        if(!FImageUtils::LoadImage(*Filename,Validation)||Validation.SizeX!=Resolution||Validation.SizeY!=Resolution||Validation.NumSlices!=1)
        { Result.Error=TEXT("Staging PNG validation failed: ")+Filename;return Result; }
        Result.bSucceeded=true;return Result;
    }
}

struct FCubeShotAssetWriter::FExecution::FState
{
    FCubeShotAssetWriter Writer;
    FCubeShotOptions Options;
    FCubeShotPrepareCaptureCallback Capture;
    TFuture<FStagePixelsResult> PendingPixels;
    FCubeShotPreflightResult Preflight;
    FCubeShotProgressCallback Progress;
    FCubeShotCancelCallback ShouldCancel;
    FCubeShotGenerationResult Result;
    TArray<FString> Staged;
    int32 Face = 0;
    int32 Stage = 0;
    bool bOverwrite = false;
    FState(const FCubeShotAssetWriter& InWriter) : Writer(InWriter) {}

    void FinishBeforeMutation()
    {
        if (Writer.DeleteStagingDirectory(Result.StagingDirectory)) { Result.StagingDirectory.Reset(); Result.StagedFilenames.Reset(); }
        else if (!Result.StagingDirectory.IsEmpty())
            Result.Warnings.Add(TEXT("Staging cleanup failed; recovery images remain at ") + Result.StagingDirectory);
        Stage = 3;
    }
};

FCubeShotAssetWriter::FExecution::FExecution(TUniquePtr<FState> InState) : State(MoveTemp(InState)) {}
FCubeShotAssetWriter::FExecution::~FExecution() { if (State->PendingPixels.IsValid()) State->PendingPixels.Wait(); }
bool FCubeShotAssetWriter::FExecution::IsCritical() const { return State->Stage == 2; }
FString FCubeShotAssetWriter::FExecution::GetStage() const
{
    if (State->Stage == 0) return TEXT("validate");
    if (State->Stage == 4) return TEXT("stage_png");
    if (State->Stage == 1) return FString::Printf(TEXT("capture_%s"), CubeShot::GetDirectionInfo(static_cast<ECubeShotDirection>(State->Face)).Label);
    return State->Stage == 2 ? TEXT("package_commit") : TEXT("complete");
}
const FCubeShotGenerationResult& FCubeShotAssetWriter::FExecution::GetResult() const { return State->Result; }

bool FCubeShotAssetWriter::FExecution::Step(bool bInlineMode)
{
    FState& S = *State;
    if (S.Stage == 3) return true;
    if (S.Stage == 0)
    {
        if (!S.Preflight.bCanProceed || S.Preflight.Outputs.Num() != 6)
        { S.Result.Message = LOCTEXT("InvalidPreflight", "CubeShot preflight is missing or invalid."); S.Result.ErrorCode = TEXT("preflight_failed"); S.Stage = 3; return true; }
        FCubeShotPreflightResult Fresh;
        if (!S.Writer.BuildPreflight(S.Options, Fresh) || !AreOutputsEqual(S.Preflight.Outputs, Fresh.Outputs))
        { S.Result.Message = Fresh.Error; S.Result.ErrorCode = TEXT("preflight_failed"); S.Stage = 3; return true; }
        if (Fresh.bHasExistingTextures && !S.bOverwrite)
        { S.Result.Message = LOCTEXT("OverwriteNotConfirmed", "Existing CubeShot textures were not approved for overwrite."); S.Result.ErrorCode = TEXT("overwrite_required"); S.Stage = 3; return true; }
        if (Fresh.bHasExistingTextures && !S.Preflight.bHasExistingTextures)
        { S.Result.Message = LOCTEXT("LateOverwriteCollision", "An output appeared after preflight. Review overwrite again."); S.Result.ErrorCode = TEXT("output_changed"); S.Stage = 3; return true; }
        S.Preflight = MoveTemp(Fresh);
        if (!S.Writer.ValidateOutputPermissions(S.Result.Message))
        { S.Result.ErrorCode=TEXT("output_not_writable");S.Stage=3;return true; }
        S.Result.StagingDirectory = S.Writer.CreateStagingDirectory(S.Result.Message);
        if (S.Result.StagingDirectory.IsEmpty()) { S.Result.ErrorCode = TEXT("staging_failed"); S.Stage = 3; return true; }
        S.Stage = 1;
        return false;
    }
    if (S.Stage == 1)
    {
        if (S.ShouldCancel && S.ShouldCancel())
        { S.Result.bCancelled=true;S.Result.ErrorCode=TEXT("cancelled");S.Result.Message=LOCTEXT("CaptureCancelled","Cancelled before output mutation.");S.FinishBeforeMutation();return true; }
        FCubeShotPixelWork Work; FText Error;
        if (!S.Capture || !S.Capture(S.Options,S.Preflight.Outputs[S.Face].Direction,S.Options.Resolution,Work,Error))
        { S.Result.Message=Error;S.Result.ErrorCode=TEXT("capture_failed");S.Result.FailedObjectPaths.Add(S.Preflight.Outputs[S.Face].GetObjectPath());S.Result.Errors.Add(Error.ToString());S.FinishBeforeMutation();return true; }
        const FString Filename=DirectionStagingFilename(S.Result.StagingDirectory,S.Face,S.Preflight.Outputs[S.Face]);
        if (bInlineMode)
        {
            TPromise<FStagePixelsResult> Promise; S.PendingPixels=Promise.GetFuture();
            Promise.SetValue(WriteOwnedPixels(MoveTemp(Work),Filename,S.Options.Resolution,S.Writer.Config.ShouldFailOperation));
        }
        else
        {
            S.PendingPixels=Async(EAsyncExecution::ThreadPool,[Work=MoveTemp(Work),Filename,Resolution=S.Options.Resolution,Fail=S.Writer.Config.ShouldFailOperation]() mutable
                { return WriteOwnedPixels(MoveTemp(Work),Filename,Resolution,MoveTemp(Fail)); });
        }
        S.Stage=4;
        if(!bInlineMode)return false;
    }
    if (S.Stage == 4)
    {
        if (!S.PendingPixels.IsReady()) return false;
        FStagePixelsResult Written=S.PendingPixels.Get();S.PendingPixels=TFuture<FStagePixelsResult>();
        if (!Written.bSucceeded)
        { S.Result.Message=FText::FromString(Written.Error);S.Result.ErrorCode=TEXT("staging_failed");S.Result.Errors.Add(Written.Error);S.Result.FailedObjectPaths.Add(S.Preflight.Outputs[S.Face].GetObjectPath());S.FinishBeforeMutation();return true; }
        S.Staged.Add(Written.Filename);S.Result.StagedFilenames.Add(Written.Filename);++S.Face;S.Result.StagedFaces=S.Face;
        if(S.Progress)S.Progress(S.Face,6,FText::Format(LOCTEXT("FaceProgress","Staged {0}/6 directions"),S.Face));
        if(S.ShouldCancel && S.ShouldCancel())
        { S.Result.bCancelled=true;S.Result.ErrorCode=TEXT("cancelled");S.Result.Message=LOCTEXT("CancelStagingFuture","Cancelled after the pending pixel/PNG worker completed, before output mutation.");S.FinishBeforeMutation();return true; }
        S.Stage=S.Face==6?2:1;return false;
    }
    // Critical section: after the final cancellation poll, finish all six package writes.
    if (S.ShouldCancel && S.ShouldCancel())
    { S.Result.bCancelled = true; S.Result.ErrorCode = TEXT("cancelled"); S.Result.Message = LOCTEXT("CancelledAfterCapture", "Cancelled before any output asset was changed."); S.FinishBeforeMutation(); return true; }
    FCubeShotPreflightResult Fresh;
    if (!S.Writer.BuildPreflight(S.Options, Fresh) || !AreOutputsEqual(S.Preflight.Outputs, Fresh.Outputs)
        || (Fresh.bHasExistingTextures && !S.Preflight.bHasExistingTextures))
    { S.Result.Message = Fresh.Error.IsEmpty() ? LOCTEXT("CollisionDuringCapture", "An output changed during capture.") : Fresh.Error; S.Result.ErrorCode = TEXT("output_changed"); S.FinishBeforeMutation(); return true; }
    if (!S.Writer.CheckoutExistingPackages(Fresh.Outputs, S.Result.Message))
    { S.Result.ErrorCode = TEXT("checkout_failed"); S.FinishBeforeMutation(); return true; }
    TArray<FPreparedAsset> PreparedAssets;
    for (int32 Index = 0; Index < Fresh.Outputs.Num(); ++Index)
    {
        FPreparedAsset Prepared; FText PrepareError;
        if (S.Writer.PrepareAsset(Fresh.Outputs[Index], S.Staged[Index], Prepared, PrepareError)) PreparedAssets.Add(MoveTemp(Prepared));
        else
        {
            S.Result.FailedObjectPaths.Add(Fresh.Outputs[Index].GetObjectPath());
            S.Result.Errors.Add(PrepareError.ToString());
        }
    }
    S.Writer.SavePreparedAssets(PreparedAssets, S.Result);
    for (const FCubeShotOutputDescriptor& Output : Fresh.Outputs)
        if (UPackage* Package = FindPackage(nullptr, *Output.LongPackageName); Package && Package->IsDirty())
            S.Result.DirtyPackages.AddUnique(Output.LongPackageName);
    S.Result.bSucceeded = S.Result.FailedObjectPaths.IsEmpty() && S.Result.SavedObjectPaths.Num() == 6;
    if (S.Result.bSucceeded)
    {
        if (S.Writer.Config.bSyncContentBrowserOnSuccess) S.Writer.SyncContentBrowser(PreparedAssets);
        S.Result.Message = LOCTEXT("GenerationSucceeded", "Generated and saved all six CubeShot textures.");
        if (S.Writer.Config.bRetainSuccessfulStaging) S.Stage = 3;
        else S.FinishBeforeMutation();
    }
    else
    {
        S.Result.ErrorCode = TEXT("partial_save");
        S.Result.Message = FText::Format(LOCTEXT("PartialSaveFailure", "Saved {0}; failed {1}. Dirty packages and staging images require review at {2}."), S.Result.SavedObjectPaths.Num(), S.Result.FailedObjectPaths.Num(), FText::FromString(S.Result.StagingDirectory));
        S.Stage = 3;
    }
    return true;
}

TUniquePtr<FCubeShotAssetWriter::FExecution> FCubeShotAssetWriter::BeginPreparedExecution(
    const FCubeShotOptions& Options, FCubeShotPrepareCaptureCallback Capture,
    const FCubeShotPreflightResult& Preflight, bool bOverwriteConfirmed,
    FCubeShotProgressCallback Progress, FCubeShotCancelCallback ShouldCancel) const
{
    // ImageWrapper must be loaded on the game thread before independent CPU workers start.
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ImageWrapper"));
    TUniquePtr<FExecution::FState> State=MakeUnique<FExecution::FState>(*this);
    State->Options=Options;State->Capture=MoveTemp(Capture);State->Preflight=Preflight;
    State->bOverwrite=bOverwriteConfirmed;State->Progress=MoveTemp(Progress);State->ShouldCancel=MoveTemp(ShouldCancel);
    return TUniquePtr<FExecution>(new FExecution(MoveTemp(State)));
}

TUniquePtr<FCubeShotAssetWriter::FExecution> FCubeShotAssetWriter::BeginExecution(
    const FCubeShotOptions& Options, const FCubeShotCaptureCallback& Capture,
    const FCubeShotPreflightResult& Preflight, bool bOverwriteConfirmed,
    FCubeShotProgressCallback Progress, FCubeShotCancelCallback ShouldCancel) const
{
    return BeginPreparedExecution(Options,[Capture](const FCubeShotOptions& InOptions,ECubeShotDirection Direction,int32 Resolution,FCubeShotPixelWork& Work,FText& Error)
    {
        FCubeShotImage Captured;if(!Capture||!Capture(InOptions,Direction,Resolution,Captured,Error))return false;
        Work=[Captured=MoveTemp(Captured)](FCubeShotImage& Image,FText&) mutable {Image=MoveTemp(Captured);return Image.IsValid();};return true;
    },Preflight,bOverwriteConfirmed,MoveTemp(Progress),MoveTemp(ShouldCancel));
}

void FCubeShotAssetWriter::ExecuteInternal(const FCubeShotOptions& Options,
    const FCubeShotCaptureCallback& Capture, const FCubeShotPreflightResult& Preflight,
    const bool bOverwriteConfirmed, const FCubeShotProgressCallback& Progress,
    const FCubeShotCancelCallback& ShouldCancel, FCubeShotGenerationResult& OutResult) const
{
    TUniquePtr<FExecution> Execution = BeginExecution(Options, Capture, Preflight, bOverwriteConfirmed, Progress, ShouldCancel);
    while (!Execution->Step()) {}
    OutResult = Execution->GetResult();
}

#undef LOCTEXT_NAMESPACE
