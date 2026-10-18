"""Install one shared unreal_tools connection for all active project MCP plugins.

Canonical entry point; sync_runtime.py distributes this file into each plugin.
"""
import unreal_tools_mcp as _runtime

if tuple(map(int, _runtime.__version__.split("."))) < (1, 2, 0):
    raise RuntimeError("Unreal Tools runtime 1.2.0 is required. Start a new Python process using the updated plugin.")

from unreal_tools_mcp.config import main as _main


def main(argv=None):
    return _main(argv, anchor=__file__)


if __name__ == "__main__":
    raise SystemExit(main())
