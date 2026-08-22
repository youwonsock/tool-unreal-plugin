#pragma once

#include "CoreMinimal.h"
#include "CubeShotTypes.h"
#include "Engine/TextureDefines.h"

class FCubeShotCaptureService;

/** Controls where generated assets and lossless intermediate images are written. */
struct FCubeShotAssetWriterConfig
{
    /** Must be a writable /Game path. Tests can inject a unique root below /Game. */
    FString OutputRoot = TEXT("/Game/CubeShot");

    /** Empty uses <Project>/Saved/CubeShot/Staging. */
    FString StagingRoot;

    /** Disable UI source-control prompts for unattended automation only. */
    bool bPromptForCheckout = true;

    /** The panel normally performs the final Content Browser sync. */
    bool bSyncContentBrowserOnSuccess = false;
};

using FCubeShotProgressCallback = TFunction<void(int32 /*Completed*/, int32 /*Total*/, const FText& /*Status*/)>;
using FCubeShotCancelCallback = TFunction<bool()>;
using FCubeShotCaptureCallback = TFunction<bool(
    const FCubeShotOptions&,
    ECubeShotDirection,
    int32 /*TargetResolution*/,
    FCubeShotImage&,
    FText& /*OutError*/)>;

/**
 * Performs the non-transactional CubeShot asset generation workflow.
 *
 * Capture output is staged before this class changes any UObject. Existing textures are updated
 * in-place to preserve references. Saving multiple packages is intentionally reported per package,
 * because Unreal package saves are not atomic.
 */
class FCubeShotAssetWriter
{
public:
    explicit FCubeShotAssetWriter(FCubeShotAssetWriterConfig InConfig = FCubeShotAssetWriterConfig());

    /** Builds output names and rejects every known collision before an overwrite prompt is shown. */
    bool BuildPreflight(const FCubeShotOptions& Options, FCubeShotPreflightResult& OutResult) const;

    /**
     * Captures and stages all faces, then checks out, mutates, and saves the six texture packages.
     * Cancellation is honored only during the staging phase, before any UObject is changed.
     */
    void Execute(
        const FCubeShotOptions& Options,
        FCubeShotCaptureService& CaptureService,
        const FCubeShotPreflightResult& Preflight,
        bool bOverwriteConfirmed,
        const FCubeShotProgressCallback& Progress,
        const FCubeShotCancelCallback& ShouldCancel,
        FCubeShotGenerationResult& OutResult) const;

    /** Injectable equivalent used by focused writer automation without requiring an RHI capture. */
    void ExecuteWithCaptureCallback(
        const FCubeShotOptions& Options,
        const FCubeShotCaptureCallback& Capture,
        const FCubeShotPreflightResult& Preflight,
        bool bOverwriteConfirmed,
        const FCubeShotProgressCallback& Progress,
        const FCubeShotCancelCallback& ShouldCancel,
        FCubeShotGenerationResult& OutResult) const;

    const FCubeShotAssetWriterConfig& GetConfig() const { return Config; }

    /** Pure path helper used by preflight and automation tests. */
    static bool BuildOutputDescriptorsFromSource(
        const FString& SourceLongPackageName,
        const FString& SourceAssetName,
        const FString& OutputRoot,
        TArray<FCubeShotOutputDescriptor>& OutOutputs,
        FText& OutError);

    static FString SanitizeAssetName(const FString& InName);
    static TextureMipGenSettings GetMipPolicy(int32 Resolution);

private:
    struct FPreparedAsset
    {
        FCubeShotOutputDescriptor Descriptor;
        FString StagedFilename;
        UPackage* Package = nullptr;
        class UTexture2D* Texture = nullptr;
        bool bWasCreated = false;
    };

    bool ValidateOutputRoot(FText& OutError) const;
    bool InspectOutput(const FCubeShotOutputDescriptor& Output, bool& bOutExists, FText& OutError) const;
    bool StageCaptures(
        const FCubeShotOptions& Options,
        const FCubeShotCaptureCallback& Capture,
        const TArray<FCubeShotOutputDescriptor>& Outputs,
        const FString& StagingDirectory,
        const FCubeShotProgressCallback& Progress,
        const FCubeShotCancelCallback& ShouldCancel,
        TArray<FString>& OutStagedFilenames,
        FCubeShotGenerationResult& OutResult) const;
    void ExecuteInternal(
        const FCubeShotOptions& Options,
        const FCubeShotCaptureCallback& Capture,
        const FCubeShotPreflightResult& Preflight,
        bool bOverwriteConfirmed,
        const FCubeShotProgressCallback& Progress,
        const FCubeShotCancelCallback& ShouldCancel,
        FCubeShotGenerationResult& OutResult) const;
    bool CheckoutExistingPackages(
        const TArray<FCubeShotOutputDescriptor>& Outputs,
        FText& OutError) const;
    bool PrepareAsset(
        const FCubeShotOutputDescriptor& Output,
        const FString& StagedFilename,
        FPreparedAsset& OutPrepared,
        FText& OutError) const;
    static bool ApplyImageToTexture(class UTexture2D& Texture, const struct FImage& Image, int32 Resolution, FText& OutError);
    void SavePreparedAssets(TArray<FPreparedAsset>& PreparedAssets, FCubeShotGenerationResult& OutResult) const;
    void SyncContentBrowser(const TArray<FPreparedAsset>& PreparedAssets) const;

    FString GetResolvedStagingRoot() const;
    FString CreateStagingDirectory(FText& OutError) const;
    bool DeleteStagingDirectory(const FString& StagingDirectory) const;

    FCubeShotAssetWriterConfig Config;
};
