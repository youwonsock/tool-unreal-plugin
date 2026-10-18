"""Compatibility entry point. Canonical synchronization lives in Scripts/sync_runtime.py."""
import importlib.util
from pathlib import Path
import sys

_name = "_unreal_tools_runtime_sync"
if _name not in sys.modules:
    _spec = importlib.util.spec_from_file_location(_name, Path(__file__).resolve().parents[1] / "Scripts/sync_runtime.py")
    _module = importlib.util.module_from_spec(_spec)
    sys.modules[_name] = _module
    _spec.loader.exec_module(_module)
_implementation = sys.modules[_name]
MCP_SOURCE = _implementation.MCP_SOURCE
NATIVE_SOURCE = _implementation.NATIVE_SOURCE


def synchronize(root=None, *, check=False, include_native=True):
    return _implementation.synchronize(_implementation.REPOSITORY if root is None else root,
                                      check=check, include_native=include_native,
                                      mcp_source=MCP_SOURCE, native_source=NATIVE_SOURCE)


def main():
    return _implementation.main()


if __name__ == "__main__":
    raise SystemExit(main())
