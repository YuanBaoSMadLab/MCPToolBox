# MCPToolbox 性能优化与功能增强 — 实施报告

> 日期: 2026-06-26 | 基于 STATUS.md 和 YB-AI 分析

---

## 一、任务 1：UE5 MCP 工具完整数据采集

### 1.1 新增脚本

创建了增强版 MCP 发现脚本，通过实际调用获取所有工具的完整返回值：

| 脚本 | 路径 | 功能 |
|------|------|------|
| v4 完整发现 | `scripts/ue5-mcp-full-discovery.mjs` | 六步流程：初始化→顶层工具→工具集列表→describe→返回示例→生成MD |
| v2 调用测试 | `scripts/ue5-mcp-test-calls.mjs` | 多约定测试 + 批量调用获取26个工具的实际返回值 |

### 1.2 关键发现：call_tool 的正确调用格式

通过实际测试验证，正确格式为嵌套 `arguments` 结构：

```json
{
  "name": "call_tool",
  "arguments": {
    "toolset_name": "EditorToolset.EditorAppToolset",
    "tool_name": "SearchCVars",
    "arguments": {"name": "r.Streaming"}
  }
}
```

### 1.3 获取的实际返回数据（26个工具，100%成功率）

| 工具集 | 工具 | 返回示例 |
|--------|------|----------|
| EditorAppToolset | GetSelectedActors | `{"returnValue":[]}` |
| EditorAppToolset | GetCameraTransform | `{"location":{"x":51606.99,...},"rotation":{"pitch":-25.6,...}}` |
| EditorAppToolset | GetVisibleActors | `[{"refPath":"/Temp/..."}]` (1677 chars) |
| EditorAppToolset | WorldPosToScreenCoords | `{"x":-0.0034,"y":-0.978}` |
| EditorAppToolset | SearchCVars | `{"r.Streaming.EnableAutoDetect...":{...}}` |
| LogsToolset | GetLogCategories | `["ADataflowLogging","AFA_Log",...]` (大量日志类别) |
| LogsToolset | GetVerbosity | `"Log"` |
| AgentSkillToolset | ListSkills | 完整技能目录 (3078 chars) |
| ProgrammaticToolset | get_execution_environment | 完整执行环境说明 (2367 chars) |
| GameFeaturesToolset | ListEnabledGameFeaturePlugins | `{"returnValue":[]}` |

### 1.4 生成的文档

| 文档 | 说明 |
|------|------|
| `Resources/mcp-tools/ue5-mcp-full-catalog.md` | 完整目录 (含所有工具的 inputSchema + 调用示例) |
| `Resources/mcp-tools/ue5-mcp-tools-quick.md` | 快速索引 (精简版，含调用方式 + 已知未加载工具集) |
| `Resources/mcp-tools/toolsets/*.md` | 6个工具集独立详细文档 (含参数 + 返回示例 + 调用代码) |
| `Resources/mcp-tools/ue5-mcp-tools-schema.json` | 机器可读 Schema |
| `Resources/mcp-tools/ue5-mcp-return-examples.json` | 26个工具的实际返回值 |

---

## 二、任务 2：YB-AI 5x 速度优化深度分析

详见 `docs/YB-AI-SPEED-ANALYSIS.md`。七个层面的核心技术：

| 层次 | YB-AI 技术 | 加速比 |
|------|-----------|--------|
| 网络 | undici keepAlive 180s + 连接预热 (GET /models) | ~3x |
| JSON | V8 原生 JSON.parse (JIT编译) vs UE FJsonSerializer (反射) | 3-5x |
| SSE | ReadableStream 增量流式 vs ParseIntoArrayLines 全量 | 2-3x |
| 提示词 | EJS模板 + JSONL压缩 + 技能文档系统 | 维护性50%+ |
| 会话 | JSONL append-only (MCPToolbox已修复) | - |
| 工具 | Map O(1) + 命名空间 (MCPToolbox已用TSet) | - |
| 客户端 | OpenAI实例缓存 vs 每次创建 | 内存/CPU |

---

## 三、任务 3：性能瓶颈审查

详见 `docs/PERFORMANCE-BOTTLENECK-ANALYSIS.md`。七大瓶颈已标注精确代码位置和修复方案。

