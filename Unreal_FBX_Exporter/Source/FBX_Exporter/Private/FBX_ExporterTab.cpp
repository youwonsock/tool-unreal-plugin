#include "FBX_ExporterService.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "IContentBrowserSingleton.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Engine/StaticMesh.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "FBX_ExporterWorkspaceController.h"

#define LOCTEXT_NAMESPACE "FBXExporterTab"
namespace FBXExporter
{
namespace
{
using namespace UnrealToolsRuntime;
const FName TabId(TEXT("FBX_Exporter.Workspace"));
TWeakPtr<class SExporterWorkspace> LiveWidget;
FString JobStateLabel(const FObject& Job)
{
    if (GetString(Job, TEXT("record_kind")) == TEXT("diagnostic"))
        return GetString(Job, TEXT("phase")) == TEXT("acknowledged") ? TEXT("Recovery acknowledged")
                                                                     : TEXT("Recovery review required");
    const FString State = GetString(Job, TEXT("state"));
    if (State == TEXT("succeeded"))
        return TEXT("Completed");
    if (State == TEXT("failed"))
        return TEXT("Failed");
    if (State == TEXT("cancelled"))
        return TEXT("Cancelled");
    if (State == TEXT("interrupted"))
        return TEXT("Interrupted");
    if (State == TEXT("running"))
        return TEXT("Running");
    if (State == TEXT("queued"))
        return TEXT("Queued");
    return State;
}
FText RecoveryTooltip(const FObject& Job)
{
    return GetString(Job, TEXT("record_kind")) == TEXT("diagnostic")
               ? LOCTEXT("DiagnosticRecoveryTooltip",
                         "Review the source and raw evidence paths first. Acknowledgement preserves and verifies "
                         "metadata bytes, then quarantines damaged metadata or keeps a repaired valid record. Output, "
                         "staging and backup files are untouched.")
               : LOCTEXT("RecoveryTooltip", "Only after inspecting retained output, staging and backup. This releases "
                                            "recovery without restoring or deleting those output files. A related "
                                            "unresolved diagnostic must be acknowledged first.");
}

void CopyCachedResultJson(const UnrealToolsWorkspace::FHistoryView& History,
                          TFunctionRef<void(const FString&)> CopyToClipboard)
{
    // An empty history has no Selected object. Its cached JSON is a safe empty
    // string, matching the existing empty-result clipboard behavior.
    CopyToClipboard(History.Json);
}

TSharedRef<SEditableTextBox> CreateOptionJsonEditor(TSharedRef<FFBXExporterWorkspaceController> Controller,
                                                    const FString& Path)
{
    const auto Token = Controller->TokenFor(Path);
    const TWeakPtr<FFBXExporterWorkspaceController> Weak = Controller;
    return SNew(SEditableTextBox)
        .Text_Lambda([Weak, Path]() {
            if (auto Model = Weak.Pin())
                return FText::FromString(Model->DraftText(Path));
            return FText::GetEmpty();
        })
        .OnTextChanged_Lambda([Weak, Token](const FText& Text) {
            if (auto Model = Weak.Pin())
                Model->JsonDraft(Token, Text.ToString());
        })
        .OnTextCommitted_Lambda([Weak, Token](const FText&, ETextCommit::Type) {
            if (auto Model = Weak.Pin())
                Model->Flush(Token);
        });
}

class SExporterWorkspace final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SExporterWorkspace)
    {
    }
    SLATE_ARGUMENT(TSharedPtr<FFBXExporterWorkspaceController>, Workspace)
    SLATE_END_ARGS() void Construct(const FArguments& Args)
    {
        Workspace = Args._Workspace;
        check(Workspace);
        Workspace->AttachView();
        ProgressStyle = FAppStyle::Get().GetWidgetStyle<FProgressBarStyle>(TEXT("ProgressBar"));
        ProgressStyle.SetEnableFillAnimation(false);
        for (int32 Value : {512, 1024, 2048, 4096})
            Resolutions.Add(MakeShared<int32>(Value));
        RebuildContents();
    }
    virtual ~SExporterWorkspace() override
    {
        if (Workspace)
            Workspace->DetachView();
    }
    void RebuildContents()
    {
        if (bRebuilding)
            return;
        TGuardValue<bool> Guard(bRebuilding, true);
        Workspace->ReconcileProfiles();
        RefreshProfiles();
        RefreshJobs();
        const auto Token = Workspace->Token();
        ChildSlot
            [SNew(SScrollBox) +
             SScrollBox::Slot().Padding(12)
                 [SNew(SVerticalBox) +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0, 0, 0,
                      8)[SNew(STextBlock).Text(LOCTEXT("Title", "FBX Exporter — Targets, profiles and output"))] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4)) +
                       SWrapBox::Slot().Padding(0, 0, 8, 0)
                           [SNew(SButton).Text(LOCTEXT("Add", "Add selected meshes")).OnClicked_Lambda([this, Token]() {
                               TArray<FAssetData> Assets;
                               FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"))
                                   .Get()
                                   .GetSelectedAssets(Assets);
                               Add(Assets);
                               return FReply::Handled();
                           })] +
                       SWrapBox::Slot()
                           [SNew(SButton).Text(LOCTEXT("Clear", "Clear targets")).OnClicked_Lambda([this, Token]() {
                               Workspace->Clear();
                               RebuildContents();
                               return FReply::Handled();
                           })] +
                       SWrapBox::Slot().FillEmptySpace(true).Padding(
                           12, 0)[SNew(STextBlock)
                                      .Text(LOCTEXT(
                                          "Drop", "Drop Static Meshes here. Each row can override profile options."))] +
                       SWrapBox::Slot().Padding(8, 0)[SNew(SButton)
                                                          .Text(LOCTEXT("SelectAllTargets", "Select all targets"))
                                                          .OnClicked_Lambda([this, Token]() {
                                                              Workspace->SelectAll(true);
                                                              return FReply::Handled();
                                                          })] +
                       SWrapBox::Slot().Padding(8,
                                                0)[SNew(SButton)
                                                       .Text(LOCTEXT("ClearTargetSelection", "Clear target selection"))
                                                       .OnClicked_Lambda([this, Token]() {
                                                           Workspace->SelectAll(false);
                                                           return FReply::Handled();
                                                       })] +
                       SWrapBox::Slot().Padding(
                           8, 0)[SNew(SButton)
                                     .Text(LOCTEXT("ApplySelected", "Apply current options to selected"))
                                     .OnClicked_Lambda([this, Token]() {
                                         Workspace->ApplySelected();
                                         RebuildContents();
                                         return FReply::Handled();
                                     })] +
                       SWrapBox::Slot().Padding(8, 0)[SNew(SButton)
                                                          .Text(LOCTEXT("RemoveSelected", "Remove selected targets"))
                                                          .OnClicked_Lambda([this, Token]() {
                                                              Workspace->RemoveSelected();
                                                              RebuildContents();
                                                              return FReply::Handled();
                                                          })]

        ] +
                  SVerticalBox::Slot().AutoHeight().Padding(0, 8)[SNew(SBox).MaxDesiredHeight(
                      210)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(TargetBox, SVerticalBox)]]] +
                  SVerticalBox::Slot().AutoHeight().Padding(0, 8)[SNew(STextBlock).Text_Lambda([this, Token]() {
                      return !Workspace->IsDefault()
                                 ? FText::FromString(TEXT("Options for ") + Workspace->EditingPath())
                                 : LOCTEXT("Profile", "Defaults for new targets");
                  })] +
                  SVerticalBox::Slot().AutoHeight()[SNew(SButton)
                                                        .Text(LOCTEXT("EditGlobal", "Edit defaults for new targets"))
                                                        .Visibility_Lambda([this, Token]() {
                                                            return !Workspace->IsDefault() ? EVisibility::Visible
                                                                                           : EVisibility::Collapsed;
                                                        })
                                                        .OnClicked_Lambda([this, Token]() {
                                                            Workspace->Edit({});
                                                            RebuildContents();
                                                            return FReply::Handled();
                                                        })] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4)) +
                       SWrapBox::Slot().FillEmptySpace(true).Padding(0, 0, 8, 0)
                           [SAssignNew(ProfileCombo, SComboBox<TSharedPtr<FString>>)
                                .OptionsSource(&ProfileIds)
                                .OnGenerateWidget_Lambda([this, Token](TSharedPtr<FString> Id) {
                                    const auto* Name = ProfileNames.Find(*Id);
                                    return SNew(STextBlock).Text(FText::FromString(Name ? *Name : *Id));
                                })
                                .OnSelectionChanged_Lambda([this, Token](TSharedPtr<FString> Id, ESelectInfo::Type) {
                                    if (!Id || bRebuilding || !Workspace->IsCurrent(Token))
                                        return;
                                    Workspace->ApplyProfile(*Id, true);
                                    RebuildContents();
                                })
                                .Content()[SNew(STextBlock).Text_Lambda([this, Token]() {
                                    const auto Id = Workspace->ProfileId();
                                    const auto* Name = ProfileNames.Find(Id);
                                    return FText::FromString(Name ? *Name : Id);
                                })]] +
                       SWrapBox::Slot()[SNew(SButton)
                                            .Text(LOCTEXT("Import", "Import profile JSON"))
                                            .OnClicked_Lambda([this, Token]() { return ImportProfile(); })] +
                       SWrapBox::Slot().Padding(
                           8, 0)[SNew(SButton)
                                     .Text(LOCTEXT("Export", "Export profile JSON"))
                                     .OnClicked_Lambda([this, Token]() { return ExportProfile(); })]] +
                  SVerticalBox::Slot().AutoHeight().Padding(0, 8)
                      [SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4)) +
                       SWrapBox::Slot().FillEmptySpace(true).Padding(0, 0, 8, 0)
                           [SNew(SEditableTextBox)
                                .HintText(LOCTEXT("ProfileId", "User profile ID"))
                                .Text_Lambda([this, Token]() { return FText::FromString(Workspace->UserProfileId()); })
                                .OnTextChanged_Lambda([this, Token](const FText& Text) {
                                    Workspace->SetUserProfileId(Text.ToString());
                                })] +
                       SWrapBox::Slot().FillEmptySpace(true).Padding(0, 0, 8, 0)
                           [SNew(SEditableTextBox)
                                .HintText(LOCTEXT("ProfileName", "Display name"))
                                .Text_Lambda([this, Token]() { return FText::FromString(Workspace->ProfileName()); })
                                .OnTextChanged_Lambda(
                                    [this, Token](const FText& Text) { Workspace->SetProfileName(Text.ToString()); })] +
                       SWrapBox::Slot()[SNew(SButton)
                                            .Text(LOCTEXT("SaveProfile", "Save user profile"))
                                            .OnClicked_Lambda([this, Token]() {
                                                SaveProfile();
                                                return FReply::Handled();
                                            })] +
                       SWrapBox::Slot().Padding(8, 0)[SNew(SButton)
                                                          .Text(LOCTEXT("DeleteProfile", "Delete user profile"))
                                                          .OnClicked_Lambda([this, Token]() {
                                                              Workspace->DeleteProfile();
                                                              RebuildContents();
                                                              return FReply::Handled();
                                                          })]] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0, 4)[SNew(STextBlock)
                                .Text(LOCTEXT("OutputRoot", "Output folder (replaces the complete mesh folder only "
                                                            "when explicitly approved)"))] +
                  SVerticalBox::Slot().AutoHeight()[SNew(SEditableTextBox)
                                                        .Text_Lambda([this, Token]() {
                                                            return FText::FromString(
                                                                Workspace->String(TEXT("output_root")));
                                                        })
                                                        .OnTextChanged_Lambda([this, Token](const FText& Text) {
                                                            SetString(Token, TEXT("output_root"), Text.ToString());
                                                        })] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0,
                      8)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4)) +
                         SWrapBox::Slot().Padding(
                             0, 0, 12, 0)[SNew(SCheckBox)
                                              .IsChecked_Lambda([this, Token]() {
                                                  return Workspace->String(TEXT("output_layout")) == TEXT("asset_path")
                                                             ? ECheckBoxState::Checked
                                                             : ECheckBoxState::Unchecked;
                                              })
                                              .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState Value) {
                                                  SetString(Token, TEXT("output_layout"),
                                                            Value == ECheckBoxState::Checked ? TEXT("asset_path")
                                                                                             : TEXT("mesh_name"));
                                              })[SNew(STextBlock).Text(LOCTEXT("PathLayout", "Preserve asset path"))]] +
                         SWrapBox::Slot().Padding(0, 0, 12, 0)
                             [SNew(SComboBox<TSharedPtr<int32>>)
                                  .OptionsSource(&Resolutions)
                                  .IsEnabled_Lambda([this, Token]() { return Workspace->HasArray(TEXT("maps")); })
                                  .OnGenerateWidget_Lambda([this, Token](TSharedPtr<int32> Value) {
                                      return SNew(STextBlock).Text(FText::AsNumber(*Value));
                                  })
                                  .OnSelectionChanged_Lambda([this, Token](TSharedPtr<int32> Value, ESelectInfo::Type) {
                                      if (Value)
                                          SetNumber(Token, TEXT("resolution"), *Value);
                                  })
                                  .Content()[SNew(STextBlock).Text_Lambda([this, Token]() {
                                      return FText::Format(LOCTEXT("Resolution", "Resolution: {0}"),
                                                           FText::AsNumber(static_cast<int32>(
                                                               Workspace->Number(TEXT("resolution"), 2048))));
                                  })]] +
                         SWrapBox::Slot().Padding(0, 0, 12, 0)
                             [SNew(SCheckBox)
                                  .IsEnabled_Lambda([this, Token]() { return Workspace->HasArray(TEXT("maps")); })
                                  .IsChecked_Lambda([this, Token]() {
                                      return Workspace->Bool(TEXT("reuse_direct_textures")) ? ECheckBoxState::Checked
                                                                                            : ECheckBoxState::Unchecked;
                                  })
                                  .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState Value) {
                                      SetBool(Token, TEXT("reuse_direct_textures"), Value == ECheckBoxState::Checked);
                                  })[SNew(STextBlock).Text(LOCTEXT("Reuse", "Reuse direct textures"))]] +
                         SWrapBox::Slot()[SNew(SCheckBox)
                                              .IsEnabled_Lambda(
                                                  [this, Token]() { return Workspace->HasArray(TEXT("maps")); })
                                              .IsChecked_Lambda([this, Token]() {
                                                  return Workspace->Bool(TEXT("embed_textures"))
                                                             ? ECheckBoxState::Checked
                                                             : ECheckBoxState::Unchecked;
                                              })
                                              .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState Value) {
                                                  SetBool(Token, TEXT("embed_textures"),
                                                          Value == ECheckBoxState::Checked);
                                              })[SNew(STextBlock).Text(LOCTEXT("Embed", "Embed PNG bytes"))]]] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SHorizontalBox) +
                       SHorizontalBox::Slot().AutoWidth()
                           [SNew(SButton).Text(LOCTEXT("AllMaps", "Select all maps")).OnClicked_Lambda([this, Token]() {
                               if (!bRebuilding)
                                   Workspace->SetAllMaps(Token, true);
                               return FReply::Handled();
                           })] +
                       SHorizontalBox::Slot().AutoWidth()
                           [SNew(SButton).Text(LOCTEXT("NoMaps", "Clear all maps")).OnClicked_Lambda([this, Token]() {
                               if (!bRebuilding)
                                   Workspace->SetAllMaps(Token, false);
                               return FReply::Handled();
                           })]] +
                  SVerticalBox::Slot().AutoHeight()
                      [SAssignNew(MapBox, SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4))] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0, 8)[SNew(SCheckBox)
                                .IsChecked_Lambda([this, Token]() {
                                    return Workspace->Overwrite() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                                })
                                .OnCheckStateChanged_Lambda([this, Token](ECheckBoxState Value) {
                                    Workspace->SetOverwrite(Value == ECheckBoxState::Checked);
                                })[SNew(STextBlock)
                                       .Text(LOCTEXT("Overwrite", "Approve replacing complete existing target folders, "
                                                                  "including user files"))]] +
                  SVerticalBox::Slot().AutoHeight()[SNew(STextBlock)
                                                        .AutoWrapText(true)
                                                        .ColorAndOpacity(FLinearColor(1, 0.5f, 0.2f))
                                                        .Text_Lambda([this, Token]() {
                                                            return FText::FromString(Workspace->Error());
                                                        })] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0,
                      8)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4)) +
                         SWrapBox::Slot().Padding(0, 0, 8, 0)
                             [SNew(SButton)
                                  .Text(LOCTEXT("Preflight", "Inspect output plan"))
                                  .OnClicked_Lambda([this, Token]() {
                                      Workspace->Inspect();
                                      return FReply::Handled();
                                  })] +
                         SWrapBox::Slot().Padding(0, 0, 8, 0)
                             [SNew(SButton).Text(LOCTEXT("Queue", "Queue export")).OnClicked_Lambda([this, Token]() {
                                 Workspace->Start();
                                 RefreshJobs();
                                 return FReply::Handled();
                             })] +
                         SWrapBox::Slot().Padding(0, 0, 8, 0)
                             [SNew(SButton).Text(LOCTEXT("Cancel", "Request cancel")).OnClicked_Lambda([this, Token]() {
                                 Workspace->ControlSelectedJob(TEXT("cancel"));
                                 return FReply::Handled();
                             })] +
                         SWrapBox::Slot().Padding(0, 0, 8, 0)
                             [SNew(SButton).Text(LOCTEXT("Retry", "Retry failed")).OnClicked_Lambda([this, Token]() {
                                 Workspace->Retry(TEXT("failed"));
                                 RefreshJobs();
                                 return FReply::Handled();
                             })] +
                         SWrapBox::Slot()
                             [SNew(SButton).Text(LOCTEXT("Skipped", "Run skipped")).OnClicked_Lambda([this, Token]() {
                                 Workspace->Retry(TEXT("skipped"));
                                 RefreshJobs();
                                 return FReply::Handled();
                             })]] +
                  SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this, Token]() {
                      return FText::FromString(Workspace->PlanSummary());
                  })] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SExpandableArea)
                           .InitiallyCollapsed(true)
                           .HeaderContent()[SNew(STextBlock)
                                                .Text(LOCTEXT("PlanDetails",
                                                              "Output plan: files, replacement scope and errors"))]
                           .BodyContent()[SNew(SBox).MaxDesiredHeight(
                               260)[SNew(SScrollBox) +
                                    SScrollBox::Slot()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this, Token]() {
                                        return FText::FromString(Workspace->PlanText());
                                    })]]]] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0, 8)[SNew(STextBlock).Text(LOCTEXT("Result", "Job progress and preserved results"))] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SWrapBox).UseAllottedSize(true) +
                       SWrapBox::Slot()
                           [SNew(SButton).Text(LOCTEXT("Recent", "Recent")).OnClicked_Lambda([this, Token]() {
                               Workspace->HistoryMode(false, false);
                               RefreshJobs();
                               return FReply::Handled();
                           })] +
                       SWrapBox::Slot().Padding(
                           8, 0)[SNew(SButton).Text(LOCTEXT("Archive", "Archive")).OnClicked_Lambda([this, Token]() {
                           Workspace->HistoryMode(true, false);
                           RefreshJobs();
                           return FReply::Handled();
                       })] +
                       SWrapBox::Slot().Padding(8, 0)[SNew(SButton)
                                                          .Text(LOCTEXT("Diagnostics", "Recovery diagnostics"))
                                                          .OnClicked_Lambda([this, Token]() {
                                                              Workspace->RefreshRecoveryHistory();
                                                              RefreshJobs();
                                                              return FReply::Handled();
                                                          })] +
                       SWrapBox::Slot().Padding(8, 0)[SNew(SButton)
                                                          .Text(LOCTEXT("PreviousPage", "Previous 50"))
                                                          .OnClicked_Lambda([this, Token]() {
                                                              Workspace->HistoryPage(-1);
                                                              RefreshJobs();
                                                              return FReply::Handled();
                                                          })] +
                       SWrapBox::Slot().Padding(8,
                                                0)[SNew(SButton)
                                                       .Text(LOCTEXT("NextPage", "Next 50"))
                                                       .IsEnabled_Lambda([this, Token]() { return JobIds.Num() == 50; })
                                                       .OnClicked_Lambda([this, Token]() {
                                                           Workspace->HistoryPage(1);
                                                           RefreshJobs();
                                                           return FReply::Handled();
                                                       })]] +
                  SVerticalBox::Slot().AutoHeight()
                      [SAssignNew(JobCombo, SComboBox<TSharedPtr<FString>>)
                           .OptionsSource(&JobIds)
                           .OnGenerateWidget_Lambda([this, Token](TSharedPtr<FString> Id) {
                               const auto* Summary = JobSummaries.Find(*Id);
                               return SNew(STextBlock)
                                   .Text(FText::FromString((Summary ? GetString(*Summary, TEXT("state")) : FString()) +
                                                           TEXT(" · ") + *Id));
                           })
                           .OnSelectionChanged_Lambda([this, Token](TSharedPtr<FString> Id, ESelectInfo::Type) {
                               if (Id)
                               {
                                   Workspace->SelectJob(*Id);
                               }
                           })
                           .Content()[SNew(STextBlock).Text_Lambda([this, Token]() {
                               return FText::FromString(Workspace->History().JobId.IsEmpty()
                                                            ? TEXT("Select a preserved job")
                                                            : Workspace->History().JobId);
                           })]] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SProgressBar).Style(&ProgressStyle).Percent_Lambda([this, Token]() -> TOptional<float> {
                          auto Job = Workspace->History().Selected;
                          const auto Progress = GetObject(Job, TEXT("progress"));
                          const double Total = GetNumber(Progress, TEXT("total"), 1);
                          return Total > 0 ? static_cast<float>(GetNumber(Progress, TEXT("completed")) / Total) : 0.0f;
                      })] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0, 8)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this, Token]() {
                      const auto Job = Workspace->History().Selected;
                      if (!Job)
                          return LOCTEXT("NoJob", "No job submitted. Native FBX and GPU steps may delay progress "
                                                  "updates. Cancellation applies at safe boundaries.");
                      return FText::FromString(JobStateLabel(Job) + TEXT(" · ") + GetString(Job, TEXT("stage")) +
                                               TEXT(" · ") + GetString(Job, TEXT("job_id")));
                  })] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(0, 4)) +
                       SWrapBox::Slot().Padding(0, 0, 8, 0)
                           [SNew(SButton).Text(LOCTEXT("Open", "Open output")).OnClicked_Lambda([this, Token]() {
                               FPlatformProcess::ExploreFolder(*OutputDirectory());
                               return FReply::Handled();
                           })] +
                       SWrapBox::Slot().Padding(0,
                                                0, 8, 0)[SNew(SButton)
                                                             .Text(LOCTEXT("CopyPath", "Copy output path"))
                                                             .OnClicked_Lambda([this, Token]() {
                                                                 FPlatformApplicationMisc::ClipboardCopy(
                                                                     *OutputDirectory());
                                                                 return FReply::Handled();
                                                             })] +
                       SWrapBox::Slot().Padding(0, 0, 8, 0)
                           [SNew(SButton).Text(LOCTEXT("Copy", "Copy result JSON")).OnClicked_Lambda([this, Token]() {
                               CopyCachedResultJson(Workspace->History(), [](const FString& Text) {
                                   FPlatformApplicationMisc::ClipboardCopy(*Text);
                               });
                               return FReply::Handled();
                           })] +
                       SWrapBox::Slot().Padding(0, 0, 8, 0)[SNew(SButton)
                                                                .Text(LOCTEXT("Recovery",
                                                                              "Acknowledge inspected recovery"))
                                                                .ToolTipText_Lambda([this, Token]() {
                                                                    return RecoveryTooltip(
                                                                        Workspace->History().Selected);
                                                                })
                                                                .OnClicked_Lambda([this, Token]() {
                                                                    Workspace->ControlSelectedJob(
                                                                        TEXT("acknowledge_recovery"));
                                                                    return FReply::Handled();
                                                                })] +
                       SWrapBox::Slot()[SNew(SButton)
                                            .Text(LOCTEXT("ShowPng", "Preview first PNG"))
                                            .OnClicked_Lambda([this, Token]() {
                                                ShowPng();
                                                return FReply::Handled();
                                            })]] +
                  SVerticalBox::Slot().AutoHeight().Padding(
                      0, 8)[SNew(SBox).HeightOverride(120).Visibility_Lambda([this, Token]() {
                      return PngBrush.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;
                  })[SNew(SImage).Image_Lambda([this, Token]() -> const FSlateBrush* { return PngBrush.Get(); })]] +
                  SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this, Token]() {
                      return Workspace->History().BodyText;
                  })] +
                  SVerticalBox::Slot().AutoHeight()
                      [SNew(SExpandableArea)
                           .InitiallyCollapsed(true)
                           .HeaderContent()[SNew(STextBlock)
                                                .Text(LOCTEXT("AdvancedResult", "Advanced: complete job JSON"))]
                           .BodyContent()[SNew(SBox).MaxDesiredHeight(
                               260)[SNew(SMultiLineEditableTextBox).IsReadOnly(true).Text_Lambda([this, Token]() {
                               return Workspace->History().JsonText;
                           })]]]]];
        for (const auto& Map : FFBXExporterWorkspaceController::MapNames())
        {
            MapBox->AddSlot().Padding(
                0, 0, 12, 0)[SNew(SCheckBox)
                                 .IsChecked_Lambda([this, Token, Map]() {
                                     return Workspace->Contains(TEXT("maps"), Map) ? ECheckBoxState::Checked
                                                                                   : ECheckBoxState::Unchecked;
                                 })
                                 .OnCheckStateChanged_Lambda([this, Token, Map](ECheckBoxState Value) {
                                     if (!bRebuilding)
                                         Workspace->SetMapEnabled(Token, Map, Value == ECheckBoxState::Checked);
                                 })[SNew(STextBlock).Text(FText::FromString(Map))]];
        }
        RebuildTargets();
    }
    void Add(const TArray<FAssetData>& Assets)
    {
        for (const auto& Asset : Assets)
            if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
                Workspace->Add(Asset.GetObjectPathString());
        RebuildContents();
    }
    virtual FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
    {
        const auto Op = Event.GetOperationAs<FAssetDragDropOp>();
        if (!Op)
            return FReply::Unhandled();
        Add(Op->GetAssets());
        return FReply::Handled();
    }
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override
    {
        SCompoundWidget::Tick(Geometry, Time, Delta);
        RefreshElapsed += Delta;
        if (RefreshElapsed >= .25f)
        {
            RefreshElapsed = 0;
            const auto Previous = Workspace->History().Revision;
            Workspace->RefreshHistory();
            if (Previous != Workspace->History().Revision)
                RefreshJobs();
        }
    }
    void PreparePreviewForTesting()
    {
        Workspace->Inspect();
    }

