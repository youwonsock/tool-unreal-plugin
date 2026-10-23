#include "SCubeShotPanel.h"

#include "CubeShotAssetWriter.h"
#include "CubeShotCaptureService.h"
#include "CubeShotWorkspaceController.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/SNullWidget.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/AppStyle.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCubeShotPanel"

namespace
{
constexpr double PreviewDebounceSeconds = 0.15;
constexpr int32 DirectionCount = static_cast<int32>(ECubeShotDirection::Count);

TSharedRef<STextBlock> MakeSectionHeading(const FText& Text)
{
    return SNew(STextBlock).Text(Text).Font(FAppStyle::GetFontStyle("HeadingExtraSmall"));
}

TSharedRef<STextBlock> MakeOptionLabel(const FText& Text)
{
    return SNew(STextBlock).Text(Text).MinDesiredWidth(116.0f);
}
} // namespace

void SCubeShotPanel::Construct(const FArguments& InArgs)
{
    Workspace = InArgs._Workspace;
    check(Workspace);
    Workspace->AttachView();
    ViewModel = MakeShared<FCubeShotViewModel>();
    BindingToken = Workspace->Token();

    CaptureService = MakeUnique<FCubeShotCaptureService>();

    CachedRawFrames.SetNum(DirectionCount);
    PendingRawFrames.SetNum(DirectionCount);
    PendingPreviewImages.SetNum(DirectionCount);
    PreviewTextures.Reserve(DirectionCount);
    PreviewBrushes.Reserve(DirectionCount);
    PreviewImages.SetNum(DirectionCount);
    VisiblePreviewPixels.Init(0, DirectionCount);
    for (int32 Index = 0; Index < DirectionCount; ++Index)
    {
        PreviewTextures.Emplace(nullptr);
        PreviewBrushes.Add(MakeShared<FSlateBrush>());
        PreviewBrushes[Index]->DrawAs = ESlateBrushDrawType::NoDrawType;
    }

    OperationStatus = LOCTEXT("SelectMeshStatus", "Select a Static Mesh to begin.");

    ChildSlot[SNew(SBorder)
                  .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                  .Padding(0.0f)[SNew(SSplitter).PhysicalSplitterHandleSize(2.0f) +
                                 SSplitter::Slot().Value(0.31f).MinSize(
                                     290.0f)[SAssignNew(OptionsHost, SBox)[BuildOptionsPanel()]] +
                                 SSplitter::Slot().Value(0.69f).MinSize(420.0f)
                                     [SNew(SVerticalBox) + SVerticalBox::Slot().FillHeight(1.0f)[BuildPreviewPanel()] +
                                      SVerticalBox::Slot().AutoHeight()[BuildResultsPanel()]]]];

    LoadEditor();
    RefreshJobObserver();
    ActiveTimerHandle =
        RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SCubeShotPanel::TickPreview));
}

SCubeShotPanel::~SCubeShotPanel()
{
    Shutdown();
}

void SCubeShotPanel::Shutdown()
{
    if (bShutdown)
    {
        return;
    }

    bShutdown = true;
    Workspace->DetachView();
    for (const auto& WeakWindow : ZoomWindows)
        if (const auto Window = WeakWindow.Pin())
            Window->RequestDestroyWindow();
    ZoomWindows.Reset();
    ++ScheduledRevision;
    ++ProcessingRevision;
    bPreviewUpdating = false;

    if (ActiveTimerHandle.IsValid())
    {
        UnRegisterActiveTimer(ActiveTimerHandle.ToSharedRef());
        ActiveTimerHandle.Reset();
    }

    ClearPreviewResources();
    PendingPreviewImages.Reset();
    PendingRawFrames.Reset();
    CachedRawFrames.Reset();

    if (ViewModel.IsValid())
    {
        ViewModel->SetStaticMesh(nullptr);
        ViewModel->SetBackgroundTexture(nullptr);
    }

    if (CaptureService)
    {
        CaptureService->Shutdown();
        CaptureService.Reset();
    }
    ChildSlot[SNullWidget::NullWidget];
    OptionsHost.Reset();
    TargetRows.Reset();
    ProfileRows.Reset();
    JobRows.Reset();
    ViewModel.Reset();
}

