#pragma once
#include "UnrealToolsWorkspaceController.h"

class FFBXExporterWorkspaceController final : public UnrealToolsWorkspace::FController
{
public:
    explicit FFBXExporterWorkspaceController(bool ReadSettings = true,
                                            TFunction<void()> InRecoveryRefresh = TFunction<void()>());
    bool Inspect(UnrealToolsRuntime::FObject Request = nullptr, bool ForExecution = false);
    void Start(UnrealToolsRuntime::FObject Request = nullptr);
    void Retry(const FString& Wanted);
    void RefreshRecoveryHistory();
    static const TArray<FString>& MapNames();
    // A true result accepts the edit; unrelated invalid drafts still block execution.
    bool SetMapEnabled(const UnrealToolsWorkspace::FEditToken& Token, const FString& Map, bool Enabled);
    bool SetAllMaps(const UnrealToolsWorkspace::FEditToken& Token, bool Enabled);
    UnrealToolsRuntime::FObject SaveProfile(const FConfirmProfileReplacement& Confirm)
    {
        return SaveActiveProfile(UserProfileId(), Confirm);
    }
    void SetOverwrite(bool Value)
    {
        bOverwrite = Value;
    }
    bool Overwrite() const
    {
        return bOverwrite;
    }

private:
    TFunction<void()> RecoveryRefresh;
    bool bOverwrite = false;
};
