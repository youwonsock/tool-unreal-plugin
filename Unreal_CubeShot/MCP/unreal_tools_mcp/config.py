"""Conservative Codex configuration migration and Unreal MCP server setup."""
import argparse
from copy import deepcopy
import difflib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import tomllib
import uuid
from urllib.parse import urlsplit

from .registry import (DEFAULT_URL, SERVER_NAME, SETTINGS_SECTION, active_providers,
                       local_provider, resolve_project)


def read_text(path):
    return path.read_text("utf-8-sig") if path.exists() else ""


def read_config(project_file):
    data = tomllib.loads(read_text(project_file.parent / ".codex/config.toml"))
    servers = data.get("mcp_servers", {})
    if not isinstance(servers, dict):
        raise ValueError("mcp_servers must be a TOML table.")
    return data


def state_path(project_file):
    return project_file.parent / ".codex/unreal-tools-state.json"


def read_state(project_file):
    path = state_path(project_file)
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text("utf-8-sig"))
        tools = data.get("enabled_tools") if isinstance(data, dict) else None
        if (type(data.get("schema_version")) is not int or data.get("schema_version") != 1 or data.get("server") != SERVER_NAME or
                not isinstance(tools, list) or not tools or any(not isinstance(tool, str) for tool in tools) or
                len(set(tools)) != len(tools)):
            raise ValueError("Invalid generated setup state")
        return data
    except (ValueError, AttributeError) as error:
        raise ValueError(f"Cannot validate {path}: {error}. Review the generated state before changing configuration.") from error


def ini_settings(text):
    values, active = {}, False
    for line in text.splitlines():
        match = re.match(r"\s*\[([^]]+)\]", line)
        if match:
            active = match[1] == SETTINGS_SECTION
        elif active and (match := re.match(r"\s*(\w+)\s*=\s*(.*?)\s*(?:;.*)?$", line)):
            values[match[1]] = match[2].strip('"')
    return values


def update_ini(text, values):
    lines, seen, active, insertion = text.splitlines(keepends=True), set(), False, None
    for index, line in enumerate(lines):
        header = re.match(r"\s*\[([^]]+)\]", line)
        if header:
            if active:
                insertion = index
            active = header[1] == SETTINGS_SECTION
            if active:
                insertion = len(lines)
        elif active:
            match = re.match(r"\s*(\w+)\s*=", line)
            if match and match[1] in values:
                key = match[1]
                seen.add(key)
                if ini_settings(f"[{SETTINGS_SECTION}]\n{line}").get(key) != values[key]:
                    lines[index] = f"{key}={values[key]}\n"
    missing = [f"{key}={value}\n" for key, value in values.items() if key not in seen]
    if insertion is None:
        return text.rstrip("\r\n") + ("\n\n" if text else "") + f"[{SETTINGS_SECTION}]\n" + "".join(missing)
    if missing:
        if insertion and not lines[insertion - 1].endswith("\n"):
            lines[insertion - 1] += "\n"
        lines[insertion:insertion] = missing
    return "".join(lines)


def positive_seconds(value, label):
    if type(value) not in (int, float) or not 0 < value < float("inf"):
        raise ValueError(f"{label} must be a positive finite number.")
    return value


def endpoint_parts(url):
    parsed = urlsplit(url)
    if (parsed.scheme != "http" or parsed.hostname not in {"127.0.0.1", "localhost", "::1"}
            or parsed.username or parsed.password or parsed.query or parsed.fragment):
        raise ValueError(f"Expected a local Unreal HTTP MCP URL, got {url!r}.")
    port = 80 if parsed.port is None else parsed.port
    if not 1 <= port <= 65535 or not re.fullmatch(r"/[A-Za-z0-9_/-]+", parsed.path) or "//" in parsed.path:
        raise ValueError(f"Invalid Unreal MCP port/path: {url!r}.")
    return parsed, port, parsed.path


def owned_entries(config, providers):
    servers = config.get("mcp_servers", {})
    names = {SERVER_NAME, *(provider.id for provider in providers)}
    entries = {name: servers[name] for name in sorted(names) if name in servers}
    for name, entry in entries.items():
        if not isinstance(entry, dict) or not isinstance(entry.get("url"), str):
            raise ValueError(f"mcp_servers.{name} must be an HTTP server table with a URL.")
        endpoint_parts(entry["url"])
    urls = {(endpoint_parts(entry["url"])[1], endpoint_parts(entry["url"])[2]) for entry in entries.values()}
    if len(urls) > 1:
        raise ValueError("Plugin MCP entries use different server addresses. Resolve the conflict before merging them.")
    return entries


