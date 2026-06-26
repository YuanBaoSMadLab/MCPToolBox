# MCPToolBox

An Unreal Engine 5.8 plugin that integrates Large Language Models (LLM) with the editor through the Model Context Protocol (MCP).

## Features

- **AI-Powered Assistance**: Integrate LLMs directly into the Unreal Engine editor
- **Tool Calling**: Support for custom tools including screenshot capture, file operations, and more
- **Visual Mode**: Toggle visual capabilities for image input/output
- **Session Management**: Organize conversations by project with persistent storage
- **Skill System**: Extensible plugin architecture for custom skills
- **Memory System**: Automatic context injection from memory files
- **DAG Multi-Tool Orchestration**: Parallel execution of independent tool calls with dependency resolution (LLMCompiler-style)
- **Auxiliary Model System**: Local lightweight model for acceleration and optimization

## Auxiliary Model System (Optional)

The plugin can leverage a local lightweight model (Qwen3VL-2B via llama.cpp) to accelerate the main AI workflow through two core techniques:

### IdleSpec — Speculative Execution
*Inspired by [SpecEyes](https://github.com/MAC-AutoML/SpecEyes)*

While a tool is executing (waiting for I/O), the auxiliary model predicts the next tool call. When the tool returns, if the prediction is correct, it immediately dispatches the next tool — **skipping one full LLM round-trip** (2-5 seconds saved).

### SWE-Pruner — Context Pruning
*Inspired by [SWE-Pruner](https://github.com/Ayanami1314/swe-pruner)*

Before each LLM request, a lightweight model judges the relevance of each historical message to the current task. Irrelevant messages are removed, reducing context length and speeding up decode.

### Local VL Image Analysis

When **Vision Mode is OFF**, the local Qwen3VL model analyzes screenshots and uploaded images, converting them to text descriptions for the main LLM. No cloud vision API needed.

### Fast/Deep Thinking Hybrid

When the conversation enters a tool-calling loop (2+ consecutive tool pairs), `reasoning_content` is suppressed, making DeepSeek respond with tool calls directly without deep reasoning overhead. Deep thinking resumes automatically when analysis is needed.

---

## Configuring the Auxiliary Model

The system **auto-detects** whether auxiliary model files are present. If missing, all features gracefully disable — no code changes needed.

### Required Files

Place the following in `<ProjectDir>/Plugins/MCPToolbox/AuxiliaryModule/`:

```
AuxiliaryModule/
├── llama/                                     # llama.cpp runtime
│   └── llama-server.exe
│   └── llama.dll
│   └── ggml-cuda.dll (and other ggml-*.dll)
│   └── ...
└── qwen3-vl/                                  # Qwen3VL-2B model
    └── Qwen3VL-2B-Instruct-Q4_K_M.gguf        # Model (~1.3 GB)
    └── mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf   # Vision projector
```

### Download Links

1. **llama.cpp** — Download pre-built Windows binaries from [llama.cpp releases](https://github.com/ggml-org/llama.cpp/releases) (look for `llama-bxxxx-win-cuda-cuXX.x.zip`)
2. **Qwen3VL-2B Model** — Download from HuggingFace:
   - Model: [Qwen3VL-2B-Instruct-Q4_K_M.gguf](https://huggingface.co/bartowski/Qwen3VL-2B-Instruct-GGUF)
   - Projector: [mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf](https://huggingface.co/bartowski/Qwen3VL-2B-Instruct-GGUF)

### Hardware Requirements

- GPU with CUDA support (the model loads via `-ngl 999`, all layers on GPU)
- ~1.3 GB VRAM for the model
- ~2 GB additional VRAM for KV cache (8-bit quantized, 10240 context)

### Verification

After placing the files, restart the UE editor. The toolbar status will show:

- "辅助模型准备就绪" (Auxiliary Model Ready) — All files present, features active
- "辅助模型不可用" (Auxiliary Model Unavailable) — Missing files, features gracefully disabled

Server starts on `localhost:8088` (auto-scans ports 8088-8138 if occupied).

---

## Requirements

- Unreal Engine 5.8 or later
- Windows 10/11
- Internet connection for LLM API access
- (Optional) GPU with CUDA for auxiliary model acceleration

## Installation

1. Clone this repository to your project's `Plugins/` directory
2. (Optional) Download and place auxiliary model files as described above
3. Enable the plugin in Unreal Engine's Plugin Manager
4. Build the project

## Usage

1. Open the MCP Toolbox from the Window menu
2. Configure your API settings in the settings panel
3. Start conversing with the AI assistant
4. Toggle Vision Mode to upload images (cloud or local VL processing)

## Performance Optimizations (v2.0)

| Area | Optimization |
|------|-------------|
| Tool dispatch | O(n) linear search → TSet O(1) lookup |
| LLM request body | Tools JSON cached (built once, reused) |
| System prompt | Pre-allocated 8KB buffer (30+ reallocs eliminated) |
| DAG planning | AdjacencyList changed to TSet, topology sort cached |
| Message building | Per-message JSON cached (not rebuilt on every send) |
| Screenshot JSON | Direct string concat (2MB base64 Printf copy eliminated) |
| llama-server | 10240 context, KV cache q8_0 quantization (75% VRAM savings) |

## Reference Projects

| Project | Usage | Status |
|---------|-------|--------|
| [assistant](https://github.com/ollama/assistant) | C++ LLM function calling library (FunctionTable, SSE parsing, OpenAI API) | Core dependency |
| [SpecEyes](https://github.com/MAC-AutoML/SpecEyes) | Agent-level speculative execution | Integrated |
| [SWE-Pruner](https://github.com/Ayanami1314/swe-pruner) | Task-aware context pruning | Integrated |
| [Hail Hydra](https://github.com/AR6420/Hail_Hydra) | Multi-head speculative execution | Researched |

## License

This project includes modified code from the [assistant](https://github.com/ollama/assistant) library (`ThirdParty/assistant/`) for LLM function calling, SSE response parsing, and OpenAI-compatible API integration.
