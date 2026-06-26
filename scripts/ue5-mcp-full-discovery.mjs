#!/usr/bin/env node
/**
 * UE5 MCP 完整工具发现脚本 v4 — 获取所有工具的完整输入/输出 Schema 及示例返回数据
 * 
 * 在三层架构上工作：
 *   1. 标准 MCP tools/list — 获取顶层工具
 *   2. list_toolsets → 发现全部工具集
 *   3. describe_toolset → 获取每个工具集的工具定义（含完整 inputSchema）
 *   4. call_tool (dry-run) → 在可能的情况下获取示例返回数据
 *
 * 输出结构：
 *   Resources/mcp-tools/
 *   ├── ue5-mcp-full-catalog.md       # 完整目录（含 inputSchema + outputSchema）
 *   ├── ue5-mcp-tools-schema.json     # 机器可读 Schema（含示例返回值）
 *   ├── toolsets/*.md                 # 每个工具集详细文档（含调用示例）
 *   ├── ue5-mcp-tools-quick.md        # 快速索引（优化后的精简版）
 *   └── ue5-mcp-return-examples.json  # 所有可获取的返回示例
 *
 * 用法: node scripts/ue5-mcp-full-discovery.mjs [port] [outDir]
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.argv[2]) || 8000;
const OUT_DIR = process.argv[3] || path.join(__dirname, "..", "Resources", "mcp-tools");
const BASE = "http://127.0.0.1:" + PORT + "/mcp";

let sessionId = null;

// ============================================================================
// MCP JSON-RPC Client (with keep-alive for performance)
// ============================================================================
async function req(method, params = {}, timeout = 30000) {
  const id = Math.floor(Math.random() * 90000) + 10000;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);

  try {
    const headers = { "Content-Type": "application/json" };
    if (sessionId) headers["Mcp-Session-Id"] = sessionId;

    const res = await fetch(BASE, {
      method: "POST",
      headers,
      body: JSON.stringify({ jsonrpc: "2.0", id, method, params }),
      signal: controller.signal,
    });

    const sid = res.headers.get("mcp-session-id");
    if (sid) sessionId = sid;

    const ct = res.headers.get("content-type") || "";

    if (ct.includes("application/json")) {
      const d = await res.json();
      return { ok: !d.error, result: d.result, error: d.error?.message };
    }

    if (ct.includes("text/event-stream") && res.body) {
      return await readSSE(res.body);
    }

    return { ok: false, error: "Unknown content type: " + ct };
  } catch (e) {
    return { ok: false, error: e.message };
  } finally {
    clearTimeout(timer);
  }
}

async function readSSE(body) {
  const reader = body.getReader();
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
        try {
          const p = JSON.parse(ed);
          if (p.error) return { ok: false, error: p.error.message };
          const content = (p.result?.content || [])
            .filter(c => c.type === "text")
            .map(c => c.text)
            .join("\n");
          return { ok: true, result: p.result, text: content, raw: p };
        } catch (e) {
          return { ok: true, text: ed };
        }
      }
    }
  }

  reader.releaseLock();
  return { ok: false, error: "SSE stream ended without result" };
}

/**
 * 调用 MCP 工具并捕获完整返回值
 */
async function callTool(toolName, args = {}, timeout = 30000) {
  const res = await req("tools/call", { name: toolName, arguments: args }, timeout);
  if (res.ok && res.text) {
    return { ok: true, result: res.text, raw: res.raw };
  }
  return res;
}

// ============================================================================
// Parsers
// ============================================================================

function parseToolsetList(text) {
  const names = [];
  for (const line of text.split("\n")) {
    const m = line.trim().match(/^-\s*([\w.]+Toolset)[:：]/);
    if (m) names.push(m[1]);
  }
  return names;
}

