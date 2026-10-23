"""Small project fixtures with real provider manifests, without Unreal binaries."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

MCP = Path(__file__).resolve().parents[1]
REPO = MCP.parent
sys.path.insert(0, str(MCP))
sys.path.insert(0, str(REPO / "Scripts"))
from unreal_tools_mcp.registry import read_manifest

PLUGINS = {path.parent.parent.name: path.parent.parent for path in REPO.glob("*/MCP/manifest.json")}


class ProjectCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="unreal-tools-mcp-")
        self.addCleanup(self.temp.cleanup)
        self.root = (Path(self.temp.name) / "Other Project").resolve()
        self.root.mkdir()
        self.project = self.root / "Other.uproject"
        self.write_project({"FileVersion": 3})
        self.plugin_paths = {}

    def write_project(self, data):
        self.project.write_text(json.dumps(data), "utf-8")

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, "utf-8")
        return path

    def add_plugin(self, name, *, root=None):
        source = PLUGINS[name]
        target = (root or self.root / "Plugins") / name
        shutil.copytree(source / "MCP", target / "MCP", ignore=shutil.ignore_patterns("__pycache__", ".venv"))
        descriptor = next(source.glob("*.uplugin"))
        shutil.copy2(descriptor, target / descriptor.name)
        provider = read_manifest(target / "MCP/manifest.json")
        self.plugin_paths[provider.id] = target
        return provider

    def quiet(self, function, *args, **kwargs):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return function(*args, **kwargs)


def server(name, tools, *, url="http://127.0.0.1:8000/mcp", **extra):
    entry = {"url": url, "enabled_tools": list(tools), **extra}
    return f"[mcp_servers.{name}]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in entry.items())
