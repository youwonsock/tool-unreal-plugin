"""Call Unreal MCP tools with project identity checks and sequential batch execution."""
import argparse
import asyncio
from copy import deepcopy
from contextlib import asynccontextmanager
from datetime import timedelta
import json
from pathlib import Path
import sys
import time
import uuid

from . import __version__
from .config import endpoint_parts, positive_seconds, read_config
from .registry import DEFAULT_URL, SERVER_NAME, active_providers, local_provider, resolve_project, version


class ProjectIdentityError(RuntimeError):
    pass


class FeatureUnavailableError(RuntimeError):
    pass


class ToolAvailabilityError(RuntimeError):
    pass


class SubmissionUncertainError(RuntimeError):
    """An unresolved start response may represent an already accepted job."""
    pass


def error_message(error):
    # The SDK transport wraps operation errors in nested TaskGroup ExceptionGroups.
    if isinstance(error, BaseExceptionGroup):
        return "; ".join(dict.fromkeys(error_message(child) for child in error.exceptions))
    return f"{type(error).__name__}: {error}"


def failed(result):
    return bool(result.get("isError") or result.get("structuredContent", {}).get("success") is False)


def save_evidence(project_file, folder, evidence, *, kind="MCPCalls"):
    # Evidence is useful even when a request fails, but it is never the operation itself.
    try:
        directory = project_file.parent / "Saved" / folder / kind
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8] + ".json")
        path.write_text(json.dumps(evidence, ensure_ascii=False, indent=2), "utf-8")
        print("MCP_EVIDENCE=" + str(path), flush=True)
    except Exception as error:
        try:
            print(f"Warning: MCP evidence could not be saved: {error_message(error)}", file=sys.stderr, flush=True)
        except Exception:
            # Closed/broken diagnostic streams are still a logging failure.
            pass


def required_features(provider, requests):
    features = set()
    for request in requests:
        if request["tool"] not in provider.tools:
            continue
        arguments = request.get("arguments", {})
        if request["tool"] in provider.job_tools.values():
            features.add("jobs")
        if request["tool"].endswith("_profiles"):
            features.add("profiles")
        if request["tool"].endswith("_export_png") and "overwrite" in arguments:
            features.add("png_overwrite")
        if "mesh_paths" in arguments and request["tool"].endswith("_preflight"):
            features.add("batch_preflight")
        if set(arguments) & {"background", "background_texture", "solid_color", "auto_fit", "distance",
                             "camera_distance", "fov", "field_of_view", "ortho_width", "profile_id",
                             "export_png", "png_directions", "layout", "output_layout"}:
            features.add("extended_options")
        # PNG framing existed in the standalone export_png tool before API 1.1.
        # Passing those settings to generation/preflight is a new capability.
        if (not request["tool"].endswith("_export_png") and
                set(arguments) & {"trim_transparent", "padding_pixels", "quarter_turns"}):
            features.add("extended_options")
    return features


def check_features(provider, context, requests):
    required = required_features(provider, requests)
    if not required:
        return
    capabilities = context.get("capabilities", [])
    if isinstance(capabilities, dict):
        capabilities = [name for name, enabled in capabilities.items() if enabled is True]
    if not isinstance(capabilities, list) or any(not isinstance(item, str) for item in capabilities):
        capabilities = []
    actual = context.get("api_version", "1.0.0")
    if version(actual) < version(provider.api_version) or not required <= set(capabilities):
        raise FeatureUnavailableError(f"{provider.plugin} Editor API {actual} cannot provide "
                                      f"{', '.join(sorted(required))}; expected API {provider.api_version}. "
                                      "Build the updated plugin and restart the target Editor.")