function parseDescribeResult(text) {
  try {
    const parsed = JSON.parse(text);
    if (parsed && parsed.tools && Array.isArray(parsed.tools)) {
      return parsed.tools.map(t => ({
        name: (t.name || "").split(".").pop(),
        fullName: t.name || "",
        description: t.description || "",
        inputSchema: t.inputSchema || { type: "object", properties: {} },
      }));
    }
    if (parsed && typeof parsed === "object") {
      return [parsed];
    }
  } catch (e) {
    // Not JSON, try line-based parsing
  }

  const tools = [];
  for (const line of text.split("\n")) {
    const m = line.trim().match(/^-\s*(\S+)\s*[:：]\s*(.+)/);
    if (m) tools.push({ name: m[1], description: m[2].trim() });
  }
  return tools;
}

// ============================================================================
// Getter tools that can be safely called for return examples
// ============================================================================
const SAFE_GETTER_TOOLS = [
  // EditorAppToolset — safe read-only getters
  { ts: "EditorToolset.EditorAppToolset", tool: "GetSelectedActors", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "GetSelectedAssets", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "GetOpenAssets", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "GetContentBrowserPath", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "GetCameraTransform", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "GetVisibleActors", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "IsPIERunning", args: {} },
  { ts: "EditorToolset.EditorAppToolset", tool: "SearchCVars", args: { name: "r." } },

  // LogsToolset — read-only
  { ts: "EditorToolset.LogsToolset", tool: "GetLogCategories", args: {} },
  { ts: "EditorToolset.LogsToolset", tool: "GetVerbosity", args: { category: "LogTemp" } },

  // GameFeaturesToolset — read-only getters
  { ts: "GameFeaturesToolset.GameFeaturesToolset", tool: "ListEnabledGameFeaturePlugins", args: {} },
  { ts: "GameFeaturesToolset.GameFeaturesToolset", tool: "ListDiscoveredGameFeaturePlugins", args: {} },

  // AgentSkillToolset — read-only
  { ts: "ToolsetRegistry.AgentSkillToolset", tool: "ListSkills", args: {} },
  { ts: "ToolsetRegistry.AgentSkillToolset", tool: "GetSkills", args: { skill_names: [] } },

  // ProgrammaticToolset — environment info
  { ts: "editor_toolset.toolsets.programmatic.ProgrammaticToolset", tool: "get_execution_environment", args: {} },
];

// Known toolsets not loaded (for reference)
const KNOWN_TOOLSETS = [
  { plugin: "MVCBlueprintToolset", func: "蓝图工具 - 创建/编辑蓝图、节点图、变量、组件、GAS" },
  { plugin: "ScriptBlueprintToolset", func: "脚本蓝图工具" },
  { plugin: "EditorScriptingToolset", func: "编辑器脚本" },
  { plugin: "StaticMeshToolset", func: "静态网格工具" },
  { plugin: "GeometryScriptingToolset", func: "几何体脚本" },
  { plugin: "ModelingModeToolset", func: "建模模式" },
  { plugin: "PCGToolset", func: "PCG 程序化生成" },
  { plugin: "AnimationAssistantToolset", func: "动画工具" },
  { plugin: "ConversationToolset", func: "对话系统" },
  { plugin: "GameplayCueToolset", func: "GameplayCue" },
  { plugin: "LayerToolset", func: "图层管理" },
  { plugin: "SmartObjectToolset", func: "SmartObject" },
  { plugin: "WorldConditionToolset", func: "世界条件" },
  { plugin: "RigVMBlueprintToolset", func: "RigVM 蓝图" },
  { plugin: "SlateUICalloutToolset", func: "UI 工具" },
  { plugin: "UIFrontendToolset", func: "UI 前端" },
  { plugin: "MVCComponentToolset", func: "组件工具" },
  { plugin: "AIModuleToolset", func: "AI 模块" },
];

