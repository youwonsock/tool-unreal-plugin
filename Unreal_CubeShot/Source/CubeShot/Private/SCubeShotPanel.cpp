#include "SCubeShotPanel.h"

#include "CubeShotAssetWriter.h"
#include "CubeShotCaptureService.h"
#include "CubeShotJobService.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/SWindow.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
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
    struct FTargetState { FString Path; TSharedPtr<FJsonObject> Override; bool bSelected=false;bool bCustom=false; };
    TArray<FTargetState> Targets;
    FString LatestJobId;
    FString PersistedProfile = TEXT("editor_default");
    int32 SelectedTarget = INDEX_NONE;
    FString ProfileName = TEXT("My Profile");

	TSharedRef<STextBlock> MakeSectionHeading(const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"));
	}

	TSharedRef<STextBlock> MakeOptionLabel(const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.MinDesiredWidth(116.0f);
	}
}

void SCubeShotPanel::Construct(const FArguments& InArgs)
{
	ViewModel = MakeShared<FCubeShotViewModel>();
    SelectedProfile = PersistedProfile;
    if(!CubeShot::Service::Profiles().Get(SelectedProfile)){SelectedProfile=TEXT("editor_default");PersistedProfile=SelectedProfile;}
    const auto History = CubeShot::Service::Jobs().GetJobSummaries();
    if (LatestJobId.IsEmpty() && !History.IsEmpty()) LatestJobId = History[0]->GetStringField(TEXT("job_id"));
	CaptureService = MakeUnique<FCubeShotCaptureService>();

	CachedRawFrames.SetNum(DirectionCount);
	PendingRawFrames.SetNum(DirectionCount);
	PendingPreviewImages.SetNum(DirectionCount);
	PreviewTextures.Reserve(DirectionCount);
	PreviewBrushes.Reserve(DirectionCount);
    PreviewImages.SetNum(DirectionCount);VisiblePreviewPixels.Init(0,DirectionCount);
	for (int32 Index = 0; Index < DirectionCount; ++Index)
	{
		PreviewTextures.Emplace(nullptr);
		PreviewBrushes.Add(MakeShared<FSlateBrush>());
		PreviewBrushes[Index]->DrawAs = ESlateBrushDrawType::NoDrawType;
	}

	OperationStatus = LOCTEXT("SelectMeshStatus", "Select a Static Mesh to begin.");

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(0.0f)
		[
			SNew(SSplitter)
			.PhysicalSplitterHandleSize(2.0f)
			+ SSplitter::Slot()
			.Value(0.31f)
			.MinSize(290.0f)
			[
				BuildOptionsPanel()
			]
			+ SSplitter::Slot()
			.Value(0.69f)
			.MinSize(420.0f)
			[
                SNew(SVerticalBox)
                + SVerticalBox::Slot().FillHeight(1.0f)[ BuildPreviewPanel() ]
                + SVerticalBox::Slot().AutoHeight()[ BuildResultsPanel() ]
			]
		]
	];

	RebuildTargets();
    JobChangeHandle = CubeShot::Service::Jobs().OnChanged.AddSP(this, &SCubeShotPanel::MarkJobsDirty);
    RefreshJobObserver(true);
    if (Targets.IsValidIndex(SelectedTarget)) SelectTarget(SelectedTarget);
	ActiveTimerHandle = RegisterActiveTimer(
		0.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SCubeShotPanel::TickPreview));
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
    CubeShot::Service::Jobs().OnChanged.Remove(JobChangeHandle); JobChangeHandle.Reset();
    for (const auto& WeakWindow : ZoomWindows) if (const auto Window = WeakWindow.Pin()) Window->RequestDestroyWindow();
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
	ViewModel.Reset();
}

