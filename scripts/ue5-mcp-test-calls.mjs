#!/usr/bin/env node
/**
 * UE5 MCP 工具调用测试 v2 — 修正参数格式
 */
import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const BASE = "http://127.0.0.1:8000/mcp";

async function init() {
  const r = await fetch(BASE, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      jsonrpc: "2.0", id: 1, method: "initialize",
      params: { protocolVersion: "2025-11-25", capabilities: {}, clientInfo: { name: "test", version: "4.0" } }
    })
  });
  return r.headers.get("mcp-session-id");
}

async function callRaw(sid, body) {
  const r = await fetch(BASE, {
    method: "POST",
    headers: { "Content-Type": "application/json", "Mcp-Session-Id": sid },
    body: JSON.stringify(body)
  });
  const ct = r.headers.get("content-type") || "";

  if (ct.includes("application/json")) {
    const d = await r.json();
    return d.error ? { ok: false, error: d.error.message } : { ok: true, result: JSON.stringify(d.result) };
  }

  if (ct.includes("text/event-stream") && r.body) {
    const reader = r.body.getReader();
    const dec = new TextDecoder();
    let buf = "", ed = "";
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      const norm = buf.replace(/\r\n/g, "\n");
      const lines = norm.split("\n");
      buf = lines.pop() || "";
      for (const line of lines) {
        const t = line.trimEnd();
        if (t.startsWith("data:")) ed = t.slice(5).trim();
        else if (t === "" && ed) {
          reader.releaseLock();
          try {
            const p = JSON.parse(ed);
            if (p.error) return { ok: false, error: p.error.message };
            const txt = (p.result?.content || []).filter(c => c.type === "text").map(c => c.text).join("\n");
            return { ok: true, result: txt || JSON.stringify(p.result) };
          } catch (e) { return { ok: true, result: ed }; }
        }
      }
    }
    reader.releaseLock();
    return { ok: false, error: "no SSE result" };
  }
  return { ok: false, error: `ct: ${ct}` };
}

async function callTool(sid, toolset, tool, toolArgs = {}) {
  // The correct convention: call_tool's arguments include the tool args in a nested "arguments" field
  // per UE5 MCP's call_tool schema
  const conventions = [
    // A: arguments nested as object (correct per schema)
    {
      label: "A-obj",
      params: {
        name: "call_tool",
        arguments: {
          toolset_name: toolset,
          tool_name: tool,
          arguments: Object.keys(toolArgs).length > 0 ? toolArgs : {}
        }
      }
    },
    // B: arguments nested as JSON string
    {
      label: "B-str",
      params: {
        name: "call_tool",
        arguments: {
          toolset_name: toolset,
          tool_name: tool,
          arguments: JSON.stringify(toolArgs)
        }
      }
    },
    // C: flat (works for no-arg tools)
    {
      label: "C-flat",
      params: {
        name: "call_tool",
        arguments: { toolset_name: toolset, tool_name: tool, ...toolArgs }
      }
    },
  ];

  for (const { label, params } of conventions) {
    const r = await callRaw(sid, {
      jsonrpc: "2.0",
      id: Math.floor(Math.random() * 90000) + 10000,
      method: "tools/call",
      params
    });
    if (r.ok) return { ok: true, result: r.result, convention: label };
  }

  // Return last error details
  const last = await callRaw(sid, {
    jsonrpc: "2.0",
    id: Math.floor(Math.random() * 90000) + 10000,
    method: "tools/call",
    params: conventions[0].params
  });
  return { ok: false, error: last.error || "unknown" };
}

