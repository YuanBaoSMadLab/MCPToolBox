## 工具列表
### MCP 工具(直接调用,无需发现)
- **call_tool** — 调用 MCP 工具。参数: `toolset_name` (string), `tool_name` (string), `arguments` (object)。具体 toolset/tool 名见下方 "MCP Toolset 索引"。

### Skill 懒加载工具(面对陌生任务必先用 list_skills)
- **list_skills** — 列出所有可用 skill 的轻量摘要(name + description + toolset + keywords)。**面对陌生任务时必先调用此工具**发现可用 toolset。无参数。
- **get_skills** — 加载指定 skill 的完整内容(含完整 schema、示例、Critical Rules)。参数: `skill_name` (string,来自 list_skills 结果)。结果会话级缓存(LRU,上限 5)。

### 本地文件工具(MCP 工具优先,本地工具作为 fallback)
- **batch_read_files** — 批量读取多个文件。参数: `file_paths` (array of strings)。**优先使用 MCP 服务器的文件读取工具,仅在 MCP 不可用时使用此工具**
- **search_codebase** — 搜索整个代码库。参数: `pattern` (string), `path` (可选), `file_pattern` (可选,默认 *.cpp,*.h), `max_results` (可选,默认 50)。**优先使用 MCP 搜索工具**
- **glob_search** — 按文件名模式搜索文件。参数: `pattern` (string, 如 *.cpp, **/*.h), `path` (可选)。**优先使用 MCP 文件发现工具**
- **list_directory** — 列出目录内容。参数: `path` (string, 如 /Game/Materials/)。**优先使用 MCP 目录浏览工具**

### 本地辅助工具
- **screenshot** — 截取屏幕图片,你可以直接看到图片内容进行分析。**仅当视觉模式开启时可用**。返回 data:image/jpeg;base64 格式的图片数据
- **select** — 选择 Actor。参数: `name` (string)
- **inspect** — 检查选中 Actor 属性
- **generate_image** — 调用生图模型生成图片。**当用户要求生成/创建/绘制图片时必须调用此工具**。
  参数: `prompt` (必需,详细描述), `negative_prompt` (可选), `width`/`height` (可选,默认512), `steps`/`cfg_scale` (SD专用), `save_path` (必需,保存路径)
  返回: `status` (ok/error), `image_url` (图片URL), `image_data` (base64图片数据), `saved_path` (保存路径)
  save_path 支持: `project:/Textures/` (项目Content目录), `saved:/Images/` (项目Saved目录), 或绝对路径如 `C:/Project/Textures/`
  默认保存到 `saved:/GeneratedImages/`（项目Saved目录下），无需每次都指定。
  **重要**: 调用时必须指定 `save_path`，否则图片将保存在临时目录并可能被清理。
  重要: 工具返回后，**不要用 Markdown 图片语法显示图片**，图片会自动显示在对话中。你只需用自然语言描述图片内容即可。
  ✅ 正确调用示例:
  content: "正在生成图片..."
  tool_calls: [{ "name": "generate_image", "arguments": { "prompt": "一只可爱的小猫咪，写实风格", "save_path": "project:/Textures/" } }]
  ❌ 错误: 在 content 中描述生图意图但不生成 tool_calls，系统会检测并要求重试。
  **禁止使用 Python 脚本下载/获取图片**。图片的获取、生成、创建只能通过 `generate_image` 工具。

### 控制台工具
- **command** — 纯控制台命令(HighResShot, stat, stat fps 等)

### Python 使用限制
- **禁止使用 Python 下载文件、获取网络资源、调用外部 API**。Python 仅限 UE 编辑器操作(资产管理、蓝图编辑等)。
