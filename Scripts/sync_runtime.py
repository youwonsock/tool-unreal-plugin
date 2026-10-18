"""Distribute the shared runtime into every manifest-bearing plugin, or check for drift."""
import argparse
from pathlib import Path
import re
import sys

REPOSITORY = Path(__file__).resolve().parents[1]
MCP_SOURCE = REPOSITORY / "MCP"
NATIVE_SOURCE = REPOSITORY / "Shared/Native"
sys.path.insert(0, str(MCP_SOURCE))
from unreal_tools_mcp.registry import plugin_descriptors, read_json, read_manifest

RUNTIME = "unreal_tools_mcp"


def synchronize(root=REPOSITORY, *, check=False, include_native=True, mcp_source=None, native_source=None):
    source_root = Path(mcp_source) if mcp_source is not None else MCP_SOURCE
    native_root = Path(native_source) if native_source is not None else NATIVE_SOURCE
    sources = [source_root / name for name in ("setup_mcp.py", "call_mcp.py", "requirements.txt")]
    if any(not path.is_file() for path in sources) or not (source_root / RUNTIME).is_dir():
        raise ValueError(f"Missing canonical MCP runtime files: {source_root}")
    def managed(path):
        return path.is_file() and not ({"__pycache__", ".venv", ".git"} & set(path.parts)) and path.suffix not in {".pyc", ".pyo"}
    sources += sorted(path for path in (source_root / RUNTIME).rglob("*") if managed(path))
    targets = []
    for descriptor in plugin_descriptors(Path(root)):
        directory = descriptor.parent / "MCP"
        if (directory / "manifest.json").is_file():
            provider = read_manifest(directory / "manifest.json")
            if provider.plugin != descriptor.stem:
                raise ValueError(f"Manifest does not match {descriptor}")
            targets.append((directory, descriptor))
    if not targets:
        raise ValueError("No plugin MCP manifests found.")
    drift = []
    if include_native and (not native_root.is_dir() or any(not (native_root / name).is_file() for name in
            ("UnrealToolsRuntime.h", "UnrealToolsRuntimeTests.inl"))):
        raise ValueError(f"Missing canonical shared C++ runtime files: {native_root}; use --python-only to skip native sync")
    native = sorted(path for path in native_root.rglob("*") if managed(path) and path.suffix in {".h", ".inl"}) if include_native else []
    for directory, descriptor in targets:
        expected = {directory / source.relative_to(source_root): source for source in sources}
        if native:
            modules = [entry["Name"] for entry in read_json(descriptor).get("Modules", [])
                       if entry.get("Type") in {"Editor", "EditorNoCommandlet", "EditorAndProgram"}]
            if (len(modules) != 1 or not isinstance(modules[0], str) or
                    not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", modules[0])):
                raise ValueError(f"Expected exactly one Editor module for shared native headers: {descriptor}")
            for source in native:
                expected[descriptor.parent / f"Source/{modules[0]}/Private" / source.relative_to(native_root)] = source
        for destination, source in expected.items():
            if not destination.is_file() or destination.read_bytes() != source.read_bytes():
                drift.append(destination)
                if not check:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(source.read_bytes())
        # This directory is wholly generated; never traverse outside it for cleanup.
        for destination in sorted((directory / RUNTIME).rglob("*")):
            if managed(destination) and destination not in expected:
                drift.append(destination)
                if not check:
                    destination.unlink()
    for path in drift:
        print(("DRIFT=" if check else "SYNCED=") + str(path))
    print(f"PROVIDERS={len(targets)} FILES_PER_PROVIDER={len(sources)} NATIVE_FILES={len(native)}")
    return 1 if check and drift else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--python-only", action="store_true", help="Skip native header distribution during independent Python work")
    args = parser.parse_args()
    try:
        return synchronize(check=args.check, include_native=not args.python_only)
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
