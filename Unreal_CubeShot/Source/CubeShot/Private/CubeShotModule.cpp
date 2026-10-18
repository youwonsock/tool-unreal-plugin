#include "CubeShotModule.h"

#include "SCubeShotPanel.h"
#include "CubeShotMcpTools.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "CubeShotModule"

namespace
{
	const FName CubeShotTabName(TEXT("CubeShot"));
}

void FCubeShotModule::StartupModule()
{
	CubeShot::Mcp::Register();
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		CubeShotTabName,
		FOnSpawnTab::CreateRaw(this, &FCubeShotModule::SpawnCubeShotTab))
		.SetDisplayName(LOCTEXT("CubeShotTabTitle", "CubeShot"))
		.SetTooltipText(LOCTEXT("CubeShotTabTooltip", "Capture a Static Mesh from six axis-aligned directions."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Camera"))
		.SetReuseTabMethod(FOnFindTabToReuse::CreateLambda([](const FTabId&) -> TSharedPtr<SDockTab>
		{
			return FGlobalTabmanager::Get()->FindExistingLiveTab(CubeShotTabName);
		}))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCubeShotModule::RegisterMenus));
}

void FCubeShotModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	CloseCubeShotTab();
	CubeShot::Mcp::Unregister();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CubeShotTabName);
}

void FCubeShotModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	if (ToolsMenu == nullptr)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("CubeShot"));
	Section.Label = LOCTEXT("CubeShotMenuSection", "CubeShot");
	Section.AddMenuEntry(
		TEXT("OpenCubeShot"),
		LOCTEXT("OpenCubeShotLabel", "CubeShot"),
		LOCTEXT("OpenCubeShotTooltip", "Open the six-direction Static Mesh capture tool."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Camera"),
		FUIAction(FExecuteAction::CreateRaw(this, &FCubeShotModule::OpenCubeShotTab)));
}

void FCubeShotModule::OpenCubeShotTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(CubeShotTabName);
}

TSharedRef<SDockTab> FCubeShotModule::SpawnCubeShotTab(const FSpawnTabArgs& Args)
{
	if (const TSharedPtr<SCubeShotPanel> ExistingPanel = ActivePanel.Pin())
	{
		ExistingPanel->Shutdown();
	}
	TSharedRef<SCubeShotPanel> Panel = SNew(SCubeShotPanel);
	ActivePanel = Panel;

	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("CubeShotDockLabel", "CubeShot"))
		[
			Panel
		];
	Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this, &FCubeShotModule::OnCubeShotTabClosed));
	return Tab;
}

void FCubeShotModule::OnCubeShotTabClosed(TSharedRef<SDockTab> ClosedTab)
{
	if (const TSharedPtr<SCubeShotPanel> Panel = ActivePanel.Pin())
	{
		Panel->Shutdown();
	}
	ActivePanel.Reset();
}

void FCubeShotModule::CloseCubeShotTab()
{
	if (FSlateApplication::IsInitialized())
	{
		if (const TSharedPtr<SDockTab> ExistingTab = FGlobalTabmanager::Get()->FindExistingLiveTab(CubeShotTabName))
		{
			// Module shutdown must not leave a live widget behind when its tab stack is
			// locked. RemoveTabFromParent invokes OnTabClosed synchronously and bypasses
			// the user-facing close guard, so the panel is torn down only after Slate has
			// committed to removing the tab.
			ExistingTab->RemoveTabFromParent();
		}
	}

	// Also covers shutdown before Slate has initialized, or a panel which was
	// constructed without ever being attached to a live tab.
	if (const TSharedPtr<SCubeShotPanel> Panel = ActivePanel.Pin())
	{
		Panel->Shutdown();
	}
	ActivePanel.Reset();
}

IMPLEMENT_MODULE(FCubeShotModule, CubeShot)

#undef LOCTEXT_NAMESPACE
