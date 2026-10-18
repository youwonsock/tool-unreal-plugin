"""Smoke-test the packaged CubeShot installer without Unreal or site-packages."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest

PLUGIN = Path(__file__).resolve().parents[2]


class PortableTests(unittest.TestCase):
    def test_plugin_only_install_and_repeat(self):
        with tempfile.TemporaryDirectory(prefix="cubeshot-portable-") as temp:
            root = Path(temp)
            project = root / "Other.uproject"
            project.write_text('{"FileVersion":3}', "utf-8")
            target = root / "Plugins/CubeShot"
            shutil.copytree(PLUGIN / "MCP", target / "MCP", ignore=shutil.ignore_patterns("__pycache__", ".venv"))
            shutil.copy2(PLUGIN / "CubeShot.uplugin", target / "CubeShot.uplugin")
            command = [sys.executable, "-S", str(target / "MCP/setup_mcp.py"), "--port", "8017"]
            result = subprocess.run(command, cwd=root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            config = root / ".codex/config.toml"
            before = config.read_bytes()
            entry = tomllib.loads(before.decode("utf-8"))["mcp_servers"]["unreal_tools"]
            tools = json.loads((target / "MCP/manifest.json").read_text("utf-8"))["tools"]
            self.assertEqual(entry["enabled_tools"], tools)
            result = subprocess.run(command, cwd=root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(config.read_bytes(), before)
            result = subprocess.run([sys.executable, "-S", str(target / "MCP/call_mcp.py"), "--help"], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