def connection(project_file, providers, url=None):
    servers = read_config(project_file).get("mcp_servers", {})
    unified = servers.get(SERVER_NAME)
    entries, allowed = [], set()
    for provider in providers:
        entry = unified if unified is not None else servers.get(provider.id, {})
        if not isinstance(entry, dict) or "command" in entry:
            raise ValueError("Unreal MCP requires an HTTP server configuration.")
        for key in ("enabled_tools", "disabled_tools"):
            if key in entry and (not isinstance(entry[key], list) or
                                 any(not isinstance(item, str) for item in entry[key])):
                raise ValueError(f"{key} must be an array of tool names.")
        if entry.get("enabled", True) is not True:
            continue
        if any(entry.get(key) for key in ("http_headers", "env_http_headers", "bearer_token_env_var")):
            raise ValueError("This local Unreal client does not support authenticated HTTP endpoints.")
        allowed.update(set(provider.tools).intersection(entry.get("enabled_tools", provider.tools))
                       - set(entry.get("disabled_tools", [])))
        entries.append(entry)
    if not allowed:
        raise ValueError("No tools are allowed by the project MCP configuration.")
    endpoints = {entry["url"] for entry in entries if "url" in entry}
    addresses = {endpoint_parts(endpoint)[1:] for endpoint in endpoints}
    if len(addresses) > 1:
        raise ValueError("Plugin MCP entries use different server addresses; run setup after resolving the conflict.")
    endpoint = url or (sorted(endpoints)[0] if endpoints else DEFAULT_URL)
    endpoint_parts(endpoint)
    def timeout(key, default):
        return max((positive_seconds(entry.get(key, default), key) for entry in entries), default=default)
    return endpoint, allowed, timeout("startup_timeout_sec", 20), timeout("tool_timeout_sec", 180)


@asynccontextmanager
async def open_session(endpoint, timeout):
    # Setup, help and registry discovery work with Python's standard library alone.
    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client
    from mcp.types import Implementation

    duration = timedelta(seconds=timeout)
    async with streamablehttp_client(endpoint, timeout=duration, sse_read_timeout=duration) as (read, write, _):
        async with ClientSession(read, write, read_timeout_seconds=duration,
                                 client_info=Implementation(name="unreal-tools-plugin-client", version=__version__)) as session:
            yield session


def serialize(result):
    return result.model_dump(mode="json", by_alias=True, exclude_none=True)


async def tool_catalog(session):
    tools, cursor, seen = [], None, set()
    while True:
        page = serialize(await session.list_tools(cursor=cursor))
        tools.extend(page["tools"])
        cursor = page.get("nextCursor")
        if not cursor:
            return tools
        if cursor in seen:
            raise RuntimeError("MCP server returned a repeated tools/list cursor.")
        seen.add(cursor)