TSharedRef<SWidget> SCubeShotPanel::BuildOptionsPanel()
{
    const auto Token = BindingToken;
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
        .Padding(FMargin(12.0f, 10.0f))
            [SNew(SVerticalBox) +
             SVerticalBox::Slot().FillHeight(
                 1.0f)[SNew(SScrollBox) +
                       SScrollBox::
                           Slot()[SNew(SVerticalBox).IsEnabled_Lambda([this]() {
                               return ViewModel.IsValid() && ViewModel->AreOptionsEnabled();
                           }) + SVerticalBox::Slot().AutoHeight()[BuildWorkflowPanel()]

                                  + SVerticalBox::Slot().AutoHeight().Padding(
                                        0.0f, 2.0f)[MakeSectionHeading(LOCTEXT("MeshHeading", "Mesh"))] +
                                  SVerticalBox::Slot().AutoHeight().Padding(
                                      0.0f, 4.0f)[SNew(SObjectPropertyEntryBox)
                                                      .AllowedClass(UStaticMesh::StaticClass())
                                                      .ObjectPath(this, &SCubeShotPanel::GetMeshObjectPath)
                                                      .OnObjectChanged(this, &SCubeShotPanel::OnMeshChanged)
                                                      .DisplayUseSelected(false)
                                                      .DisplayBrowse(true)
                                                      .AllowClear(true)] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
                                      [SNew(SButton)
                                           .Text(LOCTEXT("UseSelectedMesh", "Use Content Browser Selection"))
                                           .ToolTipText(LOCTEXT("UseSelectedMeshTooltip",
                                                                "Adds all selected Static Meshes to "
                                                                "the ordered target list."))
                                           .OnClicked(this, &SCubeShotPanel::OnUseSelectedMesh)]

                                  + SVerticalBox::Slot().AutoHeight().Padding(
                                        0.0f, 2.0f)[MakeSectionHeading(LOCTEXT("BackgroundHeading", "Background"))] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)[BuildBackgroundModeCombo()] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
                                      [SNew(SHorizontalBox).IsEnabled_Lambda([this]() {
                                          return ViewModel->AreOptionsEnabled() &&
                                                 ViewModel->GetOptions().BackgroundMode ==
                                                     ECubeShotBackgroundMode::SolidColor;
                                      }) +
                                       SHorizontalBox::Slot().AutoWidth().VAlign(
                                           VAlign_Center)[MakeOptionLabel(LOCTEXT("SolidColorLabel", "Solid Color"))] +
                                       SHorizontalBox::Slot().FillWidth(1.0f)
                                           [SNew(SColorBlock)
                                                .Color_Lambda([this]() { return ViewModel->GetOptions().SolidColor; })
                                                .Size(FVector2D(64.0f, 20.0f))
                                                .ShowBackgroundForAlpha(false)
                                                .OnMouseButtonDown(this, &SCubeShotPanel::OnSolidColorClicked)]] +
                                  SVerticalBox::Slot().AutoHeight().Padding(
                                      0.0f, 3.0f, 0.0f, 10.0f)[SNew(SBox).IsEnabled_Lambda([this]() {
                                      return ViewModel->IsBackgroundTextureEnabled();
                                  })[SNew(SObjectPropertyEntryBox)
                                         .AllowedClass(UTexture2D::StaticClass())
                                         .ObjectPath(this, &SCubeShotPanel::GetBackgroundTextureObjectPath)
                                         .OnObjectChanged_Lambda([this, Token](const FAssetData& Asset) {
                                             if (!bShutdown && !bLoadingTarget && Workspace->IsCurrent(Token))
                                                 OnBackgroundTextureChanged(Asset);
                                         })
                                         .DisplayUseSelected(true)
                                         .DisplayBrowse(true)
                                         .AllowClear(true)]]

                                  + SVerticalBox::Slot().AutoHeight().Padding(
                                        0.0f, 2.0f)[MakeSectionHeading(LOCTEXT("CameraHeading", "Camera"))] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)[BuildProjectionModeCombo()] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
                                      [SNew(SHorizontalBox) +
                                       SHorizontalBox::Slot().AutoWidth().VAlign(
                                           VAlign_Center)[MakeOptionLabel(LOCTEXT("DistanceLabel", "Distance (cm)"))] +
                                       SHorizontalBox::Slot().FillWidth(
                                           1.0f)[SNew(SNumericEntryBox<double>)
                                                     .IsEnabled_Lambda(
                                                         [this]() { return ViewModel->IsCameraDistanceEnabled(); })
                                                     .Value_Lambda([this]() {
                                                         return TOptional<double>(
                                                             ViewModel->GetOptions().CameraDistance);
                                                     })
                                                     .MinValue(0.01)
                                                     .MaxValue(10000000.0)
                                                     .MinSliderValue(1.0)
                                                     .MaxSliderValue(10000.0)
                                                     .AllowSpin(true)
                                                     .OnValueChanged_Lambda([this, Token](const double Value) {
                                                         if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                             return;
                                                         ViewModel->SetCameraDistance(Value);
                                                         PersistCurrentOptions();
                                                         SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
                                                     })]] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
                                      [SNew(SHorizontalBox) +
                                       SHorizontalBox::Slot().AutoWidth().VAlign(
                                           VAlign_Center)[MakeOptionLabel(LOCTEXT("FovLabel", "FOV (degrees)"))] +
                                       SHorizontalBox::Slot().FillWidth(
                                           1.0f)[SNew(SNumericEntryBox<double>)
                                                     .IsEnabled_Lambda(
                                                         [this]() { return ViewModel->IsFieldOfViewEnabled(); })
                                                     .Value_Lambda([this]() {
                                                         return TOptional<double>(ViewModel->GetOptions().FieldOfView);
                                                     })
                                                     .MinValue(1.0)
                                                     .MaxValue(170.0)
                                                     .MinSliderValue(10.0)
                                                     .MaxSliderValue(120.0)
                                                     .AllowSpin(true)
                                                     .OnValueChanged_Lambda([this, Token](const double Value) {
                                                         if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                             return;
                                                         ViewModel->SetFieldOfView(Value);
                                                         if (ViewModel->GetOptions().bAutoFit)
                                                         {
                                                             ApplyAutoFit();
                                                         }
                                                         PersistCurrentOptions();
                                                         SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
                                                     })]] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
                                      [SNew(SHorizontalBox) +
                                       SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[MakeOptionLabel(
                                           LOCTEXT("OrthoWidthLabel", "Ortho Width (cm)"))] +
                                       SHorizontalBox::Slot().FillWidth(1.0f)
                                           [SNew(SNumericEntryBox<double>)
                                                .IsEnabled_Lambda([this]() { return ViewModel->IsOrthoWidthEnabled(); })
                                                .Value_Lambda([this]() {
                                                    return TOptional<double>(ViewModel->GetOptions().OrthoWidth);
                                                })
                                                .MinValue(0.01)
                                                .MaxValue(10000000.0)
                                                .MinSliderValue(1.0)
                                                .MaxSliderValue(10000.0)
                                                .AllowSpin(true)
                                                .OnValueChanged_Lambda([this, Token](const double Value) {
                                                    if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                        return;
                                                    ViewModel->SetOrthoWidth(Value);
                                                    PersistCurrentOptions();
                                                    SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
                                                })]] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 10.0f)
                                      [SNew(SCheckBox)
                                           .IsChecked_Lambda([this]() {
                                               return ViewModel->GetOptions().bAutoFit ? ECheckBoxState::Checked
                                                                                       : ECheckBoxState::Unchecked;
                                           })
                                           .OnCheckStateChanged_Lambda([this, Token](const ECheckBoxState State) {
                                               OnAutoFitChanged(Token, State == ECheckBoxState::Checked);
                                           })[SNew(STextBlock)
                                                  .Text(LOCTEXT("AutoFitLabel", "Auto Fit all six directions (90% "
                                                                                "frame fill)"))]]

                                  + SVerticalBox::Slot().AutoHeight().Padding(
                                        0.0f, 2.0f)[MakeSectionHeading(LOCTEXT("OutputHeading", "Output"))] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)[BuildResolutionPresets()] +
                                  SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
                                      [SNew(SHorizontalBox) +
                                       SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[MakeOptionLabel(
                                           LOCTEXT("CustomResolutionLabel", "Custom size"))] +
                                       SHorizontalBox::Slot().FillWidth(
                                           1.0f)[SNew(SNumericEntryBox<int32>)
                                                     .Value_Lambda([this]() {
                                                         return TOptional<int32>(ViewModel->GetOptions().Resolution);
                                                     })
                                                     .MinValue(CubeShot::MinResolution)
                                                     .MaxValue(CubeShot::MaxResolution)
                                                     .MinSliderValue(CubeShot::MinResolution)
                                                     .MaxSliderValue(CubeShot::MaxResolution)
                                                     .AllowSpin(true)
                                                     .OnValueChanged_Lambda([this, Token](const int32 Value) {
                                                         if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                             return;
                                                         ViewModel->SetResolution(Value);
                                                         PersistCurrentOptions();
                                                         SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
                                                     })]]]]

             + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 4.0f)[SNew(SSeparator)] +
             SVerticalBox::Slot().AutoHeight().Padding(0.0f,
                                                       3.0f)[SNew(STextBlock)
                                                                 .Text(this, &SCubeShotPanel::GetStatusText)
                                                                 .ColorAndOpacity(this, &SCubeShotPanel::GetStatusColor)
                                                                 .AutoWrapText(true)] +
             SVerticalBox::Slot().AutoHeight().Padding(
                 0.0f, 3.0f)[SNew(STextBlock)
                                 .Visibility(this, &SCubeShotPanel::GetProgressVisibility)
                                 .Text(this, &SCubeShotPanel::GetProgressText)] +
             SVerticalBox::Slot().AutoHeight().Padding(
                 0.0f,
                 5.0f)[SNew(SButton)
                           .ButtonStyle(FAppStyle::Get(), "PrimaryButton")
                           .HAlign(HAlign_Center)
                           .Text(LOCTEXT("GenerateButton", "Start Capture Queue"))
                           .ToolTipText(LOCTEXT("GenerateTooltip", "Captures and saves Front, Back, Left, Right, Top, "
                                                                   "and Bottom textures under /Game/CubeShot."))
                           .IsEnabled(this, &SCubeShotPanel::CanStartGeneration)
                           .OnClicked(this, &SCubeShotPanel::OnGenerateClicked)]];
}

TSharedRef<SWidget> SCubeShotPanel::BuildPreviewPanel()
{
    TSharedRef<SGridPanel> Grid =
        SNew(SGridPanel).FillColumn(0, 1.0f).FillColumn(1, 1.0f).FillColumn(2, 1.0f).FillRow(0, 1.0f).FillRow(1, 1.0f);
    for (int32 Index = 0; Index < DirectionCount; ++Index)
    {
        Grid->AddSlot(Index % 3, Index / 3).Padding(5.0f)[BuildPreviewCell(Index)];
    }

    return SNew(SBorder).BorderImage(FAppStyle::GetBrush("Brushes.Panel")).Padding(7.0f)[Grid];
}

TSharedRef<SWidget> SCubeShotPanel::BuildPreviewCell(const int32 DirectionIndex)
{
    const FCubeShotDirectionInfo& Direction =
        CubeShot::GetDirectionInfo(static_cast<ECubeShotDirection>(DirectionIndex));
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(4.0f)
            [SNew(SVerticalBox) +
             SVerticalBox::Slot()
                 .AutoHeight()
                 .HAlign(HAlign_Center)
                 .Padding(0.0f, 1.0f, 0.0f,
                          4.0f)[SNew(SButton)
                                    .Text(FText::FromString(Direction.Label))
                                    .ToolTipText(LOCTEXT("ZoomPreview", "Open this view at a larger size."))
                                    .OnClicked_Lambda([this, DirectionIndex]() {
                                        if (!PreviewTextures[DirectionIndex])
                                            return FReply::Handled();
                                        auto HeldTexture = MakeShared<TStrongObjectPtr<UTexture2D>>(
                                            PreviewTextures[DirectionIndex].Get());
                                        auto Brush = MakeShared<FSlateBrush>(*PreviewBrushes[DirectionIndex]);
                                        auto Window = SNew(SWindow)
                                                          .Title(FText::FromString(
                                                              CubeShot::GetDirectionInfo(
                                                                  static_cast<ECubeShotDirection>(DirectionIndex))
                                                                  .Label))
                                                          .ClientSize(FVector2D(700, 700))[SNew(SScaleBox).Stretch(
                                                              EStretch::ScaleToFit)[SNew(SImage).Image(&Brush.Get())]];
                                        Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda(
                                            [HeldTexture, Brush](const TSharedRef<SWindow>&) {}));
                                        ZoomWindows.Add(Window);
                                        FSlateApplication::Get().AddWindow(Window);
                                        return FReply::Handled();
                                    })] +
             SVerticalBox::Slot().FillHeight(
                 1.0f)[SNew(SScaleBox)
                           .Stretch(EStretch::ScaleToFit)
                           .StretchDirection(EStretchDirection::Both)[SNew(SBox).WidthOverride(256.0f).HeightOverride(
                               256.0f)[SNew(SOverlay) + SOverlay::Slot()[BuildCheckerboard()] +
                                       SOverlay::Slot()[SAssignNew(PreviewImages[DirectionIndex], SImage)
                                                            .Image(GetPreviewBrush(DirectionIndex))] +
                                       SOverlay::Slot()
                                           .HAlign(HAlign_Center)
                                           .VAlign(VAlign_Center)[SNew(STextBlock)
                                                                      .Visibility(
                                                                          this, &SCubeShotPanel::GetUpdatingVisibility)
                                                                      .Text(LOCTEXT("UpdatingOverlay", "Updating..."))
                                                                      .Font(FAppStyle::GetFontStyle("NormalFontBold"))
                                                                      .ColorAndOpacity(FLinearColor::White)
                                                                      .ShadowOffset(FVector2D(1.0f, 1.0f))]]]]];
}