const NEED_PLUGIN_MAP = [
  ["创建/编辑蓝图", "MVCBlueprintToolset"],
  ["创建脚本蓝图", "ScriptBlueprintToolset"],
  ["创建关卡", "EditorAppToolset (built-in)"],
  ["创建/编辑静态网格", "StaticMeshToolset"],
  ["编辑器脚本/Python", "EditorScriptingToolset"],
  ["几何体操作", "GeometryScriptingToolset"],
  ["建模", "ModelingModeToolset"],
  ["PCG 程序化生成", "PCGToolset"],
  ["动画", "AnimationAssistantToolset"],
  ["AI 技能管理", "AgentSkillToolset (built-in)"],
  ["材质创建/编辑", "editor_toolset.toolsets.material (MaterialTools plugin)"],
  ["对话系统", "ConversationToolset"],
  ["GameplayCue", "GameplayCueToolset"],
  ["图层管理", "LayerToolset"],
  ["SmartObject", "SmartObjectToolset"],
  ["世界条件", "WorldConditionToolset"],
  ["RigVM 蓝图", "RigVMBlueprintToolset"],
  ["Slate UI 标注", "SlateUICalloutToolset"],
  ["UI 前端", "UIFrontendToolset"],
  ["组件工具", "MVCComponentToolset"],
  ["AI 模块", "AIModuleToolset"],
];

// ============================================================================
// Generate Markdown
// ============================================================================

function formatSchema(schema, indent = "") {
  if (!schema || !schema.properties) return indent + "(无参数)";
  const lines = [];
  const required = new Set(schema.required || []);
  for (const [name, prop] of Object.entries(schema.properties)) {
    const req = required.has(name) ? " **必需**" : "";
    const type = prop.type || "string";
    const desc = prop.description || "";
    const enum_ = prop.enum ? ` 可选值: ${prop.enum.map(e => `\`${e}\``).join(", ")}` : "";
    lines.push(`${indent}- \`${name}\` (${type})${req}: ${desc}${enum_}`);
  }
  return lines.join("\n") || indent + "(无参数)";
}

function formatReturnExample(toolName, result) {
  if (!result || !result.ok) return "";
  const text = result.text || JSON.stringify(result.result, null, 2);
  if (!text || text.trim().length === 0) return "";

  // Trim very long outputs
  const maxLen = 3000;
  const display = text.length > maxLen
    ? text.substring(0, maxLen) + `\n\n... _(共 ${text.length} 字符，已截断)_`
    : text;
  return `\n**返回示例:**
\`\`\`json
${display}
\`\`\``;
}

