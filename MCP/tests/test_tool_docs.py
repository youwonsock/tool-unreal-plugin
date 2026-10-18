"""Reference snapshots and examples must follow the captured Editor schema."""
import json

from support import ProjectCase
import generate_tool_docs as docs


class ToolDocsTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.cube = self.add_plugin("Unreal_CubeShot")
        self.fbx = self.add_plugin("Unreal_FBX_Exporter")
        # Source plugins may already include actual Editor-generated references.
        # These cases exercise fresh publication using a synthetic test catalogue.
        for provider in [self.cube, self.fbx]:
            provider.manifest.with_name("tool-spec.json").unlink(missing_ok=True)
            provider.manifest.with_name("TOOL_REFERENCE.md").unlink(missing_ok=True)
        self.catalog = {"tools": [{"name": name, "description": "Actual server description",
                                   "inputSchema": {"type": "object"}, "outputSchema": {"type": "object"}}
                                  for provider in [self.cube, self.fbx] for name in provider.tools]}

    def generate(self, *, check=False):
        return self.quiet(docs.generate, self.catalog, root=self.root / "Plugins", check=check)

    def test_write_then_check_is_stable_and_schema_changes_are_detected(self):
        self.assertEqual(self.generate(check=True), 1)
        self.assertFalse(self.cube.manifest.with_name("tool-spec.json").exists())
        self.assertEqual(self.generate(), 0)
        self.assertEqual(self.generate(check=True), 0)
        snapshot = json.loads(self.cube.manifest.with_name("tool-spec.json").read_text("utf-8"))
        self.assertEqual(len(snapshot["tools"]), 9)
        self.assertEqual(snapshot["tools"][0]["description"], "Actual server description")
        self.assertIn("### Output schema", self.cube.manifest.with_name("TOOL_REFERENCE.md").read_text("utf-8"))
        self.catalog["tools"][0]["inputSchema"]["properties"] = {"new_field": {"type": "string"}}
        self.assertEqual(self.generate(check=True), 1)

    def test_missing_tool_invalid_schema_and_bad_example_fail_before_any_write(self):
        removed = self.catalog["tools"].pop()
        with self.assertRaisesRegex(ValueError, "missing"):
            self.generate()
        self.catalog["tools"].append(removed)
        example = self.write("Plugins/Unreal_CubeShot/MCP/examples/status.json",
                             json.dumps({"tool": self.cube.status_tool, "arguments": {"wrong": True}}))
        self.catalog["tools"][0]["inputSchema"] = {"type": "object", "additionalProperties": False}
        with self.assertRaisesRegex(ValueError, "Example does not match"):
            self.generate()
        self.assertFalse(self.cube.manifest.with_name("tool-spec.json").exists())
        self.assertFalse(self.fbx.manifest.with_name("tool-spec.json").exists())
        example.unlink()
        self.catalog["tools"][0]["inputSchema"] = {"type": "impossible_type"}
        with self.assertRaises(Exception):
            self.generate()
        self.assertFalse(self.cube.manifest.with_name("tool-spec.json").exists())

    def test_legacy_argument_example_and_full_call_examples_use_actual_tool_schema(self):
        export = next(tool for tool in self.catalog["tools"] if tool["name"] == "fbx_exporter_export")
        export["inputSchema"] = {"type": "object", "required": ["mesh_path"],
                                 "properties": {"mesh_path": {"type": "string"}, "resolution": {"type": "integer"}}}
        self.write("Plugins/Unreal_CubeShot/MCP/examples/statuses.json",
                   json.dumps([{"tool": self.cube.status_tool, "arguments": {}}]))
        self.assertEqual(self.generate(), 0)
        example = self.fbx.manifest.with_name("export-arguments.example.json")
        example.write_text('{"mesh_path":"/Game/Cube.Cube","resolution":"wrong"}', "utf-8")
        with self.assertRaisesRegex(ValueError, "export-arguments.example"):
            self.generate(check=True)

    def test_parser_accepts_direct_cli_and_jsonrpc_catalogs_but_rejects_duplicates(self):
        expected = docs.catalog_tools(self.catalog)
        self.assertEqual(docs.catalog_tools([self.catalog]), expected)
        self.assertEqual(docs.catalog_tools({"jsonrpc": "2.0", "id": 1, "result": self.catalog}), expected)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            docs.catalog_tools({"tools": [self.catalog["tools"][0], self.catalog["tools"][0]]})
        with self.assertRaisesRegex(ValueError, "inputSchema"):
            docs.catalog_tools({"tools": [{"name": "foo"}]})

    def test_generated_reference_uses_server_description_and_reports_missing_output_schema(self):
        self.catalog["tools"][0].pop("outputSchema")
        self.assertEqual(self.generate(), 0)
        text = self.cube.manifest.with_name("TOOL_REFERENCE.md").read_text("utf-8")
        self.assertIn("Actual server description", text)
        self.assertIn("does not advertise an output schema", text)

    def test_job_identity_reference_explains_exactly_one_published_identifier(self):
        tool = next(tool for tool in self.catalog["tools"] if tool["name"] == self.cube.job_tools["control"])
        tool["inputSchema"] = {"type": "object", "properties": {"job_id": {"type": "string"},
            "request_id": {"type": "string"}, "action": {"type": "string"}}, "required": ["action"]}
        self.assertEqual(self.generate(), 0)
        text = self.cube.manifest.with_name("TOOL_REFERENCE.md").read_text("utf-8")
        self.assertIn("exactly one of `job_id` / `request_id`", text)
        self.assertIn("supplying both or neither is rejected", text)