TSharedRef<SWidget> SCubeShotPanel::BuildCheckerboard() const
{
    TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).MinDesiredSlotWidth(8.0f).MinDesiredSlotHeight(8.0f);
    for (int32 Row = 0; Row < 8; ++Row)
    {
        for (int32 Column = 0; Column < 8; ++Column)
        {
            const FLinearColor Color =
                ((Row + Column) & 1) == 0 ? FLinearColor(0.16f, 0.16f, 0.16f) : FLinearColor(0.30f, 0.30f, 0.30f);
            Grid->AddSlot(
                Column, Row)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(Color)];
        }
    }
    return Grid;
}

TSharedRef<SWidget> SCubeShotPanel::BuildBackgroundModeCombo()
{
    const auto Token = BindingToken;
    auto MakeModeButton = [this, Token](const ECubeShotBackgroundMode Mode, const FText& Label) {
        return SNew(SCheckBox)
            .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([this, Mode]() {
                return ViewModel->GetOptions().BackgroundMode == Mode ? ECheckBoxState::Checked
                                                                      : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this, Token, Mode](const ECheckBoxState State) {
                if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                    return;
                if (State == ECheckBoxState::Checked)
                {
                    ViewModel->SetBackgroundMode(Mode);
                    PersistCurrentOptions();
                    SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
                }
            })[SNew(STextBlock).Text(Label).Justification(ETextJustify::Center)];
    };

    return SNew(SUniformGridPanel).SlotPadding(FMargin(2.0f)) +
           SUniformGridPanel::Slot(
               0, 0)[MakeModeButton(ECubeShotBackgroundMode::Transparent, LOCTEXT("TransparentMode", "Transparent"))] +
           SUniformGridPanel::Slot(
               1, 0)[MakeModeButton(ECubeShotBackgroundMode::SolidColor, LOCTEXT("SolidMode", "Solid"))] +
           SUniformGridPanel::Slot(
               2, 0)[MakeModeButton(ECubeShotBackgroundMode::Texture, LOCTEXT("TextureMode", "Texture"))];
}