function generateFullCatalog(allToolsets, allReturnExamples, topTools) {
  const parts = [];
  const timestamp = new Date().toISOString().replace("T", " ").substring(0, 19);
  let totalTools = 0;

  parts.push("# UE5 MCP 完整工具目录 v4");
  parts.push("");
  parts.push(`> 生成时间: ${timestamp}`);
  parts.push(`> 连接地址: http://127.0.0.1:${PORT}/mcp`);
  parts.push(`> SessionId: ${sessionId || "N/A"}`);
  parts.push("");
  parts.push("---");
  parts.push("");

  // Top-level tools
  parts.push("## 顶层 MCP 工具 (标准 MCP 协议)");
  parts.push("");
  parts.push("这些是 MCP 协议标准工具，不属于任何工具集。");
  parts.push("");
  parts.push("| 工具名 | 描述 | 参数 |");
  parts.push("|--------|------|------|");
  for (const t of topTools) {
    const params = Object.keys(t.inputSchema?.properties || {}).join(", ") || "-";
    parts.push(`| \`${t.name}\` | ${(t.description || "").split("\n")[0]} | ${params} |`);
  }
  parts.push("");

  // Loaded toolsets
  parts.push("---");
  parts.push("");
  parts.push(`## 已加载工具集 (${Object.keys(allToolsets).length} 个)`);
  parts.push("");

  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    totalTools += tools.length;
    parts.push(`### ${tsName} (${tools.length} 个工具)`);
    parts.push("");

    for (const tool of tools) {
      parts.push(`#### \`${tool.name}\``);
      parts.push("");
      parts.push(`**描述:** ${tool.description || "(无描述)"}`);
      parts.push("");
      parts.push("**输入参数:**");
      parts.push(formatSchema(tool.inputSchema, ""));
      parts.push("");

      // Return example if available
      const exampleKey = `${tsName}::${tool.name}`;
      const example = allReturnExamples[exampleKey];
      if (example) {
        parts.push(formatReturnExample(tool.name, example));
        parts.push("");
      }

      // Usage hint
      parts.push("**调用示例:**");
      parts.push('```json');
      parts.push(`call_tool({`);
      parts.push(`  toolset_name: "${tsName}",`);
      parts.push(`  tool_name: "${tool.name}",`);
      parts.push(`  arguments: {}`);
      parts.push(`})`);
      parts.push('```');
      parts.push("");
      parts.push("---");
      parts.push("");
    }
  }

  // Known toolsets
  parts.push("## 已知但未加载的工具集");
  parts.push("");
  parts.push("以下工具集已有文档但未在当前 UE 实例中加载。请在 Edit > Plugins 中启用对应插件后重启 UE。");
  parts.push("");
  parts.push("| 插件名 | 功能 |");
  parts.push("|--------|------|");
  for (const k of KNOWN_TOOLSETS) {
    parts.push(`| \`${k.plugin}\` | ${k.func} |`);
  }
  parts.push("");

  // Requirement map
  parts.push("## 需求 → 插件映射");
  parts.push("");
  parts.push("| 用户需求 | 所需插件 |");
  parts.push("|----------|----------|");
  for (const [need, plugin] of NEED_PLUGIN_MAP) {
    parts.push(`| ${need} | \`${plugin}\` |`);
  }
  parts.push("");

  // Summary
  parts.push("## 统计");
  parts.push("");
  parts.push(`- **顶层工具:** ${topTools.length}`);
  parts.push(`- **工具集:** ${Object.keys(allToolsets).length}`);
  parts.push(`- **工具总数:** ${totalTools}`);
  parts.push(`- **已获取返回示例:** ${Object.keys(allReturnExamples).length} 个`);
  parts.push(`- **已知未加载工具集:** ${KNOWN_TOOLSETS.length} 个`);
  parts.push("");

  return { content: parts.join("\n"), totalTools };
}

function generateQuickIndex(allToolsets, topTools) {
  const parts = [];
  parts.push(`## 已加载 MCP 工具 — 快速索引`);
  parts.push("");
  parts.push(`> 生成时间: ${new Date().toISOString().replace("T", " ").substring(0, 19)}`);
  parts.push("");

  // Top-level tools
  parts.push("### 顶层工具");
  parts.push("");
  for (const t of topTools) {
    parts.push(`- \`${t.name}\` — ${(t.description || "").split("\n")[0]}`);
  }
  parts.push("");

  // Toolsets
  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    const names = tools.map(t => t.name).join(", ");
    parts.push(`- **${tsName}**: ${names}`);
  }
  parts.push("");

  parts.push("### 调用方式");
  parts.push("");
  parts.push('```');
  parts.push('call_tool(toolset_name="ToolsetName", tool_name="ToolName", arguments={})');
  parts.push('```');
  parts.push("");

  parts.push("### 已知未加载工具集 (启用插件后可用)");
  parts.push("");
  parts.push("| 插件 | 功能 |");
  parts.push("|------|------|");
  for (const k of KNOWN_TOOLSETS) {
    parts.push(`| ${k.plugin} | ${k.func} |`);
  }
  parts.push("");

  return parts.join("\n");
}

