#include "FBX_ExporterModule.h"

#include "FBX_ExporterMcpTools.h"
#include "FBX_ExporterService.h"
#include "FBX_ExporterWorkspaceController.h"
#include "ToolMenus.h"

void FFBXExporterModule::StartupModule()
{
    FBXExporter::InitializeJobs();
    Workspace = MakeShared<FFBXExporterWorkspaceController>();
    FBXExporter::RegisterTab(Workspace.ToSharedRef());
    FBXExporter::Mcp::Register();
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFBXExporterModule::RegisterMenus));
}

void FFBXExporterModule::ShutdownModule()
{
    FBXExporter::Mcp::Unregister();
    FBXExporter::UnregisterTab();
    Workspace->Shutdown();
    Workspace.Reset();
    FBXExporter::ShutdownJobs();
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
}

void FFBXExporterModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
    {
        FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("FBX_Exporter"));
        Section.Label = NSLOCTEXT("FBXExporter", "ToolsSection", "FBX Exporter");
        Section.AddMenuEntry(TEXT("FBX_Exporter.Open"), NSLOCTEXT("FBXExporter", "OpenTool", "FBX Exporter"),
                             NSLOCTEXT("FBXExporter", "OpenToolHelp", "Open the FBX export workspace."), FSlateIcon(),
                             FUIAction(FExecuteAction::CreateLambda([]() { FBXExporter::AddAssetsToTab({}); })));
    }
    UToolMenu* ContentBrowserMenu =
        UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.AssetActionsSubMenu"));
    if (ContentBrowserMenu)
    {
        ContentBrowserMenu->AddDynamicSection(
            TEXT("FBX_Exporter"), FNewToolMenuDelegate::CreateStatic(&FBXExporter::PopulateContentBrowserMenu));
    }
}

IMPLEMENT_MODULE(FFBXExporterModule, FBX_Exporter)
