"""Discover project-owned MCP providers without importing plugin code or loading Unreal."""
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import sys

from . import __version__

SERVER_NAME = "unreal_tools"
SETTINGS_SECTION = "/Script/ModelContextProtocolEngine.ModelContextProtocolSettings"
DEFAULT_URL = "http://127.0.0.1:8000/mcp"
PLATFORM = {"win32": "Win64", "darwin": "Mac"}.get(sys.platform, "Linux")
IGNORED = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", ".venv", "__pycache__"}


@dataclass(frozen=True)
class Provider:
    id: str
    plugin: str
    status_tool: str
    tools: tuple[str, ...]
    manifest: Path
    enabled: bool
    api_version: str = "1.0.0"
    minimum_runtime_version: str = "1.0.0"
    capabilities: tuple[str, ...] = ()
    job_tools: dict[str, str] = field(default_factory=dict)
    compatible_tool_sets: tuple[tuple[str, ...], ...] = ()


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"\d+\.\d+\.\d+", value):
        raise ValueError(f"Expected a major.minor.patch version, got {value!r}.")
    return tuple(map(int, value.split(".")))


def ensure_runtime(provider):
    if version(__version__) < version(provider.minimum_runtime_version):
        raise RuntimeError(f"{provider.plugin} requires Unreal Tools runtime {provider.minimum_runtime_version}; "
                           f"loaded {__version__}. Start a new Python process using the updated plugin.")


def resolve_project(project=None, *, anchor):
    if project is not None:
        path = Path(project).expanduser().resolve()
        if path.is_file() and path.suffix.lower() == ".uproject":
            return path
        directories = [path] if path.is_dir() else []
    else:
        directories = Path(anchor).resolve().parents
    for directory in directories:
        projects = sorted(directory.glob("*.uproject"))
        if len(projects) == 1:
            return projects[0].resolve()
        if len(projects) > 1:
            raise ValueError(f"Multiple .uproject files in {directory}; pass --project <file.uproject>.")
    raise ValueError("Cannot find the target .uproject. Pass --project <file.uproject>.")