function generateToolsetDetailDoc(tsName, tools, returnExamples) {
  const parts = [];
  parts.push(`# ${tsName}`);
  parts.push("");
  parts.push(`> ${tools.length} 个工具 | 更新时间: ${new Date().toISOString().replace("T", " ").substring(0, 19)}`);
  parts.push("");

  for (const tool of tools) {
    parts.push(`## \`${tool.name}\``);
    parts.push("");
    parts.push(`**完整名称:** \`${tool.fullName || tool.name}\``);
    parts.push("");
    parts.push(`**描述:** ${tool.description || "(无描述)"}`);
    parts.push("");
    parts.push("### 输入参数");
    parts.push("");
    parts.push(formatSchema(tool.inputSchema, ""));
    parts.push("");

    const exampleKey = `${tsName}::${tool.name}`;
    const example = returnExamples[exampleKey];
    if (example && example.ok) {
      parts.push("### 返回示例");
      parts.push("");
      const text = example.text || JSON.stringify(example.result, null, 2);
      const maxLen = 5000;
      const display = text.length > maxLen
        ? text.substring(0, maxLen) + `\n\n... _(共 ${text.length} 字符，已截断)_`
        : text;
      parts.push("```json");
      parts.push(display);
      parts.push("```");
      parts.push("");
    }

    parts.push("### 调用代码");
    parts.push("");
    parts.push("```json");
    parts.push(JSON.stringify({
      toolset_name: tsName,
      tool_name: tool.name,
      arguments: {}
    }, null, 2));
    parts.push("```");
    parts.push("");
    parts.push("---");
    parts.push("");
  }

  return parts.join("\n");
}