async def run_calls(calls, list_tools=False, *, project=None, url=None, anchor):
    project_file = resolve_project(project, anchor=anchor)
    scope = local_provider(anchor)
    _, providers = active_providers(project_file, required=scope)
    if scope:
        providers = [provider for provider in providers if provider.id == scope]
    endpoint, allowed, startup_timeout, tool_timeout = connection(project_file, providers, url)
    if not isinstance(calls, list):
        raise ValueError("A batch must be an array of tool/arguments objects.")
    for request in calls:
        if (not isinstance(request, dict) or not isinstance(request.get("tool"), str)
                or request["tool"] not in allowed or not isinstance(request.get("arguments", {}), dict)
                or set(request) - {"tool", "arguments"}):
            raise ValueError("Invalid request or tool outside the allow list: " + repr(request))
    calls = deepcopy(calls)
    starts = {provider.job_tools.get("start"): provider for provider in providers if provider.job_tools.get("start")}
    folder = providers[0].plugin if scope else "UnrealTools"
    for request in calls:
        if request["tool"] in starts:
            arguments = request.setdefault("arguments", {})
            if "request_id" not in arguments:
                arguments["request_id"] = str(uuid.uuid4())
            if not isinstance(arguments["request_id"], str) or not arguments["request_id"]:
                raise ValueError("request_id must be a nonempty string.")
            print("MCP_REQUEST_ID=" + arguments["request_id"], flush=True)
            save_evidence(project_file, folder, {"project_file": str(project_file), "request": request}, kind="MCPRequests")
    requested = {request["tool"] for request in calls}
    relevant = [provider for provider in providers if set(provider.tools) & (allowed if list_tools else requested)]
    for provider in relevant:
        if provider.status_tool not in allowed:
            raise ValueError(f"{provider.status_tool} must be allowed to verify the target project.")

    evidence = {"url": endpoint, "client": "mcp-python-sdk", "scope": scope or SERVER_NAME,
                "expected_project_file": str(project_file), "exchange": []}
    results = []
    pending_start = None
    try:
        async with open_session(endpoint, tool_timeout) as session:
            initialized = await asyncio.wait_for(session.initialize(), timeout=startup_timeout)
            evidence["initialize"] = serialize(initialized)
            catalog = await tool_catalog(session)
            available = {tool["name"] for tool in catalog}
            required = (allowed if list_tools else requested) | {provider.status_tool for provider in relevant}
            missing = required - available
            if missing:
                raise ToolAvailabilityError("Editor is missing MCP tools: " + ", ".join(sorted(missing)) +
                                            ". Build the enabled plugins and restart the target Editor.")

            async def call(name, arguments):
                data = serialize(await session.call_tool(name, arguments))
                evidence["exchange"].append({"tool": name, "arguments": arguments, "result": data})
                return data

            # Validate every provider first, including later providers in a mixed batch.
            for provider in relevant:
                status = await call(provider.status_tool, {})
                context = status.get("structuredContent", {})
                actual = context.get("project_file")
                if (failed(status) or context.get("success") is not True or not isinstance(actual, str)
                        or not Path(actual).is_absolute() or Path(actual).resolve() != project_file):
                    raise ProjectIdentityError(f"MCP endpoint is not the expected Editor for {provider.id}: "
                                               f"expected {project_file}, got {actual!r}.")
                check_features(provider, context, calls)
            if list_tools:
                listed = {"tools": [tool for tool in catalog if tool["name"] in allowed]}
                evidence["tools_list"] = listed
                results.append(listed)
            for request in calls:
                pending_start = request if request["tool"] in starts else None
                result = await call(request["tool"], request.get("arguments", {}))
                pending_start = None
                results.append(result)
                if failed(result):
                    break
        return results
    except Exception as error:
        evidence["exception"] = error_message(error)
        if pending_start is not None:
            # The server may have persisted the job before the response was lost.
            # Query the same identity in a fresh verified connection; never resubmit.
            provider = starts[pending_start["tool"]]
            request_id = pending_start["arguments"]["request_id"]
            print("MCP_RECONCILE_REQUEST_ID=" + request_id, flush=True)
            try:
                reconciled = await run_calls([{"tool": provider.job_tools["status"],
                                               "arguments": {"request_id": request_id}}],
                                             project=project_file, url=url, anchor=anchor)
                if reconciled and not failed(reconciled[0]):
                    evidence["reconciled_by_request_id"] = request_id
                    results.append(reconciled[0])
                    return results  # A transport failure stops any remaining batch entries.
            except (ProjectIdentityError, FeatureUnavailableError, ToolAvailabilityError):
                raise
            except Exception as query_error:
                evidence["reconciliation_exception"] = error_message(query_error)
            raise SubmissionUncertainError(f"Job submission response was lost; request_id={request_id}. "
                                           "Query that request ID after reconnecting. The client did not resubmit. "
                                           f"Original error: {error_message(error)}") from error
        raise
    finally:
        save_evidence(project_file, folder, evidence)


def job_context(result):
    context = result.get("structuredContent", {})
    return context.get("job", context)


async def run_and_wait(calls, *, project=None, url=None, anchor, wait_timeout=1800, poll_interval=1):
    """Only job start/status support waiting; each poll uses a newly verified connection."""
    positive_seconds(wait_timeout, "wait timeout")
    positive_seconds(poll_interval, "poll interval")
    if not isinstance(calls, list) or len(calls) != 1:
        raise ValueError("--wait requires one job_start or job_status call, not a batch.")
    project_file = resolve_project(project, anchor=anchor)
    _, providers = active_providers(project_file, required=local_provider(anchor))
    provider = next((provider for provider in providers if calls[0].get("tool") in
                     [provider.job_tools.get("start"), provider.job_tools.get("status")]), None)
    if provider is None:
        raise ValueError("--wait requires a manifest job_start or job_status tool.")
    calls = deepcopy(calls)
    arguments = calls[0].setdefault("arguments", {})
    if not isinstance(arguments, dict):
        raise ValueError("Job arguments must be a JSON object.")
    if calls[0]["tool"] == provider.job_tools["start"]:
        arguments.setdefault("request_id", str(uuid.uuid4()))
        identifier = ("request_id", arguments["request_id"])
    else:
        identifier = ("job_id", arguments.get("job_id")) if arguments.get("job_id") else ("request_id", arguments.get("request_id"))
    if not isinstance(identifier[1], str) or not identifier[1]:
        raise ValueError("Waiting requires a nonempty job_id or request_id.")
    print("MCP_WAIT_" + identifier[0].upper() + "=" + identifier[1], flush=True)
    deadline = time.monotonic() + wait_timeout
    current = calls
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"Job wait expired; {identifier[0]}={identifier[1]}. The job was not cancelled.")
        try:
            results = await asyncio.wait_for(run_calls(current, project=project_file, url=url, anchor=anchor), timeout=remaining)
        except (ProjectIdentityError, FeatureUnavailableError, ToolAvailabilityError, ValueError):
            raise
        except Exception as error:
            print(f"Warning: Job status connection failed: {error_message(error)}; {identifier[0]}={identifier[1]}",
                  file=sys.stderr, flush=True)
        else:
            if not results or failed(results[-1]):
                return results
            context = job_context(results[-1])
            if context.get("record_kind") == "diagnostic":
                raise ValueError("Diagnostic records are not executable jobs; use job_control acknowledge_recovery after reviewing the record.")
            if context.get("job_id"):
                identifier = ("job_id", context["job_id"])
            elif context.get("request_id"):
                identifier = ("request_id", context["request_id"])
            state = context.get("state", context.get("status"))
            if state in {"succeeded", "failed", "cancelled", "interrupted"}:
                return results
            if state not in {"queued", "running"}:
                raise RuntimeError("Job response has an unknown state; cannot wait safely.")
        # Even when the initial start transport failed, all following requests are reads.
        current = [{"tool": provider.job_tools["status"], "arguments": {identifier[0]: identifier[1]}}]
        remaining = deadline - time.monotonic()
        if remaining > 0:
            await asyncio.sleep(min(poll_interval, remaining))


