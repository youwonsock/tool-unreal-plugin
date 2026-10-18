"""Generate or check portable MCP references against a captured, actual tools/list response.

Capture tools/list after building and restarting the target Editor. This script
does not fabricate a catalogue from manifests or C++ source.
"""
import argparse
import json
from pathlib import Path
import sys

from unreal_tools_mcp.registry import plugin_descriptors, read_manifest

ROOT = Path(__file__).resolve().parent.parent


def catalog_tools(data):
    if isinstance(data, list) and len(data) == 1:
        data = data[0]
    if isinstance(data, dict) and isinstance(data.get("result"), dict):
        data = data["result"]
    tools = data.get("tools") if isinstance(data, dict) else None
    if not isinstance(tools, list):
        raise ValueError("Expected a tools/list response containing a tools array.")
    names = []
    for tool in tools:
        if (not isinstance(tool, dict) or not isinstance(tool.get("name"), str) or
                not isinstance(tool.get("inputSchema"), dict)):
            raise ValueError("Every tool must contain name and inputSchema.")
        names.append(tool["name"])
    if len(set(names)) != len(names):
        raise ValueError("The tools/list catalogue contains duplicate tool names.")
    return {tool["name"]: tool for tool in tools}


def json_text(value):
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def render_reference(provider, tools):
    lines = [f"# {provider.plugin} MCP tool reference", "",
             f"API `{provider.api_version}` · provider `{provider.id}` · {len(tools)} tools", "",
             "Generated from an actual Editor `tools/list` response. Edit the tool schema, then regenerate this file.", "",
             "| Tool | Required arguments |", "| --- | --- |"]
    for tool in tools:
        required = tool["inputSchema"].get("required", [])
        label = ', '.join(f'`{name}`' for name in required)
        identity = (tool["name"] in {provider.job_tools.get("status"), provider.job_tools.get("control")} and
                    {"job_id", "request_id"} <= set(tool["inputSchema"].get("properties", {})))
        if identity:
            label += ("; " if label else "") + "exactly one of `job_id` / `request_id`"
        lines.append(f"| `{tool['name']}` | {label or 'None'} |")
    for tool in tools:
        lines.extend(["", f"## {tool['name']}", "", tool.get("description", ""), "",
                      ])
        if (tool["name"] in {provider.job_tools.get("status"), provider.job_tools.get("control")} and
                {"job_id", "request_id"} <= set(tool["inputSchema"].get("properties", {}))):
            lines.extend(["Identify the job with exactly one of `job_id` and `request_id`; supplying both or neither is rejected.", ""])
        lines.extend(["### Input schema", "", "```json", json_text(tool["inputSchema"]).rstrip(), "```"])
        if "outputSchema" in tool:
            lines.extend(["", "### Output schema", "", "```json", json_text(tool["outputSchema"]).rstrip(), "```"])
        else:
            lines.extend(["", "The Editor does not advertise an output schema for this tool."])
    return "\n".join(lines).rstrip() + "\n"


def validate_examples(directory, provider, catalog):
    # MCP's SDK depends on jsonschema; setup and --help still use only stdlib.
    from jsonschema.validators import validator_for
    examples = sorted(directory.glob("*-arguments.example.json")) + sorted((directory / "examples").glob("*.json"))
    for path in examples:
        value = json.loads(path.read_text("utf-8-sig"))
        if path.name.endswith("-arguments.example.json"):
            role = path.name.removesuffix("-arguments.example.json").replace("-", "_")
            calls = [{"tool": provider.id + "_" + role, "arguments": value}]
        else:
            calls = value if isinstance(value, list) else [value]
        for call in calls:
            if (not isinstance(call, dict) or call.get("tool") not in provider.tools or
                    not isinstance(call.get("arguments"), dict) or set(call) - {"tool", "arguments"}):
                raise ValueError(f"Invalid tool/arguments example: {path}")
            schema = catalog[call["tool"]]["inputSchema"]
            try:
                validator = validator_for(schema)
                validator.check_schema(schema)
                validator(schema).validate(call["arguments"])
            except Exception as error:
                raise ValueError(f"Example does not match actual Editor schema: {path}: {error}") from error
    return len(examples)


def generate(catalog_data, *, root=ROOT, check=False):
    catalog = catalog_tools(catalog_data)
    outputs, example_count = {}, 0
    for descriptor in plugin_descriptors(Path(root)):
        manifest = descriptor.parent / "MCP/manifest.json"
        if not manifest.is_file():
            continue
        provider = read_manifest(manifest)
        missing = set(provider.tools) - set(catalog)
        if missing:
            raise ValueError(f"Actual Editor catalogue is missing {provider.id} tools: {', '.join(sorted(missing))}")
        tools = [catalog[name] for name in provider.tools]
        # Validate every schema even when a particular tool has no checked example.
        from jsonschema.validators import validator_for
        for tool in tools:
            for schema in [tool["inputSchema"], *([tool["outputSchema"]] if "outputSchema" in tool else [])]:
                validator_for(schema).check_schema(schema)
        example_count += validate_examples(manifest.parent, provider, catalog)
        snapshot = {"schema_version": 1, "provider_id": provider.id, "api_version": provider.api_version, "tools": tools}
        outputs[manifest.with_name("tool-spec.json")] = json_text(snapshot)
        outputs[manifest.with_name("TOOL_REFERENCE.md")] = render_reference(provider, tools)
    if not outputs:
        raise ValueError("No plugin MCP manifests found.")
    drift = [path for path, content in outputs.items() if not path.is_file() or path.read_text("utf-8-sig") != content]
    for path in drift:
        print(("DRIFT=" if check else "GENERATED=") + str(path))
    if not check:
        # Complete validation above prevents a bad example from partially updating documentation.
        for path in drift:
            path.write_text(outputs[path], encoding="utf-8", newline="\n")
    print(f"TOOL_SPEC_FILES={len(outputs)} EXAMPLE_FILES={example_count}")
    return 1 if check and drift else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog-file", type=Path, required=True, help="JSON captured from the actual Editor tools/list response")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="Regenerate per-plugin tool-spec.json and TOOL_REFERENCE.md")
    mode.add_argument("--check", action="store_true", help="Fail on any schema/reference drift without editing files")
    args = parser.parse_args(argv)
    try:
        return generate(json.loads(args.catalog_file.read_text("utf-8-sig")), check=args.check)
    except Exception as error:
        print(f"MCP tool documentation check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
