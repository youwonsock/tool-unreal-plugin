#include "SCubeShotPanel.h"

#include "CubeShotAssetWriter.h"
#include "CubeShotCaptureService.h"

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
	CaptureService = MakeUnique<FCubeShotCaptureService>();

	FCubeShotAssetWriterConfig WriterConfig;
	WriterConfig.bSyncContentBrowserOnSuccess = true;
	AssetWriter = MakeUnique<FCubeShotAssetWriter>(MoveTemp(WriterConfig));

	CachedRawFrames.SetNum(DirectionCount);
	PendingRawFrames.SetNum(DirectionCount);
	PendingPreviewImages.SetNum(DirectionCount);
	PreviewTextures.Reserve(DirectionCount);
	PreviewBrushes.Reserve(DirectionCount);
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
				BuildPreviewPanel()
			]
		]
	];

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
	AssetWriter.Reset();
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
						.ToolTipText(LOCTEXT("UseSelectedMeshTooltip", "Uses the first selected Static Mesh in the Content Browser."))
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
				.Text(LOCTEXT("GenerateButton", "Generate 6 Textures"))
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
				SNew(STextBlock)
				.Text(FText::FromString(Direction.Label))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
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
							SNew(SImage)
							.Image_Lambda([this, DirectionIndex]() { return GetPreviewBrush(DirectionIndex); })
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
	if (ViewModel->IsGenerating())
	{
		return EActiveTimerReturnType::Continue;
	}

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

	PreviewBrushes[DirectionIndex]->SetResourceObject(nullptr);
	PreviewTextures[DirectionIndex].Reset();

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
	PreviewBrushes[DirectionIndex]->SetResourceObject(Texture);
	PreviewBrushes[DirectionIndex]->SetImageSize(FVector2D(Image.Width, Image.Height));
	PreviewBrushes[DirectionIndex]->DrawAs = ESlateBrushDrawType::Image;
}

void SCubeShotPanel::ClearPreviewResources()
{
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
	ViewModel->SetStaticMesh(Cast<UStaticMesh>(AssetData.GetAsset()));
	ApplyAutoFit();
	SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
}

void SCubeShotPanel::OnBackgroundTextureChanged(const FAssetData& AssetData)
{
	ViewModel->SetBackgroundTexture(Cast<UTexture2D>(AssetData.GetAsset()));
	SchedulePreview(FCubeShotViewModel::EChangeKind::CompositeOnly);
}