def main(argv=None, *, default_project=None, anchor):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool", nargs="?", help="Exact manifest tool name; plugin default: status, shared default: list")
    parser.add_argument("--project", type=Path, default=default_project,
                        help="Target .uproject or directory; defaults to the project containing this entry point")
    parser.add_argument("--url", help="Override the endpoint; project identity is still checked")
    arguments = parser.add_mutually_exclusive_group()
    arguments.add_argument("--arguments", help="JSON object; prefer --arguments-file")
    arguments.add_argument("--arguments-file", type=Path)
    arguments.add_argument("--batch-file", type=Path, help="Array of tool/arguments objects, executed sequentially")
    parser.add_argument("--list-tools", action="store_true")
    parser.add_argument("--wait", action="store_true", help="Wait for a single job_start/job_status to reach a terminal state")
    parser.add_argument("--wait-timeout", type=float, default=1800, help="Overall wait deadline in seconds (default: 1800)")
    parser.add_argument("--poll-interval", type=float, default=1, help="Status polling interval in seconds (default: 1)")
    args = parser.parse_args(argv)
    try:
        if args.tool and (args.batch_file or args.list_tools):
            raise ValueError("Use either a single tool, --batch-file, or --list-tools.")
        scope = local_provider(anchor)
        tool = args.tool
        if not tool and scope and not args.list_tools and not args.batch_file:
            _, providers = active_providers(resolve_project(args.project, anchor=anchor), required=scope)
            tool = next(provider.status_tool for provider in providers if provider.id == scope)
        if args.batch_file:
            calls = json.loads(args.batch_file.read_text("utf-8-sig"))
        elif tool:
            parameters = json.loads(args.arguments_file.read_text("utf-8-sig") if args.arguments_file else args.arguments or "{}")
            calls = [{"tool": tool, "arguments": parameters}]
        else:
            if args.arguments is not None or args.arguments_file:
                raise ValueError("A tool name is required with arguments.")
            calls = []
        list_tools = args.list_tools or (not tool and not args.batch_file)
        if args.wait and (args.batch_file or list_tools):
            raise ValueError("--wait is only supported for one job_start/job_status call.")
        if args.wait:
            results = asyncio.run(run_and_wait(calls, project=args.project, url=args.url, anchor=anchor,
                                              wait_timeout=args.wait_timeout, poll_interval=args.poll_interval))
        else:
            results = asyncio.run(run_calls(calls, list_tools, project=args.project, url=args.url, anchor=anchor))
        print(json.dumps(results, ensure_ascii=False, indent=2))
        if args.wait:
            return int(not results or any(failed(result) for result in results) or
                       job_context(results[-1]).get("state", job_context(results[-1]).get("status")) != "succeeded")
        return int(any(failed(result) for result in results))
    except Exception as error:
        print(f"Unreal Tools MCP failed: {error_message(error)}", file=sys.stderr)
        return 1
