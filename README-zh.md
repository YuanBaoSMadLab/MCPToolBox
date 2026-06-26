# MCPToolBox

一个通过模型上下文协议 (MCP) 将大型语言模型 (LLM) 集成到 Unreal Engine 编辑器中的插件。

## 功能特性

- **AI 辅助**: 将 LLM 直接集成到 Unreal Engine 编辑器中
- **工具调用**: 支持截图、文件操作等自定义工具
- **视觉模式**: 切换图像输入/输出的视觉功能
- **会话管理**: 按项目组织对话，支持持久化存储
- **技能系统**: 可扩展的插件架构，支持自定义技能
- **记忆系统**: 从记忆文件自动注入上下文
- **DAG 多工具编排**: 依赖解析的并行工具执行 (LLMCompiler 风格)
- **辅助模型系统**: 本地轻量模型用于加速和优化

## 辅助模型系统（可选）

插件可利用本地轻量模型 (Qwen3VL-2B via llama.cpp) 通过两项核心技术加速主 AI 工作流：

### IdleSpec — 推测执行
*受 [SpecEyes](https://github.com/MAC-AutoML/SpecEyes) 启发*

当工具正在执行（等待 I/O）时，辅助模型预测下一个工具调用。工具返回后，若预测正确则立即派发下一工具 — **跳过一次完整 LLM 往返**（节省 2-5 秒）。

### SWE-Pruner — 上下文剪枝
*受 [SWE-Pruner](https://github.com/Ayanami1314/swe-pruner) 启发*

每轮 LLM 请求前，轻量模型判断各条历史消息与当前任务的相关性，移除无关消息以减少上下文长度并加速 decode。

### 本地 VL 图像分析

**视觉模式关闭时**，本地 Qwen3VL 模型分析截图和上传图片，将其转换为文本描述供主 LLM 使用，无需云端视觉 API。

### 快/慢思考混合

当对话进入工具调用循环（2+ 连续 tool pair）时，自动抑制 `reasoning_content`，使 DeepSeek 直接输出 tool_calls 而不进行深层推理。分析阶段自动恢复深度思考。

---

## 辅助模型配置

系统**自动检测**辅助模型文件是否存在。如文件缺失，所有功能优雅降级 — 无需修改代码。

### 所需文件

将以下内容放置于 `<ProjectDir>/Plugins/MCPToolbox/AuxiliaryModule/`：

```
AuxiliaryModule/
├── llama/                                     # llama.cpp 运行时
│   └── llama-server.exe
│   └── llama.dll
│   └── ggml-cuda.dll (及其他 ggml-*.dll)
│   └── ...
└── qwen3-vl/                                  # Qwen3VL-2B 模型
    └── Qwen3VL-2B-Instruct-Q4_K_M.gguf        # 模型 (~1.3 GB)
    └── mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf   # 视觉投影器
```

### 下载链接

1. **llama.cpp** — 从 [llama.cpp releases](https://github.com/ggml-org/llama.cpp/releases) 下载预编译 Windows 二进制包（搜索 `llama-bxxxx-win-cuda-cuXX.x.zip`）
2. **Qwen3VL-2B 模型** — 从 HuggingFace 下载：
   - 模型: [Qwen3VL-2B-Instruct-Q4_K_M.gguf](https://huggingface.co/bartowski/Qwen3VL-2B-Instruct-GGUF)
   - 投影器: [mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf](https://huggingface.co/bartowski/Qwen3VL-2B-Instruct-GGUF)

### 硬件要求

- 支持 CUDA 的 GPU（模型通过 `-ngl 999` 全部加载到 GPU）
- 模型约需 1.3 GB 显存
- KV 缓存额外约需 2 GB 显存（8-bit 量化，10240 上下文）

### 验证

放置文件后重启 UE 编辑器，工具栏状态将显示：

- "辅助模型准备就绪" — 所有文件存在，功能已激活
- "辅助模型不可用" — 文件缺失，功能已禁用

服务器启动于 `localhost:8088`（端口被占用时自动扫描 8088-8138）。

---

## 系统要求

- Unreal Engine 5.8 或更高版本
- Windows 10/11
- 访问 LLM API 需要互联网连接
- （可选）支持 CUDA 的 GPU 用于辅助模型加速

## 安装步骤

1. 将此仓库克隆到项目的 `Plugins/` 目录
2. （可选）按上述说明下载并放置辅助模型文件
3. 在 UE 插件管理器中启用该插件
4. 构建项目

## 使用方法

1. 从窗口菜单打开 MCP Toolbox
2. 在设置面板中配置 API 参数
3. 开始与 AI 助手对话
4. 切换视觉模式以上传图片（云端或本地 VL 处理）

## 性能优化 (v2.0)

| 模块 | 优化 |
|------|------|
| 工具派发 | O(n) 线性搜索 → TSet O(1) 查找 |
| LLM 请求体 | Tools JSON 缓存（构建一次，复用） |
| 系统提示 | 预分配 8KB 缓冲区（消除 30+ 次重分配） |
| DAG 规划 | AdjacencyList 改为 TSet，拓扑排序缓存 |
| 消息构建 | 每条消息 JSON 缓存（不再每轮重建） |
| 截图 JSON | 直接字符串拼接（消除 2MB base64 Printf 拷贝） |
| llama-server | 10240 上下文，KV 缓存 q8_0 量化（省 75% 显存） |

## 参考项目

| 项目 | 用途 | 状态 |
|------|------|------|
| [assistant](https://github.com/ollama/assistant) | C++ LLM 函数调用库 (FunctionTable, SSE 解析, OpenAI API) | 核心依赖 |
| [SpecEyes](https://github.com/MAC-AutoML/SpecEyes) | Agent 级投机执行 | 已集成 |
| [SWE-Pruner](https://github.com/Ayanami1314/swe-pruner) | 任务感知上下文剪枝 | 已集成 |
| [Hail Hydra](https://github.com/AR6420/Hail_Hydra) | 多头投机执行框架 | 已研究 |

## 许可证

本项目包含来自 [assistant](https://github.com/ollama/assistant) 库的修改代码 (`ThirdParty/assistant/`)，用于 LLM 函数调用、SSE 响应解析和 OpenAI 兼容 API 集成。
