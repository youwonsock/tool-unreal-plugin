#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SCubeShotPanel;
class SDockTab;
class FSpawnTabArgs;

class FCubeShotModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void OpenCubeShotTab();
	TSharedRef<SDockTab> SpawnCubeShotTab(const FSpawnTabArgs& Args);
	void OnCubeShotTabClosed(TSharedRef<SDockTab> ClosedTab);
	void CloseCubeShotTab();

	TWeakPtr<SCubeShotPanel> ActivePanel;
};
