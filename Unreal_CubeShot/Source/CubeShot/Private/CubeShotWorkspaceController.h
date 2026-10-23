#pragma once
#include "UnrealToolsWorkspaceController.h"
#include "CubeShotTypes.h"

class UPackage;

class FCubeShotWorkspaceController final : public UnrealToolsWorkspace::FController
{
public:
    using FCheckout = TFunction<bool(const TArray<UPackage*>&, FText&)>;
    struct FSubmissionServices
    {
        TFunction<UnrealToolsRuntime::FObject(const UnrealToolsRuntime::FObject&, bool)> Inspect;
        TFunction<bool(const UnrealToolsRuntime::FObject&, FText&, FCheckout)> Checkout;
        TFunction<UnrealToolsRuntime::FObject(const UnrealToolsRuntime::FObject&)> Submit;
    };
    struct FSubmissionInteraction
    {
        TFunction<void()> Preparing;
        TFunction<bool()> ConfirmOverwrite;
        FCheckout Checkout;
    };
    explicit FCubeShotWorkspaceController(FSubmissionServices Services = FSubmissionServices());
    UnrealToolsRuntime::FObject Inspect(UnrealToolsRuntime::FObject Request = nullptr, bool Interactive = false);
    UnrealToolsRuntime::FObject Start(const FSubmissionInteraction& Interaction, const FString& RetryState = FString());
    UnrealToolsRuntime::FObject SaveProfile(const FConfirmProfileReplacement& Confirm);
    bool ReadEditorOptions(FCubeShotOptions& Options, FText& Error) const;
    void CommitEditorOptions(const UnrealToolsWorkspace::FEditToken& Token, const FCubeShotOptions& Options,
                             const UnrealToolsRuntime::FObject& PngOptions);
    void SetAutoFit(const UnrealToolsWorkspace::FEditToken& Token, bool Enabled,
                    const TOptional<FCubeShotAutoFitResult>& Fit = TOptional<FCubeShotAutoFitResult>());

private:
    FSubmissionServices SubmissionServices;
};
