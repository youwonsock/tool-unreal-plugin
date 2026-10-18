"""Compatibility imports for older CubeShot scripts; the manifest owns the tool list."""
from pathlib import Path
from unreal_tools_mcp.registry import DEFAULT_URL, SETTINGS_SECTION, read_manifest
from unreal_tools_mcp.registry import resolve_project as _resolve_project

TOOLS = read_manifest(Path(__file__).with_name("manifest.json")).tools


def resolve_project(project=None):
    return _resolve_project(project, anchor=__file__)