TSharedRef<SWidget> SCubeShotPanel::BuildProjectionModeCombo()
{
    const auto Token = BindingToken;
    auto MakeModeButton = [this, Token](const ECubeShotProjectionMode Mode, const FText& Label) {
        return SNew(SCheckBox)
            .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([this, Mode]() {
                return ViewModel->GetOptions().ProjectionMode == Mode ? ECheckBoxState::Checked
                                                                      : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([this, Token, Mode](const ECheckBoxState State) {
                if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                    return;
                if (State == ECheckBoxState::Checked)
                {
                    ViewModel->SetProjectionMode(Mode);
                    if (ViewModel->GetOptions().bAutoFit)
                    {
                        ApplyAutoFit();
                    }
                    PersistCurrentOptions();
                    SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
                }
            })[SNew(STextBlock).Text(Label).Justification(ETextJustify::Center)];
    };

    return SNew(SUniformGridPanel).SlotPadding(FMargin(2.0f)) +
           SUniformGridPanel::Slot(
               0, 0)[MakeModeButton(ECubeShotProjectionMode::Perspective, LOCTEXT("PerspectiveMode", "Perspective"))] +
           SUniformGridPanel::Slot(
               1,
               0)[MakeModeButton(ECubeShotProjectionMode::Orthographic, LOCTEXT("OrthographicMode", "Orthographic"))];
}

TSharedRef<SWidget> SCubeShotPanel::BuildResolutionPresets()
{
    const auto Token = BindingToken;
    TSharedRef<SWrapBox> Presets = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(2.0f, 2.0f));
    for (const int32 Resolution : {256, 512, 1024, 2048, 4096})
    {
        Presets
            ->AddSlot()[SNew(SButton).Text(FText::AsNumber(Resolution)).OnClicked_Lambda([this, Token, Resolution]() {
                if (bShutdown || !Workspace->IsCurrent(Token))
                    return FReply::Handled();
                OnResolutionPreset(Resolution);
                return FReply::Handled();
            })];
    }
    return Presets;
}

void SCubeShotPanel::SchedulePreview(const FCubeShotViewModel::EChangeKind ChangeKind)
{
    if (bShutdown || !ViewModel.IsValid())
    {
        return;
    }

    if (Workspace->IsDefault())
    {
        ClearPreviewResources(false);
        bPreviewUpdating = false;
        return;
    }
    const bool bHadOutstandingRequest = bPreviewUpdating || ScheduledRevision != ProcessingRevision;
    ++ScheduledRevision; // A newly bound ViewModel may have the same local revision as the previous row.
    PreviewDueTime = FSlateApplication::Get().GetCurrentTime() + PreviewDebounceSeconds;
    if (ChangeKind == FCubeShotViewModel::EChangeKind::Recapture)
    {
        bScheduledCompositeOnly = false;
    }
    else if (!bHadOutstandingRequest)
    {
        bScheduledCompositeOnly = CachedRawFrames.Num() == DirectionCount;
        for (const FCubeShotRawFrame& Frame : CachedRawFrames)
        {
            bScheduledCompositeOnly &= Frame.IsValid();
        }
    }
    OperationStatus = LOCTEXT("PreviewScheduled", "Preview update scheduled.");
}

void SCubeShotPanel::StartScheduledPreview()
{
    if (!ViewModel->Validate())
    {
        bPreviewUpdating = false;
        ProcessingRevision = ScheduledRevision;
        return;
    }

    FText Error;
    const FCubeShotOptions Options = ViewModel->GetOptionsCopy();
    if (!CaptureService->Initialize(Options, Error))
    {
        PreviewError = Error;
        OperationStatus = Error;
        bPreviewUpdating = false;
        ProcessingRevision = ScheduledRevision;
        return;
    }

    ProcessingRevision = ScheduledRevision;
    bProcessingCompositeOnly = bScheduledCompositeOnly;
    NextPreviewDirection = 0;
    bPreviewUpdating = true;
    PreviewError = FText::GetEmpty();
    OperationStatus = LOCTEXT("UpdatingPreviewStatus", "Updating preview...");
    for (FCubeShotImage& Image : PendingPreviewImages)
    {
        Image.Reset();
    }
    for (FCubeShotRawFrame& Frame : PendingRawFrames)
    {
        Frame.Reset();
    }
}

void SCubeShotPanel::FinishPreviewRevision()
{
    if (ProcessingRevision != ScheduledRevision || NextPreviewDirection != DirectionCount)
    {
        return;
    }

    if (!bProcessingCompositeOnly)
    {
        Swap(CachedRawFrames, PendingRawFrames);
    }
    for (int32 Index = 0; Index < DirectionCount; ++Index)
    {
        ReplacePreviewTexture(Index, PendingPreviewImages[Index]);
    }
    PreviewError = FText::GetEmpty();
    bPreviewUpdating = false;
    OperationStatus = LOCTEXT("PreviewReadyStatus", "Preview ready.");
}

EActiveTimerReturnType SCubeShotPanel::TickPreview(const double CurrentTime, const float DeltaTime)
{
    if (bShutdown)
    {
        return EActiveTimerReturnType::Stop;
    }
    if (CurrentTime >= NextHistoryRefresh)
    {
        RefreshJobObserver();
        NextHistoryRefresh = CurrentTime + 0.25;
    }
    const bool Busy = Workspace->IsJobBusy();
    ViewModel->SetGenerating(Busy);
    if (bWasJobBusy && !Busy)
    {
        ViewModel->Validate();
        SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
    }
    bWasJobBusy = Busy;
    FText Policy;
    if (Workspace->IsDefault() || Workspace->IsSubmitting() || Busy || !CubeShot::CanExecute(Policy))
        return EActiveTimerReturnType::Continue;

    if (ProcessingRevision != ScheduledRevision)
    {
        if (CurrentTime >= PreviewDueTime)
        {
            StartScheduledPreview();
        }
        return EActiveTimerReturnType::Continue;
    }

    if (bPreviewUpdating && NextPreviewDirection < DirectionCount)
    {
        FText Error;
        if (!CapturePreviewDirection(NextPreviewDirection, Error))
        {
            PreviewError = Error;
            OperationStatus = Error;
            bPreviewUpdating = false;
            return EActiveTimerReturnType::Continue;
        }
        ++NextPreviewDirection;
        FinishPreviewRevision();
    }
    return EActiveTimerReturnType::Continue;
}

bool SCubeShotPanel::CapturePreviewDirection(const int32 DirectionIndex, FText& OutError)
{
    const FCubeShotOptions Options = ViewModel->GetOptionsCopy();
    const int32 TargetResolution = FMath::Min(Options.Resolution, CubeShot::PreviewResolution);
    const ECubeShotDirection Direction = static_cast<ECubeShotDirection>(DirectionIndex);

    const FCubeShotRawFrame* RawFrame = nullptr;
    if (bProcessingCompositeOnly)
    {
        RawFrame = &CachedRawFrames[DirectionIndex];
    }
    else
    {
        if (!CaptureService->CaptureRaw(Direction, TargetResolution, PendingRawFrames[DirectionIndex], OutError))
        {
            return false;
        }
        RawFrame = &PendingRawFrames[DirectionIndex];
    }

    return CaptureService->Composite(*RawFrame, Options, TargetResolution, PendingPreviewImages[DirectionIndex],
                                     OutError);
}

void SCubeShotPanel::ReplacePreviewTexture(const int32 DirectionIndex, const FCubeShotImage& Image)
{
    if (!Image.IsValid() || !PreviewTextures.IsValidIndex(DirectionIndex) ||
        !PreviewBrushes.IsValidIndex(DirectionIndex))
    {
        return;
    }

    const TSharedPtr<FSlateBrush> PreviousBrush = PreviewBrushes[DirectionIndex];
    const TStrongObjectPtr<UTexture2D> PreviousTexture(PreviewTextures[DirectionIndex].Get());
    VisiblePreviewPixels[DirectionIndex] = 0;
    for (const FColor& Pixel : Image.Pixels)
        VisiblePreviewPixels[DirectionIndex] += Pixel.A > 0 && FMath::Max3(Pixel.R, Pixel.G, Pixel.B) > 0;

    UTexture2D* Texture = UTexture2D::CreateTransient(Image.Width, Image.Height, PF_B8G8R8A8);
    if (Texture == nullptr || Texture->GetPlatformData() == nullptr || Texture->GetPlatformData()->Mips.IsEmpty())
    {
        return;
    }
    Texture->SRGB = true;
    Texture->NeverStream = true;
    FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
    void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(Destination, Image.Pixels.GetData(), Image.Pixels.Num() * sizeof(FColor));
    Mip.BulkData.Unlock();
    Texture->UpdateResource();

    PreviewTextures[DirectionIndex].Reset(Texture);
    PreviewBrushes[DirectionIndex] = MakeShared<FSlateBrush>();
    PreviewBrushes[DirectionIndex]->SetResourceObject(Texture);
    PreviewBrushes[DirectionIndex]->SetImageSize(FVector2D(Image.Width, Image.Height));
    PreviewBrushes[DirectionIndex]->DrawAs = ESlateBrushDrawType::Image;
    if (PreviewImages[DirectionIndex])
    {
        PreviewImages[DirectionIndex]->SetImage(PreviewBrushes[DirectionIndex].Get());
        PreviewImages[DirectionIndex]->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
    }
}

void SCubeShotPanel::ClearPreviewResources(bool Release)
{
    for (const auto& Image : PreviewImages)
        if (Image)
        {
            Image->SetImage(nullptr);
            Image->Invalidate(EInvalidateWidgetReason::Paint);
        }
    VisiblePreviewPixels.Init(0, DirectionCount);
    for (const TSharedPtr<FSlateBrush>& Brush : PreviewBrushes)
    {
        if (Brush.IsValid())
        {
            Brush->SetResourceObject(nullptr);
            Brush->DrawAs = ESlateBrushDrawType::NoDrawType;
        }
    }
    if (Release)
    {
        PreviewBrushes.Reset();
        PreviewTextures.Reset();
    }
    else
    {
        for (auto& Texture : PreviewTextures)
            Texture.Reset();
        for (auto& Frame : CachedRawFrames)
            Frame.Reset();
        for (auto& Frame : PendingRawFrames)
            Frame.Reset();
    }
}

void SCubeShotPanel::ApplyAutoFit()
{
    if (bShutdown || !CaptureService || !ViewModel.IsValid() || !ViewModel->GetOptions().bAutoFit ||
        ViewModel->GetOptions().StaticMesh == nullptr)
    {
        return;
    }
    FCubeShotAutoFitResult Fit;
    FText Error;
    if (CaptureService->CalculateAutoFit(ViewModel->GetOptionsCopy(), Fit, Error))
    {
        ViewModel->ApplyAutoFit(Fit);
    }
    else if (!Error.IsEmpty())
    {
        OperationStatus = Error;
    }
}

void SCubeShotPanel::OnMeshChanged(const FAssetData& AssetData)
{
    if (bShutdown)
        return;
    if (AssetData.IsValid())
    {
        const FString Path = AssetData.GetObjectPathString();
        AddTarget(Path);
        SelectTargetPath(Path);
    }
    else
    {
        Workspace->Edit({});
        LoadEditor();
    }
}

void SCubeShotPanel::OnBackgroundTextureChanged(const FAssetData& AssetData)
{
    ViewModel->SetBackgroundTexture(Cast<UTexture2D>(AssetData.GetAsset()));
    PersistCurrentOptions();
    SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
}

FReply SCubeShotPanel::OnUseSelectedMesh()
{
    FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Assets;
    Browser.Get().GetSelectedAssets(Assets);
    FString LastPath;
    for (const FAssetData& Asset : Assets)
        if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
        {
            LastPath = Asset.GetObjectPathString();
            AddTarget(LastPath);
        }
    if (!LastPath.IsEmpty())
        SelectTargetPath(LastPath);
    else
        OperationStatus = LOCTEXT("NoSelectedMesh", "No Static Mesh is selected in the Content Browser.");
    return FReply::Handled();
}

FReply SCubeShotPanel::OnSolidColorClicked(const FGeometry& Geometry, const FPointerEvent& PointerEvent)
{
    FColorPickerArgs Args;
    Args.InitialColor = ViewModel->GetOptions().SolidColor;
    Args.bUseAlpha = false;
    Args.bOnlyRefreshOnMouseUp = false;
    Args.ParentWidget = SharedThis(this);
    const auto Token = BindingToken;
    const TWeakPtr<SCubeShotPanel> Weak = SharedThis(this);
    Args.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([Weak, Token](FLinearColor Color) {
        if (auto Panel = Weak.Pin(); Panel && Panel->Workspace->IsCurrent(Token))
            Panel->OnSolidColorCommitted(Color);
    });
    OpenColorPicker(Args);
    return FReply::Handled();
}

void SCubeShotPanel::OnSolidColorCommitted(const FLinearColor NewColor)
{
    if (bShutdown || !ViewModel.IsValid() || !ViewModel->AreOptionsEnabled())
    {
        return;
    }
    ViewModel->SetSolidColor(NewColor);
    PersistCurrentOptions();
    SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
}

void SCubeShotPanel::OnResolutionPreset(const int32 Resolution)
{
    ViewModel->SetResolution(Resolution);
    PersistCurrentOptions();
    SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
}

FReply SCubeShotPanel::OnGenerateClicked()
{
    return StartBatch();
}

FString SCubeShotPanel::GetMeshObjectPath() const
{
    const UStaticMesh* Mesh = ViewModel.IsValid() ? ViewModel->GetOptions().StaticMesh : nullptr;
    return IsValid(Mesh) ? Mesh->GetPathName() : FString();
}

FString SCubeShotPanel::GetBackgroundTextureObjectPath() const
{
    const UTexture2D* Texture = ViewModel.IsValid() ? ViewModel->GetOptions().BackgroundTexture : nullptr;
    return IsValid(Texture) ? Texture->GetPathName() : FString();
}

const FSlateBrush* SCubeShotPanel::GetPreviewBrush(const int32 DirectionIndex) const
{
    return PreviewBrushes.IsValidIndex(DirectionIndex) ? PreviewBrushes[DirectionIndex].Get() : nullptr;
}

FText SCubeShotPanel::GetStatusText() const
{
    const auto DraftError = Workspace->ScopeError(Workspace->Token().Scope);
    if (!DraftError.IsEmpty())
        return FText::FromString(DraftError);
    if (Workspace->IsDefault())
        return LOCTEXT("DefaultOptionsStatus", "Defaults for new targets. Select a row to preview it.");
    FText Policy;
    if (!CubeShot::CanExecute(Policy))
        return Policy;
    if (ViewModel.IsValid())
    {
        if (!ViewModel->GetValidationError().IsEmpty())
        {
            return ViewModel->GetValidationError();
        }
        if (!PreviewError.IsEmpty())
        {
            return PreviewError;
        }
    }
    if (ViewModel.IsValid() && !ViewModel->GetValidationWarning().IsEmpty())
        return FText::Format(LOCTEXT("StatusWithWarnings", "{0}\n{1}"), OperationStatus,
                             ViewModel->GetValidationWarning());
    return OperationStatus;
}

FSlateColor SCubeShotPanel::GetStatusColor() const
{
    if (!Workspace->ScopeError(Workspace->Token().Scope).IsEmpty())
        return FSlateColor(FLinearColor(.9f, .15f, .12f));
    if (Workspace->IsDefault())
        return FSlateColor::UseForeground();
    if (ViewModel.IsValid() && (!ViewModel->GetValidationError().IsEmpty() || !PreviewError.IsEmpty()))
    {
        return FSlateColor(FLinearColor(0.9f, 0.15f, 0.12f));
    }
    if (ViewModel.IsValid() && !ViewModel->GetValidationWarning().IsEmpty())
    {
        return FSlateColor(FLinearColor(1.0f, 0.55f, 0.1f));
    }
    return FSlateColor::UseForeground();
}

FText SCubeShotPanel::GetProgressText() const
{
    return FText::Format(LOCTEXT("ProgressFormat", "{0} / {1}"), ProgressCompleted, ProgressTotal);
}

bool SCubeShotPanel::CanStartGeneration() const
{
    FText Policy;
    return !Workspace->Targets().IsEmpty() && !Workspace->IsSubmitting() && CubeShot::CanExecute(Policy) &&
           !Workspace->IsJobBusy();
}

EVisibility SCubeShotPanel::GetUpdatingVisibility() const
{
    return bPreviewUpdating ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCubeShotPanel::GetProgressVisibility() const
{
    return ViewModel.IsValid() && ViewModel->IsGenerating() ? EVisibility::Visible : EVisibility::Collapsed;
}

void SCubeShotPanel::LoadEditor()
{
    if (bShutdown || Workspace->IsStopped())
        return;
    Workspace->ReconcileProfiles();
    bLoadingTarget = true;
    BindingToken = Workspace->Token();
    FCubeShotOptions Options;
    FText Error;
    Workspace->ReadEditorOptions(Options, Error);
    ViewModel = MakeShared<FCubeShotViewModel>();
    ViewModel->SetStaticMesh(Options.StaticMesh);
    ViewModel->SetBackgroundTexture(Options.BackgroundTexture);
    ViewModel->SetBackgroundMode(Options.BackgroundMode);
    ViewModel->SetSolidColor(Options.SolidColor);
    ViewModel->SetProjectionMode(Options.ProjectionMode);
    ViewModel->SetAutoFit(Options.bAutoFit);
    ViewModel->SetCameraDistance(Options.CameraDistance);
    ViewModel->SetFieldOfView(Options.FieldOfView);
    ViewModel->SetOrthoWidth(Options.OrthoWidth);
    ViewModel->SetResolution(Options.Resolution);
    bExportPng = Workspace->Bool(TEXT("export_png"));
    bTrimPng = Workspace->Bool(TEXT("trim_transparent"));
    PngPadding = Workspace->Number(TEXT("padding_pixels"));
    PngTurns = Workspace->Number(TEXT("quarter_turns"));
    PngDirections.Reset();
    for (const auto& Direction :
         UnrealToolsRuntime::GetArray(Workspace->CopyActiveEditorOptions(), TEXT("png_directions")))
        PngDirections.Add(Direction->AsString());
    ApplyAutoFit();
    if (OptionsHost)
        OptionsHost->SetContent(BuildOptionsPanel());
    RebuildTargets();
    bLoadingTarget = false;
    SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
}
void SCubeShotPanel::PersistCurrentOptions()
{
    if (bShutdown || bLoadingTarget || !ViewModel || !Workspace->IsCurrent(BindingToken))
        return;
    auto Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("export_png"), bExportPng);
    Json->SetBoolField(TEXT("trim_transparent"), bTrimPng);
    Json->SetNumberField(TEXT("padding_pixels"), PngPadding);
    Json->SetNumberField(TEXT("quarter_turns"), PngTurns);
    TArray<TSharedPtr<FJsonValue>> Directions;
    for (const auto& Value : PngDirections)
        Directions.Add(MakeShared<FJsonValueString>(Value));
    Json->SetArrayField(TEXT("png_directions"), Directions);
    Workspace->CommitEditorOptions(BindingToken, ViewModel->GetOptionsCopy(), Json);
}
void SCubeShotPanel::OnAutoFitChanged(const UnrealToolsWorkspace::FEditToken& Token, bool Enabled)
{
    if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
        return;
    TOptional<FCubeShotAutoFitResult> Fit;
    if (!Enabled && !Workspace->IsDefault())
    {
        FCubeShotAutoFitResult Value;
        FText Error;
        if (CaptureService->CalculateAutoFit(ViewModel->GetOptionsCopy(), Value, Error))
            Fit = Value;
    }
    Workspace->SetAutoFit(Token, Enabled, Fit);
    LoadEditor();
}
void SCubeShotPanel::ApplyOptionsToSelected()
{
    Workspace->ApplySelected();
    LoadEditor();
}
void SCubeShotPanel::RemoveSelectedTargets()
{
    Workspace->RemoveSelected();
    LoadEditor();
}
void SCubeShotPanel::AddTarget(const FString& Path)
{
    const bool Existing = Workspace->Targets().ContainsByPredicate([&](const auto& Row) { return Row->Path == Path; });
    if (Workspace->Add(Path) && !Existing)
        Workspace->Select(Path, true);
    RebuildTargets();
}
void SCubeShotPanel::SelectTarget(int32 Index)
{
    if (Workspace->Targets().IsValidIndex(Index))
        SelectTargetPath(Workspace->Targets()[Index]->Path);
}
void SCubeShotPanel::SelectTargetPath(const FString& Path)
{
    Workspace->Edit(Path);
    LoadEditor();
}
void SCubeShotPanel::MoveTarget(int32 Index, int32 Delta)
{
    if (Workspace->Targets().IsValidIndex(Index))
        Workspace->Move(Workspace->Targets()[Index]->Path, Delta);
    RebuildTargets();
}
void SCubeShotPanel::ApplyProfile(const FString& Id)
{
    Workspace->ApplyProfile(Id);
    LoadEditor();
}
void SCubeShotPanel::RebuildTargets()
{
    if (!TargetRows)
        return;
    TargetRows->ClearChildren();
    for (const auto& Row : Workspace->Targets())
    {
        const auto Path = Row->Path;
        TargetRows->AddSlot().AutoHeight().Padding(
            0,
            2)[SNew(SHorizontalBox) +
               SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox)
                                                      .IsChecked_Lambda([this, Path]() {
                                                          for (const auto& Item : Workspace->Targets())
                                                              if (Item->Path == Path)
                                                                  return Item->bSelected ? ECheckBoxState::Checked
                                                                                         : ECheckBoxState::Unchecked;
                                                          return ECheckBoxState::Unchecked;
                                                      })
                                                      .OnCheckStateChanged_Lambda([this, Path](ECheckBoxState Value) {
                                                          Workspace->Select(Path, Value == ECheckBoxState::Checked);
                                                      })] +
               SHorizontalBox::Slot().FillWidth(1)[SNew(SButton)
                                                       .Text_Lambda([this, Path]() {
                                                           return Workspace->TargetLabel(Path, true);
                                                       })
                                                       .ToolTipText(FText::FromString(Path))
                                                       .OnClicked_Lambda([this, Path]() {
                                                           SelectTargetPath(Path);
                                                           return FReply::Handled();
                                                       })] +
               SHorizontalBox::Slot()
                   .AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("↑"))).OnClicked_Lambda([this, Path]() {
                       Workspace->Move(Path, -1);
                       RebuildTargets();
                       return FReply::Handled();
                   })] +
               SHorizontalBox::Slot()
                   .AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("↓"))).OnClicked_Lambda([this, Path]() {
                       Workspace->Move(Path, 1);
                       RebuildTargets();
                       return FReply::Handled();
                   })] +
               SHorizontalBox::Slot()
                   .AutoWidth()[SNew(SButton).Text(LOCTEXT("ResetRow", "Reset")).OnClicked_Lambda([this, Path]() {
                       Workspace->Reset(Path);
                       LoadEditor();
                       return FReply::Handled();
                   })] +
               SHorizontalBox::Slot()
                   .AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("×"))).OnClicked_Lambda([this, Path]() {
                       Workspace->Remove(Path);
                       LoadEditor();
                       return FReply::Handled();
                   })]];
    }
}