async function main() {
  console.log("UE5 MCP 工具调用测试 v2\n");
  const sid = await init();
  console.log(`SessionId: ${sid}\n`);

  const allResults = {};

  // Test all tools across all toolsets
  const toolsToTest = [
    // === EditorAppToolset ===
    ["EditorToolset.EditorAppToolset", "GetSelectedActors", {}],
    ["EditorToolset.EditorAppToolset", "GetSelectedAssets", {}],
    ["EditorToolset.EditorAppToolset", "GetOpenAssets", {}],
    ["EditorToolset.EditorAppToolset", "GetContentBrowserPath", {}],
    ["EditorToolset.EditorAppToolset", "GetCameraTransform", {}],
    ["EditorToolset.EditorAppToolset", "IsPIERunning", {}],
    ["EditorToolset.EditorAppToolset", "GetVisibleActors", {}],
    ["EditorToolset.EditorAppToolset", "SearchCVars", { name: "r.Streaming" }],
    ["EditorToolset.EditorAppToolset", "SetContentBrowserPath", { path: "/Game" }],
    ["EditorToolset.EditorAppToolset", "WorldPosToScreenCoords", { position: { x: 0, y: 0, z: 0 } }],
    ["EditorToolset.EditorAppToolset", "ScreenCoordsToWorld", { x: 0.5, y: 0.5 }],
    ["EditorToolset.EditorAppToolset", "SelectActors", { actor_refs: [] }],
    ["EditorToolset.EditorAppToolset", "SelectAssets", { asset_paths: ["/Game/RedBlueTransition_MAT"] }],
    ["EditorToolset.EditorAppToolset", "FocusOnActors", { actor_refs: [] }],

    // === LogsToolset ===
    ["EditorToolset.LogsToolset", "GetLogCategories", { filter: "Log" }],
    ["EditorToolset.LogsToolset", "GetLogEntries", { category: "LogTemp", maxEntries: 3 }],
    ["EditorToolset.LogsToolset", "GetVerbosity", { category: "LogTemp" }],
    ["EditorToolset.LogsToolset", "SetVerbosity", { category: "LogTemp", verbosity: "Verbose" }],

    // === GameFeaturesToolset ===
    ["GameFeaturesToolset.GameFeaturesToolset", "ListEnabledGameFeaturePlugins", {}],
    ["GameFeaturesToolset.GameFeaturesToolset", "ListDiscoveredGameFeaturePlugins", {}],
    ["GameFeaturesToolset.GameFeaturesToolset", "IsGameFeaturePlugin", { pluginName: "SomePlugin" }],
    ["GameFeaturesToolset.GameFeaturesToolset", "GetGameFeatureState", { pluginName: "SomePlugin" }],

    // === AgentSkillToolset ===
    ["ToolsetRegistry.AgentSkillToolset", "ListSkills", {}],
    ["ToolsetRegistry.AgentSkillToolset", "GetSkills", { skillPaths: [] }],

    // === ProgrammaticToolset ===
    ["editor_toolset.toolsets.programmatic.ProgrammaticToolset", "get_execution_environment", {}],

    // === PhysicsAssetToolset ===
    ["PhysicsToolsets.PhysicsAssetToolset", "GetBodyNames", { physicsAsset: { path: "/Game/SomeAsset" } }],
  ];

  let okCount = 0, failCount = 0;

  for (const [toolset, tool, args] of toolsToTest) {
    const shortTs = toolset.split(".").pop();
    process.stdout.write(`[${shortTs}.${tool}] `);
    const r = await callTool(sid, toolset, tool, args);
    const key = `${toolset}::${tool}`;

    if (r.ok) {
      console.log(`OK (${r.convention})`);
      allResults[key] = { ok: true, convention: r.convention, result: r.result };
      okCount++;
    } else {
      const msg = (r.error || "").substring(0, 120);
      console.log(`FAIL - ${msg}`);
      allResults[key] = { ok: false, error: r.error };
      failCount++;
    }
  }

  console.log(`\n成功: ${okCount}, 失败: ${failCount}, 总计: ${okCount + failCount}\n`);

  // Write results
  const outDir = path.join(__dirname, "..", "Resources", "mcp-tools");
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(
    path.join(outDir, "ue5-mcp-return-examples.json"),
    JSON.stringify(allResults, null, 2),
    "utf8"
  );
  console.log(`已保存到: Resources/mcp-tools/ue5-mcp-return-examples.json`);
}

main().catch(e => { console.error(e.message); process.exit(1); });
