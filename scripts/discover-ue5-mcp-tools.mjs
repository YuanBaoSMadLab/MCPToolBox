#!/usr/bin/env node
/**
 * UE5 MCP 全工具集探测 -> 输出 MD 文件
 * 用法: node scripts/discover-ue5-mcp-tools.mjs [port] [outDir]
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.argv[2]) || 8000;
const OUT_DIR = process.argv[3] || path.join(__dirname, "..", "Resources", "mcp-tools");
const BASE = "http://127.0.0.1:" + PORT + "/mcp";
let sessionId = null;

async function req(method, params = {}, timeout = 15000) {
  const id = Math.floor(Math.random() * 90000) + 10000;
  const controller = new AbortController();
  setTimeout(function() { controller.abort(); }, timeout);

  try {
    const opts = {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ jsonrpc: "2.0", id, method, params }),
      signal: controller.signal,
    };
    if (sessionId) opts.headers["Mcp-Session-Id"] = sessionId;

    const res = await fetch(BASE, opts);
    const sid = res.headers.get("mcp-session-id");
    if (sid) sessionId = sid;
    const ct = res.headers.get("content-type") || "";

    if (ct.includes("application/json")) {
      const d = await res.json();
      return d.error ? "ERROR: " + d.error.message : d.result || d;
    }
    if (ct.includes("text/event-stream") && res.body) {
      const reader = res.body.getReader();
      const decoder = new TextDecoder();
      let buf = "";
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buf += decoder.decode(value, { stream: true });
        const norm = buf.replace(/\r\n/g, "\n");
        const lines = norm.split("\n");
        buf = lines.pop() || "";
        let ed = "";
        for (const line of lines) {
          const t = line.trimEnd();
          if (t.startsWith("data:")) ed = t.slice(5).trim();
          else if (t === "" && ed) {
            reader.releaseLock();
            return parseSSEEvent(ed);
          }
        }
      }
      reader.releaseLock();
      return "SSE stream ended";
    }
    return "Unknown: " + ct;
  } catch (e) {
    return "FAIL: " + e.message;
  }
}

function parseSSEEvent(data) {
  try {
    const p = JSON.parse(data);
    if (p.result && p.result.content) {
      return p.result.content.filter(function(c) { return c.type === "text"; }).map(function(c) { return c.text; }).join("\n");
    }
    return p.error ? "ERROR: " + p.error.message : JSON.stringify(p.result);
  } catch (e) {
    return data;
  }
}

function parseToolsetList(text) {
  const names = [];
  for (const line of text.split("\n")) {
    const m = line.trim().match(/^-\s*([\w.]+Toolset)[:：]/);
    if (m) names.push(m[1]);
  }
  return names;
}

function parseToolDescriptions(text) {
  const tools = [];
  try {
    const parsed = JSON.parse(text);
    if (parsed.tools && Array.isArray(parsed.tools)) {
      for (const t of parsed.tools) {
        const name = (t.name || "").split(".").pop();
        tools.push({ name: name, desc: t.description || "" });
      }
      return tools;
    }
  } catch (e) {}
  for (const line of text.split("\n")) {
    const m = line.trim().match(/^-\s*(\S+)\s*[:：]\s*(.+)/);
    if (m) { tools.push({ name: m[1], desc: m[2].trim() }); break; }
  }
  return tools;
}

// ====== Known toolsets (not currently loaded) ======
const KNOWN_TOOLSETS = [
  { plugin: "MVCBlueprintToolset", desc: "蓝图工具 - 创建/编辑蓝图、节点图、变量、组件、GAS" },
  { plugin: "ScriptBlueprintToolset", desc: "脚本蓝图工具" },
  { plugin: "EditorScriptingToolset", desc: "编辑器脚本" },
  { plugin: "StaticMeshToolset", desc: "静态网格工具" },
  { plugin: "GeometryScriptingToolset", desc: "几何体脚本" },
  { plugin: "ModelingModeToolset", desc: "建模模式" },
  { plugin: "PCGToolset", desc: "PCG 程序化生成" },
  { plugin: "AnimationAssistantToolset", desc: "动画工具" },
  { plugin: "ConversationToolset", desc: "对话系统" },
  { plugin: "GameplayCueToolset", desc: "GameplayCue" },
  { plugin: "LayerToolset", desc: "图层管理" },
  { plugin: "SmartObjectToolset", desc: "SmartObject" },
  { plugin: "WorldConditionToolset", desc: "世界条件" },
  { plugin: "RigVMBlueprintToolset", desc: "RigVM 蓝图" },
  { plugin: "SlateUICalloutToolset", desc: "UI 工具" },
  { plugin: "UIFrontendToolset", desc: "UI 前端" },
  { plugin: "MVCComponentToolset", desc: "组件工具" },
  { plugin: "AIModuleToolset", desc: "AI 模块" },
];

const NEED_PLUGIN_MAP = [
  ["创建/编辑蓝图", "MVCBlueprintToolset"],
  ["创建关卡", "EditorAppToolset (built-in)"],
  ["创建/编辑静态网格", "StaticMeshToolset"],
  ["编辑器脚本/Python", "EditorScriptingToolset"],
  ["几何体操作", "GeometryScriptingToolset"],
  ["建模", "ModelingModeToolset"],
  ["PCG 程序化生成", "PCGToolset"],
  ["动画", "AnimationAssistantToolset"],
  ["AI 技能管理", "AgentSkillToolset (built-in)"],
  ["材质创建/编辑", "editor_toolset.toolsets.material (MaterialTools plugin)"],
];

// ====== Main ======
async function main() {
  console.log("=".repeat(60));
  console.log("  UE5 MCP Tool Discovery");
  console.log("=".repeat(60));

  console.log("\n[1/4] Init MCP...");
  await req("initialize", { protocolVersion: "2025-11-25", capabilities: {}, clientInfo: { name: "tool-discover", version: "1.0" } });
  console.log("       Session: " + sessionId);

  console.log("\n[2/4] Top-level tools...");
  const topTools = await req("tools/list");
  console.log("       Top-level: " + (topTools && topTools.tools ? topTools.tools.length : 0));

  console.log("\n[3/4] List toolsets...");
  const listText = await req("tools/call", { name: "list_toolsets", arguments: {} });
  if (typeof listText !== "string") {
    console.error("ERROR: Cannot get toolset list. Is UE running with MCP?");
    process.exit(1);
  }

  const tsNames = parseToolsetList(listText);
  console.log("       Found: " + tsNames.length + " toolsets");

  console.log("\n[4/4] Describe each toolset...");
  const allToolsets = {};
  let totalTools = 0;

  for (const name of tsNames) {
    process.stdout.write("       " + name + "... ");
    const desc = await req("tools/call", { name: "describe_toolset", arguments: { toolset_name: name } });
    if (typeof desc !== "string") { console.log("FAIL"); continue; }
    const tools = parseToolDescriptions(desc);
    allToolsets[name] = tools;
    totalTools += tools.length;
    console.log(tools.length + " tools");
  }

  console.log("\n       Total: " + Object.keys(allToolsets).length + " toolsets, " + totalTools + " tools\n");

  fs.mkdirSync(OUT_DIR, { recursive: true });
  const timestamp = new Date().toISOString().replace("T", " ").substring(0, 19);

  // ===== 1. Full catalog =====
  const cat = [];
  cat.push("# UE5 MCP Toolset Catalog");
  cat.push("");
  cat.push("> Generated: " + timestamp + " | Loaded: " + Object.keys(allToolsets).length + " sets | Tools: " + totalTools);
  cat.push("");
  cat.push("## Loaded Toolsets");
  cat.push("");

  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    cat.push("### " + tsName);
    cat.push("");
    cat.push("| Tool | Description |");
    cat.push("|------|-------------|");
    for (const t of tools) {
      cat.push("| " + t.name + " | " + t.desc.split("\n")[0] + " |");
    }
    cat.push("");
  }

  cat.push("---");
  cat.push("");
  cat.push("## Known Toolsets (Not Loaded - Enable Plugin)");
  cat.push("");
  cat.push("These toolsets are documented but not loaded in the current UE instance.");
  cat.push("Go to Edit > Plugins, search for the plugin, enable it, restart UE.");
  cat.push("");
  cat.push("| Plugin | Function |");
  cat.push("|--------|----------|");
  for (const k of KNOWN_TOOLSETS) {
    cat.push("| " + k.plugin + " | " + k.desc + " |");
  }
  cat.push("");
  cat.push("## Requirement -> Plugin Map");
  cat.push("");
  cat.push("| User Need | Required Plugin |");
  cat.push("|-----------|-----------------|");
  for (const [need, plugin] of NEED_PLUGIN_MAP) {
    cat.push("| " + need + " | " + plugin + " |");
  }

  const catalogPath = path.join(OUT_DIR, "ue5-mcp-tools-catalog.md");
  fs.writeFileSync(catalogPath, cat.join("\n"), "utf8");
  console.log("OK: " + catalogPath);

  // ===== 2. Quick reference =====
  const qk = [];
  qk.push("## Loaded MCP Tools (" + totalTools + " tools)");
  qk.push("");
  qk.push("Call: call_tool(toolset_name=\"...\", tool_name=\"...\", arguments={})");
  qk.push("");

  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    const names = tools.map(function(t) { return t.name; }).join(", ");
    qk.push("- **" + tsName + "**: " + names);
  }

  qk.push("");
  qk.push("## Known Toolsets (not loaded, enable plugin)");
  qk.push("");
  qk.push("| Plugin | Function |");
  qk.push("|--------|----------|");
  for (const k of KNOWN_TOOLSETS) {
    qk.push("| " + k.plugin + " | " + k.desc + " |");
  }
  qk.push("");
  qk.push("Enable: Edit > Plugins > search Toolsets > enable plugin > restart UE");

  const quickPath = path.join(OUT_DIR, "ue5-mcp-tools-quick.md");
  fs.writeFileSync(quickPath, qk.join("\n"), "utf8");
  console.log("OK: " + quickPath);

  // ===== 3. Per-toolset detail =====
  const detailDir = path.join(OUT_DIR, "toolsets");
  fs.mkdirSync(detailDir, { recursive: true });
  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    const shortName = tsName.split(".").pop();
    const d = [];
    d.push("# " + tsName);
    d.push("");
    d.push(tools.length + " tools");
    d.push("");
    d.push("| Tool | Description |");
    d.push("|------|-------------|");
    for (const t of tools) {
      d.push("| " + t.name + " | " + t.desc.split("\n")[0] + " |");
    }
    fs.writeFileSync(path.join(detailDir, shortName + ".md"), d.join("\n"), "utf8");
  }
  console.log("OK: " + detailDir + " (" + Object.keys(allToolsets).length + " files)");

  // ===== 4. JSON dump =====
  fs.writeFileSync(path.join(OUT_DIR, "ue5-mcp-tools.json"), JSON.stringify(allToolsets, null, 2), "utf8");
  console.log("OK: " + path.join(OUT_DIR, "ue5-mcp-tools.json"));

  console.log("\nDone! Output: " + OUT_DIR);
}

main().catch(function(e) { console.error(e); process.exit(1); });