TSharedRef<SWidget> SCubeShotPanel::BuildOptionsPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(FMargin(12.0f, 10.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					.IsEnabled_Lambda([this]() { return ViewModel.IsValid() && ViewModel->AreOptionsEnabled(); })
                    + SVerticalBox::Slot().AutoHeight()[ BuildWorkflowPanel() ]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						MakeSectionHeading(LOCTEXT("MeshHeading", "Mesh"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
					[
						SNew(SObjectPropertyEntryBox)
						.AllowedClass(UStaticMesh::StaticClass())
						.ObjectPath(this, &SCubeShotPanel::GetMeshObjectPath)
						.OnObjectChanged(this, &SCubeShotPanel::OnMeshChanged)
						.DisplayUseSelected(false)
						.DisplayBrowse(true)
						.AllowClear(true)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("UseSelectedMesh", "Use Content Browser Selection"))
						.ToolTipText(LOCTEXT("UseSelectedMeshTooltip", "Adds all selected Static Meshes to the ordered target list."))
						.OnClicked(this, &SCubeShotPanel::OnUseSelectedMesh)
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						MakeSectionHeading(LOCTEXT("BackgroundHeading", "Background"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
					[
						BuildBackgroundModeCombo()
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
					[
						SNew(SHorizontalBox)
						.IsEnabled_Lambda([this]()
						{
							return ViewModel->AreOptionsEnabled()
								&& ViewModel->GetOptions().BackgroundMode == ECubeShotBackgroundMode::SolidColor;
						})
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							MakeOptionLabel(LOCTEXT("SolidColorLabel", "Solid Color"))
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SColorBlock)
							.Color_Lambda([this]() { return ViewModel->GetOptions().SolidColor; })
							.Size(FVector2D(64.0f, 20.0f))
							.ShowBackgroundForAlpha(false)
							.OnMouseButtonDown(this, &SCubeShotPanel::OnSolidColorClicked)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 10.0f)
					[
						SNew(SBox)
						.IsEnabled_Lambda([this]() { return ViewModel->IsBackgroundTextureEnabled(); })
						[
							SNew(SObjectPropertyEntryBox)
							.AllowedClass(UTexture2D::StaticClass())
							.ObjectPath(this, &SCubeShotPanel::GetBackgroundTextureObjectPath)
							.OnObjectChanged(this, &SCubeShotPanel::OnBackgroundTextureChanged)
							.DisplayUseSelected(true)
							.DisplayBrowse(true)
							.AllowClear(true)
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						MakeSectionHeading(LOCTEXT("CameraHeading", "Camera"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
					[
						BuildProjectionModeCombo()
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakeOptionLabel(LOCTEXT("DistanceLabel", "Distance (cm)")) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SNumericEntryBox<double>)
							.IsEnabled_Lambda([this]() { return ViewModel->IsCameraDistanceEnabled(); })
							.Value_Lambda([this]() { return TOptional<double>(ViewModel->GetOptions().CameraDistance); })
							.MinValue(0.01).MaxValue(10000000.0).MinSliderValue(1.0).MaxSliderValue(10000.0)
							.AllowSpin(true)
							.OnValueChanged_Lambda([this](const double Value)
							{
								ViewModel->SetCameraDistance(Value);
								SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
							})
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakeOptionLabel(LOCTEXT("FovLabel", "FOV (degrees)")) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SNumericEntryBox<double>)
							.IsEnabled_Lambda([this]() { return ViewModel->IsFieldOfViewEnabled(); })
							.Value_Lambda([this]() { return TOptional<double>(ViewModel->GetOptions().FieldOfView); })
							.MinValue(1.0).MaxValue(170.0).MinSliderValue(10.0).MaxSliderValue(120.0)
							.AllowSpin(true)
							.OnValueChanged_Lambda([this](const double Value)
							{
								ViewModel->SetFieldOfView(Value);
								if (ViewModel->GetOptions().bAutoFit)
								{
									ApplyAutoFit();
								}
								SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
							})
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakeOptionLabel(LOCTEXT("OrthoWidthLabel", "Ortho Width (cm)")) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SNumericEntryBox<double>)
							.IsEnabled_Lambda([this]() { return ViewModel->IsOrthoWidthEnabled(); })
							.Value_Lambda([this]() { return TOptional<double>(ViewModel->GetOptions().OrthoWidth); })
							.MinValue(0.01).MaxValue(10000000.0).MinSliderValue(1.0).MaxSliderValue(10000.0)
							.AllowSpin(true)
							.OnValueChanged_Lambda([this](const double Value)
							{
								ViewModel->SetOrthoWidth(Value);
								SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
							})
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 10.0f)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return ViewModel->GetOptions().bAutoFit ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
						{
							ViewModel->SetAutoFit(State == ECheckBoxState::Checked);
							if (ViewModel->GetOptions().bAutoFit)
							{
								ApplyAutoFit();
							}
							SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
						})
						[
							SNew(STextBlock).Text(LOCTEXT("AutoFitLabel", "Auto Fit all six directions (90% frame fill)"))
						]
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						MakeSectionHeading(LOCTEXT("OutputHeading", "Output"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
					[
						BuildResolutionPresets()
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ MakeOptionLabel(LOCTEXT("CustomResolutionLabel", "Custom size")) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SNumericEntryBox<int32>)
							.Value_Lambda([this]() { return TOptional<int32>(ViewModel->GetOptions().Resolution); })
							.MinValue(CubeShot::MinResolution).MaxValue(CubeShot::MaxResolution)
							.MinSliderValue(CubeShot::MinResolution).MaxSliderValue(CubeShot::MaxResolution)
							.AllowSpin(true)
							.OnValueChanged_Lambda([this](const int32 Value)
							{
								ViewModel->SetResolution(Value);
								SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
							})
						]
					]
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 4.0f)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
			[
				SNew(STextBlock)
				.Text(this, &SCubeShotPanel::GetStatusText)
				.ColorAndOpacity(this, &SCubeShotPanel::GetStatusColor)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)
			[
				SNew(STextBlock)
				.Visibility(this, &SCubeShotPanel::GetProgressVisibility)
				.Text(this, &SCubeShotPanel::GetProgressText)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("GenerateButton", "Start Capture Queue"))
				.ToolTipText(LOCTEXT("GenerateTooltip", "Captures and saves Front, Back, Left, Right, Top, and Bottom textures under /Game/CubeShot."))
				.IsEnabled(this, &SCubeShotPanel::CanStartGeneration)
				.OnClicked(this, &SCubeShotPanel::OnGenerateClicked)
			]
		];
}

TSharedRef<SWidget> SCubeShotPanel::BuildPreviewPanel()
{
	TSharedRef<SGridPanel> Grid = SNew(SGridPanel)
		.FillColumn(0, 1.0f).FillColumn(1, 1.0f).FillColumn(2, 1.0f)
		.FillRow(0, 1.0f).FillRow(1, 1.0f);
	for (int32 Index = 0; Index < DirectionCount; ++Index)
	{
		Grid->AddSlot(Index % 3, Index / 3)
		.Padding(5.0f)
		[
			BuildPreviewCell(Index)
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(7.0f)
		[
			Grid
		];
}

TSharedRef<SWidget> SCubeShotPanel::BuildPreviewCell(const int32 DirectionIndex)
{
	const FCubeShotDirectionInfo& Direction = CubeShot::GetDirectionInfo(static_cast<ECubeShotDirection>(DirectionIndex));
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 1.0f, 0.0f, 4.0f)
			[
                SNew(SButton).Text(FText::FromString(Direction.Label))
                .ToolTipText(LOCTEXT("ZoomPreview", "Open this view at a larger size."))
                .OnClicked_Lambda([this,DirectionIndex]()
                {
                    if (!PreviewTextures[DirectionIndex]) return FReply::Handled();
                    auto HeldTexture = MakeShared<TStrongObjectPtr<UTexture2D>>(PreviewTextures[DirectionIndex].Get());
                    auto Brush = MakeShared<FSlateBrush>(*PreviewBrushes[DirectionIndex]);
                    auto Window = SNew(SWindow).Title(FText::FromString(CubeShot::GetDirectionInfo(static_cast<ECubeShotDirection>(DirectionIndex)).Label)).ClientSize(FVector2D(700,700))
                        [ SNew(SScaleBox).Stretch(EStretch::ScaleToFit)[ SNew(SImage).Image(&Brush.Get()) ] ];
                    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([HeldTexture,Brush](const TSharedRef<SWindow>&) {}));
                    ZoomWindows.Add(Window); FSlateApplication::Get().AddWindow(Window); return FReply::Handled();
                })
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::Both)
				[
					SNew(SBox)
					.WidthOverride(256.0f)
					.HeightOverride(256.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							BuildCheckerboard()
						]
						+ SOverlay::Slot()
						[
                            SAssignNew(PreviewImages[DirectionIndex],SImage)
                            .Image(GetPreviewBrush(DirectionIndex))
						]
						+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Visibility(this, &SCubeShotPanel::GetUpdatingVisibility)
							.Text(LOCTEXT("UpdatingOverlay", "Updating..."))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
							.ColorAndOpacity(FLinearColor::White)
							.ShadowOffset(FVector2D(1.0f, 1.0f))
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SCubeShotPanel::BuildCheckerboard() const
{
	TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel)
		.MinDesiredSlotWidth(8.0f)
		.MinDesiredSlotHeight(8.0f);
	for (int32 Row = 0; Row < 8; ++Row)
	{
		for (int32 Column = 0; Column < 8; ++Column)
		{
			const FLinearColor Color = ((Row + Column) & 1) == 0
				? FLinearColor(0.16f, 0.16f, 0.16f)
				: FLinearColor(0.30f, 0.30f, 0.30f);
			Grid->AddSlot(Column, Row)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(Color)
			];
		}
	}
	return Grid;
}

TSharedRef<SWidget> SCubeShotPanel::BuildBackgroundModeCombo()
{
	auto MakeModeButton = [this](const ECubeShotBackgroundMode Mode, const FText& Label)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([this, Mode]() { return ViewModel->GetOptions().BackgroundMode == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Mode](const ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					ViewModel->SetBackgroundMode(Mode);
					SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
				}
			})
			[
				SNew(STextBlock).Text(Label).Justification(ETextJustify::Center)
			];
	};

	return SNew(SUniformGridPanel).SlotPadding(FMargin(2.0f))
		+ SUniformGridPanel::Slot(0, 0)[ MakeModeButton(ECubeShotBackgroundMode::Transparent, LOCTEXT("TransparentMode", "Transparent")) ]
		+ SUniformGridPanel::Slot(1, 0)[ MakeModeButton(ECubeShotBackgroundMode::SolidColor, LOCTEXT("SolidMode", "Solid")) ]
		+ SUniformGridPanel::Slot(2, 0)[ MakeModeButton(ECubeShotBackgroundMode::Texture, LOCTEXT("TextureMode", "Texture")) ];
}

TSharedRef<SWidget> SCubeShotPanel::BuildProjectionModeCombo()
{
	auto MakeModeButton = [this](const ECubeShotProjectionMode Mode, const FText& Label)
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([this, Mode]() { return ViewModel->GetOptions().ProjectionMode == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Mode](const ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					ViewModel->SetProjectionMode(Mode);
					if (ViewModel->GetOptions().bAutoFit)
					{
						ApplyAutoFit();
					}
					SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
				}
			})
			[
				SNew(STextBlock).Text(Label).Justification(ETextJustify::Center)
			];
	};

	return SNew(SUniformGridPanel).SlotPadding(FMargin(2.0f))
		+ SUniformGridPanel::Slot(0, 0)[ MakeModeButton(ECubeShotProjectionMode::Perspective, LOCTEXT("PerspectiveMode", "Perspective")) ]
		+ SUniformGridPanel::Slot(1, 0)[ MakeModeButton(ECubeShotProjectionMode::Orthographic, LOCTEXT("OrthographicMode", "Orthographic")) ];
}

TSharedRef<SWidget> SCubeShotPanel::BuildResolutionPresets()
{
	TSharedRef<SWrapBox> Presets = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(2.0f, 2.0f));
	for (const int32 Resolution : {256, 512, 1024, 2048, 4096})
	{
		Presets->AddSlot()
		[
			SNew(SButton)
			.Text(FText::AsNumber(Resolution))
			.OnClicked_Lambda([this, Resolution]()
			{
				OnResolutionPreset(Resolution);
				return FReply::Handled();
			})
		];
	}
	return Presets;
}

