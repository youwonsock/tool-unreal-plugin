#pragma once

#include "Modules/ModuleManager.h"

class FFBXExporterModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	// Posted MCP callbacks and codec workers must finish before code is unloaded.
	virtual bool SupportsDynamicReloading() override { return false; }

private:
	void RegisterMenus();
};