TSharedRef<SWidget> SCubeShotPanel::BuildWorkflowPanel()
{
    const auto Token = BindingToken;
    SAssignNew(ProfileRows, SVerticalBox);
    RebuildProfiles();
    return SNew(SVerticalBox) +
           SVerticalBox::Slot()
               .AutoHeight()[MakeSectionHeading(LOCTEXT("TargetWorkflow", "Targets · Profiles · Output plan"))] +
           SVerticalBox::Slot().AutoHeight()[SNew(SBox).MaxDesiredHeight(
               160)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(TargetRows, SVerticalBox)]]] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(STextBlock)
                                 .Text(LOCTEXT("DropHelp", "Check rows for bulk edits/removal. Click a name to "
                                                           "preview/edit that row. Capture runs the ordered list."))
                                 .AutoWrapText(true)] +
           SVerticalBox::Slot().AutoHeight()
               [SNew(SHorizontalBox) +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("SelectRows", "Select All")).OnClicked_Lambda([this]() {
                    Workspace->SelectAll(true);
                    return FReply::Handled();
                })] +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("ClearRows", "Clear Selection")).OnClicked_Lambda([this]() {
                    Workspace->SelectAll(false);
                    return FReply::Handled();
                })] +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("RemoveRows", "Remove Selected")).OnClicked_Lambda([this]() {
                    RemoveSelectedTargets();
                    return FReply::Handled();
                })]] +
           SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]() {
               return FText::FromString(Workspace->IsDefault() ? TEXT("Defaults for new targets")
                                                               : TEXT("Options for ") + Workspace->EditingPath());
           })] +
           SVerticalBox::Slot().AutoHeight()
               [SNew(SButton).Text(LOCTEXT("EditDefaults", "Edit defaults for new targets")).OnClicked_Lambda([this]() {
                   Workspace->Edit({});
                   LoadEditor();
                   return FReply::Handled();
               })] +
           SVerticalBox::Slot().AutoHeight()[ProfileRows.ToSharedRef()] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(SEditableTextBox)
                                 .Text_Lambda([this]() { return FText::FromString(Workspace->ProfileName()); })
                                 .HintText(LOCTEXT("ProfileName", "User profile name"))
                                 .OnTextChanged_Lambda(
                                     [this](const FText& Text) { Workspace->SetProfileName(Text.ToString()); })] +
           SVerticalBox::Slot().AutoHeight()
               [SNew(SHorizontalBox) +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("SaveProfile", "Save Profile"))
                           .OnClicked(this, &SCubeShotPanel::SaveProfile)] +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("DeleteProfile", "Delete Profile")).OnClicked_Lambda([this]() {
                    const auto Result = Workspace->DeleteProfile();
                    RebuildProfiles();
                    LastResultStatus = FText::FromString(Result->GetBoolField(TEXT("success"))
                                                             ? TEXT("Profile deleted.")
                                                             : Result->GetStringField(TEXT("error")));
                    return FReply::Handled();
                })]] +
           SVerticalBox::Slot().AutoHeight()
               [SNew(SHorizontalBox) +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("ImportProfile", "Import JSON")).OnClicked_Lambda([this]() {
                    return TransferProfile(true);
                })] +
                SHorizontalBox::Slot().FillWidth(
                    1)[SNew(SButton).Text(LOCTEXT("ExportProfile", "Export JSON")).OnClicked_Lambda([this]() {
                    return TransferProfile(false);
                })]] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(SButton)
                                 .Text(LOCTEXT("SaveTargetOptions", "Apply current options to checked rows"))
                                 .IsEnabled_Lambda([this]() {
                                     return Workspace->Targets().ContainsByPredicate(
                                         [](const auto& Row) { return Row->bSelected; });
                                 })
                                 .OnClicked_Lambda([this]() {
                                     ApplyOptionsToSelected();
                                     return FReply::Handled();
                                 })] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(SCheckBox)
                                 .IsChecked_Lambda([this]() {
                                     return bExportPng ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                                 })
                                 .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                     if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                         return;
                                     bExportPng = State == ECheckBoxState::Checked;
                                     PersistCurrentOptions();
                                 })[SNew(STextBlock).Text(LOCTEXT("ExportPNG", "Publish selected PNG views"))]] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(SWrapBox).UseAllottedSize(true) +
                             SWrapBox::Slot()[SNew(SCheckBox)
                                                  .IsChecked_Lambda([this]() {
                                                      return PngDirections.Contains(TEXT("Front"))
                                                                 ? ECheckBoxState::Checked
                                                                 : ECheckBoxState::Unchecked;
                                                  })
                                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                                      if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                          return;
                                                      if (State == ECheckBoxState::Checked)
                                                          PngDirections.AddUnique(TEXT("Front"));
                                                      else if (PngDirections.Num() > 1)
                                                          PngDirections.Remove(TEXT("Front"));
                                                      PersistCurrentOptions();
                                                  })[SNew(STextBlock).Text(FText::FromString(TEXT("Front")))]] +
                             SWrapBox::Slot()[SNew(SCheckBox)
                                                  .IsChecked_Lambda([this]() {
                                                      return PngDirections.Contains(TEXT("Back"))
                                                                 ? ECheckBoxState::Checked
                                                                 : ECheckBoxState::Unchecked;
                                                  })
                                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                                      if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                          return;
                                                      if (State == ECheckBoxState::Checked)
                                                          PngDirections.AddUnique(TEXT("Back"));
                                                      else if (PngDirections.Num() > 1)
                                                          PngDirections.Remove(TEXT("Back"));
                                                      PersistCurrentOptions();
                                                  })[SNew(STextBlock).Text(FText::FromString(TEXT("Back")))]] +
                             SWrapBox::Slot()[SNew(SCheckBox)
                                                  .IsChecked_Lambda([this]() {
                                                      return PngDirections.Contains(TEXT("Left"))
                                                                 ? ECheckBoxState::Checked
                                                                 : ECheckBoxState::Unchecked;
                                                  })
                                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                                      if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                          return;
                                                      if (State == ECheckBoxState::Checked)
                                                          PngDirections.AddUnique(TEXT("Left"));
                                                      else if (PngDirections.Num() > 1)
                                                          PngDirections.Remove(TEXT("Left"));
                                                      PersistCurrentOptions();
                                                  })[SNew(STextBlock).Text(FText::FromString(TEXT("Left")))]] +
                             SWrapBox::Slot()[SNew(SCheckBox)
                                                  .IsChecked_Lambda([this]() {
                                                      return PngDirections.Contains(TEXT("Right"))
                                                                 ? ECheckBoxState::Checked
                                                                 : ECheckBoxState::Unchecked;
                                                  })
                                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                                      if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                          return;
                                                      if (State == ECheckBoxState::Checked)
                                                          PngDirections.AddUnique(TEXT("Right"));
                                                      else if (PngDirections.Num() > 1)
                                                          PngDirections.Remove(TEXT("Right"));
                                                      PersistCurrentOptions();
                                                  })[SNew(STextBlock).Text(FText::FromString(TEXT("Right")))]] +
                             SWrapBox::Slot()[SNew(SCheckBox)
                                                  .IsChecked_Lambda([this]() {
                                                      return PngDirections.Contains(TEXT("Top"))
                                                                 ? ECheckBoxState::Checked
                                                                 : ECheckBoxState::Unchecked;
                                                  })
                                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                                      if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                          return;
                                                      if (State == ECheckBoxState::Checked)
                                                          PngDirections.AddUnique(TEXT("Top"));
                                                      else if (PngDirections.Num() > 1)
                                                          PngDirections.Remove(TEXT("Top"));
                                                      PersistCurrentOptions();
                                                  })[SNew(STextBlock).Text(FText::FromString(TEXT("Top")))]] +
                             SWrapBox::Slot()[SNew(SCheckBox)
                                                  .IsChecked_Lambda([this]() {
                                                      return PngDirections.Contains(TEXT("Bottom"))
                                                                 ? ECheckBoxState::Checked
                                                                 : ECheckBoxState::Unchecked;
                                                  })
                                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                                      if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                          return;
                                                      if (State == ECheckBoxState::Checked)
                                                          PngDirections.AddUnique(TEXT("Bottom"));
                                                      else if (PngDirections.Num() > 1)
                                                          PngDirections.Remove(TEXT("Bottom"));
                                                      PersistCurrentOptions();
                                                  })[SNew(STextBlock).Text(FText::FromString(TEXT("Bottom")))]]] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(SCheckBox)
                                 .IsChecked_Lambda([this]() {
                                     return bTrimPng ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                                 })
                                 .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState State) {
                                     if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                         return;
                                     bTrimPng = State == ECheckBoxState::Checked;
                                     PersistCurrentOptions();
                                 })[SNew(STextBlock).Text(LOCTEXT("TrimPNG", "Trim transparent margins"))]] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(SHorizontalBox) +
                             SHorizontalBox::Slot().FillWidth(
                                 1)[SNew(SNumericEntryBox<int32>)
                                        .Value_Lambda([this]() { return TOptional<int32>(PngPadding); })
                                        .MinValue(0)
                                        .MaxValue(64)
                                        .OnValueChanged_Lambda([this, Token](int32 Value) {
                                            if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                return;
                                            PngPadding = Value;
                                            PersistCurrentOptions();
                                        })
                                        .Label()[SNew(STextBlock).Text(LOCTEXT("Padding", "Padding"))]] +
                             SHorizontalBox::Slot().FillWidth(
                                 1)[SNew(SNumericEntryBox<int32>)
                                        .Value_Lambda([this]() { return TOptional<int32>(PngTurns); })
                                        .MinValue(0)
                                        .MaxValue(3)
                                        .OnValueChanged_Lambda([this, Token](int32 Value) {
                                            if (bShutdown || bLoadingTarget || !Workspace->IsCurrent(Token))
                                                return;
                                            PngTurns = Value;
                                            PersistCurrentOptions();
                                        })
                                        .Label()[SNew(STextBlock).Text(LOCTEXT("Turns", "90° turns"))]]] +
           SVerticalBox::Slot().AutoHeight()[SNew(SButton)
                                                 .Text(LOCTEXT("PreflightAll", "Inspect all outputs"))
                                                 .OnClicked(this, &SCubeShotPanel::PreflightTargets)] +
           SVerticalBox::Slot()
               .AutoHeight()[SNew(STextBlock)
                                 .Text_Lambda([this]() { return FText::FromString(Workspace->PlanText()); })
                                 .AutoWrapText(true)];
}