FReply SCubeShotPanel::OnUseSelectedMesh()
{
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowser.Get().GetSelectedAssets(SelectedAssets);
	for (const FAssetData& Asset : SelectedAssets)
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset()))
		{
			ViewModel->SetStaticMesh(Mesh);
			ApplyAutoFit();
			SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
			return FReply::Handled();
		}
	}
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
	if (!ViewModel->Validate() || !ViewModel->CanGenerate())
	{
		return FReply::Handled();
	}

	const FCubeShotOptions Options = ViewModel->GetOptionsCopy();
	FCubeShotPreflightResult Preflight;
	if (!AssetWriter->BuildPreflight(Options, Preflight))
	{
		OperationStatus = Preflight.Error.IsEmpty() ? LOCTEXT("PreflightFailed", "Output validation failed.") : Preflight.Error;
		return FReply::Handled();
	}

	bool bOverwriteConfirmed = !Preflight.bHasExistingTextures;
	if (Preflight.bHasExistingTextures)
	{
		const FText Message = FText::Format(
			LOCTEXT("OverwritePrompt", "One or more CubeShot textures already exist. Update all existing textures in place and preserve their references?\n\n{0}"),
			Preflight.Warning);
		bOverwriteConfirmed = FMessageDialog::Open(EAppMsgType::YesNo, Message, LOCTEXT("OverwriteTitle", "Overwrite CubeShot Textures")) == EAppReturnType::Yes;
		if (!bOverwriteConfirmed)
		{
			OperationStatus = LOCTEXT("OverwriteDeclined", "Generation cancelled before capture.");
			return FReply::Handled();
		}
	}

	ViewModel->SetGenerating(true);
	bPreviewUpdating = false;
	ProgressCompleted = 0;
	ProgressTotal = DirectionCount;
	OperationStatus = LOCTEXT("GenerationStarting", "Starting final capture...");

	TUniquePtr<FScopedSlowTask> SlowTask = MakeUnique<FScopedSlowTask>(
		static_cast<float>(DirectionCount),
		LOCTEXT("GenerationProgress", "Generating CubeShot textures..."));
	SlowTask->MakeDialog(true);
	int32 PreviousCompleted = 0;
	bool bCaptureCancellationRequested = false;
	FCubeShotGenerationResult Result;
	AssetWriter->Execute(
		Options,
		*CaptureService,
		Preflight,
		bOverwriteConfirmed,
		[this, &SlowTask, &PreviousCompleted, &bCaptureCancellationRequested](const int32 Completed, const int32 Total, const FText& Status)
		{
			ProgressCompleted = Completed;
			ProgressTotal = Total;
			const FText EffectiveStatus = Completed >= Total
				? LOCTEXT("SavingCannotCancel", "All captures are staged. Saving packages; cancellation is no longer available.")
				: Status;
			OperationStatus = EffectiveStatus;
			const int32 Delta = FMath::Max(0, Completed - PreviousCompleted);
			if (Delta > 0 && SlowTask.IsValid())
			{
				SlowTask->EnterProgressFrame(static_cast<float>(Delta), EffectiveStatus);
				PreviousCompleted = Completed;
			}
			if (Completed >= Total)
			{
				// Cancellation is only valid while staging captures. Close the cancelable
				// dialog at the mutation boundary, but preserve a request made during the
				// final blocking capture so the writer's last pre-mutation poll observes it.
				bCaptureCancellationRequested = SlowTask.IsValid() && SlowTask->ShouldCancel();
				SlowTask.Reset();
			}
		},
		[&SlowTask, &bCaptureCancellationRequested]()
		{
			return bCaptureCancellationRequested || (SlowTask.IsValid() && SlowTask->ShouldCancel());
		},
		Result);

	const FText FinalResultStatus = Result.Message.IsEmpty()
		? (Result.bSucceeded ? LOCTEXT("GenerationSucceeded", "Generated six CubeShot textures.") : LOCTEXT("GenerationFailed", "CubeShot generation failed."))
		: Result.Message;
	OperationStatus = FinalResultStatus;
	if (!Result.FailedObjectPaths.IsEmpty())
	{
		FString Details = TEXT("Saved:\n");
		Details += Result.SavedObjectPaths.IsEmpty() ? TEXT("  (none)\n") : TEXT("  ") + FString::Join(Result.SavedObjectPaths, TEXT("\n  ")) + TEXT("\n");
		Details += TEXT("\nFailed (left dirty; staging retained):\n  ");
		Details += FString::Join(Result.FailedObjectPaths, TEXT("\n  "));
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(Details),
			LOCTEXT("PartialSaveTitle", "CubeShot Save Results"));
	}
	ViewModel->SetGenerating(false);
	ViewModel->Validate();
	ViewModel->RequestPreviewRecapture();
	SchedulePreview(FCubeShotViewModel::EChangeKind::Recapture);
	OperationStatus = FinalResultStatus;
	return FReply::Handled();
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
		if (!ViewModel->GetValidationWarning().IsEmpty())
		{
			return ViewModel->GetValidationWarning();
		}
	}
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
	return ViewModel.IsValid()
		&& ViewModel->CanGenerate()
		&& PreviewError.IsEmpty()
		&& !bPreviewUpdating
		&& ScheduledRevision == ProcessingRevision;
}

EVisibility SCubeShotPanel::GetUpdatingVisibility() const
{
	return bPreviewUpdating ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCubeShotPanel::GetProgressVisibility() const
{
	return ViewModel.IsValid() && ViewModel->IsGenerating() ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
