#pragma once
#include "CoreMinimal.h"
#include "CubeShotTypes.h"
#include "UnrealToolsRuntime.h"

class UPackage;

namespace CubeShot::Service
{
    void Initialize();
    void Shutdown();
    UnrealToolsRuntime::FProfileStore& Profiles();
    UnrealToolsRuntime::FJobService& Jobs();
    UnrealToolsRuntime::FObject DefaultOptions();
    bool Normalize(UnrealToolsRuntime::FObject& Options, FString& Error);
    bool LoadOptions(const FString& MeshPath, const UnrealToolsRuntime::FObject& Json, FCubeShotOptions& Options, FText& Error);
    UnrealToolsRuntime::FObject SerializeOptions(const FCubeShotOptions& Options);
    UnrealToolsRuntime::FObject BuildPreflight(const FString& MeshPath, const UnrealToolsRuntime::FObject& Options,
        bool bAllowInteractiveCheckout = false);
    /** Shared application policy. Inspection does not write files or acquire output reservations. */
    UnrealToolsRuntime::FObject InspectBatch(const UnrealToolsRuntime::FObject& Request, bool bAllowInteractiveCheckout = false);
    bool Resolve(const UnrealToolsRuntime::FObject& Request, UnrealToolsRuntime::FPreparedBatch& Batch, FString& Error);
    UnrealToolsRuntime::FObject Submit(const UnrealToolsRuntime::FObject& Request);
    bool CheckoutForSubmission(const UnrealToolsRuntime::FObject& Request, FText& Error,
        TFunction<bool(const TArray<UPackage*>&, FText&)> CheckoutCallback = {});
    UnrealToolsRuntime::FObject HandleProfiles(const UnrealToolsRuntime::FObject& Request);
    UnrealToolsRuntime::FObject ExportPng(const UnrealToolsRuntime::FObject& Request);
    TArray<FString> Reserve(const UnrealToolsRuntime::FItemSpec& Spec);
#if WITH_DEV_AUTOMATION_TESTS
    TUniquePtr<UnrealToolsRuntime::IItemRunner> CreateRunnerForAutomation(const UnrealToolsRuntime::FItemSpec& Spec);
#endif
}
