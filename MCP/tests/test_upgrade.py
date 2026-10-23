"""Upgrade, recovery and version boundaries, independent of a running Unreal Editor."""
import importlib.util
import json
from pathlib import Path
import shutil
from unittest.mock import patch

from support import MCP, ProjectCase, server
from unreal_tools_mcp import config, registry
import unreal_tools_mcp
import sync_runtime


class UpgradeTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.cube = self.add_plugin("Unreal_CubeShot")
        self.fbx = self.add_plugin("Unreal_FBX_Exporter")

    def install(self, *args):
        return self.quiet(config.main, ["--project", str(self.project), *args], anchor=MCP / "setup_mcp.py")

    def tools(self):
        return config.read_config(self.project)["mcp_servers"]["unreal_tools"]["enabled_tools"]

    def test_provider_specific_tables_are_rejected_without_migration(self):
        for text in [server(self.cube.id, self.cube.tools), server(self.fbx.id, self.fbx.tools),
                     server(self.cube.id, self.cube.tools) + server(self.fbx.id, self.fbx.tools)]:
            path = self.write(".codex/config.toml", text)
            self.assertEqual(self.install(), 1)
            self.assertEqual(self.install("--replace-owned-config"), 1)
            self.assertEqual(path.read_text("utf-8"), text)
            self.assertFalse(config.state_path(self.project).exists())

    def test_disabled_old_provider_tables_are_rejected_before_any_write(self):
        originals = {provider.id: provider.manifest.read_text("utf-8") for provider in [self.cube, self.fbx]}
        for current, disabled in [(self.cube, self.fbx), (self.fbx, self.cube)]:
            current.manifest.write_text(originals[current.id], "utf-8")
            older = json.loads(originals[disabled.id])
            older.update(api_version="1.2.0", minimum_runtime_version="1.2.0")
            for kind, source in [("1.2", json.dumps(older)), ("missing_version", json.dumps({"id": disabled.id})),
                                 ("malformed", "{unreadable")]:
                with self.subTest(current=current.id, manifest=kind):
                    disabled.manifest.write_text(source, "utf-8")
                    self.write_project({"Plugins": [{"Name": current.plugin, "Enabled": True},
                                                    {"Name": disabled.plugin, "Enabled": False}]})
                    self.assertEqual([provider.id for provider in registry.discover(self.project)], [current.id])
                    original = server(disabled.id, [disabled.id + "_retired_tool"])
                    path = self.write(".codex/config.toml", original)
                    for args in [(), ("--dry-run",), ("--replace-owned-config",)]:
                        for anchor in [MCP / "setup_mcp.py", current.manifest.with_name("setup_mcp.py")]:
                            with self.subTest(args=args, anchor=anchor):
                                self.assertEqual(self.quiet(config.main, ["--project", str(self.project), *args],
                                                            anchor=anchor), 1)
                                self.assertEqual(path.read_text("utf-8"), original)
                                self.assertEqual(list(path.parent.iterdir()), [path])
                                self.assertFalse((self.root / "Config").exists())
                                self.assertFalse(config.state_path(self.project).exists())
                    with self.assertRaisesRegex(ValueError, "Provider-specific.*" + disabled.id):
                        config.prepare(self.project)

    def test_disabled_provider_table_preserves_current_setup_until_manually_removed(self):
        self.write_project({"Plugins": [{"Name": self.fbx.plugin, "Enabled": False}]})
        self.fbx.manifest.write_text("{unreadable", "utf-8")
        self.assertEqual(self.install(), 0)
        path = self.root / ".codex/config.toml"
        current = path.read_text("utf-8") + '[mcp_servers.unrelated]\ncommand="keep"\n'
        path.write_text(current + server(self.fbx.id, ["fbx_exporter_export"]), "utf-8")
        tracked = [path, config.state_path(self.project), self.root / "Config/DefaultEditorPerProjectUserSettings.ini"]
        before = {target: target.read_bytes() for target in tracked}
        backups = set(self.root.rglob("*.bak"))
        for args in [(), ("--replace-owned-config",), ("--replace-owned-config", "--dry-run")]:
            self.assertEqual(self.install(*args), 1)
            self.assertEqual({target: target.read_bytes() for target in tracked}, before)
            self.assertEqual(set(self.root.rglob("*.bak")), backups)
        # Manual removal of only the retired table permits ordinary idempotent setup.
        path.write_text(current, "utf-8")
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.tools(), list(self.cube.tools))
        self.assertEqual(config.read_config(self.project)["mcp_servers"]["unrelated"], {"command": "keep"})
        self.assertEqual(config.prepare(self.project)[2], {})

    def test_retired_server_detection_does_not_require_an_installed_provider(self):
        shutil.rmtree(self.plugin_paths[self.fbx.id])
        path = self.write(".codex/config.toml", server(self.fbx.id, ["fbx_exporter_export"]))
        original = path.read_bytes()
        self.assertEqual([provider.id for provider in registry.discover(self.project)], [self.cube.id])
        self.assertEqual(self.install(), 1)
        self.assertEqual(path.read_bytes(), original)
        self.assertFalse(config.state_path(self.project).exists())
        self.assertFalse((self.root / "Config").exists())

    def test_generated_state_equality_is_required_when_it_exists(self):
        self.assertEqual(self.install(), 0)
        # Even a complete known old group must not override a later user edit.
        path = self.write(".codex/config.toml", server("unreal_tools", self.cube.tools[:2]))
        before = path.read_bytes()
        self.assertEqual(self.install(), 1)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(len(config.read_state(self.project)["enabled_tools"]), 15)
        self.assertEqual(self.install("--replace-owned-config"), 0)

    def test_previous_setup_state_is_not_automatically_upgraded(self):
        self.assertEqual(self.install(), 0)
        current = config.read_state(self.project)
        before = (self.root / ".codex/config.toml").read_bytes()
        for api in [None, "1.2.0"]:
            previous = {**current}
            if api is None:
                previous.pop("api_version")
            else:
                previous["api_version"] = api
            source = json.dumps(previous)
            config.state_path(self.project).write_text(source, "utf-8")
            self.assertEqual(self.install(), 1)
            self.assertEqual((self.root / ".codex/config.toml").read_bytes(), before)
            self.assertEqual(config.state_path(self.project).read_text("utf-8"), source)
        self.assertEqual(self.install("--replace-owned-config"), 0)
        self.assertEqual(config.read_state(self.project)["api_version"], "2.0.0")

    def test_provider_specific_table_is_not_merged_even_with_current_state(self):
        self.assertEqual(self.install(), 0)
        text = server("unreal_tools", self.tools()) + server(self.cube.id, self.cube.tools)
        path = self.write(".codex/config.toml", text)
        self.assertEqual(self.install(), 1)
        self.assertEqual(self.install("--replace-owned-config"), 1)
        self.assertEqual(path.read_text("utf-8"), text)

    def test_missing_setup_state_never_infers_ownership(self):
        self.assertEqual(self.install(), 0)
        config.state_path(self.project).unlink()
        path = self.root / ".codex/config.toml"
        before = path.read_bytes()
        self.assertEqual(self.install(), 1)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(self.install("--replace-owned-config"), 0)

    def test_timeout_changes_do_not_change_management_ownership(self):
        self.assertEqual(self.install(), 0)
        self.write(".codex/config.toml", server("unreal_tools", self.tools(), tool_timeout_sec=432, startup_timeout_sec=51))
        self.assertEqual(self.install(), 0)
        entry = config.read_config(self.project)["mcp_servers"]["unreal_tools"]
        self.assertEqual(entry["tool_timeout_sec"], 432)
        self.assertEqual(entry["startup_timeout_sec"], 51)

    def test_invalid_state_does_not_fall_back_to_heuristic_ownership(self):
        self.assertEqual(self.install(), 0)
        before = (self.root / ".codex/config.toml").read_bytes()
        config.state_path(self.project).write_text("{broken", "utf-8")
        self.assertEqual(self.install(), 1)
        self.assertEqual((self.root / ".codex/config.toml").read_bytes(), before)
        self.assertEqual(self.install("--replace-owned-config"), 0)

    def test_boolean_generated_state_schema_does_not_authorize_upgrade(self):
        self.assertEqual(self.install(), 0)
        state = config.read_state(self.project)
        state["schema_version"] = True
        config.state_path(self.project).write_text(json.dumps(state), "utf-8")
        before = (self.root / ".codex/config.toml").read_bytes()
        self.assertEqual(self.install(), 1)
        self.assertEqual((self.root / ".codex/config.toml").read_bytes(), before)

    def test_multi_file_replacement_rolls_back_to_this_run_not_first_backup(self):
        first = self.write(".codex/config.toml", "# current user text\n")
        first.with_name(first.name + ".unreal-tools.bak").write_text("# ancient text\n", "utf-8")
        second = self.root / "Config/New.ini"
        state = config.state_path(self.project)
        replace = config.os.replace
        replaced = []

        def fail_second(source, target):
            replaced.append(Path(target))
            if len(replaced) == 2:
                raise OSError("second target locked")
            replace(source, target)

        with patch.object(config.os, "replace", side_effect=fail_second):
            with self.assertRaisesRegex(OSError, "second target locked"):
                self.quiet(config.write_changes, {state: "{}", first: "# new\n", second: "new ini"})
        self.assertEqual(first.read_text("utf-8"), "# current user text\n")
        self.assertFalse(second.exists())
        self.assertFalse(state.exists())
        backup = next(first.parent.glob(first.name + ".unreal-tools.*.bak"))
        self.assertEqual(backup.read_text("utf-8"), "# current user text\n")

    def test_failure_to_replace_state_rolls_back_existing_and_created_files(self):
        first = self.write(".codex/config.toml", "# before\n")
        second = self.root / "Config/New.ini"
        state = config.state_path(self.project)
        replace = config.os.replace

        def fail_state(source, target):
            if Path(target) == state:
                raise OSError("state locked")
            replace(source, target)

        with patch.object(config.os, "replace", side_effect=fail_state):
            with self.assertRaisesRegex(OSError, "state locked"):
                self.quiet(config.write_changes, {state: "{}", first: "# after\n", second: "new ini"})
        self.assertEqual(first.read_text("utf-8"), "# before\n")
        self.assertFalse(second.exists())
        self.assertFalse(state.exists())

    def test_rollback_failure_reports_original_error_and_recovery_file(self):
        first = self.write(".codex/config.toml", "# original\n")
        second = self.write("Config/Before.ini", "before")
        real = config.os.replace
        count = 0

        def fail_write_and_restore(source, target):
            nonlocal count
            count += 1
            if count > 1:
                raise OSError("write or rollback locked")
            real(source, target)

        with patch.object(config.os, "replace", side_effect=fail_write_and_restore):
            with self.assertRaisesRegex(OSError, r"rollback also failed:.*recovery copy:"):
                self.quiet(config.write_changes, {first: "# changed\n", second: "changed"})
        self.assertEqual(next(first.parent.glob(first.name + ".unreal-tools.*.bak")).read_text("utf-8"), "# original\n")

    def test_all_replacements_are_prepared_before_any_target_changes(self):
        first = self.write(".codex/config.toml", "# original\n")
        second = self.root / "Config/New.ini"
        with patch.object(config.os, "fsync", side_effect=[None, OSError("disk full during staging")]):
            with self.assertRaisesRegex(OSError, "disk full"):
                self.quiet(config.write_changes, {first: "# new\n", second: "new ini"})
        self.assertEqual(first.read_text("utf-8"), "# original\n")
        self.assertFalse(second.exists())

    def test_state_is_last_and_successful_reexecution_is_idempotent(self):
        real = config.os.replace
        order = []

        def trace(source, target):
            order.append(Path(target))
            real(source, target)

        with patch.object(config.os, "replace", side_effect=trace):
            self.assertEqual(self.install(), 0)
        self.assertEqual(order[-1], config.state_path(self.project))
        self.assertEqual(config.prepare(self.project)[2], {})

    def test_old_manifests_and_incompatible_runtime_are_rejected(self):
        original = self.cube.manifest.read_text("utf-8")
        data = json.loads(original)
        for older in [{key: data[key] for key in ["schema_version", "id", "plugin", "status_tool", "tools"]},
                      {**data, "api_version": "1.2.0", "minimum_runtime_version": "1.2.0"}]:
            self.cube.manifest.write_text(json.dumps(older), "utf-8")
            with self.assertRaises(ValueError):
                registry.read_manifest(self.cube.manifest)
        self.cube.manifest.write_text(original, "utf-8")
        for runtime in ["1.2.0", "3.0.0"]:
            with patch.object(registry, "__version__", runtime):
                with self.assertRaisesRegex(RuntimeError, "new Python process"):
                    registry.active_providers(self.project)

    def test_new_entrypoints_reject_old_already_loaded_package(self):
        for name in ["call_mcp.py", "setup_mcp.py"]:
            path = self.cube.manifest.with_name(name)
            spec = importlib.util.spec_from_file_location("mixed_version_wrapper", path)
            module = importlib.util.module_from_spec(spec)
            with patch.object(unreal_tools_mcp, "__version__", "1.0.0"):
                with self.assertRaisesRegex(RuntimeError, "new Python process"):
                    spec.loader.exec_module(module)

    def test_manifest_role_version_and_capability_validation(self):
        original = json.loads(self.cube.manifest.read_text("utf-8"))
        for change in [{"api_version": "1.1"}, {"minimum_runtime_version": "next"},
                       {"job_tools": {"start": "other_tool"}}, {"job_tools": {"remove": self.cube.tools[1]}},
                       {"job_tools": {}}, {"capabilities": [True]}]:
            self.cube.manifest.write_text(json.dumps({**original, **change}), "utf-8")
            with self.assertRaises(ValueError):
                registry.read_manifest(self.cube.manifest)

    def test_recursive_runtime_and_native_header_sync_detect_drift_and_exclude_caches(self):
        source = Path(self.temp.name) / "Canonical"
        shutil.copytree(MCP / "unreal_tools_mcp", source / "unreal_tools_mcp", ignore=shutil.ignore_patterns("__pycache__"))
        for filename in ["setup_mcp.py", "call_mcp.py", "requirements.txt"]:
            shutil.copy2(MCP / filename, source / filename)
        nested = source / "unreal_tools_mcp/nested"
        nested.mkdir()
        (nested / "__init__.py").write_text("# nested fixture", "utf-8")
        (nested / "data.json").write_text("{}", "utf-8")
        (nested / "cache.pyc").write_bytes(b"cache")
        native = Path(self.temp.name) / "Shared/Native"
        native.mkdir(parents=True)
        (native / "UnrealToolsRuntime.h").write_text("// fixture header", "utf-8")
        (native / "UnrealToolsRuntimeTests.inl").write_text("// fixture tests", "utf-8")
        (native / "UnrealToolsWorkspace.h").write_text("// workspace fixture", "utf-8")
        (native / "UnrealToolsWorkspaceController.h").write_text("// adapter fixture", "utf-8")
        (native / "UnrealToolsServiceInstance.h").write_text("// instance fixture", "utf-8")
        obsolete = self.cube.manifest.parent / "unreal_tools_mcp/obsolete.py"
        obsolete.write_text("# stale", "utf-8")
        with patch.object(sync_runtime, "MCP_SOURCE", source), patch.object(sync_runtime, "NATIVE_SOURCE", native):
            self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins", check=True), 1)
            self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins"), 0)
            self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins", check=True), 0)
        self.assertFalse(obsolete.exists())
        for provider in [self.cube, self.fbx]:
            package = provider.manifest.parent / "unreal_tools_mcp/nested"
            self.assertEqual((package / "data.json").read_text("utf-8"), "{}")
            self.assertFalse((package / "cache.pyc").exists())
            header = provider.manifest.parents[1] / f"Source/{provider.plugin}/Private/UnrealToolsRuntime.h"
            self.assertEqual(header.read_text("utf-8"), "// fixture header")
            workspace = header.with_name("UnrealToolsWorkspace.h")
            self.assertEqual(workspace.read_text("utf-8"), "// workspace fixture")
            instance = header.with_name("UnrealToolsServiceInstance.h")
            self.assertEqual(instance.read_text("utf-8"), "// instance fixture")
            for managed_header in [workspace, instance]:
                managed_header.write_text("// drift", "utf-8")
                with patch.object(sync_runtime, "MCP_SOURCE", source), patch.object(sync_runtime, "NATIVE_SOURCE", native):
                    self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins", check=True), 1)
                    self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins"), 0)

    def test_missing_shared_native_source_fails_before_deploying_python(self):
        native = Path(self.temp.name) / "missing-native"
        before = (self.cube.manifest.parent / "call_mcp.py").read_bytes()
        with patch.object(sync_runtime, "NATIVE_SOURCE", native):
            with self.assertRaisesRegex(ValueError, "Missing canonical shared C\\+\\+"):
                self.quiet(sync_runtime.synchronize, self.root / "Plugins")
            self.assertEqual(before, (self.cube.manifest.parent / "call_mcp.py").read_bytes())
            self.assertEqual(self.quiet(sync_runtime.synchronize, self.root / "Plugins", include_native=False), 0)
