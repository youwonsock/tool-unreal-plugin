import asyncio
from contextlib import asynccontextmanager
import importlib.util
import json
from pathlib import Path
import io
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

from support import MCP, ProjectCase, server
from unreal_tools_mcp import client


class Result:
    def __init__(self, **data):
        self.data = data

    def model_dump(self, **kwargs):
        return self.data


class Session:
    def __init__(self, project, providers):
        self.project = project
        self.tools = [tool for provider in providers for tool in provider.tools] + ["native_unrelated_tool"]
        self.statuses = {provider.status_tool for provider in providers}
        self.calls = []
        self.responses = {}
        self.paginate = False

    async def initialize(self):
        return Result(protocolVersion="2025-11-25")

    async def list_tools(self, cursor=None):
        names = self.tools
        extra = {}
        if self.paginate:
            names = names[2:] if cursor else names[:2]
            if not cursor:
                extra["nextCursor"] = "page2"
        return Result(tools=[{"name": name} for name in names], **extra)

    async def call_tool(self, name, arguments):
        self.calls.append((name, arguments))
        if name in self.responses:
            response = self.responses[name]
            if isinstance(response, list):
                response = response.pop(0)
            if callable(response):
                response = response(arguments)
            if isinstance(response, Exception):
                raise response
            return response
        return Result(structuredContent={"success": True, **({"project_file": str(self.project), "api_version": "2.0.0",
            "capabilities": ["profiles", "jobs", "extended_options", "batch_preflight", "png_overwrite",
                             "history_retention", "record_diagnostics"]} if name in self.statuses else {})})


class ClientTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.cube = self.add_plugin("Unreal_CubeShot")
        self.fbx = self.add_plugin("Unreal_FBX_Exporter")
        self.session = Session(self.project, [self.cube, self.fbx])
        self.opened = False

        @asynccontextmanager
        async def fake_open(endpoint, timeout):
            self.opened = True
            yield self.session
        patcher = patch.object(client, "open_session", fake_open)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_calls(self, calls, list_tools=False, *, scope=None):
        anchor = self.plugin_paths[scope] / "MCP/call_mcp.py" if scope else MCP / "call_mcp.py"
        return self.quiet(asyncio.run, client.run_calls(calls, list_tools, project=self.project, anchor=anchor))

    def test_list_filters_native_tools_and_handles_pagination(self):
        self.session.paginate = True
        listed = self.run_calls([], True)[0]["tools"]
        self.assertEqual(len(listed), 15)
        self.assertNotIn("native_unrelated_tool", [tool["name"] for tool in listed])
        self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool, self.fbx.status_tool])

    def test_disabled_old_provider_does_not_block_current_list_or_call(self):
        originals = {provider.id: provider.manifest.read_text("utf-8") for provider in [self.cube, self.fbx]}
        for current, disabled in [(self.cube, self.fbx), (self.fbx, self.cube)]:
            current.manifest.write_text(originals[current.id], "utf-8")
            older = json.loads(originals[disabled.id])
            older.update(api_version="1.2.0", minimum_runtime_version="1.2.0")
            disabled.manifest.write_text(json.dumps(older), "utf-8")
            self.write_project({"Plugins": [{"Name": current.plugin, "Enabled": True},
                                            {"Name": disabled.plugin, "Enabled": False}]})
            for scope in [None, current.id]:
                with self.subTest(current=current.id, scope=scope):
                    self.session = Session(self.project, [current])
                    listed = self.run_calls([], True, scope=scope)[0]["tools"]
                    self.assertEqual({tool["name"] for tool in listed}, set(current.tools))
                    query = {"tool": current.tools[1], "arguments": {"query": "Cube"}}
                    self.assertFalse(client.failed(self.run_calls([query], scope=scope)[0]))
                    self.assertEqual(self.session.calls, [(current.status_tool, {}), (current.status_tool, {}),
                                                         (query["tool"], query["arguments"])])

    def test_active_old_provider_is_rejected_before_connecting(self):
        older = json.loads(self.fbx.manifest.read_text("utf-8"))
        older.update(api_version="1.2.0", minimum_runtime_version="1.2.0")
        self.fbx.manifest.write_text(json.dumps(older), "utf-8")
        with self.assertRaisesRegex(ValueError, "major version 2"):
            self.run_calls([{"tool": self.cube.status_tool}])
        self.assertFalse(self.opened)

    def test_disabled_retired_server_table_blocks_shared_and_scoped_calls_before_connecting(self):
        originals = {provider.id: provider.manifest.read_text("utf-8") for provider in [self.cube, self.fbx]}
        for current, disabled in [(self.cube, self.fbx), (self.fbx, self.cube)]:
            current.manifest.write_text(originals[current.id], "utf-8")
            disabled.manifest.write_text("{unreadable", "utf-8")
            self.write_project({"Plugins": [{"Name": current.plugin, "Enabled": True},
                                            {"Name": disabled.plugin, "Enabled": False}]})
            original = server("unreal_tools", current.tools) + server(disabled.id, [disabled.id + "_retired_tool"])
            path = self.write(".codex/config.toml", original)
            for scope in [None, current.id]:
                for list_tools in [False, True]:
                    with self.subTest(current=current.id, scope=scope, list_tools=list_tools):
                        with self.assertRaisesRegex(ValueError, "Provider-specific.*" + disabled.id):
                            self.run_calls([] if list_tools else [{"tool": current.status_tool}], list_tools, scope=scope)
                        self.assertFalse(self.opened)
                        self.assertEqual(self.session.calls, [])
                        self.assertEqual(path.read_text("utf-8"), original)

    def test_all_statuses_checked_before_mixed_batch_mutation(self):
        calls = [{"tool": self.cube.tools[3]}, {"tool": self.fbx.tools[3]}]
        results = self.run_calls(calls)
        self.assertEqual(len(results), 2)
        self.assertEqual([name for name, _ in self.session.calls],
                         [self.cube.status_tool, self.fbx.status_tool, self.cube.tools[3], self.fbx.tools[3]])

    def test_same_project_name_different_directory_blocks_all_work(self):
        self.session.responses[self.fbx.status_tool] = Result(structuredContent={
            "success": True, "project_file": str(self.root.parent / "Wrong" / self.project.name)})
        with self.assertRaisesRegex(RuntimeError, "not the expected Editor"):
            self.run_calls([{"tool": self.cube.tools[3]}, {"tool": self.fbx.tools[3]}])
        self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool, self.fbx.status_tool])
        evidence = json.loads(next((self.root / "Saved/UnrealTools/MCPCalls").glob("*.json")).read_text("utf-8"))
        self.assertIn("not the expected Editor", evidence["exception"])

    def test_scoped_adapter_rejects_other_provider_before_connecting(self):
        with self.assertRaisesRegex(ValueError, "outside the allow list"):
            self.run_calls([{"tool": self.fbx.tools[3]}], scope=self.cube.id)
        self.assertFalse(self.opened)

    def test_provider_specific_connection_is_not_used_as_a_fallback(self):
        self.write(".codex/config.toml", server(self.cube.id, self.cube.tools))
        with self.assertRaisesRegex(ValueError, "Provider-specific"):
            self.run_calls([{"tool": self.cube.status_tool}])
        self.assertFalse(self.opened)

    def test_capability_maps_are_not_coerced_to_current_protocol_arrays(self):
        self.session.responses[self.cube.status_tool] = Result(structuredContent={
            "success": True, "project_file": str(self.project), "api_version": "2.0.0",
            "capabilities": {"jobs": True}})
        with self.assertRaises(client.FeatureUnavailableError):
            self.run_calls([{"tool": self.cube.job_tools["start"]}])
        self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool])

    def test_allowlist_and_disabled_tools_are_enforced_before_connecting(self):
        self.write(".codex/config.toml", server("unreal_tools", self.cube.tools, disabled_tools=[self.cube.tools[3]]))
        with self.assertRaises(ValueError):
            self.run_calls([{"tool": self.cube.tools[3]}])
        self.assertFalse(self.opened)
        listed = self.run_calls([], True)[0]["tools"]
        self.assertEqual(len(listed), 7)
        self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool])

    def test_status_exclusion_or_disabled_connection_blocks_work(self):
        for extra in ({"disabled_tools": [self.cube.status_tool]}, {"enabled": False}):
            self.write(".codex/config.toml", server("unreal_tools", self.cube.tools, **extra))
            with self.assertRaises(ValueError):
                self.run_calls([{"tool": self.cube.tools[3]}])
            self.assertFalse(self.opened)

    def test_entire_batch_validated_before_first_call(self):
        for invalid in ({"tool": "outside"}, {"tool": []}, {"tool": self.cube.tools[1], "arguments": []},
                        {"tool": self.cube.tools[1], "argument": {}}):
            with self.assertRaises(ValueError):
                self.run_calls([{"tool": self.cube.tools[3]}, invalid])
            self.assertFalse(self.opened)

    def test_failed_result_stops_batch_and_preserves_result_contract(self):
        for error in (Result(isError=True), Result(structuredContent={"success": False})):
            self.session.calls.clear()
            self.session.responses[self.cube.tools[3]] = error
            results = self.run_calls([{"tool": self.cube.tools[3]}, {"tool": self.cube.tools[4]}], scope=self.cube.id)
            self.assertEqual(len(results), 1)
            self.assertTrue(client.failed(results[0]))
            self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool, self.cube.tools[3]])
        self.assertEqual(len(list((self.root / "Saved/CubeShot/MCPCalls").glob("*.json"))), 2)

    def test_transport_error_stops_batch(self):
        self.session.responses[self.cube.tools[3]] = RuntimeError("disconnected")
        with self.assertRaisesRegex(RuntimeError, "disconnected"):
            self.run_calls([{"tool": self.cube.tools[3]}, {"tool": self.cube.tools[4]}])
        self.assertEqual(len(self.session.calls), 2)

    def test_missing_editor_module_reports_build_restart(self):
        self.session.tools.remove(self.fbx.tools[3])
        with self.assertRaisesRegex(RuntimeError, "restart"):
            self.run_calls([], True)
        self.assertEqual(self.session.calls, [])

    def test_cli_json_argument_file_batch_and_exit_code(self):
        arguments = self.write("arguments.json", '{"query":"Cube"}')
        common = ["--project", str(self.project)]
        result = self.quiet(client.main, [self.cube.tools[1], "--arguments-file", str(arguments), *common],
                            anchor=MCP / "call_mcp.py")
        self.assertEqual(result, 0)
        self.assertEqual(self.session.calls[-1], (self.cube.tools[1], {"query": "Cube"}))
        batch = self.write("batch.json", json.dumps([{"tool": self.cube.tools[3]}, {"tool": self.fbx.tools[3]}]))
        self.session.responses[self.cube.tools[3]] = Result(isError=True)
        result = self.quiet(client.main, ["--batch-file", str(batch), *common], anchor=MCP / "call_mcp.py")
        self.assertEqual(result, 1)
        self.assertNotIn(self.fbx.tools[3], [name for name, _ in self.session.calls])

    def test_plugin_public_python_interface_works_after_copy(self):
        path = self.cube.manifest.with_name("call_mcp.py")
        spec = importlib.util.spec_from_file_location("copied_cube_client", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(self.quiet(module.main, ["--list-tools", "--project", str(self.project)]), 0)
        self.assertTrue((self.root / "Saved/CubeShot/MCPCalls").is_dir())

    def test_evidence_failure_does_not_replace_success_or_original_error(self):
        with patch.object(Path, "write_text", side_effect=OSError("evidence denied")):
            result = self.run_calls([{"tool": self.cube.status_tool}])
            self.assertTrue(result[0]["structuredContent"]["success"])
            self.session.responses[self.cube.tools[3]] = RuntimeError("original failure")
            with self.assertRaisesRegex(RuntimeError, "original failure"):
                self.run_calls([{"tool": self.cube.tools[3]}])

    def test_broken_diagnostic_stream_does_not_mask_results_or_original_failure(self):
        with patch("builtins.print", side_effect=OSError("closed diagnostic stream")):
            result = self.run_calls([{"tool": self.cube.status_tool}])
            self.assertTrue(result[0]["structuredContent"]["success"])
            self.session.responses[self.cube.tools[3]] = RuntimeError("actual transport failure")
            with self.assertRaisesRegex(RuntimeError, "actual transport failure"):
                self.run_calls([{"tool": self.cube.tools[3]}])

    def test_all_calls_reject_missing_or_older_editor_versions(self):
        for api in [None, "1.2.0", "3.0.0", "bad"]:
            context = {"success": True, "project_file": str(self.project), "capabilities": []}
            if api is not None:
                context["api_version"] = api
            self.session.responses[self.cube.status_tool] = Result(structuredContent=context)
            for tool in [self.cube.status_tool, self.cube.tools[1], self.cube.job_tools["start"], "cubeshot_export_png"]:
                self.session.calls.clear()
                with self.assertRaises(client.FeatureUnavailableError):
                    self.run_calls([{"tool": tool}])
                self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool])

    def test_retired_tools_are_rejected_before_connecting(self):
        for tool in ["cubeshot_generate", "fbx_exporter_export"]:
            with self.assertRaisesRegex(ValueError, "outside the allow list"):
                self.run_calls([{"tool": tool, "arguments": {"mesh_path": "/Engine/BasicShapes/Cube.Cube"}}])
            self.assertFalse(self.opened)

    def test_lost_start_response_queries_same_request_id_without_resubmitting(self):
        start, status = self.cube.job_tools["start"], self.cube.job_tools["status"]
        self.session.responses[start] = ConnectionError("response lost after persistence")
        self.session.responses[status] = lambda arguments: Result(structuredContent={
            "success": True, "job_id": "accepted", "request_id": arguments["request_id"], "state": "running"})
        calls = [{"tool": start, "arguments": {"mesh_paths": ["/Game/Cube.Cube"]}}, {"tool": self.cube.tools[3]}]
        result = self.run_calls(calls)
        self.assertEqual(result[0]["structuredContent"]["job_id"], "accepted")
        submitted = next(arguments for name, arguments in self.session.calls if name == start)
        queried = next(arguments for name, arguments in self.session.calls if name == status)
        self.assertEqual(queried, {"request_id": submitted["request_id"]})
        self.assertEqual([name for name, _ in self.session.calls].count(start), 1)
        self.assertNotIn(self.cube.tools[3], [name for name, _ in self.session.calls])
        self.assertNotIn("request_id", calls[0]["arguments"])
        self.assertEqual(len(list((self.root / "Saved/UnrealTools/MCPRequests").glob("*.json"))), 1)

    def test_lost_start_response_never_reposts_when_lookup_fails(self):
        start, status = self.cube.job_tools["start"], self.cube.job_tools["status"]
        self.session.responses[start] = ConnectionError("lost")
        self.session.responses[status] = Result(structuredContent={"success": False, "code": "not_found"})
        with self.assertRaisesRegex(RuntimeError, "request_id=stable-id"):
            self.run_calls([{"tool": start, "arguments": {"request_id": "stable-id", "mesh_paths": ["/Game/Cube.Cube"]}}])
        self.assertEqual([name for name, _ in self.session.calls].count(start), 1)

    def test_reconciliation_rechecks_project_on_new_connection(self):
        start = self.cube.job_tools["start"]
        self.session.responses[start] = ConnectionError("lost")
        self.session.responses[self.cube.status_tool] = [
            Result(structuredContent={"success": True, "project_file": str(self.project), "api_version": "2.0.0",
                                      "capabilities": ["jobs"]}),
            Result(structuredContent={"success": True, "project_file": str(self.root.parent / "Wrong.uproject")})]
        with self.assertRaises(client.ProjectIdentityError):
            self.run_calls([{"tool": start, "arguments": {"mesh_paths": ["/Game/Cube.Cube"]}}])
        self.assertNotIn(self.cube.job_tools["status"], [name for name, _ in self.session.calls])

    def wait_cli(self, tool, arguments=None, *extra):
        return self.quiet(client.main, [tool, "--arguments", json.dumps(arguments or {}), "--wait",
                                       "--poll-interval", "0.001", "--project", str(self.project), *extra],
                          anchor=MCP / "call_mcp.py")

    def test_wait_succeeds_only_for_terminal_success_and_never_changes_run_calls_contract(self):
        start, status = self.cube.job_tools["start"], self.cube.job_tools["status"]
        for terminal in ["succeeded", "failed", "cancelled", "interrupted"]:
            self.session.calls.clear()
            self.session.responses[start] = Result(structuredContent={"success": True, "job_id": "job", "state": "queued"})
            self.session.responses[status] = [Result(structuredContent={"success": True, "job_id": "job", "state": "running"}),
                Result(structuredContent={"success": True, "job_id": "job", "state": terminal})]
            self.assertEqual(self.wait_cli(start, {"mesh_paths": ["/Game/Cube.Cube"]}), int(terminal != "succeeded"))
            self.assertEqual([name for name, _ in self.session.calls].count(self.cube.status_tool), 3)

    def test_wait_on_existing_job_retries_transient_connection_without_resubmitting(self):
        status = self.cube.job_tools["status"]
        self.session.responses[status] = [Result(structuredContent={"success": True, "job_id": "job", "state": "running"}),
            ConnectionError("temporary disconnect"), Result(structuredContent={"success": True, "job_id": "job", "state": "succeeded"})]
        self.assertEqual(self.wait_cli(status, {"request_id": "persisted"}), 0)
        self.assertNotIn(self.cube.job_tools["start"], [name for name, _ in self.session.calls])

    def test_wait_retries_initial_status_connection_and_unresolved_submission_as_reads(self):
        start, status = self.cube.job_tools["start"], self.cube.job_tools["status"]
        terminal = Result(structuredContent={"success": True, "job_id": "accepted", "state": "succeeded"})
        self.session.responses[status] = [ConnectionError("initial status disconnected"), terminal]
        self.assertEqual(self.wait_cli(status, {"request_id": "known-before-connect"}), 0)
        self.assertNotIn(start, [name for name, _ in self.session.calls])
        self.session.calls.clear()
        self.session.responses[start] = ConnectionError("start response lost")
        self.session.responses[status] = [ConnectionError("reconciliation temporarily disconnected"), terminal]
        self.assertEqual(self.wait_cli(start, {"mesh_paths": ["/Game/Cube.Cube"]}), 0)
        self.assertEqual([name for name, _ in self.session.calls].count(start), 1)
        submitted = next(arguments for name, arguments in self.session.calls if name == start)
        for name, arguments in self.session.calls:
            if name == status:
                self.assertEqual(arguments, {"request_id": submitted["request_id"]})

    def test_wait_stops_on_project_or_capability_change_after_reconnection(self):
        status = self.cube.job_tools["status"]
        self.session.responses[status] = Result(structuredContent={"success": True, "job_id": "job", "state": "running"})
        good = Result(structuredContent={"success": True, "project_file": str(self.project), "api_version": "2.0.0",
                                        "capabilities": ["jobs"]})
        for bad in [Result(structuredContent={"success": True, "project_file": str(self.root.parent / "Wrong.uproject")}),
                    Result(structuredContent={"success": True, "project_file": str(self.project), "api_version": "1.0.0"})]:
            self.session.calls.clear()
            self.session.responses[self.cube.status_tool] = [good, bad]
            self.assertEqual(self.wait_cli(status, {"job_id": "job"}), 1)
            self.assertEqual([name for name, _ in self.session.calls].count(status), 1)

    def test_wait_deadline_retains_id_and_does_not_cancel(self):
        status = self.cube.job_tools["status"]
        self.session.responses[status] = Result(structuredContent={"success": True, "job_id": "keep-me", "state": "running"})
        output, errors = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(errors):
            result = client.main([status, "--arguments", '{"job_id":"keep-me"}', "--wait", "--wait-timeout", "0.005",
                                  "--poll-interval", "0.001", "--project", str(self.project)], anchor=MCP / "call_mcp.py")
        self.assertEqual(result, 1)
        self.assertIn("job_id=keep-me", errors.getvalue())
        self.assertNotIn(self.cube.job_tools["control"], [name for name, _ in self.session.calls])

    def test_wait_rejects_batch_sync_calls_and_bad_intervals_before_connecting(self):
        self.assertEqual(self.wait_cli(self.cube.tools[3]), 1)
        self.assertFalse(self.opened)
        batch = self.write("batch.json", json.dumps([{"tool": self.cube.job_tools["status"]}]))
        self.assertEqual(self.quiet(client.main, ["--batch-file", str(batch), "--wait", "--project", str(self.project)],
                                    anchor=MCP / "call_mcp.py"), 1)
        self.assertFalse(self.opened)
        for option in ["--wait-timeout", "--poll-interval"]:
            self.assertEqual(self.wait_cli(self.cube.job_tools["status"], {"job_id": "job"}, option, "0"), 1)
        self.assertFalse(self.opened)

    def test_job_request_id_is_printed_even_if_recording_fails(self):
        start = self.cube.job_tools["start"]
        output = io.StringIO()
        self.session.responses[start] = Result(structuredContent={"success": True, "job_id": "job", "state": "queued"})
        with redirect_stdout(output), redirect_stderr(io.StringIO()), patch.object(Path, "write_text", side_effect=OSError("denied")):
            result = asyncio.run(client.run_calls([{"tool": start, "arguments": {"mesh_paths": ["/Game/Cube.Cube"]}}],
                                                 project=self.project, anchor=MCP / "call_mcp.py"))
        self.assertTrue(result[0]["structuredContent"]["success"])
        self.assertIn("MCP_REQUEST_ID=", output.getvalue())

    def test_png_export_always_requires_current_capability(self):
        self.session.responses[self.cube.status_tool] = Result(structuredContent={
            "success": True, "project_file": str(self.project), "api_version": "2.0.0", "capabilities": []})
        parameters = {"texture_path": "/Game/CubeShot/Test.Test"}
        for values in [parameters, {**parameters, "overwrite": False}]:
            self.session.calls.clear()
            with self.assertRaises(client.FeatureUnavailableError):
                self.run_calls([{"tool": "cubeshot_export_png", "arguments": values}])
            self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool])
        del self.session.responses[self.cube.status_tool]
        self.run_calls([{"tool": "cubeshot_export_png", "arguments": {**parameters, "overwrite": False}}])
        self.assertIs(self.session.calls[-1][1]["overwrite"], False)

    def test_diagnostic_record_cannot_be_waited_or_resubmitted(self):
        status = self.cube.job_tools["status"]
        self.session.responses[status] = Result(structuredContent={
            "success": True, "record_kind": "diagnostic", "job_id": "diagnostic-id",
            "state": "interrupted", "recovery_required": True})
        self.assertEqual(self.wait_cli(status, {"job_id": "diagnostic-id"}), 1)
        self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool, status])

    def test_expired_job_wait_returns_not_found_without_resubmitting(self):
        status = self.cube.job_tools["status"]
        self.session.responses[status] = Result(structuredContent={
            "success": False, "error_code": "NOT_FOUND", "error": "Job record expired."})
        self.assertEqual(self.wait_cli(status, {"request_id": "expired-request"}), 1)
        self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool, status])

    def test_wait_does_not_translate_old_job_envelopes_or_state_names(self):
        status = self.cube.job_tools["status"]
        for payload in [{"job": {"job_id": "old", "state": "succeeded"}}, {"status": "succeeded"}]:
            self.session.calls.clear()
            self.session.responses[status] = Result(structuredContent={"success": True, **payload})
            self.assertEqual(self.wait_cli(status, {"job_id": "old"}), 1)
            self.assertEqual([name for name, _ in self.session.calls], [self.cube.status_tool, status])
