"""Install one shared unreal_tools connection for all active project MCP plugins.

Canonical entry point; sync_runtime.py distributes this file into each plugin.
"""
import unreal_tools_mcp as _runtime

_runtime.require_runtime("2.0.0")

from unreal_tools_mcp.config import main as _main


def main(argv=None):
    return _main(argv, anchor=__file__)


if __name__ == "__main__":
    raise SystemExit(main())
