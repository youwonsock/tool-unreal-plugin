import json
import subprocess
import sys
import tomllib
from unittest.mock import patch

from support import MCP, ProjectCase, server
from unreal_tools_mcp import config, registry
import sync_runtime


class ConfigTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.cube = self.add_plugin("Unreal_CubeShot")

    def install(self, *args, anchor=None):
        return self.quiet(config.main, ["--project", str(self.project), *args], anchor=anchor or MCP / "setup_mcp.py")

    def result(self):
        return config.read_config(self.project)["mcp_servers"]

    def test_single_and_both_plugins_and_each_entrypoint_are_idempotent(self):
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.result()["unreal_tools"]["enabled_tools"], list(self.cube.tools))
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        for anchor in [MCP / "setup_mcp.py", self.cube.manifest.with_name("setup_mcp.py"), fbx.manifest.with_name("setup_mcp.py")]:
            self.assertEqual(self.install(anchor=anchor), 0)
            self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 17)
            self.assertEqual(config.prepare(self.project)[2], {})

    def test_disabled_plugin_and_editor_platform_filters(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        self.assertEqual(self.install(), 0)
        self.write_project({"Plugins": [{"Name": "CubeShot", "Enabled": False}]})
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.result()["unreal_tools"]["enabled_tools"], list(fbx.tools))
        self.assertEqual(self.install(anchor=self.cube.manifest.with_name("setup_mcp.py")), 1)
        for restriction in ({"TargetAllowList": ["Game"]}, {"PlatformDenyList": [registry.PLATFORM]}):
            self.write_project({"Plugins": [{"Name": "CubeShot", "Enabled": True, **restriction}]})
            self.assertEqual([p.id for p in registry.active_providers(self.project)[1]], [fbx.id])

    def test_external_directory_and_repeated_roots(self):
        external = self.root.parent / "Shared Tools"
        self.add_plugin("Unreal_FBX_Exporter", root=external)
        self.write_project({"AdditionalPluginDirectories": ["../Shared Tools", str(external), "Plugins"]})
        self.assertEqual(self.install(), 0)
        self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 17)

    def test_project_plugin_without_enabled_by_default_matches_unreal(self):
        descriptor = self.cube.manifest.parents[1] / "CubeShot.uplugin"
        data = json.loads(descriptor.read_text("utf-8"))
        data.pop("EnabledByDefault")
        descriptor.write_text(json.dumps(data), "utf-8")
        self.assertEqual(self.install(), 0)
        self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 9)

    def test_duplicate_ids_and_tool_names_fail_before_any_write(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        original = json.loads(fbx.manifest.read_text("utf-8"))
        for change in ({"id": "cubeshot"}, {"tools": [fbx.status_tool, self.cube.status_tool]}):
            fbx.manifest.write_text(json.dumps({**original, **change}), "utf-8")
            self.assertEqual(self.install(), 1)
            self.assertFalse((self.root / ".codex").exists())
            self.assertFalse((self.root / "Config").exists())

    def test_legacy_migration_preserves_address_timeouts_other_tables_and_backup(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        unrelated = '# keep comment\nmodel = "keep"\n[mcp_servers.other]\ncommand = "keep-server"\n'
        original = unrelated + server(self.cube.id, self.cube.tools, url="http://localhost:8123/tools/mcp",
                                      startup_timeout_sec=37, tool_timeout_sec=240)
        original += server(fbx.id, fbx.tools, url="http://127.0.0.1:8123/tools/mcp", tool_timeout_sec=360)
        original += '[[profiles.custom.rules]]\nname = "keep me"\n'
        path = self.write(".codex/config.toml", original)
        self.assertEqual(self.install(), 0)
        self.assertTrue(path.read_text("utf-8").startswith(unrelated))
        result = self.result()
        self.assertEqual(set(result), {"other", "unreal_tools"})
        self.assertEqual(result["unreal_tools"]["url"], "http://localhost:8123/tools/mcp")
        self.assertEqual(result["unreal_tools"]["startup_timeout_sec"], 37)
        self.assertEqual(result["unreal_tools"]["tool_timeout_sec"], 360)
        self.assertEqual(path.with_name(path.name + ".unreal-tools.bak").read_text("utf-8"), original)
        self.assertEqual(tomllib.loads(path.read_text("utf-8"))["profiles"]["custom"]["rules"], [{"name": "keep me"}])

    def test_conflicting_endpoints_or_restrictions_make_no_changes(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        cases = [server(self.cube.id, self.cube.tools) + server(fbx.id, fbx.tools, url="http://127.0.0.1:8001/mcp"),
                 server(self.cube.id, self.cube.tools[:1]),
                 server(self.cube.id, self.cube.tools, disabled_tools=[self.cube.tools[-1]]),
                 server(self.cube.id, self.cube.tools, enabled=False),
                 server(self.cube.id, self.cube.tools, custom_field="keep")]
        for original in cases:
            path = self.write(".codex/config.toml", original)
            self.assertEqual(self.install(), 1)
            self.assertEqual(path.read_text("utf-8"), original)
            self.assertFalse((self.root / "Config").exists())

    def test_explicit_replacement_and_dry_run_are_scoped(self):
        self.add_plugin("Unreal_FBX_Exporter")
        original = server(self.cube.id, [self.cube.status_tool]) + '[mcp_servers.other]\ncommand="keep"\n'
        path = self.write(".codex/config.toml", original)
        anchor = self.cube.manifest.with_name("setup_mcp.py")
        self.assertEqual(self.install("--replace-cubeshot", "--dry-run", anchor=anchor), 0)
        self.assertEqual(path.read_text("utf-8"), original)
        self.assertFalse((self.root / "Config").exists())
        self.assertEqual(self.install("--replace-cubeshot", anchor=anchor), 0)
        self.assertEqual(self.result()["other"], {"command": "keep"})
        path.write_text(server("unreal_tools", [self.cube.status_tool]), "utf-8")
        self.assertEqual(self.install("--replace-cubeshot", anchor=anchor), 1)
        self.assertEqual(self.install("--replace-owned-config"), 0)

    def test_saved_ini_overrides_defaults_and_keeps_unrelated_keys(self):
        section = registry.SETTINGS_SECTION
        self.write("Config/DefaultEditorPerProjectUserSettings.ini", f"[{section}]\nServerPortNumber=8000\n")
        platform = {"win32": "WindowsEditor", "darwin": "MacEditor"}.get(sys.platform, "LinuxEditor")
        path = self.write(f"Saved/Config/{platform}/EditorPerProjectUserSettings.ini",
                          f"[{section}]\nServerPortNumber=8134\nServerUrlPath=/tools\nbAutoStartServer=False\n"
                          "bEnableToolSearch=False\n\n[Other]\nKeep=42\n")
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.result()["unreal_tools"]["url"], "http://127.0.0.1:8134/tools")
        self.assertIn("bEnableToolSearch=False", path.read_text("utf-8"))
        self.assertIn("[Other]\nKeep=42", path.read_text("utf-8"))
        self.assertEqual(config.ini_settings(path.read_text("utf-8"))["bAutoStartServer"], "True")
        self.assertEqual(config.prepare(self.project)[2], {})

    def test_editor_client_conflict_requires_explicit_override(self):
        self.write(".codex/config.toml", server(self.cube.id, self.cube.tools))
        ini = self.write("Config/DefaultEditorPerProjectUserSettings.ini",
                         f"[{registry.SETTINGS_SECTION}]\nServerPortNumber=8018\n")
        self.assertEqual(self.install(), 1)
        self.assertIn("8018", ini.read_text("utf-8"))
        self.assertEqual(self.install("--port", "8018"), 0)

    def test_invalid_endpoint_timeout_and_disabled_engine_fail(self):
        for args in [("--port", "0"), ("--port", "70000"), ("--url-path", "/bad?query"), ("--url-path", "/")]:
            self.assertEqual(self.install(*args), 1)
        self.write(".codex/config.toml", server(self.cube.id, self.cube.tools, tool_timeout_sec=-1))
        self.assertEqual(self.install(), 1)
        self.write_project({"Plugins": [{"Name": "ModelContextProtocol", "Enabled": False}]})
        self.assertEqual(self.install(), 1)
        self.assertFalse((self.root / "Config").exists())

    def test_ambiguous_project_is_rejected(self):
        self.write("Second.uproject", "{}")
        with self.assertRaises(ValueError):
            registry.resolve_project(self.root, anchor=MCP / "setup_mcp.py")
        self.assertEqual(registry.resolve_project(self.project, anchor=MCP / "setup_mcp.py"), self.project)

    def test_atomic_replace_failure_retains_original(self):
        path = self.write(".codex/config.toml", "# original\n")
        with patch.object(config.os, "replace", side_effect=OSError("locked")):
            with self.assertRaises(OSError):
                self.quiet(config.write_changes, {path: "# changed\n"})
        self.assertEqual(path.read_text("utf-8"), "# original\n")
        self.assertEqual(len(list(path.parent.glob("config.toml.unreal-tools.*.bak"))), 1)
        self.assertEqual(len(list(path.parent.iterdir())), 3)

    def test_nonstandard_owned_table_is_preserved_on_error(self):
        path = self.write(".codex/config.toml", '[mcp_servers."cubeshot"]\nurl="http://localhost:8000/mcp"\n')
        original = path.read_bytes()
        self.assertEqual(self.install("--replace-owned-config"), 1)
        self.assertEqual(path.read_bytes(), original)

    def test_sync_check_detects_and_repairs_drift(self):
        target = self.cube.manifest.parent / "unreal_tools_mcp/client.py"
        target.write_text("# drift", "utf-8")
        self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins", check=True), 1)
        self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins"), 0)
        self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins", check=True), 0)


class PortableTests(ProjectCase):
    def test_each_plugin_without_parent_repository_or_sdk(self):
        # A separate process proves imports never escape the copied plugin.
        for name, count in [("Unreal_CubeShot", 9), ("Unreal_FBX_Exporter", 8)]:
            with self.subTest(plugin=name):
                provider = self.add_plugin(name)
                self.write_project({"Plugins": [{"Name": other.plugin, "Enabled": other.id == provider.id}
                                                for other in registry.discover(self.project)]})
                result = subprocess.run([sys.executable, "-S", str(provider.manifest.with_name("setup_mcp.py")),
                                         "--port", "8017"], cwd=self.temp.name, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                tools = config.read_config(self.project)["mcp_servers"]["unreal_tools"]["enabled_tools"]
                self.assertEqual(len(tools), count)
                help_result = subprocess.run([sys.executable, "-S", str(provider.manifest.with_name("call_mcp.py")), "--help"],
                                             cwd=self.temp.name, capture_output=True, text=True)
                self.assertEqual(help_result.returncode, 0, help_result.stderr)