void SCubeShotPanel::SchedulePreview(const FCubeShotViewModel::EChangeKind ChangeKind)
{
	if (bShutdown || !ViewModel.IsValid())
	{
		return;
	}

    PersistCurrentOptions();
	const bool bHadOutstandingRequest = bPreviewUpdating || ScheduledRevision != ProcessingRevision;
	ScheduledRevision = ViewModel->GetRevision();
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
    if (CurrentTime >= NextHistoryRefresh) { RefreshJobObserver(); NextHistoryRefresh = CurrentTime + 0.25; }
    const bool Busy = CubeShot::Service::Jobs().IsBusy();
    ViewModel->SetGenerating(Busy);
    if (bWasJobBusy && !Busy) { ViewModel->RequestPreviewRecapture(); SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture); }
    bWasJobBusy = Busy;
    FText Policy;
    if (Busy || !CubeShot::CanExecute(Policy)) return EActiveTimerReturnType::Continue;

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

	return CaptureService->Composite(*RawFrame, Options, TargetResolution, PendingPreviewImages[DirectionIndex], OutError);
}

void SCubeShotPanel::ReplacePreviewTexture(const int32 DirectionIndex, const FCubeShotImage& Image)
{
	if (!Image.IsValid() || !PreviewTextures.IsValidIndex(DirectionIndex) || !PreviewBrushes.IsValidIndex(DirectionIndex))
	{
		return;
	}

    const TSharedPtr<FSlateBrush> PreviousBrush=PreviewBrushes[DirectionIndex];
    const TStrongObjectPtr<UTexture2D> PreviousTexture(PreviewTextures[DirectionIndex].Get());
    VisiblePreviewPixels[DirectionIndex]=0;
    for(const FColor& Pixel:Image.Pixels)VisiblePreviewPixels[DirectionIndex]+=Pixel.A>0&&FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>0;

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
	PreviewBrushes[DirectionIndex]=MakeShared<FSlateBrush>();
	PreviewBrushes[DirectionIndex]->SetResourceObject(Texture);
	PreviewBrushes[DirectionIndex]->SetImageSize(FVector2D(Image.Width, Image.Height));
	PreviewBrushes[DirectionIndex]->DrawAs = ESlateBrushDrawType::Image;
    if(PreviewImages[DirectionIndex])
    {
        PreviewImages[DirectionIndex]->SetImage(PreviewBrushes[DirectionIndex].Get());
        PreviewImages[DirectionIndex]->Invalidate(EInvalidateWidgetReason::Layout|EInvalidateWidgetReason::Paint);
    }
}

void SCubeShotPanel::ClearPreviewResources()
{
    for(const auto& Image:PreviewImages)if(Image){Image->SetImage(nullptr);Image->Invalidate(EInvalidateWidgetReason::Paint);}
    VisiblePreviewPixels.Init(0,DirectionCount);
	for (const TSharedPtr<FSlateBrush>& Brush : PreviewBrushes)
	{
		if (Brush.IsValid())
		{
			Brush->SetResourceObject(nullptr);
			Brush->DrawAs = ESlateBrushDrawType::NoDrawType;
		}
	}
	PreviewBrushes.Reset();
	PreviewTextures.Reset();
}

void SCubeShotPanel::ApplyAutoFit()
{
	if (!ViewModel.IsValid() || !ViewModel->GetOptions().bAutoFit || ViewModel->GetOptions().StaticMesh == nullptr)
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
    if(AssetData.IsValid())
    {const FString Path=AssetData.GetObjectPathString();AddTarget(Path);SelectTargetPath(Path);}
    else{SelectedTarget=INDEX_NONE;ViewModel->SetStaticMesh(nullptr);SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);}
}

void SCubeShotPanel::OnBackgroundTextureChanged(const FAssetData& AssetData)
{
	ViewModel->SetBackgroundTexture(Cast<UTexture2D>(AssetData.GetAsset()));
	SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
}

FReply SCubeShotPanel::OnUseSelectedMesh()
{
    FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Assets; Browser.Get().GetSelectedAssets(Assets);
    FString LastPath;
    for (const FAssetData& Asset : Assets) if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName()){LastPath=Asset.GetObjectPathString();AddTarget(LastPath);}
    if (!LastPath.IsEmpty()) SelectTargetPath(LastPath);
    else OperationStatus = LOCTEXT("NoSelectedMesh", "No Static Mesh is selected in the Content Browser.");
    return FReply::Handled();
}

FReply SCubeShotPanel::OnSolidColorClicked(const FGeometry& Geometry, const FPointerEvent& PointerEvent)
{
	FColorPickerArgs Args;
	Args.InitialColor = ViewModel->GetOptions().SolidColor;
	Args.bUseAlpha = false;
	Args.bOnlyRefreshOnMouseUp = false;
	Args.ParentWidget = SharedThis(this);
	Args.OnColorCommitted = FOnLinearColorValueChanged::CreateSP(this, &SCubeShotPanel::OnSolidColorCommitted);
	OpenColorPicker(Args);
	return FReply::Handled();
}

void SCubeShotPanel::OnSolidColorCommitted(const FLinearColor NewColor)
{
	if (!ViewModel.IsValid() || !ViewModel->AreOptionsEnabled())
	{
		return;
	}
	ViewModel->SetSolidColor(NewColor);
	SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
}

void SCubeShotPanel::OnResolutionPreset(const int32 Resolution)
{
	ViewModel->SetResolution(Resolution);
	SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
}

FReply SCubeShotPanel::OnGenerateClicked()
{
    if (Targets.IsEmpty() && ViewModel->GetOptions().StaticMesh) AddTarget(ViewModel->GetOptions().StaticMesh->GetPathName());
    TArray<FString> Paths; for (const auto& Target : Targets) Paths.Add(Target.Path);
    return StartBatch(Paths);
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
    FText Policy;
    if (!CubeShot::CanExecute(Policy)) return Policy;
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
        return FText::Format(LOCTEXT("StatusWithWarnings", "{0}\n{1}"),OperationStatus,ViewModel->GetValidationWarning());
	return OperationStatus;
}

FSlateColor SCubeShotPanel::GetStatusColor() const
{
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
	return CubeShot::CanExecute(Policy) && ViewModel.IsValid()
		&& ViewModel->CanGenerate()
		&& PreviewError.IsEmpty()
		&& !bPreviewUpdating
        && ScheduledRevision == ProcessingRevision
        && !CubeShot::Service::Jobs().IsBusy();
}

EVisibility SCubeShotPanel::GetUpdatingVisibility() const
{
	return bPreviewUpdating ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCubeShotPanel::GetProgressVisibility() const
{
	return ViewModel.IsValid() && ViewModel->IsGenerating() ? EVisibility::Visible : EVisibility::Collapsed;
}


TSharedPtr<FJsonObject> SCubeShotPanel::CurrentOptionsJson()const
{
    auto Json=CubeShot::Service::SerializeOptions(ViewModel->GetOptionsCopy());Json->SetBoolField(TEXT("export_png"),bExportPng);Json->SetBoolField(TEXT("trim_transparent"),bTrimPng);Json->SetNumberField(TEXT("padding_pixels"),PngPadding);Json->SetNumberField(TEXT("quarter_turns"),PngTurns);
    TArray<TSharedPtr<FJsonValue>> Directions;for(const FString& Direction:PngDirections)Directions.Add(MakeShared<FJsonValueString>(Direction));Json->SetArrayField(TEXT("png_directions"),Directions);return Json;
}
void SCubeShotPanel::PersistCurrentOptions()
{
    if(bLoadingTarget||!Targets.IsValidIndex(SelectedTarget)||!ViewModel)return;
    auto Json=CurrentOptionsJson();Targets[SelectedTarget].Override=Json;Targets[SelectedTarget].bCustom=true;
}
void SCubeShotPanel::ApplyOptionsToSelected()
{
    const auto Json=CurrentOptionsJson();for(auto& Target:Targets)if(Target.bSelected){Target.Override=UnrealToolsRuntime::Clone(Json);Target.bCustom=true;}RebuildTargets();
}
void SCubeShotPanel::RemoveSelectedTargets()
{
    const FString PreviewPath=GetMeshObjectPath();Targets.RemoveAll([](const FTargetState& Target){return Target.bSelected;});
    SelectedTarget=Targets.IndexOfByPredicate([&](const FTargetState& Target){return Target.Path==PreviewPath;});
    if(SelectedTarget==INDEX_NONE&&!Targets.IsEmpty())SelectedTarget=0;
    if(SelectedTarget!=INDEX_NONE)SelectTarget(SelectedTarget);else{ViewModel->SetStaticMesh(nullptr);SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);RebuildTargets();}
}