private:
    void SetString(const UnrealToolsWorkspace::FEditToken& Token, const FString& Key, const FString& Value)
    {
        if (bRebuilding)
            return;
        auto Patch = MakeShared<FJsonObject>();
        Patch->SetStringField(Key, Value);
        Workspace->Patch(Token, Patch);
    }
    void SetNumber(const UnrealToolsWorkspace::FEditToken& Token, const FString& Key, double Value)
    {
        if (bRebuilding)
            return;
        auto Patch = MakeShared<FJsonObject>();
        Patch->SetNumberField(Key, Value);
        Workspace->Patch(Token, Patch);
    }
    void SetBool(const UnrealToolsWorkspace::FEditToken& Token, const FString& Key, bool Value)
    {
        if (bRebuilding)
            return;
        auto Patch = MakeShared<FJsonObject>();
        Patch->SetBoolField(Key, Value);
        Workspace->Patch(Token, Patch);
    }
    FString OutputDirectory() const
    {
        for (const auto& Item : GetArray(Workspace->History().Selected, TEXT("items")))
        {
            const auto Path = GetString(GetObject(Item->AsObject(), TEXT("result")), TEXT("final_directory"));
            if (!Path.IsEmpty())
                return Path;
        }
        return Workspace->String(TEXT("output_root"));
    }
    void RefreshProfiles()
    {
        ProfileIds.Empty();
        ProfileNames.Empty();
        for (const auto& Profile : Workspace->ListProfiles())
        {
            const auto Id = GetString(Profile, TEXT("id"));
            ProfileIds.Add(MakeShared<FString>(Id));
            ProfileNames.Add(Id, GetString(Profile, TEXT("display_name"), Id));
        }
        if (ProfileCombo)
            ProfileCombo->RefreshOptions();
    }
    void RefreshJobs()
    {
        Workspace->RefreshHistory();
        JobIds.Empty();
        JobSummaries.Empty();
        for (const auto& Job : Workspace->History().Summaries)
        {
            const auto Id = GetString(Job, TEXT("job_id"));
            JobIds.Add(MakeShared<FString>(Id));
            JobSummaries.Add(Id, Job);
        }
        if (JobCombo)
            JobCombo->RefreshOptions();
    }
    void RebuildTargets()
    {
        if (!TargetBox)
            return;
        TargetBox->ClearChildren();
        for (const auto& Target : Workspace->Targets())
        {
            const auto Path = Target->Path;
            const auto Token = Workspace->TokenFor(Path);
            TargetBox->AddSlot().AutoHeight().Padding(0, 4)
                [SNew(SVerticalBox) +
                 SVerticalBox::Slot().AutoHeight()
                     [SNew(SHorizontalBox) +
                      SHorizontalBox::Slot()
                          .AutoWidth()[SNew(SCheckBox)
                                           .IsChecked_Lambda([this, Path]() {
                                               for (const auto& Row : Workspace->Targets())
                                                   if (Row->Path == Path)
                                                       return Row->bSelected ? ECheckBoxState::Checked
                                                                             : ECheckBoxState::Unchecked;
                                               return ECheckBoxState::Unchecked;
                                           })
                                           .OnCheckStateChanged_Lambda([this, Path](ECheckBoxState Value) {
                                               Workspace->Select(Path, Value == ECheckBoxState::Checked);
                                           })] +
                      SHorizontalBox::Slot().FillWidth(
                          1)[SNew(STextBlock)
                                 .Text_Lambda([this, Path]() { return Workspace->TargetLabel(Path); })
                                 .AutoWrapText(true)] +
                      SHorizontalBox::Slot().AutoWidth()
                          [SNew(SButton).Text(FText::FromString(TEXT("Up"))).OnClicked_Lambda([this, Path]() {
                              Workspace->Move(Path, -1);
                              RebuildContents();
                              return FReply::Handled();
                          })] +
                      SHorizontalBox::Slot().AutoWidth()
                          [SNew(SButton).Text(FText::FromString(TEXT("Down"))).OnClicked_Lambda([this, Path]() {
                              Workspace->Move(Path, 1);
                              RebuildContents();
                              return FReply::Handled();
                          })] +
                      SHorizontalBox::Slot().AutoWidth()
                          [SNew(SButton).Text(FText::FromString(TEXT("Edit options"))).OnClicked_Lambda([this, Path]() {
                              Workspace->Edit(Path);
                              RebuildContents();
                              return FReply::Handled();
                          })] +
                      SHorizontalBox::Slot().AutoWidth()[SNew(SButton)
                                                             .Text(FText::FromString(TEXT("Reset to defaults")))
                                                             .OnClicked_Lambda([this, Path]() {
                                                                 Workspace->Reset(Path);
                                                                 RebuildContents();
                                                                 return FReply::Handled();
                                                             })] +
                      SHorizontalBox::Slot().AutoWidth()
                          [SNew(SButton).Text(FText::FromString(TEXT("Remove"))).OnClicked_Lambda([this, Path]() {
                              Workspace->Remove(Path);
                              RebuildContents();
                              return FReply::Handled();
                          })]] +
                 SVerticalBox::Slot()
                     .AutoHeight()[SNew(SExpandableArea)
                                       .InitiallyCollapsed(true)
                                       .HeaderContent()[SNew(STextBlock)
                                                            .Text(FText::FromString(
                                                                TEXT("Advanced: apply JSON fields to this row")))]
                                       .BodyContent()[CreateOptionJsonEditor(Workspace.ToSharedRef(), Path)]] +
                 SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this, Path]() {
                     return FText::FromString(Workspace->ScopeError(Path));
                 })]];
        }
    }
    bool ConfirmProfileReplacement()
    {
        return FMessageDialog::Open(EAppMsgType::YesNo,
                                    LOCTEXT("ReplaceProfile", "Replace this existing user profile?")) ==
               EAppReturnType::Yes;
    }
    void SaveProfile()
    {
        const auto Panel = SharedThis(this);
        Panel->Workspace->SaveProfile([this]() { return ConfirmProfileReplacement(); });
        RebuildContents();
    }
    FReply ImportProfile()
    {
        const auto Panel = SharedThis(this);
        IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
        TArray<FString> Files;
        if (!Desktop ||
            !Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(Panel),
                                     TEXT("Import FBX profile"), FPaths::ProjectSavedDir(), TEXT(""),
                                     TEXT("JSON profile (*.json)|*.json"), EFileDialogFlags::None, Files) ||
            Files.IsEmpty())
            return FReply::Handled();
        Workspace->ImportProfileFile(Files[0], [this]() { return ConfirmProfileReplacement(); });
        RefreshProfiles();
        return FReply::Handled();
    }
    FReply ExportProfile()
    {
        const auto Panel = SharedThis(this);
        IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
        TArray<FString> Files;
        if (!Desktop ||
            !Desktop->SaveFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(Panel),
                                     TEXT("Export FBX profile"), FPaths::ProjectSavedDir(),
                                     Workspace->ProfileId() + TEXT(".json"), TEXT("JSON profile (*.json)|*.json"),
                                     EFileDialogFlags::None, Files) ||
            Files.IsEmpty())
            return FReply::Handled();
        Workspace->ExportProfileFile(Files[0]);
        return FReply::Handled();
    }

    void ShowPng()
    {
        const auto Job = Workspace->History().Selected;
        for (const auto& Item : GetArray(Job, TEXT("items")))
            for (const auto& File : GetArray(GetObject(Item->AsObject(), TEXT("result")), TEXT("files")))
                if (File->AsString().EndsWith(TEXT(".png")))
                {
                    PngBrush = MakeShared<FSlateDynamicImageBrush>(FName(*File->AsString()), FVector2D(120, 120));
                    return;
                }
    }
    TSharedPtr<FFBXExporterWorkspaceController> Workspace;
    TSharedPtr<SVerticalBox> TargetBox;
    TSharedPtr<SWrapBox> MapBox;
    TArray<TSharedPtr<int32>> Resolutions;
    TArray<TSharedPtr<FString>> ProfileIds, JobIds;
    TMap<FString, FString> ProfileNames;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ProfileCombo, JobCombo;
    bool bRebuilding = false;
    TSharedPtr<FSlateDynamicImageBrush> PngBrush;
    TMap<FString, FObject> JobSummaries;
    float RefreshElapsed = 0;
    FProgressBarStyle ProgressStyle;
};
} // namespace
void RegisterTab(TSharedRef<FFBXExporterWorkspaceController> Workspace)
{
    FGlobalTabmanager::Get()
        ->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateLambda([Workspace](const FSpawnTabArgs&) {
                                      auto Widget = SNew(SExporterWorkspace).Workspace(Workspace);
                                      LiveWidget = Widget;
                                      return SNew(SDockTab).TabRole(ETabRole::NomadTab)[Widget];
                                  }))
        .SetDisplayName(LOCTEXT("Tab", "FBX Exporter"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
}
void UnregisterTab()
{
    if (const auto Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabId))
        Tab->RemoveTabFromParent();
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
    LiveWidget.Reset();
}
void AddAssetsToTab(const TArray<FAssetData>& Assets)
{
    FGlobalTabmanager::Get()->TryInvokeTab(TabId);
    if (auto Widget = LiveWidget.Pin())
        Widget->Add(Assets);
}
namespace Testing
{
bool WorkspaceStateRegression(FString& Error)
{
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false);
    const FString A = TEXT("/Engine/BasicShapes/Cube.Cube"), B = TEXT("/Engine/BasicShapes/Sphere.Sphere");
    bool Passed = true;
    auto Check = [&](bool Value, const TCHAR* Message) {
        if (!Value)
        {
            Passed = false;
            Error += FString(Message) + TEXT("\n");
        }
    };
    // Exercise the same clipboard path as the production button without changing
    // the user's clipboard or requiring any historical jobs.
    UnrealToolsWorkspace::FHistoryView EmptyHistory;
    FString Clipboard = TEXT("previous clipboard contents");
    int32 CopyCalls = 0;
    const auto Copy = [&](const FString& Text) {
        Clipboard = Text;
        ++CopyCalls;
    };
    CopyCachedResultJson(EmptyHistory, Copy);
    Check(CopyCalls == 1 && Clipboard.IsEmpty() && !EmptyHistory.Selected,
          TEXT("Copying an empty history safely copies an empty string without a selected job."));
    UnrealToolsWorkspace::FHistoryView CachedHistory;
    CachedHistory.Selected = Ok();
    CachedHistory.Json = TEXT("{\n  \"job_id\": \"cached-job\", \"items\": []\n}");
    CachedHistory.Revision = 7;
    CachedHistory.Reads = 3;
    CopyCachedResultJson(CachedHistory, Copy);
    Check(
        CopyCalls == 2 && Clipboard == CachedHistory.Json && CachedHistory.Revision == 7 && CachedHistory.Reads == 3,
        TEXT(
            "Result copying preserves the cached JSON exactly and does not refresh or serialize the selected object."));
    Model->Add(A);
    Model->Add(B);
    Model->Add(A);
    Check(Model->Targets().Num() == 2, TEXT("Deduplicate"));
    auto Defaults = Model->CopyActiveOptions();
    Defaults->SetNumberField(TEXT("resolution"), 512);
    Model->Replace(Model->Token(), Defaults, TEXT("pbr"));
    Check(GetNumber(Model->CopyOptions(A), TEXT("resolution")) == 2048, TEXT("Defaults do not propagate"));
    Model->Select(A, true);
    Model->ApplySelected();
    Check(GetNumber(Model->CopyOptions(A), TEXT("resolution")) == 512, TEXT("Bulk snapshot"));
    Model->Edit(A);
    const auto Old = Model->Token();
    Model->JsonDraft(Old, TEXT("{\"resolution\":"));
    Check(!Model->BuildRequest(), TEXT("Uncommitted invalid text blocks requests"));
    Model->Edit(B);
    Model->JsonDraft(Old, TEXT("{\"resolution\":4096}"));
    Check(GetNumber(Model->CopyOptions(B), TEXT("resolution")) == 2048, TEXT("Stale scope does not affect B"));
    Model->Reset(A);
    Model->JsonDraft(Model->TokenFor(A), TEXT("{\"maps\":[]}"));
    const auto Request = Model->BuildRequest();
    Check(Request.IsValid() && GetNumber(Model->CopyOptions(A), TEXT("resolution")) == 512,
          TEXT("Partial JSON preserves omitted fields"));
    Model->Edit(A);
    const auto Builds = Model->PresentationBuilds();
    Model->Targets();
    Model->Targets();
    Check(Builds == Model->PresentationBuilds(), TEXT("Read cache is revision based"));
    auto Editor = CreateOptionJsonEditor(Model.ToSharedRef(), A);
    TFunction<TSharedPtr<SWidget>(const TSharedRef<SWidget>&)> FindEditable;
    FindEditable = [&](const TSharedRef<SWidget>& Widget) -> TSharedPtr<SWidget> {
        if (Widget->GetType() == FName(TEXT("SEditableText")))
            return Widget;
        auto* Children = Widget->GetChildren();
        for (int32 I = 0; I < Children->Num(); ++I)
            if (auto Found = FindEditable(Children->GetChildAt(I)))
                return Found;
        return nullptr;
    };
    const auto Editable = FindEditable(Editor);
    Check(Editable.IsValid(), TEXT("Production JSON editor has an editable child"));
    if (Editable)
    {
        Editor->SelectAllText();
        for (const TCHAR C : FString(TEXT("{\"resolution\":")))
            Editable->OnKeyChar(FGeometry(), FCharacterEvent(C, FModifierKeysState(), 0, false));
        Check(!Model->BuildRequest(), TEXT("Raw typing blocks submission before focus loss"));
        Editable->OnFocusLost(FFocusEvent(EFocusCause::Mouse, 0));
        Check(!Model->ScopeError(A).IsEmpty(), TEXT("Invalid text survives focus loss"));
        Model->Reset(A);
        const auto Before = JsonText(Model->CopyOptions(A));
        Editable->OnFocusLost(FFocusEvent(EFocusCause::Mouse, 0));
        Check(Before == JsonText(Model->CopyOptions(A)) && Model->ScopeError(A).IsEmpty(),
              TEXT("Late pre-reset commit cannot restore the invalid draft"));
    }
    // Keep the same production text widget while controller edits advance its
    // display revision. Rebuilding the entire row here would hide stale labels.
    Model->Reset(A);
    auto WorkspaceWidget = SNew(SExporterWorkspace).Workspace(Model);
    WorkspaceWidget->Invalidate(EInvalidateWidgetReason::Layout);
    WorkspaceWidget->SlatePrepass();
    TFunction<TSharedPtr<STextBlock>(TSharedRef<SWidget>)> FindLabel;
    FindLabel = [&](TSharedRef<SWidget> Widget) -> TSharedPtr<STextBlock> {
        if (Widget->GetType() == FName(TEXT("STextBlock")))
        {
            auto Text = StaticCastSharedRef<STextBlock>(Widget);
            if (Text->GetText().ToString() == A)
                return Text;
        }
        auto* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index)
            if (auto Found = FindLabel(Children->GetChildAt(Index)))
                return Found;
        return nullptr;
    };
    const auto Label = FindLabel(WorkspaceWidget);
    Check(Label.IsValid(), TEXT("Find the production FBX row label"));
    if (Label)
    {
        Model->SetAllMaps(Model->TokenFor(A), false);
        WorkspaceWidget->Invalidate(EInvalidateWidgetReason::Layout);
        WorkspaceWidget->SlatePrepass();
        Check(Label->GetText().ToString() == A + TEXT(" *"), TEXT("Map edits immediately mark the existing FBX row"));
        const auto Builds = Model->PresentationBuilds();
        for (int32 Index = 0; Index < 50; ++Index)
        {
            WorkspaceWidget->Invalidate(EInvalidateWidgetReason::Layout);
            WorkspaceWidget->SlatePrepass();
        }
        Check(Model->PresentationBuilds() == Builds, TEXT("FBX label reads reuse revision-cached display data"));
        Model->Reset(A);
        WorkspaceWidget->Invalidate(EInvalidateWidgetReason::Layout);
        WorkspaceWidget->SlatePrepass();
        Check(Label->GetText().ToString() == A, TEXT("Reset clears the same FBX row's modification marker"));
    }
    auto Diagnostic = MakeShared<FJsonObject>();
    Diagnostic->SetStringField(TEXT("record_kind"), TEXT("diagnostic"));
    Diagnostic->SetStringField(TEXT("job_id"), TEXT("owned-diagnostic"));
    Diagnostic->SetStringField(TEXT("phase"), TEXT("issue"));
    Diagnostic->SetStringField(TEXT("source_path"), TEXT("owned.json"));
    Check(UnrealToolsWorkspace::FController::FormatResult(Diagnostic).Contains(TEXT("Raw evidence: Not created yet.")),
          TEXT("Diagnostic raw evidence guidance is preserved"));
    return Passed;
}
TSharedRef<SWidget> CreateWorkspaceWidget(const TArray<FAssetData>& Assets)
{
    auto Model = MakeShared<FFBXExporterWorkspaceController>(false);
    for (const auto& Job : Jobs().GetJobSummaries())
        if (GetString(Job, TEXT("state")) == TEXT("succeeded"))
        {
            Model->SelectJob(GetString(Job, TEXT("job_id")));
            break;
        }
    auto Widget = SNew(SExporterWorkspace).Workspace(Model);
    Widget->Add(Assets);
    Widget->PreparePreviewForTesting();
    return Widget;
}
} // namespace Testing
} // namespace FBXExporter
#undef LOCTEXT_NAMESPACE
