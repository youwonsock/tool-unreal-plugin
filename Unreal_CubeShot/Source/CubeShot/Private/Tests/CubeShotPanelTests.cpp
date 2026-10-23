#include "SCubeShotPanel.h"
#include "CubeShotTypes.h"
#include "CubeShotJobService.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotClosedProfileDialogTest, "CubeShot.Editor.UI.ProfileDialogAfterShutdown",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCubeShotClosedProfileDialogTest::RunTest(const FString&)
{
    using namespace UnrealToolsRuntime;
    if (!FSlateApplication::IsInitialized())
    {
        AddWarning(TEXT("This regression needs an initialized Slate application."));
        return true;
    }
    for (int32 Variant = 0; Variant < 3; ++Variant)
    {
        auto Model = MakeShared<FCubeShotWorkspaceController>();
        auto Panel = SNew(SCubeShotPanel).Workspace(Model);
        const FString Path = TEXT("/Engine/BasicShapes/Cube.Cube");
        Panel->AddTargetForAutomation(Path);
        const auto Id = TEXT("closed_dialog_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        const auto File = FPaths::ProjectSavedDir() / TEXT("CubeShot/Profiles") / (Id + TEXT(".json"));
        ON_SCOPE_EXIT
        {
            IFileManager::Get().Delete(*File);
        };
        if (!TestTrue(TEXT("Create an isolated user profile for the production replacement path"),
                      GetBool(Model->SaveActiveProfile(Id, []() { return true; }), TEXT("success"))))
            return false;
        Panel->ApplyProfileForAutomation(Id);
        const auto Before = JsonText(Model->CopyOptions(Path));
        int32 Prompts = 0;
        Panel->SetProfileReplacementForAutomation([&]() {
            ++Prompts;
            Panel->Shutdown();
            if (Variant == 1)
                Model->Shutdown();
            return Variant != 2;
        });
        Panel->SaveProfileForAutomation();
        TestEqual(TEXT("The production save path reached replacement confirmation"), Prompts, 1);
        TestFalse(TEXT("Returning from confirmation cannot recreate a closed view or preview service"),
                  Panel->HasViewResourcesForAutomation());
        TestEqual(TEXT("Closing a view preserves the row's options"), JsonText(Model->CopyOptions(Path)), Before);
        if (Variant != 1)
            TestTrue(TEXT("A session workspace remains usable after its view closes"), Model->BuildRequest().IsValid());
        Panel->Shutdown();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotLiveRowLabelTest, "CubeShot.Editor.UI.LiveModifiedLabel",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCubeShotLiveRowLabelTest::RunTest(const FString&)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddWarning(TEXT("This regression needs an initialized Slate application."));
        return true;
    }
    auto Model = MakeShared<FCubeShotWorkspaceController>();
    auto Panel = SNew(SCubeShotPanel).Workspace(Model);
    const FString Path = TEXT("/Engine/BasicShapes/Cube.Cube");
    Panel->AddTargetForAutomation(Path);
    Panel->Invalidate(EInvalidateWidgetReason::Layout);
    Panel->SlatePrepass();
    TFunction<TSharedPtr<STextBlock>(TSharedRef<SWidget>)> FindLabel;
    FindLabel = [&](TSharedRef<SWidget> Widget) -> TSharedPtr<STextBlock> {
        if (Widget->GetType() == FName(TEXT("STextBlock")))
        {
            auto Text = StaticCastSharedRef<STextBlock>(Widget);
            if (Text->GetText().ToString() == TEXT("Cube"))
                return Text;
        }
        auto* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index)
            if (auto Found = FindLabel(Children->GetChildAt(Index)))
                return Found;
        return nullptr;
    };
    auto Label = FindLabel(Panel);
    if (!TestTrue(TEXT("Find the production row label"), Label.IsValid()))
        return false;
    Panel->SetResolutionForAutomation(512);
    Panel->Invalidate(EInvalidateWidgetReason::Layout);
    Panel->SlatePrepass();
    TestEqual(TEXT("The same row widget immediately shows a typed edit"), Label->GetText().ToString(),
              FString(TEXT("Cube *")));
    const auto Builds = Model->PresentationBuilds();
    for (int32 Index = 0; Index < 50; ++Index)
    {
        Panel->Invalidate(EInvalidateWidgetReason::Layout);
        Panel->SlatePrepass();
    }
    TestEqual(TEXT("Repeated label reads reuse the presentation cache"), Model->PresentationBuilds(), Builds);
    Model->Reset(Path);
    Panel->Invalidate(EInvalidateWidgetReason::Layout);
    Panel->SlatePrepass();
    TestEqual(TEXT("An explicit reset updates the same row widget"), Label->GetText().ToString(),
              FString(TEXT("Cube")));
    Panel->Shutdown();
    return true;
}