def read_json(path):
    data = json.loads(Path(path).read_text("utf-8-sig"))
    if not isinstance(data, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return data


def local_provider(anchor):
    manifest = Path(anchor).resolve().parent / "manifest.json"
    return read_manifest(manifest).id if manifest.exists() else None


def read_manifest(path, *, enabled=True):
    path = Path(path).resolve()
    data = read_json(path)
    if type(data.get("schema_version")) is not int or data["schema_version"] != 1:
        raise ValueError(f"Unsupported MCP manifest schema: {path}")
    provider_id, plugin = data.get("id"), data.get("plugin")
    if not isinstance(provider_id, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", provider_id):
        raise ValueError(f"Invalid provider id: {path}")
    if provider_id == SERVER_NAME or not isinstance(plugin, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", plugin):
        raise ValueError(f"Invalid/reserved plugin identity: {path}")
    names = data.get("tools")
    if (not isinstance(names, list) or not names or any(not isinstance(name, str) or
            not re.fullmatch(r"[a-z][a-z0-9_]*", name) for name in names) or len(set(names)) != len(names)):
        raise ValueError(f"Invalid or duplicate tool names: {path}")
    if data.get("status_tool") not in names:
        raise ValueError(f"status_tool must belong to tools: {path}")
    api = data.get("api_version", "1.0.0")
    minimum = data.get("minimum_runtime_version", "1.0.0")
    version(api)
    version(minimum)
    capabilities = data.get("capabilities", [])
    if (not isinstance(capabilities, list) or any(not isinstance(item, str) or not item for item in capabilities)
            or len(set(capabilities)) != len(capabilities)):
        raise ValueError(f"Invalid capabilities: {path}")
    jobs = data.get("job_tools", {})
    if (not isinstance(jobs, dict) or set(jobs) - {"start", "status", "control"}
            or any(not isinstance(name, str) or name not in names for name in jobs.values())
            or len(set(jobs.values())) != len(jobs)):
        raise ValueError(f"Invalid job_tools roles: {path}")
    previous = data.get("compatible_tool_sets", [])
    if (not isinstance(previous, list) or any(not isinstance(group, list) or not group or
            any(not isinstance(name, str) or name not in names for name in group) or
            len(set(group)) != len(group) for group in previous)):
        raise ValueError(f"Invalid compatible_tool_sets: {path}")
    return Provider(provider_id, plugin, data["status_tool"], tuple(names), path, enabled,
                    api, minimum, tuple(capabilities), dict(jobs), tuple(tuple(group) for group in previous))


def plugin_descriptors(root):
    if not root.is_dir():
        return
    for directory, subdirs, files in os.walk(root):
        subdirs[:] = sorted(name for name in subdirs if name not in IGNORED)
        descriptors = sorted(name for name in files if name.endswith(".uplugin"))
        if descriptors:
            subdirs[:] = []  # Unreal does not search inside an already discovered plugin.
            for name in descriptors:
                yield (Path(directory) / name).resolve()


def editor_supported(settings):
    for dimension, value in (("Platform", PLATFORM), ("Target", "Editor")):
        allow = settings.get(dimension + "AllowList", settings.get("Whitelist" + dimension + "s"))
        deny = settings.get(dimension + "DenyList", settings.get("Blacklist" + dimension + "s", []))
        if (allow and value not in allow) or value in deny:
            return False
    platforms = settings.get("SupportedTargetPlatforms")
    return not platforms or PLATFORM in platforms


def discover(project_file):
    project_file = Path(project_file).resolve()
    descriptor = read_json(project_file)
    entries = descriptor.get("Plugins", [])
    overrides = {entry["Name"]: entry for entry in entries}
    roots = [project_file.parent / "Plugins"]
    roots.extend((project_file.parent / directory).resolve() for directory in descriptor.get("AdditionalPluginDirectories", []))
    descriptors = sorted({path for root in roots for path in plugin_descriptors(root)})
    providers, ids, plugins, tools = [], {}, {}, {}
    for path in descriptors:
        manifest = path.parent / "MCP/manifest.json"
        if not manifest.is_file():
            continue
        data = read_json(path)
        override = overrides.get(path.stem, {})
        # These roots contain project plugins. UE enables them by default when
        # EnabledByDefault is omitted (FPlugin::IsEnabledByDefault).
        enabled = override.get("Enabled", data.get("EnabledByDefault", True)) is True
        enabled = enabled and editor_supported(data) and editor_supported(override)
        modules = data.get("Modules", [])
        enabled = enabled and any(module.get("Type") in {
            "Editor", "EditorNoCommandlet", "EditorAndProgram", "UncookedOnly", "Developer", "DeveloperTool",
            "Runtime", "RuntimeNoCommandlet", "RuntimeAndProgram"
        } and editor_supported(module) for module in modules)
        provider = read_manifest(manifest, enabled=enabled)
        if provider.plugin != path.stem:
            raise ValueError(f"Manifest plugin {provider.plugin} does not match {path.name}")
        for table, keys, label in ((ids, [provider.id], "provider"), (plugins, [provider.plugin], "plugin"),
                                   (tools, provider.tools, "tool")):
            for key in keys:
                normalized = key.casefold()
                if normalized in table:
                    raise ValueError(f"Duplicate {label} {key}: {table[normalized]} and {manifest}")
                table[normalized] = manifest
        providers.append(provider)
    return sorted(providers, key=lambda provider: provider.id)


def active_providers(project_file, *, required=None):
    providers = discover(project_file)
    active = [provider for provider in providers if provider.enabled]
    if not active:
        raise ValueError("No active Editor plugins with MCP/manifest.json were found in this project.")
    if required and required not in {provider.id for provider in active}:
        raise ValueError(f"Provider {required} is missing or disabled in this project.")
    for provider in active:
        ensure_runtime(provider)
    for entry in read_json(project_file).get("Plugins", []):
        if entry.get("Name") == "ModelContextProtocol" and entry.get("Enabled") is False:
            raise ValueError("Enable ModelContextProtocol in the target .uproject before using MCP.")
    return providers, active
