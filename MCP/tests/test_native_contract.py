"""Static native/manifest contract checks; these do not run an Unreal tool."""
import json
import re

from support import REPO, ProjectCase


class NativeContractTests(ProjectCase):
    def test_shared_automation_registry_expectations_cover_all_declared_cases(self):
        source = (REPO / "Shared/Native/UnrealToolsRuntimeTests.inl").read_text("utf-8")
        declared = re.findall(r'^UNREAL_TOOLS_RUNTIME_IMPLEMENT\((\w+),', source, re.MULTILINE)
        expected = re.findall(r'TEXT\("(Runtime\w+Test)"\)', source)
        count = re.search(r'all shared cases are registered"\), Count, (\d+)\)', source)
        self.assertIsNotNone(count)
        self.assertEqual(len(declared), int(count.group(1)))
        self.assertCountEqual(expected, declared)

    def test_registered_tool_sets_match_each_deployed_manifest(self):
        for directory, module, expected in [("Unreal_CubeShot", "CubeShot", 8),
                                            ("Unreal_FBX_Exporter", "FBX_Exporter", 7)]:
            with self.subTest(plugin=module):
                root = REPO / directory
                manifest = json.loads((root / "MCP/manifest.json").read_text("utf-8"))
                source = (root / f"Source/{module}/Private/{module}McpTools.cpp").read_text("utf-8")
                registered = re.findall(r'\bAdd\(TEXT\("([a-z_]+)"\)', source)
                self.assertEqual(len(registered), expected)
                self.assertEqual(len(set(registered)), expected)
                self.assertEqual(set(registered), set(manifest["tools"]))
                self.assertNotIn("cubeshot_generate", registered)
                self.assertNotIn("fbx_exporter_export", registered)

    def test_status_descriptor_and_manifest_advertise_the_same_major_version(self):
        for directory, module in [("Unreal_CubeShot", "CubeShot"), ("Unreal_FBX_Exporter", "FBX_Exporter")]:
            with self.subTest(plugin=module):
                root = REPO / directory
                manifest = json.loads((root / "MCP/manifest.json").read_text("utf-8"))
                descriptor = json.loads((root / f"{module}.uplugin").read_text("utf-8"))
                source = (root / f"Source/{module}/Private/{module}McpTools.cpp").read_text("utf-8")
                advertised = re.findall(r'SetStringField\(TEXT\("(?:api_version|plugin_version)"\),\s*TEXT\("([0-9.]+)"\)\)', source)
                self.assertEqual(advertised, ["2.0.0", "2.0.0"])
                self.assertEqual(manifest["api_version"], descriptor["VersionName"])
                self.assertEqual(manifest["minimum_runtime_version"], "2.0.0")