FReply SCubeShotPanel::PreflightTargets()
{
    Workspace->Inspect(nullptr, true);
    return FReply::Handled();
}
FReply SCubeShotPanel::StartBatch(const FString& RetryState)
{
    FCubeShotWorkspaceController::FSubmissionInteraction Interaction;
    Interaction.Preparing = [this]() {
        ++ScheduledRevision;
        ++ProcessingRevision;
        bPreviewUpdating = false;
    };
    Interaction.ConfirmOverwrite = []() {
        return FMessageDialog::Open(
                   EAppMsgType::YesNo,
                   LOCTEXT("QueueOverwrite",
                           "Replace the inspected textures or PNGs? Existing Texture2D references are preserved.")) ==
               EAppReturnType::Yes;
    };
#if WITH_DEV_AUTOMATION_TESTS
    if (OverwriteDecisionForAutomation)
        Interaction.ConfirmOverwrite = OverwriteDecisionForAutomation;
    Interaction.Checkout = CheckoutDecisionForAutomation;
#endif
    const auto Panel = SharedThis(this); // Modal UI may close the owning tab.
    const auto Result = Panel->Workspace->Start(Interaction, RetryState);
    if (Result)
        LastResultStatus = FText::FromString(UnrealToolsRuntime::GetBool(Result, TEXT("success"))
                                                 ? TEXT("Capture queued. The task continues when this tab closes.")
                                                 : UnrealToolsRuntime::GetString(Result, TEXT("error")));
    RefreshJobObserver();
    if (!Workspace->IsSubmitting() && !Workspace->IsJobBusy())
        SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
    return FReply::Handled();
}

