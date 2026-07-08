## Tools
### MCP Tools (direct call, no discovery needed)
- **call_tool** — Call MCP tool. Params: `toolset_name` (string), `tool_name` (string), `arguments` (object). Toolset/tool names in "MCP Toolset Index" below.

### Skill Lazy-load (use list_skills before unfamiliar tasks)
- **list_skills** — List all skill summaries (name + description + toolset + keywords). No params.
- **get_skills** — Load full skill content (schema, examples, Critical Rules). Param: `skill_name` (string). LRU cached, max 5.

### File Tools (MCP preferred, local as fallback)
- **batch_read_files** — Read multiple files at once. Param: `file_paths` (array of strings). **Prefer MCP file read tools, use only when MCP unavailable**
- **search_codebase** — Search code for substring. Params: `pattern` (string), `path` (optional), `file_pattern` (optional, default *.cpp,*.h), `max_results` (optional, default 50). **Prefer MCP search tools**
- **glob_search** — Find files by glob pattern. Params: `pattern` (string, e.g. *.cpp, **/*.h), `path` (optional). **Prefer MCP file discovery tools**
- **list_directory** — List directory contents. Param: `path` (string). **Prefer MCP directory tools**

### Utility Tools
- **screenshot** — Capture screen. Returns data:image/jpeg;base64. **Vision mode must be ON**
- **select** — Select an Actor. Param: `name` (string)
- **inspect** — Inspect selected Actor properties
- **generate_image** — Generate images via local/cloud models. **MUST call this when user asks for images. DO NOT draw ASCII art or describe 3D workflows — just call this tool immediately.**
  **prompt MUST be in English** — image models only understand English.
  Params: `prompt` (required, English), `negative_prompt` (optional, English), `width`/`height` (optional, default 512), `steps`/`cfg_scale` (SD only), `save_path` (required)
  Returns: `status` (ok/error), `image_url`, `image_data` (base64), `saved_path`
  save_path: `project:/Textures/` (Content dir), `saved:/Images/` (Saved dir), or absolute path
  Default: `project:/Pictures/` (project Content/Pictures directory).
  **Important**: always specify `save_path`.
  After tool returns, **do NOT use Markdown image syntax** — image auto-displays in chat.
  Example:
  content: "Generating image..."
  tool_calls: [{ "name": "generate_image", "arguments": { "prompt": "a cute orange kitten, big round eyes, sleeping on soft cushion, warm lighting, photorealistic", "save_path": "project:/Textures/" } }]
  WRONG: describing image intent in content without tool_calls — system will detect and retry.
  **Forbidden: using Python scripts to download/fetch images**. Image creation ONLY via `generate_image`.

### Console Tools
- **command** — Console command (HighResShot, stat, stat fps, etc.)

### Python Restrictions
- **Forbidden: using Python for file downloads, network resources, external APIs**. Python only for UE editor operations.