void SCubeShotPanel::AddTarget(const FString& Path)
{
    if (Targets.ContainsByPredicate([&](const FTargetState& Target) { return Target.Path == Path; })) return;
    FString Error;auto Options=CubeShot::Service::Profiles().Resolve(SelectedProfile,CubeShot::Service::DefaultOptions(),Error);
    Targets.Add({Path,Options,true,false}); SelectedTarget = Targets.Num()-1; RebuildTargets();
}

void SCubeShotPanel::SelectTarget(int32 Index)
{
    if (!Targets.IsValidIndex(Index)) return;
    SelectedTarget = Index;
    bLoadingTarget=true;
    FString ProfileError;
    TSharedPtr<FJsonObject> Json = Targets[Index].Override.IsValid() ? Targets[Index].Override : CubeShot::Service::Profiles().Resolve(SelectedProfile, CubeShot::Service::DefaultOptions(), ProfileError);
    if (!Json) { OperationStatus = FText::FromString(ProfileError);bLoadingTarget=false; return; }
    FCubeShotOptions Options; FText Error;
    if (!CubeShot::Service::LoadOptions(Targets[Index].Path, Json, Options, Error)) { OperationStatus = Error;bLoadingTarget=false; return; }
    ViewModel->SetStaticMesh(Options.StaticMesh); ViewModel->SetBackgroundTexture(Options.BackgroundTexture); ViewModel->SetBackgroundMode(Options.BackgroundMode);
    ViewModel->SetSolidColor(Options.SolidColor); ViewModel->SetProjectionMode(Options.ProjectionMode); ViewModel->SetAutoFit(Options.bAutoFit);
    ViewModel->SetCameraDistance(Options.CameraDistance); ViewModel->SetFieldOfView(Options.FieldOfView); ViewModel->SetOrthoWidth(Options.OrthoWidth); ViewModel->SetResolution(Options.Resolution);
    bExportPng = Json->GetBoolField(TEXT("export_png")); bTrimPng = Json->GetBoolField(TEXT("trim_transparent"));
    PngPadding = Json->GetIntegerField(TEXT("padding_pixels")); PngTurns = Json->GetIntegerField(TEXT("quarter_turns")); PngDirections.Reset();
    for (const auto& Direction : Json->GetArrayField(TEXT("png_directions"))) PngDirections.Add(Direction->AsString());
    ApplyAutoFit(); SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);bLoadingTarget=false; RebuildTargets();
}

void SCubeShotPanel::SelectTargetPath(const FString& Path)
{
    const int32 Index=Targets.IndexOfByPredicate([&](const FTargetState& Target){return Target.Path==Path;});
    if(Index!=INDEX_NONE)SelectTarget(Index);
}

void SCubeShotPanel::MoveTarget(int32 Index,int32 Delta)
{
    const int32 Destination=Index+Delta;if(!Targets.IsValidIndex(Index)||!Targets.IsValidIndex(Destination))return;
    FString PreviewPath=GetMeshObjectPath();if(PreviewPath.IsEmpty()&&Targets.IsValidIndex(SelectedTarget))PreviewPath=Targets[SelectedTarget].Path;
    Targets.Swap(Index,Destination);
    SelectedTarget=Targets.IndexOfByPredicate([&](const FTargetState& Target){return Target.Path==PreviewPath;});
    RebuildTargets();
}

void SCubeShotPanel::ApplyProfile(const FString& Id)
{
    SelectedProfile=CubeShot::Service::Profiles().Get(Id)?Id:FString(TEXT("editor_default"));PersistedProfile=SelectedProfile;
    if(Targets.IsValidIndex(SelectedTarget)){FString Error;Targets[SelectedTarget].Override=CubeShot::Service::Profiles().Resolve(SelectedProfile,CubeShot::Service::DefaultOptions(),Error);Targets[SelectedTarget].bCustom=false;SelectTarget(SelectedTarget);}
}

void SCubeShotPanel::RebuildTargets()
{
    if (!TargetRows) return;
    TargetRows->ClearChildren();
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        TargetRows->AddSlot().AutoHeight().Padding(0,2)[ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).IsChecked_Lambda([Index](){return Targets[Index].bSelected?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([Index](ECheckBoxState State){Targets[Index].bSelected=State==ECheckBoxState::Checked;})]
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SButton).Text(FText::FromString(FPaths::GetBaseFilename(Targets[Index].Path) + (Targets[Index].bCustom ? TEXT(" *") : TEXT(""))))
                .ToolTipText(FText::FromString(Targets[Index].Path)).OnClicked_Lambda([this,Index]() { SelectTarget(Index); return FReply::Handled(); }) ]
            + SHorizontalBox::Slot().AutoWidth()[ SNew(SButton).Text(FText::FromString(TEXT("↑"))).IsEnabled(Index>0).OnClicked_Lambda([this,Index]() { MoveTarget(Index,-1);return FReply::Handled(); }) ]
            + SHorizontalBox::Slot().AutoWidth()[ SNew(SButton).Text(FText::FromString(TEXT("↓"))).IsEnabled(Index<Targets.Num()-1).OnClicked_Lambda([this,Index]() { MoveTarget(Index,1);return FReply::Handled(); }) ]
            + SHorizontalBox::Slot().AutoWidth()[ SNew(SButton).Text(FText::FromString(TEXT("×"))).OnClicked_Lambda([this,Index]() { Targets.RemoveAt(Index); SelectedTarget=Targets.IsEmpty()?INDEX_NONE:FMath::Min(Index,Targets.Num()-1); if(SelectedTarget!=INDEX_NONE)SelectTarget(SelectedTarget);else{ViewModel->SetStaticMesh(nullptr);SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);RebuildTargets();} return FReply::Handled(); }) ] ];
    }
}