FText SCubeShotPanel::ResultsText() const
{
    const auto Revision = Workspace->History().ResultRevision;
    if (ResultsRevision != Revision || !ResultsStatus.EqualTo(LastResultStatus))
    {
        ResultsRevision = Revision;
        ResultsStatus = LastResultStatus;
        ResultsCache = FText::FromString(LastResultStatus.ToString() + TEXT("\n") + Workspace->History().Body);
    }
    return ResultsCache;
}
void SCubeShotPanel::RefreshJobObserver(bool Force)
{
    Workspace->RefreshHistory(Force);
    if (Force || DisplayedHistoryRevision != Workspace->History().Revision)
    {
        DisplayedHistoryRevision = Workspace->History().Revision;
        RebuildJobs();
    }
}

void SCubeShotPanel::RebuildJobs()
{
    if (!JobRows)
        return;
    JobRows->ClearChildren();
    for (const auto& Job : Workspace->History().Summaries)
    {
        const FString Id = UnrealToolsRuntime::GetString(Job, TEXT("job_id"));
        const FString Label = UnrealToolsRuntime::GetString(Job, TEXT("created_at")) + TEXT(" · ") +
                              UnrealToolsRuntime::GetString(Job, TEXT("state")) + TEXT(" · ") +
                              UnrealToolsRuntime::GetString(Job, TEXT("record_kind"));
        JobRows->AddSlot().AutoHeight()[SNew(SButton)
                                            .Text(FText::FromString(Label))
                                            .ToolTipText(FText::FromString(Id))
                                            .OnClicked_Lambda([this, Id]() {
                                                Workspace->SelectJob(Id);
                                                RefreshJobObserver();
                                                return FReply::Handled();
                                            })];
    }
}

void SCubeShotPanel::RebuildProfiles()
{
    if (!ProfileRows)
        return;
    ProfileRows->ClearChildren();
    for (const auto& Profile : Workspace->ListProfiles())
    {
        const FString Id = Profile->GetStringField(TEXT("id"));
        const FString Name = Profile->GetStringField(TEXT("display_name"));
        ProfileRows->AddSlot().AutoHeight()[SNew(SButton).Text(FText::FromString(Name)).OnClicked_Lambda([this, Id]() {
            ApplyProfile(Id);
            return FReply::Handled();
        })];
    }
}

TSharedRef<SWidget> SCubeShotPanel::BuildResultsPanel()
{
    return SNew(SBorder).Padding(8)
        [SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight()[MakeSectionHeading(LOCTEXT("Results", "Progress · Results · Recovery"))] +
         SVerticalBox::Slot().AutoHeight()
             [SNew(SHorizontalBox) +
              SHorizontalBox::Slot()
                  .AutoWidth()[SNew(SButton).Text(LOCTEXT("RecentHistory", "Recent")).OnClicked_Lambda([this]() {
                      Workspace->HistoryMode(false, false);
                      RefreshJobObserver();
                      return FReply::Handled();
                  })] +
              SHorizontalBox::Slot()
                  .AutoWidth()[SNew(SButton).Text(LOCTEXT("ArchiveHistory", "Archive")).OnClicked_Lambda([this]() {
                      Workspace->HistoryMode(true, false);
                      RefreshJobObserver();
                      return FReply::Handled();
                  })] +
              SHorizontalBox::Slot().AutoWidth()
                  [SNew(SButton).Text(LOCTEXT("DiagnosticHistory", "Diagnostics")).OnClicked_Lambda([this]() {
                      Workspace->HistoryMode(false, true);
                      RefreshJobObserver();
                      return FReply::Handled();
                  })] +
              SHorizontalBox::Slot()
                  .AutoWidth()[SNew(SButton)
                                   .Text(FText::FromString(TEXT("←")))
                                   .IsEnabled_Lambda([this]() { return Workspace->History().Offset > 0; })
                                   .OnClicked_Lambda([this]() {
                                       Workspace->HistoryPage(-1);
                                       RefreshJobObserver();
                                       return FReply::Handled();
                                   })] +
              SHorizontalBox::Slot()
                  .AutoWidth()[SNew(SButton)
                                   .Text(FText::FromString(TEXT("→")))
                                   .IsEnabled_Lambda([this]() { return Workspace->History().Summaries.Num() == 50; })
                                   .OnClicked_Lambda([this]() {
                                       Workspace->HistoryPage(1);
                                       RefreshJobObserver();
                                       return FReply::Handled();
                                   })] +
              SHorizontalBox::Slot().AutoWidth()[SNew(STextBlock).Text_Lambda([this]() {
                  return FText::FromString(
                      FString::Printf(TEXT("  Page %d · 50 per page"), Workspace->History().Offset / 50 + 1));
              })]] +
         SVerticalBox::Slot().AutoHeight()[SNew(SBox).MaxDesiredHeight(
             64)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(JobRows, SVerticalBox)]]] +
         SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(
             155)[SNew(SMultiLineEditableTextBox)
                      .IsReadOnly(true)
                      .AutoWrapText(true)
                      .WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
                      .Text(this, &SCubeShotPanel::ResultsText)]] +
         SVerticalBox::Slot().AutoHeight()
             [SNew(SWrapBox).UseAllottedSize(true) +
              SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Cancel", "Request Cancel")).OnClicked_Lambda([this]() {
                  Workspace->ControlSelectedJob(TEXT("cancel"));
                  return FReply::Handled();
              })] +
              SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("RetryFailed", "Retry failed")).OnClicked_Lambda([this]() {
                  return Retry(false);
              })] +
              SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("RunSkipped", "Run skipped")).OnClicked_Lambda([this]() {
                  return Retry(true);
              })] +
              SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("OpenOutputs", "Open output folder")).OnClicked_Lambda([]() {
                  FPlatformProcess::ExploreFolder(*(FPaths::ProjectSavedDir() / TEXT("CubeShot/MCPExports")));
                  return FReply::Handled();
              })] +
              SWrapBox::Slot()[SNew(SButton)
                                   .Text(LOCTEXT("BrowseTextures", "Browse generated textures"))
                                   .OnClicked_Lambda([this]() {
                                       Workspace->RefreshHistory(true);
                                       const auto Job = Workspace->SelectedJob();
                                       TArray<FAssetData> Assets;
                                       for (const auto& Item : UnrealToolsRuntime::GetArray(Job, TEXT("items")))
                                           for (const auto& Path : UnrealToolsRuntime::GetArray(
                                                    UnrealToolsRuntime::GetObject(Item->AsObject(), TEXT("result")),
                                                    TEXT("saved")))
                                               if (UObject* Asset = LoadObject<UObject>(nullptr, *Path->AsString()))
                                                   Assets.Add(FAssetData(Asset));
                                       if (!Assets.IsEmpty())
                                           FModuleManager::LoadModuleChecked<FContentBrowserModule>(
                                               TEXT("ContentBrowser"))
                                               .Get()
                                               .SyncBrowserToAssets(Assets);
                                       return FReply::Handled();
                                   })] +
              SWrapBox::Slot()
                  [SNew(SButton).Text(LOCTEXT("CopyResults", "Copy result paths")).OnClicked_Lambda([this]() {
                      FPlatformApplicationMisc::ClipboardCopy(*ResultsText().ToString());
                      return FReply::Handled();
                  })] +
              SWrapBox::Slot()[SNew(SButton)
                                   .Text(LOCTEXT("AcknowledgeRecovery", "Acknowledge reviewed recovery"))
                                   .OnClicked_Lambda([this]() {
                                       const bool Diagnostic =
                                           UnrealToolsRuntime::GetString(Workspace->History().Selected,
                                                                         TEXT("record_kind")) == TEXT("diagnostic");
                                       const FText Confirmation =
                                           Diagnostic
                                               ? LOCTEXT("ReviewDiagnosticMetadata",
                                                         "Have you reviewed the source metadata and any preserved raw "
                                                         "evidence? Acknowledgement verifies and preserves raw bytes "
                                                         "before damaged source metadata may be removed. Output files, "
                                                         "backups, and recovery staging are preserved.")
                                               : LOCTEXT("ReviewRecovery",
                                                         "Have you reviewed the saved, dirty and staging paths and "
                                                         "resolved any pending package changes? This releases the "
                                                         "output block without restoring or deleting files.");
                                       if (FMessageDialog::Open(EAppMsgType::YesNo, Confirmation) ==
                                           EAppReturnType::Yes)
                                       {
                                           const auto Result =
                                               Workspace->ControlSelectedJob(TEXT("acknowledge_recovery"));
                                           LastResultStatus =
                                               FText::FromString(Result->GetBoolField(TEXT("success"))
                                                                     ? TEXT("Recovery block released.")
                                                                     : Result->GetStringField(TEXT("error")));
                                       }
                                       return FReply::Handled();
                                   })]]];
}

