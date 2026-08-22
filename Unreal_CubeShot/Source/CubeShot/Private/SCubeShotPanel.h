#pragma once

#include "CoreMinimal.h"
#include "CubeShotAssetWriter.h"
#include "CubeShotCaptureService.h"
#include "CubeShotTypes.h"
#include "CubeShotViewModel.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class FActiveTimerHandle;
struct FSlateBrush;
class UTexture2D;

/** Slate front-end for the CubeShot capture workflow. */
class SCubeShotPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCubeShotPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCubeShotPanel() override;

	/** Safe to call repeatedly. Used by both tab and module shutdown. */
	void Shutdown();

private:
	TSharedRef<SWidget> BuildOptionsPanel();
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
	void ClearPreviewResources();
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
	TUniquePtr<FCubeShotAssetWriter> AssetWriter;
	TSharedPtr<FActiveTimerHandle> ActiveTimerHandle;

	TArray<FCubeShotRawFrame> CachedRawFrames;
	TArray<FCubeShotRawFrame> PendingRawFrames;
	TArray<FCubeShotImage> PendingPreviewImages;
	TArray<TStrongObjectPtr<UTexture2D>> PreviewTextures;
	TArray<TSharedPtr<FSlateBrush>> PreviewBrushes;

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
