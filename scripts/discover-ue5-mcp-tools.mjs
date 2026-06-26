#!/usr/bin/env node
/**
 * UE5 MCP 全工具集探测 → 输出数个 MD 文件
 * 用法: node scripts/discover-ue5-mcp-tools.mjs [port] [outDir]
 * 默认: port=8000, outDir=./Resources/mcp-tools
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.argv[2]) || 8000;
const OUT_DIR = process.argv[3] || path.join(__dirname, "..", "Resources", "mcp-tools");
const BASE = `http://127.0.0.1:${PORT}/mcp`;
let sessionId = null;

async function req(method, params = {}, timeout = 15000) {
  const id = Math.floor(Math.random() * 90000) + 10000;
  const controller = new AbortController();
  setTimeout(() => controller.abort(), timeout);

  try {
    const res = await fetch(BASE, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(sessionId ? { "Mcp-Session-Id": sessionId } : {}),
      },
      body: JSON.stringify({ jsonrpc: "2.0", id, method, params }),
      signal: controller.signal,
    });
    const sid = res.headers.get("mcp-session-id");
    if (sid) sessionId = sid;
    const ct = res.headers.get("content-type") || "";

    if (ct.includes("application/json")) {
      const d = await res.json();
      return d.error ? `ERROR: ${d.error.message}` : d.result || d;
    }
    if (ct.includes("text/event-stream") && res.body) {
      const reader = res.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        const norm = buffer.replace(/\r\n/g, "\n");
        const lines = norm.split("\n");
        buffer = lines.pop() || "";
        let eventData = "";
        for (const line of lines) {
          const t = line.trimEnd();
          if (t.startsWith("data:")) eventData = t.slice(5).trim();
          else if (t === "" && eventData) {
            reader.releaseLock();
            return parseSSEEvent(eventData);
          }
        }
      }
      reader.releaseLock();
      return "SSE stream ended";
    }
    return `Unknown type: ${ct}`;
  } catch (e) {
    return `FAIL: ${e.message}`;
  }
}

function parseSSEEvent(data) {
  try {
    const p = JSON.parse(data);
    if (p.result?.content) {
      return p.result.content
        .filter(c => c.type === "text")
        .map(c => c.text)
        .join("\n");
    }
    return p.error ? `ERROR: ${p.error.message}` : JSON.stringify(p.result);
  } catch {
    return data;
  }
}

// ====== Parse tool output ======
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

  // Try JSON first (newer UE MCP versions return structured JSON)
  try {
    const parsed = JSON.parse(text);
    if (parsed.tools && Array.isArray(parsed.tools)) {
      for (const t of parsed.tools) {
        const name = (t.name || "").split(".").pop(); // short name
        tools.push({ name, desc: t.description || "" });
      }
      return tools;
    }
  } catch {}

  // Fallback: text format "- toolName: description"
  for (const line of text.split("\n")) {
    const t = line.trim();
    let m = t.match(/^-\s*(\S+)\s*[:：]\s*(.+)/);
    if (m) { tools.push({ name: m[1], desc: m[2].trim() }); continue; }
    m = t.match(/^-\s*(\S+)\s*$/);
    if (m) { tools.push({ name: m[1], desc: "" }); }
  }
  return tools;
}

// ====== Main ======
async function main() {
  console.log("=".repeat(60));
  console.log("  UE5 MCP 全工具集探测");
  console.log("=".repeat(60));

  // 1. Initialize
  console.log("\n[1/4] 初始化 MCP 会话...");
  await req("initialize", { protocolVersion: "2025-11-25", capabilities: {}, clientInfo: { name: "tool-discover", version: "1.0" } });
  console.log(`       Session: ${sessionId}`);

  // 2. Get top-level tools
  console.log("\n[2/4] 获取顶层工具...");
  const topTools = await req("tools/list");
  const topCount = topTools?.tools?.length || 0;
  console.log(`       顶层工具: ${topCount}`);

  // 3. List toolsets
  console.log("\n[3/4] 获取工具集列表...");
  const listText = await req("tools/call", { name: "list_toolsets", arguments: {} });
  if (typeof listText !== "string") {
    console.error("ERROR: 无法获取工具集列表。请确认 UE 编辑器和 MCP 服务已启动。");
    process.exit(1);
  }
  console.log(`       原始: ${listText.substring(0, 100)}...`);

  const tsNames = parseToolsetList(listText);
  console.log(`       解析到 ${tsNames.length} 个工具集: ${tsNames.join(", ")}`);

  // 4. Describe each toolset
  console.log("\n[4/4] 获取工具集详情...");
  const allToolsets = {};
  let totalTools = 0;

  for (const name of tsNames) {
    process.stdout.write(`       ${name}... `);
    const desc = await req("tools/call", { name: "describe_toolset", arguments: { toolset_name: name } });
    if (typeof desc !== "string") {
      console.log("FAIL");
      continue;
    }
    const tools = parseToolDescriptions(desc);
    allToolsets[name] = tools;
    totalTools += tools.length;
    console.log(`${tools.length} tools`);
  }

  console.log(`\n       总计: ${Object.keys(allToolsets).length} 工具集, ${totalTools} 工具\n`);

  // Ensure output dir
  fs.mkdirSync(OUT_DIR, { recursive: true });
  const timestamp = new Date().toISOString().replace("T", " ").substring(0, 19);

  // ----- 1. Full catalog -----
  const catalogLines = [];
  catalogLines.push(`# UE5 MCP 工具集目录`);
  catalogLines.push(``);
  catalogLines.push(`> 自动生成: ${timestamp} | 工具集: ${Object.keys(allToolsets).length} | 工具: ${totalTools}`);
  catalogLines.push(``);

  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    catalogLines.push(`### ${tsName}`);
    catalogLines.push(``);
    catalogLines.push(`| 工具名 | 说明 |`);
    catalogLines.push(`|--------|------|`);
    for (const t of tools) {
      catalogLines.push(`| \`${t.name}\` | ${t.desc} |`);
    }
    catalogLines.push(``);
  }
  const catalogPath = path.join(OUT_DIR, "ue5-mcp-tools-catalog.md");
  fs.writeFileSync(catalogPath, catalogLines.join("\n"), "utf8");
  console.log(`已生成: ${catalogPath}`);

  // ----- 2. Quick reference (compact, for system prompt) -----
  const quickLines = [];
  quickLines.push(`## 已发现MCP工具 (${totalTools}个)`);
  quickLines.push(``);
  quickLines.push(`直接 \`call_tool(toolset_name="...", tool_name="...", arguments=\{\})\` 调用。`);
  quickLines.push(``);

  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    const names = tools.map(t => t.name).join(", ");
    quickLines.push(`- **${tsName}**: ${names}`);
  }
  const quickPath = path.join(OUT_DIR, "ue5-mcp-tools-quick.md");
  fs.writeFileSync(quickPath, quickLines.join("\n"), "utf8");
  console.log(`已生成: ${quickPath}`);

  // ----- 3. Per-toolset detail -----
  const detailDir = path.join(OUT_DIR, "toolsets");
  fs.mkdirSync(detailDir, { recursive: true });
  for (const [tsName, tools] of Object.entries(allToolsets).sort()) {
    const shortName = tsName.split(".").pop();
    const lines = [];
    lines.push(`# ${tsName}`);
    lines.push(``);
    lines.push(`${tools.length} 个工具`);
    lines.push(``);
    lines.push(`| 工具名 | 说明 |`);
    lines.push(`|--------|------|`);
    for (const t of tools) {
      lines.push(`| \`${t.name}\` | ${t.desc} |`);
    }
    const p = path.join(detailDir, `${shortName}.md`);
    fs.writeFileSync(p, lines.join("\n"), "utf8");
  }
  console.log(`已生成: ${detailDir}/ (${Object.keys(allToolsets).length} 个文件)`);

  // ----- 4. JSON dump for programmatic use -----
  const jsonPath = path.join(OUT_DIR, "ue5-mcp-tools.json");
  fs.writeFileSync(jsonPath, JSON.stringify(allToolsets, null, 2), "utf8");
  console.log(`已生成: ${jsonPath}`);

  console.log(`\n完成！`);
  console.log(`  输出目录: ${OUT_DIR}`);
}

main().catch(e => { console.error(e); process.exit(1); });