FReply SCubeShotPanel::Retry(bool Skipped)
{
    return StartBatch(Skipped ? TEXT("skipped") : TEXT("failed"));
}

FReply SCubeShotPanel::SaveProfile()
{
    if (bShutdown || Workspace->IsStopped())
        return FReply::Handled();
    const auto Panel = SharedThis(this);
    UnrealToolsWorkspace::FController::FConfirmProfileReplacement Confirm = []() {
        return FMessageDialog::Open(EAppMsgType::YesNo,
                                    LOCTEXT("ReplaceProfile", "Replace this existing user profile?")) ==
               EAppReturnType::Yes;
    };
#if WITH_DEV_AUTOMATION_TESTS
    if (ProfileReplacementForAutomation)
        Confirm = ProfileReplacementForAutomation;
#endif
    const auto Result = Panel->Workspace->SaveProfile(Confirm);
    // Keeping the panel alive across a modal does not keep its resources alive.
    if (bShutdown || Workspace->IsStopped() || !Result)
        return FReply::Handled();
    LastResultStatus = FText::FromString(UnrealToolsRuntime::GetBool(Result, TEXT("success"))
                                             ? TEXT("Profile saved.")
                                             : UnrealToolsRuntime::GetString(Result, TEXT("error")));
    LoadEditor();
    return FReply::Handled();
}

FReply SCubeShotPanel::TransferProfile(bool Import)
{
    const auto Panel = SharedThis(this);
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop)
        return FReply::Handled();
    TArray<FString> Files;
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(Panel);
    if (Import)
    {
        if (!Desktop->OpenFileDialog(Parent, TEXT("Import CubeShot profile"), FPaths::ProjectSavedDir(), TEXT(""),
                                     TEXT("JSON (*.json)|*.json"), 0, Files) ||
            Files.IsEmpty())
            return FReply::Handled();
        const auto Result = Workspace->ImportProfileFile(Files[0], []() {
            return FMessageDialog::Open(EAppMsgType::YesNo,
                                        LOCTEXT("ReplaceImportedProfile",
                                                "Replace this existing user profile with the imported JSON?")) ==
                   EAppReturnType::Yes;
        });
        if (!Result)
            return FReply::Handled();
        RebuildProfiles();
        LastResultStatus = FText::FromString(UnrealToolsRuntime::GetBool(Result, TEXT("success"))
                                                 ? TEXT("Profile imported.")
                                                 : UnrealToolsRuntime::GetString(Result, TEXT("error")));
    }
    else
    {
        if (!Desktop->SaveFileDialog(Parent, TEXT("Export CubeShot profile"), FPaths::ProjectSavedDir(),
                                     Workspace->ProfileId() + TEXT(".json"), TEXT("JSON (*.json)|*.json"), 0, Files) ||
            Files.IsEmpty())
            return FReply::Handled();
        const auto Result = Workspace->ExportProfileFile(Files[0]);
        LastResultStatus = FText::FromString(UnrealToolsRuntime::GetBool(Result, TEXT("success"))
                                                 ? TEXT("Profile exported.")
                                                 : UnrealToolsRuntime::GetString(Result, TEXT("error")));
    }
    return FReply::Handled();
}

FReply SCubeShotPanel::OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event)
{
    auto Drag = Event.GetOperationAs<FAssetDragDropOp>();
    if (!Drag)
        return FReply::Unhandled();
    FString LastPath;
    for (const auto& Asset : Drag->GetAssets())
        if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
        {
            LastPath = Asset.GetObjectPathString();
            AddTarget(LastPath);
        }
    if (!LastPath.IsEmpty())
        SelectTargetPath(LastPath);
    return FReply::Handled();
}
void SCubeShotPanel::OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event)
{
    SCompoundWidget::OnDragEnter(Geometry, Event);
}

#if WITH_DEV_AUTOMATION_TESTS
void SCubeShotPanel::ClearTargetsForAutomation()
{
    Workspace->Clear();
    LoadEditor();
}
void SCubeShotPanel::SetSelectedTargetsForAutomation(const TArray<FString>& Paths)
{
    for (const auto& Row : Workspace->Targets())
        Workspace->Select(Row->Path, Paths.Contains(Row->Path));
}
void SCubeShotPanel::SetResolutionForAutomation(int32 Resolution)
{
    ViewModel->SetResolution(Resolution);
    PersistCurrentOptions();
}
TArray<FString> SCubeShotPanel::TargetPathsForAutomation() const
{
    TArray<FString> Paths;
    for (const auto& Row : Workspace->Targets())
        Paths.Add(Row->Path);
    return Paths;
}
TSharedPtr<FJsonObject> SCubeShotPanel::TargetOptionsForAutomation(const FString& Path) const
{
    return Workspace->CopyOptions(Path);
}
void SCubeShotPanel::SelectJobForAutomation(const FString& JobId)
{
    Workspace->SelectJob(JobId);
    RefreshJobObserver();
}
void SCubeShotPanel::PickTargetForAutomation(const FString& Path)
{
    OnMeshChanged(FAssetData(LoadObject<UStaticMesh>(nullptr, *Path)));
}

#endif

#if WITH_DEV_AUTOMATION_TESTS
bool SCubeShotPanel::HasCompletePreviewPixelsForAutomation() const
{
    if (bPreviewUpdating || VisiblePreviewPixels.Num() != DirectionCount)
        return false;
    for (int64 Visible : VisiblePreviewPixels)
        if (Visible <= 0)
            return false;
    return true;
}
bool SCubeShotPanel::TakePaintedPreviewForAutomation(TArray<FColor>& Pixels, FIntVector& Size) const
{
    return !PreviewImages.IsEmpty() && PreviewImages[0].IsValid() &&
           FSlateApplication::Get().TakeScreenshot(PreviewImages[0].ToSharedRef(), Pixels, Size);
}
#endif

#undef LOCTEXT_NAMESPACE
