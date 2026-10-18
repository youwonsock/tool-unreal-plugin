"""Call Unreal MCP tools; a plugin-local copy is scoped to its own manifest.

Canonical entry point; sync_runtime.py distributes this file into each plugin.
"""
import unreal_tools_mcp as _runtime

if tuple(map(int, _runtime.__version__.split("."))) < (1, 2, 0):
    raise RuntimeError("Unreal Tools runtime 1.2.0 is required. Start a new Python process using the updated plugin.")

from unreal_tools_mcp.client import main as _main, run_calls as _run_calls


async def run_calls(calls, list_tools=False, *, project=None, url=None):
    return await _run_calls(calls, list_tools, project=project, url=url, anchor=__file__)


def main(argv=None, *, default_project=None):
    return _main(argv, default_project=default_project, anchor=__file__)


if __name__ == "__main__":
    raise SystemExit(main())
