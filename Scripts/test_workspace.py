"""Compile the portable workspace core and service identity allocator, including standalone copies."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check_slate_boundaries():
    """Source ownership check only; this does not execute a UE adapter or Slate."""
    sources = [
        ROOT / "Unreal_CubeShot/Source/CubeShot/Private/SCubeShotPanel.cpp",
        ROOT / "Unreal_FBX_Exporter/Source/FBX_Exporter/Private/FBX_ExporterTab.cpp",
    ]
    forbidden = {
        "direct provider service call": r"\b(?:CubeShot::Service|FBXExporter::Application)::\w+\s*\(",
        "direct profile store access": r"\bProfiles\s*\(\s*\)",
        "direct job service access": r"\bJobs\s*\(\s*\)",
        "direct recovery scan service call": r"\bRefreshRecoveryDiagnostics\s*\(",
        "submission or profile request construction": r'Set(?:String|Object|Array|Bool)Field\s*\(\s*TEXT\("(?:action|profile|profile_id|mesh_paths|overrides|request_id|overwrite)"\)',
        "option normalization or profile serialization": r"\b(?:NormalizeJobOptions|FJsonSerializer::\w+|AtomicJson)\s*\(",
        "JSON serialization in Slate instead of cached display data": r"\bJsonText\s*\(",
        "map option construction in Slate instead of a controller command": r'SetArrayField\s*\(\s*TEXT\("maps"\)',
    }
    for source in sources:
        # The FBX file also contains an integration-test fixture which may read
        # real job summaries. The production UI above it must use the controller.
        text = source.read_text().split("namespace Testing", 1)[0]
        for description, pattern in forbidden.items():
            match = re.search(pattern, text)
            if match:
                line = text.count("\n", 0, match.start()) + 1
                raise RuntimeError(f"{source.relative_to(ROOT)}:{line}: {description}")
    print("Slate service ownership: 2 sources passed (static check)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        parser.error("Install clang++/g++ or set CXX/--compiler; Unreal is not required.")
    check_slate_boundaries()
    suites = [
        ("UnrealToolsWorkspace.h", ["WorkspaceModelTests.cpp"], []),
        ("UnrealToolsServiceInstance.h", ["ServiceInstanceTests.cpp", "ServiceInstanceOtherTests.cpp"], ["-pthread"]),
    ]
    with tempfile.TemporaryDirectory(prefix="unreal-workspace-") as temporary:
        for name, sources, flags in suites:
            headers = [ROOT / "Shared/Native" / name]
            headers += sorted(ROOT.glob(f"*/Source/*/Private/{name}"))
            if len(headers) != 3:
                raise RuntimeError(f"Expected canonical {name} and both plugin copies; run sync_runtime.py.")
            for index, header in enumerate(headers):
                # Only the chosen header is exposed: no repository/UE parent include paths.
                include = Path(temporary) / name / str(index)
                include.mkdir(parents=True)
                shutil.copy2(header, include / header.name)
                executable = include / ("checks.exe" if os.name == "nt" else "checks")
                subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags, "-I", str(include),
                                *(str(ROOT / "Tests" / source) for source in sources), "-o", str(executable)], check=True)
                print(header.relative_to(ROOT), flush=True)
                subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
