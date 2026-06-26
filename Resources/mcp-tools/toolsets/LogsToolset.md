# EditorToolset.LogsToolset

> 4 个工具 | 更新时间: 2026-06-26 11:30:56

## `SetVerbosity`

**完整名称:** `EditorToolset.LogsToolset.SetVerbosity`

**描述:** 设置日志类别的冗长度级别。

### 输入参数

- `category` (string): 日志类别名称，例如“LogTemp”。
- `verbosity` (string) **必需**: 详细程度："NoLogging"、"Fatal"、"Error"、 "Warning"、"Display"、"Log"、"Verbose"或"VeryVerbose"之一。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.LogsToolset",
  "tool_name": "SetVerbosity",
  "arguments": {}
}
```

---

## `GetVerbosity`

**完整名称:** `EditorToolset.LogsToolset.GetVerbosity`

**描述:** 返回日志类别的当前冗长度级别。

### 输入参数

- `category` (string): 日志类别名称，例如“LogTemp”。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.LogsToolset",
  "tool_name": "GetVerbosity",
  "arguments": {}
}
```

---

## `GetLogEntries`

**完整名称:** `EditorToolset.LogsToolset.GetLogEntries`

**描述:** 返回来自当前会话的日志文件的日志条目。

### 输入参数

- `category` (string): 如果非空，则只返回此日志类别中的条目（例如"LogTemp")。
- `pattern` (string) **必需**: 如果非空，则只返回文本与此正则表达式 匹配的条目。
- `maxEntries` (integer): 要返回的最大条目数，取自日志末尾。 通过0表示无限制。默认为1000。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.LogsToolset",
  "tool_name": "GetLogEntries",
  "arguments": {}
}
```

---

## `GetLogCategories`

**完整名称:** `EditorToolset.LogsToolset.GetLogCategories`

**描述:** 返回已注册日志类别的排序列表。

### 输入参数

- `filter` (string) **必需**: 

### 调用代码

```json
{
  "toolset_name": "EditorToolset.LogsToolset",
  "tool_name": "GetLogCategories",
  "arguments": {}
}
```

---