def validate_ownership(entries, providers, replace_names, generated_state=None):
    groups = {provider.id: set(provider.tools) for provider in providers}
    known = set().union(*groups.values())
    fields = {"url", "enabled_tools", "disabled_tools", "enabled", "startup_timeout_sec", "tool_timeout_sec"}
    for name, entry in entries.items():
        if name in replace_names:
            continue
        tools = entry.get("enabled_tools")
        valid = (isinstance(tools, list) and all(isinstance(tool, str) for tool in tools)
                 and len(set(tools)) == len(tools))
        selected = set(tools) if valid else set()
        if name == SERVER_NAME:
            if generated_state is not None:
                owned = valid and tools == generated_state["enabled_tools"]
            else:
                # Only exact full provider groups qualify without generation history.
                # A partial custom allow list cannot be inferred to be managed.
                owned = bool(selected) and selected <= known and all(
                    not selected.intersection(set(provider.tools)) or
                    selected.intersection(set(provider.tools)) in
                    [set(provider.tools), *(set(group) for group in provider.compatible_tool_sets)]
                    for provider in providers)
        else:
            provider = next(provider for provider in providers if provider.id == name)
            # A generated state always records the unified table. A later legacy
            # table is a user edit, not a prior managed installation to expand.
            owned = generated_state is None and selected in [set(provider.tools), *(set(group) for group in provider.compatible_tool_sets)]
        if (not valid or not owned or entry.get("enabled", True) is not True or entry.get("disabled_tools")
                or set(entry) - fields):
            raise ValueError(f"mcp_servers.{name} has custom restrictions or unknown settings. No files changed. "
                             "Review --dry-run --replace-owned-config, or edit that entry manually.")


def update_codex(text, entries, target):
    before = tomllib.loads(text)
    if set(entries) == {SERVER_NAME} and entries[SERVER_NAME] == target:
        return text
    block = f"[mcp_servers.{SERVER_NAME}]\n" + "".join(
        f"{key} = {json.dumps(value, ensure_ascii=False)}\n" for key, value in target.items())
    # Only explicit, flat server tables are edited. Parsing before and after protects
    # unrelated TOML values, including multiline strings and arrays of tables.
    headers = list(re.finditer(r"(?m)^[ \t]*(\[\[?[^\n]+?\]\]?)[ \t]*(?:#[^\n]*)?$", text))
    ranges = []
    for name in entries:
        expected = f"[mcp_servers.{name}]"
        matches = [i for i, header in enumerate(headers) if header[1] == expected]
        if len(matches) != 1 or any(header[1].startswith(f"[mcp_servers.{name}.") for header in headers):
            raise ValueError(f"Nonstandard or nested TOML layout for {name}; edit that entry manually.")
        index = matches[0]
        ranges.append((headers[index].start(), headers[index + 1].start() if index + 1 < len(headers) else len(text)))
    updated = text
    for start, end in sorted(ranges, reverse=True):
        updated = updated[:start] + updated[end:]
    updated = updated.rstrip("\r\n") + ("\n\n" if updated else "") + block
    after = tomllib.loads(updated)
    expected = deepcopy(before)
    servers = expected.setdefault("mcp_servers", {})
    for name in entries:
        servers.pop(name)
    servers[SERVER_NAME] = target
    if after != expected:
        raise ValueError("Cannot modify this TOML layout without changing unrelated settings; edit it manually.")
    return updated


def prepare(project_file, port=None, url_path=None, *, required=None, replace_names=()):
    providers, active = active_providers(project_file, required=required)
    project = project_file.parent
    codex = project / ".codex/config.toml"
    config = read_config(project_file)
    entries = owned_entries(config, providers)
    validate_ownership(entries, providers, set(entries) if replace_names == "all" else set(replace_names),
                       None if replace_names == "all" else read_state(project_file))
    default_ini = project / "Config/DefaultEditorPerProjectUserSettings.ini"
    user_inis = sorted((project / "Saved/Config").glob("*/EditorPerProjectUserSettings.ini"))
    settings = ini_settings(read_text(default_ini))
    platform = {"win32": "WindowsEditor", "darwin": "MacEditor"}.get(sys.platform, "LinuxEditor")
    for path in user_inis:
        if path.parent.name == platform:
            settings.update(ini_settings(read_text(path)))
    old_url = next(iter(entries.values()))["url"] if entries else DEFAULT_URL
    old_parts, old_port, old_path = endpoint_parts(old_url)
    selected_port = port if port is not None else int(settings.get("ServerPortNumber", old_port))
    selected_path = url_path if url_path is not None else settings.get("ServerUrlPath", old_path)
    # Use parsed hostname to retain localhost/IPv6 spelling while replacing the port.
    host = f"[{old_parts.hostname}]" if old_parts.hostname == "::1" else old_parts.hostname
    url = f"http://{host}:{selected_port}{selected_path}"
    endpoint_parts(url)
    if entries and ((port is None and selected_port != old_port) or (url_path is None and selected_path != old_path)):
        raise ValueError("Editor and client MCP addresses differ. Specify --port/--url-path to resolve the conflict.")
    def timeout(key, default):
        values = [positive_seconds(entry[key], key) for entry in entries.values() if key in entry]
        return max(values) if values else default
    target = {"url": url, "enabled_tools": [tool for provider in active for tool in provider.tools],
              "startup_timeout_sec": timeout("startup_timeout_sec", 20), "tool_timeout_sec": timeout("tool_timeout_sec", 180)}
    changes = {codex: update_codex(read_text(codex), entries, target)}
    values = {"bAutoStartServer": "True", "ServerPortNumber": str(selected_port), "ServerUrlPath": selected_path}
    changes[default_ini] = update_ini(read_text(default_ini), values)
    for path in user_inis:
        overrides = ini_settings(read_text(path))
        if overrides:
            changes[path] = update_ini(read_text(path), {key: value for key, value in values.items() if key in overrides})
    state = {"schema_version": 1, "server": SERVER_NAME, "enabled_tools": target["enabled_tools"],
             "providers": [{"id": provider.id, "api_version": provider.api_version,
                            "tools": list(provider.tools)} for provider in active]}
    # The state is the commit marker. It must be replaced after every configuration file.
    changes[state_path(project_file)] = json.dumps(state, ensure_ascii=False, indent=2) + "\n"
    return url, active, {path: content for path, content in changes.items() if content != read_text(path)}