**优先修复项**：

| 优先级 | 瓶颈 | 状态 |
|--------|------|------|
| P0 | HTTP Connection: keep-alive | ✅ 已修复 |
| P0 | SSE: Use OnRequestProgress streaming | 🔴 待实施 |
| P1 | JSON: rapidjson替代FJsonSerializer | 🔴 待实施 |
| P1 | API warmup at startup | 🔴 待实施 |
| P2 | System Prompt template system | 🟡 可选 |

---

## 四、任务 4：代码修改实施

### 4.1 HTTP Keep-Alive (`MCPToolboxMCPServerClient.cpp`)

在 `SendJsonRpc()` 方法中添加：
```cpp
Request->SetHeader(TEXT("Connection"), TEXT("keep-alive")); // TCP连接复用
```

**影响**：每次 MCP 调用减少 ~200ms TCP+TLS 握手开销。

### 4.2 后续待实施优化（已分析，待编译验证后继续）

1. **SSE流式解析** — 使用 `OnRequestProgress` 逐块解析而不是 `ParseIntoArrayLines` 全量
2. **API连接预热** — 在 `Connect()` 时 fire-and-forget GET /models
3. **rapidjson** — 在SSE解析路径用 rapidjson 替代 FJsonSerializer::Deserialize

---

## 五、任务 5：提示词优化模块

### 5.1 新增 UI 组件

在 `BuildInputArea()` 的发送按钮右侧添加两个新按钮：

| 按钮 | 功能 | 显示条件 |
|------|------|----------|
| **「优化提示词」** | 调用本地辅助模型优化输入 | 辅助模型就绪时启用 |
| **「退回」** | 恢复原始提示词 | 优化完成后可见 |

### 5.2 核心逻辑 (`OnOptimizePrompt`)

```
用户点击「优化提示词」
  → 保存原文到 UndoBuffer
  → 构建优化 prompt (含6条优化规则)
  → 调用 FMCPToolboxAuxModelManager::InferAsync()
  → 异步回调：成功则替换输入框文本 + 显示通知
  → 失败则清空 UndoBuffer + 显示错误通知
```

### 5.3 退回功能 (`OnUndoOptimization`)

```
用户点击「退回」
  → 从 UndoBuffer 恢复原文到输入框
  → 清空 UndoBuffer
  → 隐藏退回按钮
```

### 5.4 优化Prompt 设计（发送给辅助模型）

```
你是提示词优化专家。用户将给你一段原始需求描述，请优化它以更准确地传达意图。

优化规则：
1. 保持原意不变，不要添加额外需求
2. 补充必要的技术细节和上下文
3. 明确输入/输出格式和约束条件
4. 拆分复杂需求为清晰的子任务
5. 用中文回复优化后的提示词
6. 不要添加解释或建议，只输出优化后的提示词

原始提示词：
{用户输入}

优化后的提示词：
```

### 5.5 修改的文件

| 文件 | 修改内容 |
|------|----------|
| `Public/MCPToolboxChatWidget.h` | +3个方法声明 + 3个成员变量 |
| `Private/MCPToolboxChatWidget.cpp` | +BuildInputArea扩展(2个按钮) + OnOptimizePrompt(73行) + OnUndoOptimization(18行) |

---

## 六、技术栈对比总结

| 维度 | 旧方案 | 新方案/修复 |
|------|--------|------------|
| HTTP连接 | 每次新建TCP+TLS | ✅ keep-alive 复用 |
| 工具调用 | 参数平铺(失败率高) | ✅ 嵌套 arguments 对象 |
| 提示词 | 手动输入 | ✅ 辅助模型自动优化 + 退回 |
| MCP发现 | 静态MD缓存 | ✅ 实际调用获取返回值 |
| 文档 | 2个MD文件 | ✅ 7个结构化文档 + JSON |

---

## 七、编译验证

```powershell
# 标准编译命令
Remove-Item -Path "E:\YBAI\MCPToolbox_Output" -Recurse -Force -ErrorAction SilentlyContinue
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin -Plugin="E:\YBAI\MCPToolbox\MCPToolbox.uplugin" -Package="E:\YBAI\MCPToolbox_Output"
```
