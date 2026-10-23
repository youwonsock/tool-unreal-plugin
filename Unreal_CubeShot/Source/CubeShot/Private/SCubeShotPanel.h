#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CubeShotAssetWriter.h"
#include "CubeShotCaptureService.h"
#include "CubeShotTypes.h"
#include "CubeShotViewModel.h"
#include "CubeShotWorkspaceController.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class FActiveTimerHandle;
struct FSlateBrush;
class UTexture2D;
class SVerticalBox;
class SWindow;
class SImage;

/** Slate front-end for the CubeShot capture workflow. */
class SCubeShotPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCubeShotPanel)
    {
    }
    SLATE_ARGUMENT(TSharedPtr<FCubeShotWorkspaceController>, Workspace)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCubeShotPanel() override;

    /** Safe to call repeatedly. Used by both tab and module shutdown. */
    void Shutdown();
    virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override;
    virtual void OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event) override;
#if WITH_DEV_AUTOMATION_TESTS
    void AddTargetForAutomation(const FString& Path)
    {
        AddTarget(Path);
        SelectTargetPath(Path);
    }
    bool HasCompletePreviewPixelsForAutomation() const;
    bool TakePaintedPreviewForAutomation(TArray<FColor>& Pixels, FIntVector& Size) const;
    FText ResultsForAutomation() const
    {
        return ResultsText();
    }
    void ClearTargetsForAutomation();
    void SetSelectedTargetsForAutomation(const TArray<FString>& Paths);
    void SetPreviewTargetForAutomation(int32 Index)
    {
        SelectTarget(Index);
    }
    void SetResolutionForAutomation(int32 Resolution);
    void ApplyOptionsToSelectedForAutomation()
    {
        ApplyOptionsToSelected();
    }
    void RemoveSelectedForAutomation()
    {
        RemoveSelectedTargets();
    }
    TArray<FString> TargetPathsForAutomation() const;
    TSharedPtr<FJsonObject> TargetOptionsForAutomation(const FString& Path) const;
    FString PreviewTargetForAutomation() const
    {
        return GetMeshObjectPath();
    }
    void StartBatchForAutomation()
    {
        OnGenerateClicked();
    }
    FText OutputPlanForAutomation() const
    {
        return FText::FromString(Workspace->PlanText());
    }
    void ApplyProfileForAutomation(const FString& Id)
    {
        ApplyProfile(Id);
    }
    void SetProfileReplacementForAutomation(TFunction<bool()> Decision)
    {
        ProfileReplacementForAutomation = MoveTemp(Decision);
    }
    void SaveProfileForAutomation()
    {
        SaveProfile();
    }
    bool HasViewResourcesForAutomation() const
    {
        return ViewModel.IsValid() || CaptureService.IsValid() || ActiveTimerHandle.IsValid();
    }
    FString ProfileForAutomation() const
    {
        return Workspace->ProfileId();
    }
    void SetSubmissionDecisionsForAutomation(TFunction<bool()> OverwriteDecision,
                                             TFunction<bool(const TArray<UPackage*>&, FText&)> CheckoutDecision)
    {
        OverwriteDecisionForAutomation = MoveTemp(OverwriteDecision);
        CheckoutDecisionForAutomation = MoveTemp(CheckoutDecision);
    }
    FText StatusForAutomation() const
    {
        return GetStatusText();
    }
    void RefreshHistoryForAutomation()
    {
        RefreshJobObserver(true);
    }
    uint64 HistoryReadsForAutomation() const
    {
        return Workspace->History().Reads;
    }
    FString SelectedJobForAutomation() const
    {
        return Workspace->History().JobId;
    }
    void SelectJobForAutomation(const FString& JobId);
    void MoveTargetForAutomation(int32 Index, int32 Delta)
    {
        MoveTarget(Index, Delta);
    }
    void PickTargetForAutomation(const FString& Path);
#endif