def write_changes(changes, *, dry_run=False):
    if dry_run:
        for path, content in changes.items():
            print("".join(difflib.unified_diff(read_text(path).splitlines(True), content.splitlines(True),
                                            fromfile=str(path), tofile=str(path))), end="")
        return
    paths = sorted(changes, key=lambda path: path.name == "unreal-tools-state.json")
    originals, temporary, backups, applied = {}, {}, {}, []
    run = uuid.uuid4().hex[:12]

    def stage(path, data):
        with tempfile.NamedTemporaryFile(mode="wb", dir=path.parent, prefix=path.name + ".",
                                         delete=False) as handle:
            staged = Path(handle.name)
            temporary.setdefault(path, []).append(staged)
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        return staged

    try:
        # Prepare every replacement and every per-run recovery copy before modifying a target.
        for path in paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            originals[path] = path.read_bytes() if path.exists() else None
            stage(path, changes[path].encode("utf-8"))
            if originals[path] is not None:
                backup = path.with_name(path.name + f".unreal-tools.{run}.bak")
                backup.write_bytes(originals[path])
                backups[path] = backup
                initial = path.with_name(path.name + ".unreal-tools.bak")
                if not initial.exists():
                    initial.write_bytes(originals[path])
        for path in paths:
            os.replace(temporary[path][0], path)
            applied.append(path)
    except Exception as original:
        recovery_errors = []
        for path in reversed(applied):
            try:
                if originals[path] is None:
                    path.unlink(missing_ok=True)
                else:
                    os.replace(stage(path, originals[path]), path)
            except Exception as recovery:
                recovery_errors.append(f"{path}: {recovery}; recovery copy: {backups.get(path, 'originally absent')}")
        if recovery_errors:
            raise OSError(f"Configuration update failed ({original}); rollback also failed: " +
                          "; ".join(recovery_errors)) from original
        raise
    finally:
        for staged_paths in temporary.values():
            for staged in staged_paths:
                try:
                    staged.unlink(missing_ok=True)
                except OSError as cleanup:
                    print(f"Warning: could not remove configuration staging file {staged}: {cleanup}", file=sys.stderr)
    for path in paths:
        print("UPDATED=" + str(path))


def main(argv=None, *, anchor):
    required = local_provider(anchor)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--port", type=int)
    parser.add_argument("--url-path")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--replace-owned-config", action="store_true",
                        help="Replace custom restrictions/settings only in discovered provider and unreal_tools entries")
    if required:
        parser.add_argument("--replace-" + required.replace("_", "-"), dest="replace_local", action="store_true",
                            help="Compatibility option: replace only this provider's legacy client entry")
    args = parser.parse_args(argv)
    try:
        project_file = resolve_project(args.project, anchor=anchor)
        replace = "all" if args.replace_owned_config else ([required] if getattr(args, "replace_local", False) else [])
        url, providers, changes = prepare(project_file, args.port, args.url_path, required=required, replace_names=replace)
        write_changes(changes, dry_run=args.dry_run)
        print("PROJECT=" + str(project_file))
        print("MCP_URL=" + url)
        print("PROVIDERS=" + ",".join(provider.id for provider in providers))
        print("TOOLS=" + str(sum(len(provider.tools) for provider in providers)))
        print("DRY_RUN=true" if args.dry_run else "Setup complete. Reopen Unreal Editor and reload the unreal_tools MCP connection.")
        return 0
    except (OSError, ValueError, TypeError, RuntimeError) as error:
        print(f"Unreal Tools MCP setup failed: {error}", file=sys.stderr)
        return 1