namespace
{
    void VerifyDiagnosticResults(FAutomationTestBase& Test, const TSharedPtr<SCubeShotPanel>& Panel)
    {
        using namespace UnrealToolsRuntime;
        FString Project=FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());Project.ReplaceInline(TEXT("\\"),TEXT("/"));
        if(!Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/")))return;
        const FString Filename=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("CubeShot/Jobs")/
            (TEXT("cubeshot-ui-diagnostic-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".json")));
        if(!Test.TestFalse(TEXT("UI diagnostic fixture never replaces existing metadata"),IFileManager::Get().FileExists(*Filename)))return;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename),true);
        if(!Test.TestTrue(TEXT("Owned malformed UI metadata fixture created"),FFileHelper::SaveStringToFile(TEXT("{ intentionally invalid UI metadata"),*Filename)))return;
        const FString Reason=TEXT("UI diagnostic metadata fixture requires explicit review.");
        const auto Issue=CubeShot::Service::Jobs().RegisterRecordIssue(Filename,Reason);
        if(!Test.TestTrue(TEXT("UI fixture has a typed diagnostic record"),GetBool(Issue,TEXT("success"))&&GetString(Issue,TEXT("record_kind"))==TEXT("diagnostic")))return;
        const FString Id=GetString(Issue,TEXT("job_id"));
        auto CheckText=[&](const FObject& Record)
        {
            Panel->SelectJobForAutomation(Id);
            const FString Text=Panel->ResultsForAutomation().ToString();
            Test.TestEqual(TEXT("UI diagnostic selection keeps the stable ID"),Panel->SelectedJobForAutomation(),Id);
            Test.TestTrue(TEXT("Diagnostic result visibly identifies the selected ID"),Text.Contains(TEXT("Diagnostic ID: ")+Id));
            Test.TestTrue(TEXT("Diagnostic result visibly identifies its phase"),Text.Contains(TEXT("Phase: ")+GetString(Record,TEXT("phase"))));
            Test.TestTrue(TEXT("Diagnostic result visibly identifies source metadata"),Text.Contains(TEXT("Source metadata: ")+GetString(Record,TEXT("source_path"))));
            Test.TestTrue(TEXT("Diagnostic result visibly explains the metadata error"),Text.Contains(Reason));
            Test.TestFalse(TEXT("Metadata diagnostic does not claim a capture completion count"),Text.Contains(TEXT("Completed ")));
            Test.TestFalse(TEXT("Metadata diagnostic does not request dirty-package resolution"),Text.Contains(TEXT("Resolve dirty packages")));
            if(!GetString(Record,TEXT("raw_path")).IsEmpty())
                Test.TestTrue(TEXT("Diagnostic result visibly identifies preserved raw evidence"),Text.Contains(TEXT("Preserved raw evidence: ")+GetString(Record,TEXT("raw_path"))));
        };
        CheckText(Issue);
        Test.TestTrue(TEXT("Unresolved diagnostic displays metadata review guidance"),Panel->ResultsForAutomation().ToString().Contains(TEXT("Metadata review required.")));
        auto Args=MakeShared<FJsonObject>();Args->SetStringField(TEXT("job_id"),Id);Args->SetStringField(TEXT("action"),TEXT("acknowledge_recovery"));
        const auto Acknowledged=CubeShot::Service::Jobs().Control(Args);
        if(!Test.TestTrue(TEXT("Only the owned UI metadata fixture is acknowledged"),GetBool(Acknowledged,TEXT("success"))&&GetString(Acknowledged,TEXT("phase"))==TEXT("acknowledged")))return;
        Test.TestFalse(TEXT("Acknowledgement removed the owned malformed source"),IFileManager::Get().FileExists(*Filename));
        Test.TestFalse(TEXT("Acknowledged UI fixture exposes a raw evidence path"),GetString(Acknowledged,TEXT("raw_path")).IsEmpty());
        CheckText(Acknowledged);
        Test.TestTrue(TEXT("Acknowledged diagnostic displays completed metadata review"),Panel->ResultsForAutomation().ToString().Contains(TEXT("Metadata review acknowledged.")));
    }