// ============================================================================
// Main
// ============================================================================
async function main() {
  console.log("=".repeat(70));
  console.log("  UE5 MCP 完整工具发现 v4 — Full Schema + Return Examples");
  console.log("=".repeat(70));
  console.log(`  Target: ${BASE}`);
  console.log("");

  // ── Step 1: Initialize ──
  console.log("[1/6] 初始化 MCP 会话...");
  await req("initialize", {
    protocolVersion: "2025-11-25",
    capabilities: {},
    clientInfo: { name: "ue5-full-discovery-v4", version: "4.0.0" },
  });
  console.log(`      SessionId: ${sessionId || "N/A"}`);
  console.log("");

  // ── Step 2: Top-level tools ──
  console.log("[2/6] 获取顶层 MCP 工具 (tools/list)...");
  const topRes = await req("tools/list");
  const topTools = topRes.ok && topRes.result?.tools ? topRes.result.tools : [];
  console.log(`      顶层工具: ${topTools.length} 个`);
  for (const t of topTools) {
    console.log(`        - ${t.name}: ${(t.description || "").substring(0, 80)}`);
  }
  console.log("");

  // ── Step 3: List toolsets ──
  console.log("[3/6] 发现工具集 (list_toolsets)...");
  const listRes = await callTool("list_toolsets");
  if (!listRes.ok || !listRes.result) {
    console.error("  ERROR: 无法获取工具集列表。UE5 MCP 是否正在运行？");
    console.error(`  ${listRes.error || listRes.result}`);
    process.exit(1);
  }

  const tsNames = parseToolsetList(listRes.result);
  console.log(`      发现 ${tsNames.length} 个工具集:`);
  for (const name of tsNames) console.log(`        - ${name}`);
  console.log("");

  // ── Step 4: Describe each toolset ──
  console.log("[4/6] 获取每个工具集的详细定义 (describe_toolset)...");
  const allToolsets = {};
  let totalTools = 0;

  for (const name of tsNames) {
    process.stdout.write(`      ${name}... `);
    const descRes = await callTool("describe_toolset", { toolset_name: name });
    if (!descRes.ok || !descRes.result) {
      console.log("FAIL");
      continue;
    }
    const tools = parseDescribeResult(descRes.result);
    if (tools.length === 0) {
      // Try JSON parse
      try {
        const j = JSON.parse(descRes.result);
        if (j.tools) {
          tools.push(...j.tools.map(t => ({
            name: (t.name || "").split(".").pop(),
            fullName: t.name || "",
            description: t.description || "",
            inputSchema: t.inputSchema || { type: "object", properties: {} },
          })));
        }
      } catch (e) {}
    }
    allToolsets[name] = tools;
    totalTools += tools.length;
    console.log(`${tools.length} tools`);
  }
  console.log(`      总计: ${totalTools} 个工具\n`);

  // ── Step 5: Get return examples ──
  console.log("[5/6] 获取安全可调用的 Getter 工具返回值...");
  const allReturnExamples = {};

  for (const { ts, tool, args } of SAFE_GETTER_TOOLS) {
    // Only try if toolset is loaded
    if (!allToolsets[ts]) continue;
    const toolExists = allToolsets[ts].some(t => t.name === tool);
    if (!toolExists) continue;

    process.stdout.write(`      ${ts}.${tool}... `);
    const res = await callTool(tool, args, 10000);
    if (res.ok) {
      const key = `${ts}::${tool}`;
      allReturnExamples[key] = res;
      console.log(`OK (${(res.text || "").length} chars)`);
    } else {
      console.log(`SKIP (${res.error || "no result"})`);
    }
  }
  console.log(`      获取到 ${Object.keys(allReturnExamples).length} 个返回示例\n`);

  // ── Step 6: Generate documentation ──
  console.log("[6/6] 生成文档...");

  fs.mkdirSync(OUT_DIR, { recursive: true });

  // 6a. Full catalog
  const { content: fullCatalog, totalTools: tt } = generateFullCatalog(allToolsets, allReturnExamples, topTools);
  const catalogPath = path.join(OUT_DIR, "ue5-mcp-full-catalog.md");
  fs.writeFileSync(catalogPath, fullCatalog, "utf8");
  console.log(`      OK: ue5-mcp-full-catalog.md`);

  // 6b. Quick index
  const quickIndex = generateQuickIndex(allToolsets, topTools);
  const quickPath = path.join(OUT_DIR, "ue5-mcp-tools-quick.md");
  fs.writeFileSync(quickPath, quickIndex, "utf8");
  console.log(`      OK: ue5-mcp-tools-quick.md`);

  // 6c. Per-toolset detail docs
  const detailDir = path.join(OUT_DIR, "toolsets");
  fs.mkdirSync(detailDir, { recursive: true });
  let detailCount = 0;
  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    const shortName = tsName.split(".").pop();
    const doc = generateToolsetDetailDoc(tsName, tools, allReturnExamples);
    fs.writeFileSync(path.join(detailDir, shortName + ".md"), doc, "utf8");
    detailCount++;
  }
  console.log(`      OK: toolsets/ (${detailCount} files)`);

  // 6d. Schema JSON
  const schemaJson = {};
  for (const [tsName, tools] of Object.entries(allToolsets)) {
    schemaJson[tsName] = tools.map(t => ({
      name: t.name,
      fullName: t.fullName,
      description: t.description,
      inputSchema: t.inputSchema,
    }));
  }
  fs.writeFileSync(
    path.join(OUT_DIR, "ue5-mcp-tools-schema.json"),
    JSON.stringify({ toolsets: schemaJson, topTools }, null, 2),
    "utf8"
  );
  console.log(`      OK: ue5-mcp-tools-schema.json`);

  // 6e. Return examples
  fs.writeFileSync(
    path.join(OUT_DIR, "ue5-mcp-return-examples.json"),
    JSON.stringify(allReturnExamples, null, 2),
    "utf8"
  );
  console.log(`      OK: ue5-mcp-return-examples.json`);

  console.log("");
  console.log("=".repeat(70));
  console.log(`  完成! 输出目录: ${OUT_DIR}`);
  console.log(`  工具集: ${Object.keys(allToolsets).length} | 工具: ${tt} | 返回示例: ${Object.keys(allReturnExamples).length}`);
  console.log("=".repeat(70));
}

main().catch(e => {
  console.error(`\nFATAL: ${e.message}`);
  process.exit(1);
});
