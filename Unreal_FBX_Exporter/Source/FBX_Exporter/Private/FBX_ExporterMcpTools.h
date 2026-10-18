#pragma once

namespace FBXExporter::Mcp
{
	void Register();
	void Unregister();
	namespace Testing { void ForcePostRequests(bool bForce); }
}