    class FCubeShotLayoutScreenshotCommand final : public IAutomationLatentCommand
    {
        FAutomationTestBase* Test;
        TSharedPtr<SWindow> Window;
        TSharedPtr<SCubeShotPanel> Panel;
        TSharedPtr<FCubeShotWorkspaceController> Workspace;
        double Deadline=0;
        int32 Phase=0;
        FText OriginalResults;
        FString SelectedJobId;
    public:
        explicit FCubeShotLayoutScreenshotCommand(FAutomationTestBase* InTest):Test(InTest) {}
        virtual bool Update() override
        {
            if(Phase==0)
            {
                if(!Workspace)Workspace=MakeShared<FCubeShotWorkspaceController>();Panel=SNew(SCubeShotPanel).Workspace(Workspace);Panel->AddTargetForAutomation(TEXT("/Engine/BasicShapes/Cube.Cube"));
                const auto History=CubeShot::Service::Jobs().GetJobSummaries();
                if(!History.IsEmpty())Panel->SelectJobForAutomation(History[FMath::Min(1,History.Num()-1)]->GetStringField(TEXT("job_id")));
                Window=SNew(SWindow).Title(FText::FromString(TEXT("CubeShot layout verification"))).ClientSize(FVector2D(1280,720)).SupportsMaximize(false).SupportsMinimize(false)[Panel.ToSharedRef()];
                FSlateApplication::Get().AddWindow(Window.ToSharedRef());Deadline=FPlatformTime::Seconds()+8;Phase=1;return false;
            }
            if(FPlatformTime::Seconds()<Deadline)return false;
            Test->TestTrue(TEXT("All six preview buffers contain visible colored pixels"),Panel->HasCompletePreviewPixelsForAutomation());
            TArray<FColor> Painted;FIntVector PaintedSize;
            if(Test->TestTrue(TEXT("Actual Slate preview screenshot"),Panel->TakePaintedPreviewForAutomation(Painted,PaintedSize))&&PaintedSize.X>16&&PaintedSize.Y>16)
            {
                const FColor CheckerA=Painted[PaintedSize.X+1];const FColor CheckerB=Painted[PaintedSize.X+PaintedSize.X/8+2];
                auto Distance=[](const FColor& A,const FColor& B){return FMath::Max3(FMath::Abs(static_cast<int32>(A.R)-B.R),FMath::Abs(static_cast<int32>(A.G)-B.G),FMath::Abs(static_cast<int32>(A.B)-B.B));};
                int64 MeshPixels=0;for(const FColor& Pixel:Painted)MeshPixels+=Distance(Pixel,CheckerA)>4&&Distance(Pixel,CheckerB)>4;
                Test->TestTrue(TEXT("Slate actually paints the mesh over checkerboard"),MeshPixels>Painted.Num()/20);
            }
            TArray<FColor> Pixels;FIntVector Size;
            if(Test->TestTrue(TEXT("Slate screenshot"),FSlateApplication::Get().TakeScreenshot(Panel.ToSharedRef(),Pixels,Size)))
            {
                Test->TestTrue(TEXT("Screenshot captures complete workspace width"),Size.X >= (Phase==1?1100:1700));
                Test->TestTrue(TEXT("Screenshot captures complete workspace height"),Size.Y >= (Phase==1?600:900));
                FImage Image;Image.Init(Size.X,Size.Y,ERawImageFormat::BGRA8,EGammaSpace::sRGB);FMemory::Memcpy(Image.RawData.GetData(),Pixels.GetData(),Pixels.Num()*sizeof(FColor));
                const FString Folder=FPaths::ProjectSavedDir()/TEXT("UnrealTools/V12/UI");IFileManager::Get().MakeDirectory(*Folder,true);
                const FString Filename=Folder/(Phase==1?TEXT("CubeShot_1280x720.png"):TEXT("CubeShot_1920x1080.png"));
                Test->TestTrue(TEXT("UI screenshot saved"),FImageUtils::SaveImageByExtension(*Filename,Image,100));Test->AddInfo(TEXT("Saved actual Slate layout: ")+Filename);
            }
            if(Phase==1)
            {
                OriginalResults=Panel->ResultsForAutomation();
                SelectedJobId=Panel->SelectedJobForAutomation();
                if(!SelectedJobId.IsEmpty())
                {
                    auto Selector=MakeShared<FJsonObject>();Selector->SetStringField(TEXT("job_id"),SelectedJobId);
                    const auto Selected=CubeShot::Service::Jobs().Status(Selector);
                    Test->TestTrue(TEXT("Selected historical job still exists"),UnrealToolsRuntime::GetBool(Selected,TEXT("success")));
                    const FString State=UnrealToolsRuntime::GetString(Selected,TEXT("state"));
                    Test->TestTrue(TEXT("Selected existing job result is visible"),!OriginalResults.IsEmpty()&&OriginalResults.ToString().Contains(State));
                    if(State!=TEXT("queued")&&State!=TEXT("running"))
                        Test->TestFalse(TEXT("Terminal history does not claim cancellation is still pending"),OriginalResults.ToString().Contains(TEXT("must reach a safe boundary")));
                }
                VerifyDiagnosticResults(*Test,Panel);
                Panel->SelectJobForAutomation(SelectedJobId);
                Test->TestEqual(TEXT("Diagnostic selection leaves normal job result formatting unchanged"),Panel->ResultsForAutomation().ToString(),OriginalResults.ToString());
                // Close the observer, reconstruct the real panel and verify results survive reopening.
                Panel->Shutdown();if(!Workspace)Workspace=MakeShared<FCubeShotWorkspaceController>();Panel=SNew(SCubeShotPanel).Workspace(Workspace);Panel->AddTargetForAutomation(TEXT("/Engine/BasicShapes/Cube.Cube"));Window->SetContent(Panel.ToSharedRef());
                Test->TestEqual(TEXT("Result text survives tab reopening"),Panel->ResultsForAutomation().ToString(),OriginalResults.ToString());
                Test->TestEqual(TEXT("Historical selection survives tab reopening"),Panel->SelectedJobForAutomation(),SelectedJobId);
                Window->Resize(FVector2D(1920,1080));Deadline=FPlatformTime::Seconds()+8;Phase=2;return false;
            }
            Panel->Shutdown();Window->RequestDestroyWindow();Window.Reset();Panel.Reset();return true;
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotPanelLayoutTest,"CubeShot.Editor.UI.LayoutScreenshots",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotPanelLayoutTest::RunTest(const FString&)
{
    FText Error;if(!FSlateApplication::IsInitialized()||!CubeShot::CanExecute(Error)){AddWarning(TEXT("UI screenshot needs a rendering Editor: ")+Error.ToString());return true;}
    ADD_LATENT_AUTOMATION_COMMAND(FCubeShotLayoutScreenshotCommand(this));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubeShotMultiSelectionTest,"CubeShot.Editor.UI.MultiSelectionAndWholePreflight",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FCubeShotMultiSelectionTest::RunTest(const FString&)
{
    FString Project=FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());Project.ReplaceInline(TEXT("\\"),TEXT("/"));
    if(!Project.Contains(TEXT("/Saved/UnrealTools/V12/Projects/"))){AddWarning(TEXT("UI model mutation regression is restricted to owned QA projects."));return true;}
    auto Workspace=MakeShared<FCubeShotWorkspaceController>();auto Panel=SNew(SCubeShotPanel).Workspace(Workspace);const auto BeforePaths=Panel->TargetPathsForAutomation();Panel->ClearTargetsForAutomation();
    const FString Cube=TEXT("/Engine/BasicShapes/Cube.Cube"),Sphere=TEXT("/Engine/BasicShapes/Sphere.Sphere"),Cone=TEXT("/Engine/BasicShapes/Cone.Cone"),Cylinder=TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
    for(const FString& Path:{Cube,Sphere,Cone,Cylinder})Panel->AddTargetForAutomation(Path);Panel->AddTargetForAutomation(Cube);
    TestEqual(TEXT("Multiple targets deduplicate"),Panel->TargetPathsForAutomation().Num(),4);
    Panel->SetSelectedTargetsForAutomation({Cube,Cone});Panel->SetPreviewTargetForAutomation(1);TestEqual(TEXT("Preview active row separate from checked edit rows"),Panel->PreviewTargetForAutomation(),Sphere);
    Panel->SetPreviewTargetForAutomation(0);Panel->SetResolutionForAutomation(777);Panel->ApplyOptionsToSelectedForAutomation();
    TestEqual(TEXT("First checked row receives bulk edit"),Panel->TargetOptionsForAutomation(Cube)->GetIntegerField(TEXT("resolution")),777);
    TestEqual(TEXT("Second checked row receives bulk edit"),Panel->TargetOptionsForAutomation(Cone)->GetIntegerField(TEXT("resolution")),777);
    TestEqual(TEXT("Unchecked row retains its own resolved options"),Panel->TargetOptionsForAutomation(Sphere)->GetIntegerField(TEXT("resolution")),1024);
    Panel->MoveTargetForAutomation(2,-1);
    TestEqual(TEXT("Moving an unpreviewed row preserves active preview"),Panel->PreviewTargetForAutomation(),Cube);
    Panel->SetResolutionForAutomation(888);
    TestEqual(TEXT("Editing after reorder updates active preview target"),Panel->TargetOptionsForAutomation(Cube)->GetIntegerField(TEXT("resolution")),888);
    TestEqual(TEXT("Unpreviewed moved row keeps its options"),Panel->TargetOptionsForAutomation(Cone)->GetIntegerField(TEXT("resolution")),777);
    Panel->MoveTargetForAutomation(2,-1);Panel->MoveTargetForAutomation(1,-1);
    TestEqual(TEXT("Reorder across active index preserves preview binding"),Panel->PreviewTargetForAutomation(),Cube);
    Panel->SetResolutionForAutomation(889);
    TestEqual(TEXT("Active target index follows its path through swap"),Panel->TargetOptionsForAutomation(Cube)->GetIntegerField(TEXT("resolution")),889);
    TestEqual(TEXT("Other target not overwritten after crossing reorder"),Panel->TargetOptionsForAutomation(Sphere)->GetIntegerField(TEXT("resolution")),1024);
    Panel->SetPreviewTargetForAutomation(0);Panel->SetResolutionForAutomation(333);
    Panel->PickTargetForAutomation(Cube);
    TestEqual(TEXT("Picker reselects existing target by path"),Panel->PreviewTargetForAutomation(),Cube);
    Panel->SetResolutionForAutomation(900);
    TestEqual(TEXT("Editing picked existing target updates its own row"),Panel->TargetOptionsForAutomation(Cube)->GetIntegerField(TEXT("resolution")),900);
    TestEqual(TEXT("Previously active row stays unchanged after picker deduplication"),Panel->TargetOptionsForAutomation(Sphere)->GetIntegerField(TEXT("resolution")),333);
    Panel->RemoveSelectedForAutomation();const auto Remaining=Panel->TargetPathsForAutomation();TestTrue(TEXT("Removing checked rows preserves remaining run order"),Remaining==TArray<FString>{Sphere,Cylinder});
    Panel->SetSelectedTargetsForAutomation({});Panel->RemoveSelectedForAutomation();TestTrue(TEXT("Cleared selection removes no rows"),Panel->TargetPathsForAutomation()==Remaining);
    Panel->ClearTargetsForAutomation();const FString BadA=TEXT("/Game/UnrealToolsQA/Bad..Bad"),BadB=TEXT("/Game/UnrealToolsQA/AlsoBad..Bad");
    Panel->AddTargetForAutomation(BadA);Panel->AddTargetForAutomation(BadB);Panel->AddTargetForAutomation(Sphere);
    const int32 Before=CubeShot::Service::Jobs().GetJobs().Num();Panel->StartBatchForAutomation();const FString Plan=Panel->OutputPlanForAutomation().ToString();
    TestTrue(TEXT("Whole preflight includes both errors and subsequent valid target"),Plan.Contains(BadA)&&Plan.Contains(BadB)&&Plan.Contains(Sphere));
    TestEqual(TEXT("Any preflight error refuses entire job admission"),CubeShot::Service::Jobs().GetJobs().Num(),Before);
    // A profile may be removed by MCP while a panel still remembers it. Reopening must recover.
    const FString ProfileId=TEXT("ui_deleted_")+FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    auto Profile=MakeShared<FJsonObject>();Profile->SetNumberField(TEXT("schema_version"),1);Profile->SetStringField(TEXT("provider_id"),TEXT("cubeshot"));Profile->SetStringField(TEXT("id"),ProfileId);Profile->SetStringField(TEXT("display_name"),TEXT("UI deleted fixture"));Profile->SetObjectField(TEXT("options"),CubeShot::Service::DefaultOptions());
    auto Save=MakeShared<FJsonObject>();Save->SetStringField(TEXT("action"),TEXT("save"));Save->SetObjectField(TEXT("profile"),Profile);TestTrue(TEXT("User profile saved"),UnrealToolsRuntime::GetBool(CubeShot::Service::Profiles().Handle(Save),TEXT("success")));
    Panel->ApplyProfileForAutomation(ProfileId);TestEqual(TEXT("Fixture profile active"),Panel->ProfileForAutomation(),ProfileId);
    auto Delete=MakeShared<FJsonObject>();Delete->SetStringField(TEXT("action"),TEXT("delete"));Delete->SetStringField(TEXT("id"),ProfileId);
    const auto Deleted=IModelContextProtocolModule::GetChecked().FindTool(TEXT("cubeshot_profiles"))->Run(Delete);const TSharedPtr<FJsonObject>* Content;
    TestTrue(TEXT("MCP deletes remembered profile"),Deleted.JsonObject->TryGetObjectField(TEXT("structuredContent"),Content)&&UnrealToolsRuntime::GetBool(*Content,TEXT("success")));
    Panel->Shutdown();Panel=SNew(SCubeShotPanel).Workspace(Workspace);TestEqual(TEXT("Deleted persisted profile falls back on reopening"),Panel->ProfileForAutomation(),FString(TEXT("editor_default")));
    Panel->ClearTargetsForAutomation();for(const FString& Path:BeforePaths)Panel->AddTargetForAutomation(Path);Panel->Shutdown();return true;
}
#endif