TSharedRef<SWidget> SCubeShotPanel::BuildWorkflowPanel()
{
    auto CurrentJson = [this]() { return CurrentOptionsJson(); };
    SAssignNew(ProfileRows,SVerticalBox); RebuildProfiles();
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[ MakeSectionHeading(LOCTEXT("TargetWorkflow","Targets · Profiles · Output plan")) ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SBox).MaxDesiredHeight(160)[ SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(TargetRows,SVerticalBox)] ] ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("DropHelp","Check rows for bulk edits/removal. Click a name to preview/edit that row. Capture runs the ordered list." )).AutoWrapText(true) ]
        + SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Text(LOCTEXT("SelectRows","Select All")).OnClicked_Lambda([](){for(auto& Row:Targets)Row.bSelected=true;return FReply::Handled();})]
            + SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Text(LOCTEXT("ClearRows","Clear Selection")).OnClicked_Lambda([](){for(auto& Row:Targets)Row.bSelected=false;return FReply::Handled();})]
            + SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Text(LOCTEXT("RemoveRows","Remove Selected")).OnClicked_Lambda([this](){RemoveSelectedTargets();return FReply::Handled();})]]
        + SVerticalBox::Slot().AutoHeight()[ ProfileRows.ToSharedRef() ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SEditableTextBox).Text_Lambda([]() { return FText::FromString(ProfileName); }).HintText(LOCTEXT("ProfileName","User profile name")).OnTextChanged_Lambda([](const FText& Text){ProfileName=Text.ToString();}) ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SButton).Text(LOCTEXT("SaveProfile","Save Profile")).OnClicked_Lambda([this,CurrentJson]()
            {
                auto Profile=MakeShared<FJsonObject>(); Profile->SetNumberField(TEXT("schema_version"),1); Profile->SetStringField(TEXT("provider_id"),TEXT("cubeshot")); Profile->SetStringField(TEXT("id"),(SelectedProfile==TEXT("editor_default")||SelectedProfile==TEXT("item_icon"))?TEXT("user_")+FGuid::NewGuid().ToString(EGuidFormats::Digits):SelectedProfile); Profile->SetStringField(TEXT("display_name"),ProfileName); Profile->SetObjectField(TEXT("options"),CurrentJson());
                auto Args=MakeShared<FJsonObject>(); Args->SetStringField(TEXT("action"),TEXT("save")); Args->SetObjectField(TEXT("profile"),Profile); if(CubeShot::Service::Profiles().Get(Profile->GetStringField(TEXT("id")))){if(FMessageDialog::Open(EAppMsgType::YesNo,LOCTEXT("ReplaceProfile","Replace this existing user profile?"))!=EAppReturnType::Yes)return FReply::Handled();Args->SetBoolField(TEXT("overwrite"),true);} auto Result=CubeShot::Service::HandleProfiles(Args);if(Result->GetBoolField(TEXT("success")))SelectedProfile=Profile->GetStringField(TEXT("id"));PersistedProfile=SelectedProfile;RebuildProfiles();
                LastResultStatus=FText::FromString(Result->GetBoolField(TEXT("success"))?TEXT("Profile saved."):Result->GetStringField(TEXT("error"))); return FReply::Handled();
            }) ]
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SButton).Text(LOCTEXT("DeleteProfile","Delete Profile")).OnClicked_Lambda([this]() { auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("action"),TEXT("delete"));Args->SetStringField(TEXT("id"),SelectedProfile);auto Result=CubeShot::Service::HandleProfiles(Args);if(Result->GetBoolField(TEXT("success"))){SelectedProfile=TEXT("editor_default");PersistedProfile=SelectedProfile;}RebuildProfiles();LastResultStatus=FText::FromString(Result->GetBoolField(TEXT("success"))?TEXT("Profile deleted."):Result->GetStringField(TEXT("error"))); return FReply::Handled(); }) ] ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SButton).Text(LOCTEXT("ImportProfile","Import JSON")).OnClicked_Lambda([this](){return TransferProfile(true);}) ]
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SButton).Text(LOCTEXT("ExportProfile","Export JSON")).OnClicked_Lambda([this](){return TransferProfile(false);}) ] ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SButton).Text(LOCTEXT("SaveTargetOptions","Apply current options to checked rows")).IsEnabled_Lambda([](){return Targets.ContainsByPredicate([](const FTargetState& Row){return Row.bSelected;});}).OnClicked_Lambda([this](){ApplyOptionsToSelected();return FReply::Handled();}) ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SCheckBox).IsChecked_Lambda([this](){return bExportPng?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){bExportPng=State==ECheckBoxState::Checked;PersistCurrentOptions();})[SNew(STextBlock).Text(LOCTEXT("ExportPNG","Publish selected PNG views"))] ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SWrapBox).UseAllottedSize(true)
            + SWrapBox::Slot()[ SNew(SCheckBox).IsChecked_Lambda([this](){return PngDirections.Contains(TEXT("Front"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){if(State==ECheckBoxState::Checked)PngDirections.AddUnique(TEXT("Front"));else if(PngDirections.Num()>1)PngDirections.Remove(TEXT("Front"));PersistCurrentOptions();})[SNew(STextBlock).Text(FText::FromString(TEXT("Front")))] ]
            + SWrapBox::Slot()[ SNew(SCheckBox).IsChecked_Lambda([this](){return PngDirections.Contains(TEXT("Back"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){if(State==ECheckBoxState::Checked)PngDirections.AddUnique(TEXT("Back"));else if(PngDirections.Num()>1)PngDirections.Remove(TEXT("Back"));PersistCurrentOptions();})[SNew(STextBlock).Text(FText::FromString(TEXT("Back")))] ]
            + SWrapBox::Slot()[ SNew(SCheckBox).IsChecked_Lambda([this](){return PngDirections.Contains(TEXT("Left"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){if(State==ECheckBoxState::Checked)PngDirections.AddUnique(TEXT("Left"));else if(PngDirections.Num()>1)PngDirections.Remove(TEXT("Left"));PersistCurrentOptions();})[SNew(STextBlock).Text(FText::FromString(TEXT("Left")))] ]
            + SWrapBox::Slot()[ SNew(SCheckBox).IsChecked_Lambda([this](){return PngDirections.Contains(TEXT("Right"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){if(State==ECheckBoxState::Checked)PngDirections.AddUnique(TEXT("Right"));else if(PngDirections.Num()>1)PngDirections.Remove(TEXT("Right"));PersistCurrentOptions();})[SNew(STextBlock).Text(FText::FromString(TEXT("Right")))] ]
            + SWrapBox::Slot()[ SNew(SCheckBox).IsChecked_Lambda([this](){return PngDirections.Contains(TEXT("Top"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){if(State==ECheckBoxState::Checked)PngDirections.AddUnique(TEXT("Top"));else if(PngDirections.Num()>1)PngDirections.Remove(TEXT("Top"));PersistCurrentOptions();})[SNew(STextBlock).Text(FText::FromString(TEXT("Top")))] ]
            + SWrapBox::Slot()[ SNew(SCheckBox).IsChecked_Lambda([this](){return PngDirections.Contains(TEXT("Bottom"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){if(State==ECheckBoxState::Checked)PngDirections.AddUnique(TEXT("Bottom"));else if(PngDirections.Num()>1)PngDirections.Remove(TEXT("Bottom"));PersistCurrentOptions();})[SNew(STextBlock).Text(FText::FromString(TEXT("Bottom")))] ]
        ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SCheckBox).IsChecked_Lambda([this](){return bTrimPng?ECheckBoxState::Checked:ECheckBoxState::Unchecked;}).OnCheckStateChanged_Lambda([this](ECheckBoxState State){bTrimPng=State==ECheckBoxState::Checked;PersistCurrentOptions();})[SNew(STextBlock).Text(LOCTEXT("TrimPNG","Trim transparent margins"))] ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SNumericEntryBox<int32>).Value_Lambda([this](){return TOptional<int32>(PngPadding);}).MinValue(0).MaxValue(64).OnValueChanged_Lambda([this](int32 Value){PngPadding=Value;PersistCurrentOptions();}).Label()[SNew(STextBlock).Text(LOCTEXT("Padding","Padding"))] ]
            + SHorizontalBox::Slot().FillWidth(1)[ SNew(SNumericEntryBox<int32>).Value_Lambda([this](){return TOptional<int32>(PngTurns);}).MinValue(0).MaxValue(3).OnValueChanged_Lambda([this](int32 Value){PngTurns=Value;PersistCurrentOptions();}).Label()[SNew(STextBlock).Text(LOCTEXT("Turns","90° turns"))] ] ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(SButton).Text(LOCTEXT("PreflightAll","Inspect all outputs")).OnClicked(this,&SCubeShotPanel::PreflightTargets) ]
        + SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text_Lambda([this](){return OutputPlan;}).AutoWrapText(true) ];
}

TSharedPtr<FJsonObject> SCubeShotPanel::BuildBatchRequest(const TArray<FString>& Paths,
    TSharedPtr<FJsonObject> RetryOverrides, FText& Error) const
{
    FString Message;auto Common=CubeShot::Service::Profiles().Resolve(SelectedProfile,CubeShot::Service::DefaultOptions(),Message);
    if(!Common){Error=FText::FromString(Message);return nullptr;}
    auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("request_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
    Args->SetStringField(TEXT("profile_id"),SelectedProfile);Args->SetObjectField(TEXT("options"),Common);
    TArray<TSharedPtr<FJsonValue>> MeshPaths;auto Overrides=MakeShared<FJsonObject>();
    for(const FString& Path:Paths)
    {
        MeshPaths.Add(MakeShared<FJsonValueString>(Path));
        const auto* Target=Targets.FindByPredicate([&](const FTargetState& Row){return Row.Path==Path;});
        auto RetryOption=UnrealToolsRuntime::GetObject(RetryOverrides,Path);
        Overrides->SetObjectField(Path,UnrealToolsRuntime::Clone(RetryOption?RetryOption:Target&&Target->Override?Target->Override:Common));
    }
    Args->SetArrayField(TEXT("mesh_paths"),MeshPaths);Args->SetObjectField(TEXT("overrides"),Overrides);Args->SetBoolField(TEXT("overwrite"),false);
    return Args;
}

void SCubeShotPanel::ShowOutputPlan(const TSharedPtr<FJsonObject>& Plan)
{
    using namespace UnrealToolsRuntime;FString Text;
    for(const auto& Value:GetArray(Plan,TEXT("items")))
    {
        const auto Item=Value->AsObject();Text+=GetString(Item,TEXT("mesh_path"))+TEXT("\n");
        if(!GetBool(Item,TEXT("can_proceed")))Text+=TEXT("ERROR: ")+GetString(Item,TEXT("error"))+TEXT("\n");
        else
        {
            Text+=GetBool(Item,TEXT("requires_checkout"))?TEXT("Checkout required\n"):GetBool(Item,TEXT("requires_overwrite"))?TEXT("Overwrite required\n"):TEXT("Ready\n");
            for(const TCHAR* Key:{TEXT("outputs"),TEXT("png_outputs"),TEXT("warnings")})for(const auto& Entry:GetArray(Item,Key))Text+=Entry->AsString()+TEXT("\n");
        }
    }
    for(const auto& Entry:GetArray(Plan,TEXT("errors")))Text+=TEXT("ERROR: ")+Entry->AsString()+TEXT("\n");
    OutputPlan=FText::FromString(Text);
}

FReply SCubeShotPanel::PreflightTargets()
{
    TArray<FString> Paths;for(const auto& Target:Targets)Paths.Add(Target.Path);
    FText Error;auto Request=BuildBatchRequest(Paths,nullptr,Error);
    if(!Request){OutputPlan=Error;return FReply::Handled();}
    ShowOutputPlan(CubeShot::Service::InspectBatch(Request,true));return FReply::Handled();
}

FReply SCubeShotPanel::StartBatch(const TArray<FString>& Paths, TSharedPtr<FJsonObject> RetryOverrides)
{
    if(Paths.IsEmpty())return FReply::Handled();
    if(!CubeShot::Service::Profiles().Get(SelectedProfile)){SelectedProfile=TEXT("editor_default");PersistedProfile=SelectedProfile;RebuildProfiles();}
    FText Error;auto Args=BuildBatchRequest(Paths,RetryOverrides,Error);
    if(!Args){LastResultStatus=Error;return FReply::Handled();}
    auto Plan=CubeShot::Service::InspectBatch(Args,true);ShowOutputPlan(Plan);
    if(!UnrealToolsRuntime::GetBool(Plan,TEXT("can_proceed")))
    {LastResultStatus=FText::FromString(TEXT("Batch was not submitted. Resolve all preflight errors:\n")+UnrealToolsRuntime::GetString(Plan,TEXT("error")));return FReply::Handled();}
    bool Overwrite=false;
    if(Plan->GetBoolField(TEXT("requires_overwrite")))
    {
#if WITH_DEV_AUTOMATION_TESTS
        if(OverwriteDecisionForAutomation)Overwrite=OverwriteDecisionForAutomation();
        else
#endif
        Overwrite=FMessageDialog::Open(EAppMsgType::YesNo,LOCTEXT("QueueOverwrite","The inspected outputs include existing textures or PNGs. Replace these outputs? Existing Texture2D references are preserved."))==EAppReturnType::Yes;
        if(!Overwrite)return FReply::Handled();
    }
    Args->SetBoolField(TEXT("overwrite"),Overwrite);
    TFunction<bool(const TArray<UPackage*>&,FText&)> CheckoutDecision;
#if WITH_DEV_AUTOMATION_TESTS
    CheckoutDecision=CheckoutDecisionForAutomation;
#endif
    if(!CubeShot::Service::CheckoutForSubmission(Args,Error,MoveTemp(CheckoutDecision))){LastResultStatus=Error;return FReply::Handled();}
    auto StrictPlan=CubeShot::Service::InspectBatch(Args);ShowOutputPlan(StrictPlan);
    if(!UnrealToolsRuntime::GetBool(StrictPlan,TEXT("can_proceed")) ||
        (!Overwrite&&UnrealToolsRuntime::GetBool(StrictPlan,TEXT("requires_overwrite"))))
    {LastResultStatus=FText::FromString(TEXT("Batch was not submitted. Outputs changed or remain unwritable; inspect again.\n")+UnrealToolsRuntime::GetString(StrictPlan,TEXT("error")));return FReply::Handled();}
    auto Result=CubeShot::Service::Submit(Args);
    if(Result->GetBoolField(TEXT("success"))){LatestJobId=Result->GetStringField(TEXT("job_id"));bPreviewUpdating=false;LastResultStatus=LOCTEXT("Queued","Capture queued. The task continues when this tab closes.");MarkJobsDirty();RefreshJobObserver(true);}
    else LastResultStatus=FText::FromString(Result->GetStringField(TEXT("error")));
    return FReply::Handled();
}

FText SCubeShotPanel::ResultsText() const
{
    const FString Status=LastResultStatus.ToString();
    if(bResultsDirty||CachedStatusText!=Status)
    {CachedStatusText=Status;CachedResultsText=FText::FromString(Status+CachedResultsBody);bResultsDirty=false;}
    return CachedResultsText;
}

void SCubeShotPanel::MarkJobsDirty() { bJobsDirty=true; }

void SCubeShotPanel::RefreshJobObserver(bool bForce)
{
    using namespace UnrealToolsRuntime;
    const uint64 Revision=CubeShot::Service::Jobs().GetRevision();
    if(!bForce&&!bJobsDirty&&Revision==ObservedJobRevision&&CachedSelectedJobId==LatestJobId)return;
    if(!bForce&&Revision==ObservedJobRevision&&CachedSelectedJobId==LatestJobId){bJobsDirty=false;return;}
#if WITH_DEV_AUTOMATION_TESTS
    ++HistorySnapshotReads;
#endif
    ObservedJobRevision=Revision;bJobsDirty=false;
    JobSummaries=bDiagnosticHistory?CubeShot::Service::Jobs().GetDiagnosticSummaries(HistoryOffset,50)
        :CubeShot::Service::Jobs().GetJobSummaries(HistoryOffset,50,bArchiveHistory);
    FString Signature;
    for(const auto& Job:JobSummaries)Signature+=GetString(Job,TEXT("job_id"))+GetString(Job,TEXT("state"))+GetString(Job,TEXT("created_at"))+GetString(Job,TEXT("record_kind"));
    if(bForce||Signature!=HistoryRowsSignature){HistoryRowsSignature=Signature;RebuildJobs();}
    if(LatestJobId.IsEmpty()&&!JobSummaries.IsEmpty())LatestJobId=GetString(JobSummaries[0],TEXT("job_id"));
    CachedSelectedJobId=LatestJobId;CachedSelectedJob.Reset();CachedResultsBody.Reset();
    if(!LatestJobId.IsEmpty())
    {
        auto Selector=MakeShared<FJsonObject>();Selector->SetStringField(TEXT("job_id"),LatestJobId);
        CachedSelectedJob=CubeShot::Service::Jobs().Status(Selector);
        if(!GetBool(CachedSelectedJob,TEXT("success"))&&!JobSummaries.IsEmpty())
        {LatestJobId=GetString(JobSummaries[0],TEXT("job_id"));CachedSelectedJobId=LatestJobId;Selector->SetStringField(TEXT("job_id"),LatestJobId);CachedSelectedJob=CubeShot::Service::Jobs().Status(Selector);}
    }
    const auto Job=CachedSelectedJob;
    if(Job&&GetBool(Job,TEXT("success")))
    {
        if(GetString(Job,TEXT("record_kind"))==TEXT("diagnostic"))
        {
            CachedResultsBody+=TEXT("\nDiagnostic ID: ")+GetString(Job,TEXT("job_id"))+TEXT("\nPhase: ")+GetString(Job,TEXT("phase"))+TEXT("\n");
            CachedResultsBody+=GetBool(Job,TEXT("recovery_required"))
                ?TEXT("Metadata review required. Review the source metadata and preserved raw evidence before acknowledging recovery.\n")
                :TEXT("Metadata review acknowledged. Preserved raw evidence remains available.\n");
            if(!GetString(Job,TEXT("source_path")).IsEmpty())CachedResultsBody+=TEXT("Source metadata: ")+GetString(Job,TEXT("source_path"))+TEXT("\n");
            if(!GetString(Job,TEXT("raw_path")).IsEmpty())CachedResultsBody+=TEXT("Preserved raw evidence: ")+GetString(Job,TEXT("raw_path"))+TEXT("\n");
            if(!GetString(Job,TEXT("error")).IsEmpty())CachedResultsBody+=TEXT("Error: ")+GetString(Job,TEXT("error"))+TEXT("\n");
        }
        else
        {
        const auto Progress=GetObject(Job,TEXT("progress"));
        CachedResultsBody+=FString::Printf(TEXT("\n%s · %s\nCompleted %.0f / %.0f\n"),*GetString(Job,TEXT("state")),*GetString(Job,TEXT("stage")),GetNumber(Progress,TEXT("completed")),GetNumber(Progress,TEXT("total")));
        const FString State=GetString(Job,TEXT("state"));
        if(GetBool(Job,TEXT("cancel_requested"))&&(State==TEXT("queued")||State==TEXT("running")))
            CachedResultsBody+=TEXT("Cancellation requested; current capture/save must reach a safe boundary.\n");
        if(GetBool(Job,TEXT("recovery_required")))CachedResultsBody+=TEXT("Recovery review required. Resolve dirty packages before retrying these outputs.\n");
        for(const TCHAR* Key:{TEXT("source_path"),TEXT("reason"),TEXT("error")})if(!GetString(Job,Key).IsEmpty())CachedResultsBody+=FString(Key)+TEXT(": ")+GetString(Job,Key)+TEXT("\n");
        for(const auto& Value:GetArray(Job,TEXT("items")))
        {
            const auto Item=Value->AsObject();CachedResultsBody+=GetString(Item,TEXT("mesh_path"))+TEXT(" · ")+GetString(Item,TEXT("state"))+TEXT("\n");
            CachedResultsBody+=GetBool(Item,TEXT("overwrite"))?TEXT("Overwrite allowed: yes\n"):TEXT("Overwrite allowed: no\n");
            auto Result=GetObject(Item,TEXT("result"));
            if(Result&&Result->HasField(TEXT("staged_faces")))CachedResultsBody+=FString::Printf(TEXT("Staged directions: %.0f / 6\n"),GetNumber(Result,TEXT("staged_faces")));
            for(const TCHAR* Key:{TEXT("saved"),TEXT("png_paths"),TEXT("failed"),TEXT("dirty_packages"),TEXT("errors"),TEXT("warnings")})
                for(const auto& Entry:GetArray(Result,Key))CachedResultsBody+=FString(Key)+TEXT(": ")+Entry->AsString()+TEXT("\n");
            if(!GetString(Result,TEXT("error")).IsEmpty())CachedResultsBody+=TEXT("Error: ")+GetString(Result,TEXT("error"))+TEXT("\n");
            if(!GetString(Result,TEXT("recovery_staging")).IsEmpty())CachedResultsBody+=TEXT("Staging: ")+GetString(Result,TEXT("recovery_staging"))+TEXT("\n");
            if(!GetString(Item,TEXT("reason")).IsEmpty())CachedResultsBody+=TEXT("Reason: ")+GetString(Item,TEXT("reason"))+TEXT("\n");
        }
        }
    }
    else if(Job)CachedResultsBody=TEXT("\n")+GetString(Job,TEXT("error"));
    bResultsDirty=true;
}

void SCubeShotPanel::RebuildJobs()
{
    if(!JobRows)return;JobRows->ClearChildren();
    for(const auto& Job:JobSummaries)
    {
        const FString Id=UnrealToolsRuntime::GetString(Job,TEXT("job_id"));
        const FString Label=UnrealToolsRuntime::GetString(Job,TEXT("created_at"))+TEXT(" · ")+UnrealToolsRuntime::GetString(Job,TEXT("state"))+TEXT(" · ")+UnrealToolsRuntime::GetString(Job,TEXT("record_kind"));
        JobRows->AddSlot().AutoHeight()[SNew(SButton).Text(FText::FromString(Label)).ToolTipText(FText::FromString(Id)).OnClicked_Lambda([this,Id](){LatestJobId=Id;RefreshJobObserver(true);return FReply::Handled();})];
    }
}

void SCubeShotPanel::RebuildProfiles()
{
    if(!ProfileRows)return;ProfileRows->ClearChildren();
    for(const auto& Profile:CubeShot::Service::Profiles().GetAll())
    {
        const FString Id=Profile->GetStringField(TEXT("id"));const FString Name=Profile->GetStringField(TEXT("display_name"));
        ProfileRows->AddSlot().AutoHeight()[SNew(SButton).Text(FText::FromString(Name)).OnClicked_Lambda([this,Id](){ApplyProfile(Id);return FReply::Handled();})];
    }
}

TSharedRef<SWidget> SCubeShotPanel::BuildResultsPanel()
{
    return SNew(SBorder).Padding(8)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[MakeSectionHeading(LOCTEXT("Results","Progress · Results · Recovery"))]
        + SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("RecentHistory","Recent")).OnClicked_Lambda([this](){bArchiveHistory=false;bDiagnosticHistory=false;HistoryOffset=0;LatestJobId.Reset();RefreshJobObserver(true);return FReply::Handled();})]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("ArchiveHistory","Archive")).OnClicked_Lambda([this](){bArchiveHistory=true;bDiagnosticHistory=false;HistoryOffset=0;LatestJobId.Reset();RefreshJobObserver(true);return FReply::Handled();})]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("DiagnosticHistory","Diagnostics")).OnClicked_Lambda([this](){bDiagnosticHistory=true;HistoryOffset=0;LatestJobId.Reset();RefreshJobObserver(true);return FReply::Handled();})]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("←"))).IsEnabled_Lambda([this](){return HistoryOffset>0;}).OnClicked_Lambda([this](){HistoryOffset=FMath::Max(0,HistoryOffset-50);LatestJobId.Reset();RefreshJobObserver(true);return FReply::Handled();})]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("→"))).IsEnabled_Lambda([this](){return JobSummaries.Num()==50;}).OnClicked_Lambda([this](){HistoryOffset+=50;LatestJobId.Reset();RefreshJobObserver(true);return FReply::Handled();})]
            + SHorizontalBox::Slot().AutoWidth()[SNew(STextBlock).Text_Lambda([this](){return FText::FromString(FString::Printf(TEXT("  Page %d · 50 per page"),HistoryOffset/50+1));})]]
        + SVerticalBox::Slot().AutoHeight()[SNew(SBox).MaxDesiredHeight(64)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(JobRows,SVerticalBox)]]]
        + SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(155)[SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping).Text(this,&SCubeShotPanel::ResultsText)]]
        + SVerticalBox::Slot().AutoHeight()[SNew(SWrapBox).UseAllottedSize(true)
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Cancel","Request Cancel")).OnClicked_Lambda([](){auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("action"),TEXT("cancel"));Args->SetStringField(TEXT("job_id"),LatestJobId);CubeShot::Service::Jobs().Control(Args);return FReply::Handled();})]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("RetryFailed","Retry failed")).OnClicked_Lambda([this](){return Retry(false);})]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("RunSkipped","Run skipped")).OnClicked_Lambda([this](){return Retry(true);})]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("OpenOutputs","Open output folder")).OnClicked_Lambda([](){FPlatformProcess::ExploreFolder(*(FPaths::ProjectSavedDir()/TEXT("CubeShot/MCPExports")));return FReply::Handled();})]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("BrowseTextures","Browse generated textures")).OnClicked_Lambda([]()
            {
                auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("job_id"),LatestJobId);auto Job=CubeShot::Service::Jobs().Status(Args);TArray<FAssetData> Assets;
                for(const auto& Item:UnrealToolsRuntime::GetArray(Job,TEXT("items")))for(const auto& Path:UnrealToolsRuntime::GetArray(UnrealToolsRuntime::GetObject(Item->AsObject(),TEXT("result")),TEXT("saved")))
                    if(UObject* Asset=LoadObject<UObject>(nullptr,*Path->AsString()))Assets.Add(FAssetData(Asset));
                if(!Assets.IsEmpty())FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(Assets);
                return FReply::Handled();
            })]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("CopyResults","Copy result paths")).OnClicked_Lambda([this](){FPlatformApplicationMisc::ClipboardCopy(*ResultsText().ToString());return FReply::Handled();})]
            + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("AcknowledgeRecovery","Acknowledge reviewed recovery")).OnClicked_Lambda([this]()
            {
                const bool Diagnostic=UnrealToolsRuntime::GetString(CachedSelectedJob,TEXT("record_kind"))==TEXT("diagnostic");
                const FText Confirmation=Diagnostic
                    ?LOCTEXT("ReviewDiagnosticMetadata","Have you reviewed the source metadata and any preserved raw evidence? Acknowledgement verifies and preserves raw bytes before damaged source metadata may be removed. Output files, backups, and recovery staging are preserved.")
                    :LOCTEXT("ReviewRecovery","Have you reviewed the saved, dirty and staging paths and resolved any pending package changes? This releases the output block without restoring or deleting files.");
                if(FMessageDialog::Open(EAppMsgType::YesNo,Confirmation)==EAppReturnType::Yes)
                {auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("action"),TEXT("acknowledge_recovery"));Args->SetStringField(TEXT("job_id"),LatestJobId);auto Result=CubeShot::Service::Jobs().Control(Args);LastResultStatus=FText::FromString(Result->GetBoolField(TEXT("success"))?TEXT("Recovery block released."):Result->GetStringField(TEXT("error")));}
                return FReply::Handled();
            })]
        ]];
}