private:
    TSharedRef<SWidget> BuildOptionsPanel();
    TSharedRef<SWidget> BuildWorkflowPanel();
    TSharedRef<SWidget> BuildResultsPanel();
    void RebuildTargets();
    void PersistCurrentOptions();
    void ApplyOptionsToSelected();
    void RemoveSelectedTargets();
    bool bLoadingTarget = false;
    void AddTarget(const FString& Path);
    void SelectTarget(int32 Index);
    void SelectTargetPath(const FString& Path);
    void MoveTarget(int32 Index, int32 Delta);
    void ApplyProfile(const FString& Id);
    FReply StartBatch(const FString& RetryState = FString());
    FReply Retry(bool bSkipped);
    FReply PreflightTargets();
    FReply TransferProfile(bool bImport);
    FReply SaveProfile();
    FText ResultsText() const;
    TSharedPtr<SVerticalBox> TargetRows;
    TSharedPtr<SVerticalBox> ProfileRows;
    void RebuildProfiles();
    TArray<TWeakPtr<SWindow>> ZoomWindows;
    TSharedPtr<SVerticalBox> JobRows;
    void RebuildJobs();
    void RefreshJobObserver(bool bForce = false);
    TSharedPtr<FCubeShotWorkspaceController> Workspace;
    UnrealToolsWorkspace::FEditToken BindingToken;
    TSharedPtr<class SBox> OptionsHost;
    uint64 DisplayedHistoryRevision = MAX_uint64;
    mutable uint64 ResultsRevision = MAX_uint64;
    mutable FText ResultsCache, ResultsStatus;
    void LoadEditor();
    void OnAutoFitChanged(const UnrealToolsWorkspace::FEditToken& Token, bool Enabled);
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<bool()> OverwriteDecisionForAutomation;
    TFunction<bool()> ProfileReplacementForAutomation;
    TFunction<bool(const TArray<UPackage*>&, FText&)> CheckoutDecisionForAutomation;
#endif
    bool bExportPng = false;
    bool bTrimPng = true;
    int32 PngPadding = 8;
    int32 PngTurns = 0;
    TArray<FString> PngDirections = {TEXT("Front")};
    FText LastResultStatus;
    bool bWasJobBusy = false;
    TSharedRef<SWidget> BuildPreviewPanel();
    TSharedRef<SWidget> BuildPreviewCell(int32 DirectionIndex);
    TSharedRef<SWidget> BuildCheckerboard() const;
    TSharedRef<SWidget> BuildBackgroundModeCombo();
    TSharedRef<SWidget> BuildProjectionModeCombo();
    TSharedRef<SWidget> BuildResolutionPresets();

    void SchedulePreview(FCubeShotViewModel::EChangeKind ChangeKind);
    void StartScheduledPreview();
    void FinishPreviewRevision();
    EActiveTimerReturnType TickPreview(double CurrentTime, float DeltaTime);
    bool CapturePreviewDirection(int32 DirectionIndex, FText& OutError);
    void ReplacePreviewTexture(int32 DirectionIndex, const FCubeShotImage& Image);
    void ClearPreviewResources(bool Release = true);
    void ApplyAutoFit();

    void OnMeshChanged(const struct FAssetData& AssetData);
    void OnBackgroundTextureChanged(const struct FAssetData& AssetData);
    FReply OnUseSelectedMesh();
    FReply OnSolidColorClicked(const FGeometry& Geometry, const FPointerEvent& PointerEvent);
    void OnSolidColorCommitted(FLinearColor NewColor);
    void OnResolutionPreset(int32 Resolution);
    FReply OnGenerateClicked();

    FString GetMeshObjectPath() const;
    FString GetBackgroundTextureObjectPath() const;
    const FSlateBrush* GetPreviewBrush(int32 DirectionIndex) const;
    FText GetStatusText() const;
    FSlateColor GetStatusColor() const;
    FText GetProgressText() const;
    bool CanStartGeneration() const;
    EVisibility GetUpdatingVisibility() const;
    EVisibility GetProgressVisibility() const;

    TSharedPtr<FCubeShotViewModel> ViewModel;
    TUniquePtr<FCubeShotCaptureService> CaptureService;
    TSharedPtr<FActiveTimerHandle> ActiveTimerHandle;

    TArray<FCubeShotRawFrame> CachedRawFrames;
    TArray<FCubeShotRawFrame> PendingRawFrames;
    TArray<FCubeShotImage> PendingPreviewImages;
    TArray<TStrongObjectPtr<UTexture2D>> PreviewTextures;
    TArray<TSharedPtr<FSlateBrush>> PreviewBrushes;
    TArray<TSharedPtr<SImage>> PreviewImages;
    TArray<int64> VisiblePreviewPixels;

    uint64 ScheduledRevision = 0;
    uint64 ProcessingRevision = 0;
    double PreviewDueTime = 0.0;
    int32 NextPreviewDirection = 0;
    int32 ProgressCompleted = 0;
    int32 ProgressTotal = 0;
    FText OperationStatus;
    FText PreviewError;
    bool bScheduledCompositeOnly = false;
    bool bProcessingCompositeOnly = false;
    bool bPreviewUpdating = false;
    bool bShutdown = false;
};
