#!/usr/bin/env python3
"""Compatibility check with the official MCP Python SDK (optional: needs `pip install mcp`).

Drives Pixel3D-mcp through the SDK's stdio client and ClientSession: initialize,
list tools/resources/prompts, reconstruct fixture A to PASS, read a resource,
get an inline render. Exit 0 = compatible; 77 = SDK not installed (skip).

usage: sdk_client_check.py SERVER_EXE FIXTURES_DIR
"""
import asyncio
import os
import sys

try:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError:
    print("mcp SDK not installed; skipping")
    sys.exit(77)


def attr(obj, *names):
    """SDK 1.x uses camelCase attributes, 2.x snake_case."""
    for n in names:
        if hasattr(obj, n):
            return getattr(obj, n)
    raise AttributeError(names)


async def main(exe, fx):
    params = StdioServerParameters(command=exe, args=[])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            init = await session.initialize()
            info = attr(init, "server_info", "serverInfo")
            print("server:", info.name, info.version, "protocol", attr(init, "protocol_version", "protocolVersion"))
            tools = await session.list_tools()
            names = {t.name for t in tools.tools}
            assert {"reconstruct", "validate", "export_glb", "render_view"} <= names, names
            resources = await session.list_resources()
            assert any(str(r.uri) == "pixel3d://validation" for r in resources.resources)
            prompts = await session.list_prompts()
            assert prompts.prompts[0].name == "reconstruct_from_views"

            def views(name):
                return {r: os.path.join(fx, name, r + ".png") for r in ("front", "side", "top")}

            for tool, args in [("new_project", {"profile": "strict"}), ("import_views", views("A_synthetic_chair")),
                               ("reconstruct", {}), ("project_textures", {})]:
                r = await session.call_tool(tool, args)
                assert not attr(r, "is_error", "isError"), (tool, r.content)
            r = await session.call_tool("validate", {})
            status = attr(r, "structured_content", "structuredContent")["status"]
            print("validate:", status)
            assert status == "PASS"
            r = await session.call_tool("render_view", {"views": ["front"], "pixels_per_unit": 4})
            assert any(c.type == "image" and attr(c, "mime_type", "mimeType") == "image/png" for c in r.content)
            res = await session.read_resource("pixel3d://validation")
            assert '"status":"PASS"' in res.contents[0].text
            print("official MCP SDK client: OK")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    asyncio.run(main(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])))