FReply SCubeShotPanel::Retry(bool Skipped)
{
    auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("job_id"),LatestJobId);auto Job=CubeShot::Service::Jobs().Status(Args);if(!Job->GetBoolField(TEXT("success")))return FReply::Handled();
    TArray<FString> Paths;auto Overrides=MakeShared<FJsonObject>();const TArray<TSharedPtr<FJsonValue>>* Items;if(Job->TryGetArrayField(TEXT("items"),Items))for(const auto& Value:*Items){auto Item=Value->AsObject();if(Item->GetStringField(TEXT("state"))==(Skipped?TEXT("skipped"):TEXT("failed"))){const FString Path=Item->GetStringField(TEXT("mesh_path"));Paths.Add(Path);Overrides->SetObjectField(Path,UnrealToolsRuntime::GetObject(Item,TEXT("options")));}}
    return StartBatch(Paths,Overrides);
}

FReply SCubeShotPanel::TransferProfile(bool Import)
{
    IDesktopPlatform* Desktop=FDesktopPlatformModule::Get();if(!Desktop)return FReply::Handled();TArray<FString> Files;
    const void* Parent=FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if(Import)
    {
        if(!Desktop->OpenFileDialog(Parent,TEXT("Import CubeShot profile"),FPaths::ProjectSavedDir(),TEXT(""),TEXT("JSON (*.json)|*.json"),0,Files)||Files.IsEmpty())return FReply::Handled();
        FString Text;TSharedPtr<FJsonObject> Profile;if(!FFileHelper::LoadFileToString(Text,*Files[0])||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Profile)){LastResultStatus=LOCTEXT("InvalidProfileJSON","Profile JSON could not be read.");return FReply::Handled();}
        const TSharedPtr<FJsonObject>* Options;FString Error;if(!Profile->TryGetObjectField(TEXT("options"),Options)){LastResultStatus=LOCTEXT("MissingOptions","Profile JSON requires options.");return FReply::Handled();}auto Normalized=UnrealToolsRuntime::Clone(*Options);if(!CubeShot::Service::Normalize(Normalized,Error)){LastResultStatus=FText::FromString(Error);return FReply::Handled();}Profile->SetObjectField(TEXT("options"),Normalized);Profile->RemoveField(TEXT("builtin"));Profile->RemoveField(TEXT("success"));
        auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("action"),TEXT("save"));Args->SetObjectField(TEXT("profile"),Profile);if(CubeShot::Service::Profiles().Get(Profile->GetStringField(TEXT("id")))){if(FMessageDialog::Open(EAppMsgType::YesNo,LOCTEXT("ReplaceImportedProfile","Replace this existing user profile with the imported JSON?"))!=EAppReturnType::Yes)return FReply::Handled();Args->SetBoolField(TEXT("overwrite"),true);}auto Result=CubeShot::Service::HandleProfiles(Args);RebuildProfiles();LastResultStatus=FText::FromString(Result->GetBoolField(TEXT("success"))?TEXT("Profile imported."):Result->GetStringField(TEXT("error")));
    }
    else
    {
        auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("action"),TEXT("get"));Args->SetStringField(TEXT("id"),SelectedProfile);auto Result=CubeShot::Service::HandleProfiles(Args);const TSharedPtr<FJsonObject>* Profile;if(!Result->TryGetObjectField(TEXT("profile"),Profile))return FReply::Handled();
        if(!Desktop->SaveFileDialog(Parent,TEXT("Export CubeShot profile"),FPaths::ProjectSavedDir(),SelectedProfile+TEXT(".json"),TEXT("JSON (*.json)|*.json"),0,Files)||Files.IsEmpty())return FReply::Handled();FString Error;auto ExportProfile=UnrealToolsRuntime::Clone(*Profile);ExportProfile->RemoveField(TEXT("builtin"));ExportProfile->RemoveField(TEXT("success"));UnrealToolsRuntime::AtomicJson(Files[0],ExportProfile,Error);LastResultStatus=FText::FromString(Error.IsEmpty()?TEXT("Profile exported."):Error);
    }
    return FReply::Handled();
}

