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
            self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 15)
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

    def test_disabled_old_plugin_does_not_block_current_setup(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        originals = {provider.id: provider.manifest.read_text("utf-8") for provider in [self.cube, fbx]}
        for current, disabled in [(self.cube, fbx), (fbx, self.cube)]:
            for previous in ["1.2.0", None]:
                with self.subTest(current=current.id, previous=previous):
                    current.manifest.write_text(originals[current.id], "utf-8")
                    older = json.loads(originals[disabled.id])
                    for key in ["api_version", "minimum_runtime_version"]:
                        if previous is None:
                            older.pop(key)
                        else:
                            older[key] = previous
                    source = json.dumps(older)
                    disabled.manifest.write_text(source, "utf-8")
                    self.write_project({"Plugins": [{"Name": current.plugin, "Enabled": True},
                                                    {"Name": disabled.plugin, "Enabled": False}]})
                    self.assertEqual([provider.id for provider in registry.discover(self.project)], [current.id])
                    for anchor in [MCP / "setup_mcp.py", current.manifest.with_name("setup_mcp.py")]:
                        self.assertEqual(self.install(anchor=anchor), 0)
                        self.assertEqual(self.result()["unreal_tools"]["enabled_tools"], list(current.tools))
                    self.assertEqual(config.prepare(self.project)[2], {})
                    self.assertEqual(disabled.manifest.read_text("utf-8"), source)
                    with self.assertRaisesRegex(ValueError, "missing or disabled"):
                        registry.active_providers(self.project, required=disabled.id)

    def test_filtered_old_manifest_is_not_read(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        descriptor = fbx.manifest.parents[1] / "FBX_Exporter.uplugin"
        original = json.loads(descriptor.read_text("utf-8"))
        # A filtered provider need not even have a readable manifest.
        fbx.manifest.write_text("{unsupported", "utf-8")
        cases = [({"EnabledByDefault": False}, {}),
                 ({"SupportedTargetPlatforms": ["OtherPlatform"]}, {}),
                 ({"Modules": [{"Name": "FBX_Exporter", "Type": "Editor", "TargetAllowList": ["Game"]}]}, {}),
                 ({}, {"PlatformAllowList": ["OtherPlatform"]}),
                 ({}, {"PlatformDenyList": [registry.PLATFORM]}),
                 ({}, {"TargetAllowList": ["Game"]}),
                 ({}, {"TargetDenyList": ["Editor"]})]
        for settings, override in cases:
            with self.subTest(settings=settings, override=override):
                descriptor.write_text(json.dumps({**original, **settings}), "utf-8")
                self.write_project({"Plugins": [{"Name": fbx.plugin, **override}]})
                self.assertEqual(self.install(), 0)
                self.assertEqual(self.result()["unreal_tools"]["enabled_tools"], list(self.cube.tools))

    def test_active_old_manifest_is_rejected_before_any_write(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        older = json.loads(fbx.manifest.read_text("utf-8"))
        older.update(api_version="1.2.0", minimum_runtime_version="1.2.0")
        fbx.manifest.write_text(json.dumps(older), "utf-8")
        for anchor in [MCP / "setup_mcp.py", self.cube.manifest.with_name("setup_mcp.py")]:
            with self.subTest(anchor=anchor):
                self.assertEqual(self.install(anchor=anchor), 1)
                self.assertFalse((self.root / ".codex").exists())
                self.assertFalse((self.root / "Config").exists())

    def test_descriptor_filters_use_only_current_keys(self):
        for dimension, current, other in [("Platform", registry.PLATFORM, "OtherPlatform"),
                                           ("Target", "Editor", "Game")]:
            with self.subTest(dimension=dimension):
                self.assertTrue(registry.editor_supported({dimension + "AllowList": [current]}))
                self.assertFalse(registry.editor_supported({dimension + "AllowList": [other]}))
                self.assertTrue(registry.editor_supported({dimension + "DenyList": [other]}))
                self.assertFalse(registry.editor_supported({dimension + "DenyList": [current]}))
                # Retired keys cannot affect eligibility on their own or override current keys.
                for key, values in [("Whitelist" + dimension + "s", [other]),
                                    ("Blacklist" + dimension + "s", [current])]:
                    self.assertTrue(registry.editor_supported({key: values}))
                    self.assertTrue(registry.editor_supported({key: values, dimension + "AllowList": [current]}))
                    self.assertFalse(registry.editor_supported({key: values, dimension + "DenyList": [current]}))

    def test_external_directory_and_repeated_roots(self):
        external = self.root.parent / "Shared Tools"
        self.add_plugin("Unreal_FBX_Exporter", root=external)
        self.write_project({"AdditionalPluginDirectories": ["../Shared Tools", str(external), "Plugins"]})
        self.assertEqual(self.install(), 0)
        self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 15)

    def test_project_plugin_without_enabled_by_default_matches_unreal(self):
        descriptor = self.cube.manifest.parents[1] / "CubeShot.uplugin"
        data = json.loads(descriptor.read_text("utf-8"))
        data.pop("EnabledByDefault")
        descriptor.write_text(json.dumps(data), "utf-8")
        self.assertEqual(self.install(), 0)
        self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 8)

    def test_duplicate_ids_and_tool_names_fail_before_any_write(self):
        fbx = self.add_plugin("Unreal_FBX_Exporter")
        original = json.loads(fbx.manifest.read_text("utf-8"))
        for change in ({"id": "cubeshot"}, {"tools": [fbx.status_tool, self.cube.status_tool]}):
            fbx.manifest.write_text(json.dumps({**original, **change}), "utf-8")
            self.assertEqual(self.install(), 1)
            self.assertFalse((self.root / ".codex").exists())
            self.assertFalse((self.root / "Config").exists())

    def test_current_settings_preserve_address_timeouts_other_tables_and_backup(self):
        self.assertEqual(self.install(), 0)
        unrelated = '# keep comment\nmodel = "keep"\n[mcp_servers.other]\ncommand = "keep-server"\n'
        original = unrelated + server("unreal_tools", self.cube.tools, url="http://localhost:8000/mcp",
                                      startup_timeout_sec=37, tool_timeout_sec=360)
        original += '[[profiles.custom.rules]]\nname = "keep me"\n'
        path = self.write(".codex/config.toml", original)
        self.add_plugin("Unreal_FBX_Exporter")
        self.assertEqual(self.install(), 0)
        self.assertTrue(path.read_text("utf-8").startswith(unrelated))
        result = self.result()
        self.assertEqual(set(result), {"other", "unreal_tools"})
        self.assertEqual(result["unreal_tools"]["url"], "http://localhost:8000/mcp")
        self.assertEqual(result["unreal_tools"]["startup_timeout_sec"], 37)
        self.assertEqual(result["unreal_tools"]["tool_timeout_sec"], 360)
        backups = list(path.parent.glob("config.toml.unreal-tools.*.bak"))
        self.assertTrue(any(backup.read_text("utf-8") == original for backup in backups))
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
        original = server("unreal_tools", [self.cube.status_tool]) + '[mcp_servers.other]\ncommand="keep"\n'
        path = self.write(".codex/config.toml", original)
        self.assertEqual(self.install("--replace-owned-config", "--dry-run"), 0)
        self.assertEqual(path.read_text("utf-8"), original)
        self.assertFalse((self.root / "Config").exists())
        self.assertEqual(self.install("--replace-owned-config"), 0)
        self.assertEqual(self.result()["other"], {"command": "keep"})
        self.assertEqual(len(self.result()["unreal_tools"]["enabled_tools"]), 15)

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
        self.assertEqual(self.install(), 0)
        ini = self.write("Config/DefaultEditorPerProjectUserSettings.ini",
                         f"[{registry.SETTINGS_SECTION}]\nServerPortNumber=8018\n")
        self.assertEqual(self.install(), 1)
        self.assertIn("8018", ini.read_text("utf-8"))
        self.assertEqual(self.install("--port", "8018"), 0)

    def test_invalid_endpoint_timeout_and_disabled_engine_fail(self):
        for args in [("--port", "0"), ("--port", "70000"), ("--url-path", "/bad?query"), ("--url-path", "/")]:
            self.assertEqual(self.install(*args), 1)
        self.write(".codex/config.toml", server("unreal_tools", self.cube.tools, tool_timeout_sec=-1))
        self.assertEqual(self.install("--replace-owned-config"), 1)
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
    def test_copied_setup_rejects_disabled_old_provider_table_without_changes(self):
        providers = [self.add_plugin(name) for name in ["Unreal_CubeShot", "Unreal_FBX_Exporter"]]
        originals = {provider.id: provider.manifest.read_text("utf-8") for provider in providers}
        for current, disabled in [providers, providers[::-1]]:
            with self.subTest(current=current.id):
                current.manifest.write_text(originals[current.id], "utf-8")
                older = json.loads(originals[disabled.id])
                older.update(api_version="1.2.0", minimum_runtime_version="1.2.0")
                disabled.manifest.write_text(json.dumps(older), "utf-8")
                self.write_project({"Plugins": [{"Name": current.plugin, "Enabled": True},
                                                {"Name": disabled.plugin, "Enabled": False}]})
                path = self.write(".codex/config.toml", server(disabled.id, [disabled.id + "_retired_tool"]))
                before = path.read_bytes()
                command = [sys.executable, "-S", str(current.manifest.with_name("setup_mcp.py")),
                           "--project", str(self.project)]
                result = subprocess.run(command, cwd=self.temp.name, capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("Provider-specific", result.stderr)
                self.assertIn(disabled.id, result.stderr)
                self.assertEqual(path.read_bytes(), before)
                self.assertEqual(list(path.parent.iterdir()), [path])
                self.assertFalse((self.root / "Config").exists())

    def test_each_current_plugin_setup_ignores_disabled_old_neighbor(self):
        providers = [self.add_plugin(name) for name in ["Unreal_CubeShot", "Unreal_FBX_Exporter"]]
        originals = {provider.id: provider.manifest.read_text("utf-8") for provider in providers}
        for current, disabled in [providers, providers[::-1]]:
            with self.subTest(current=current.id):
                current.manifest.write_text(originals[current.id], "utf-8")
                older = json.loads(originals[disabled.id])
                older.update(api_version="1.2.0", minimum_runtime_version="1.2.0")
                disabled.manifest.write_text(json.dumps(older), "utf-8")
                self.write_project({"Plugins": [{"Name": current.plugin, "Enabled": True},
                                                {"Name": disabled.plugin, "Enabled": False}]})
                result = subprocess.run([sys.executable, "-S", str(current.manifest.with_name("setup_mcp.py")),
                                         "--project", str(self.project)], cwd=self.temp.name, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(config.read_config(self.project)["mcp_servers"]["unreal_tools"]["enabled_tools"],
                                 list(current.tools))

    def test_each_plugin_without_parent_repository_or_sdk(self):
        # A separate process proves imports never escape the copied plugin.
        for name, count in [("Unreal_CubeShot", 8), ("Unreal_FBX_Exporter", 7)]:
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