FReply SCubeShotPanel::OnDrop(const FGeometry& Geometry,const FDragDropEvent& Event)
{
    auto Drag=Event.GetOperationAs<FAssetDragDropOp>();if(!Drag)return FReply::Unhandled();FString LastPath;for(const auto& Asset:Drag->GetAssets())if(Asset.AssetClassPath==UStaticMesh::StaticClass()->GetClassPathName()){LastPath=Asset.GetObjectPathString();AddTarget(LastPath);}if(!LastPath.IsEmpty())SelectTargetPath(LastPath);return FReply::Handled();
}
void SCubeShotPanel::OnDragEnter(const FGeometry& Geometry,const FDragDropEvent& Event) { SCompoundWidget::OnDragEnter(Geometry,Event); }


#if WITH_DEV_AUTOMATION_TESTS
void SCubeShotPanel::ClearTargetsForAutomation(){Targets.Reset();SelectedTarget=INDEX_NONE;ViewModel->SetStaticMesh(nullptr);RebuildTargets();}
void SCubeShotPanel::SelectJobForAutomation(const FString& JobId){LatestJobId=JobId;RefreshJobObserver(true);}
void SCubeShotPanel::PickTargetForAutomation(const FString& Path){OnMeshChanged(FAssetData(LoadObject<UStaticMesh>(nullptr,*Path)));}
void SCubeShotPanel::SetSelectedTargetsForAutomation(const TArray<FString>& Paths){for(auto& Row:Targets)Row.bSelected=Paths.Contains(Row.Path);}
void SCubeShotPanel::SetResolutionForAutomation(int32 Resolution){ViewModel->SetResolution(Resolution);PersistCurrentOptions();}
TArray<FString> SCubeShotPanel::TargetPathsForAutomation()const{TArray<FString> Paths;for(const auto& Row:Targets)Paths.Add(Row.Path);return Paths;}
TSharedPtr<FJsonObject> SCubeShotPanel::TargetOptionsForAutomation(const FString& Path)const{const auto* Row=Targets.FindByPredicate([&](const FTargetState& Value){return Value.Path==Path;});return Row?UnrealToolsRuntime::Clone(Row->Override):nullptr;}
#endif

#if WITH_DEV_AUTOMATION_TESTS
bool SCubeShotPanel::HasCompletePreviewPixelsForAutomation()const
{
    if(bPreviewUpdating||VisiblePreviewPixels.Num()!=DirectionCount)return false;
    for(int64 Visible:VisiblePreviewPixels)if(Visible<=0)return false;
    return true;
}
bool SCubeShotPanel::TakePaintedPreviewForAutomation(TArray<FColor>& Pixels,FIntVector& Size)const
{
    return !PreviewImages.IsEmpty()&&PreviewImages[0].IsValid()
        &&FSlateApplication::Get().TakeScreenshot(PreviewImages[0].ToSharedRef(),Pixels,Size);
}
#endif

#undef LOCTEXT_NAMESPACE
