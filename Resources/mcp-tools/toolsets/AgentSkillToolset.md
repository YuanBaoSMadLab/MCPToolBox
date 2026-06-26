# ToolsetRegistry.AgentSkillToolset

> 4 个工具 | 更新时间: 2026-06-26 11:30:56

## `UpdateSkill`

**完整名称:** `ToolsetRegistry.AgentSkillToolset.UpdateSkill`

**描述:** 更新现有AgentSkill。
这应该只在获得用户的明确指示或许可后才调用。

### 输入参数

- `skillPath` (string) **必需**: 要修改的技能的完整路径，即/Game/Skills/MySkill.MySkill_C。
- `description` (string) **必需**: 技能的简要描述。
- `details` (object) **必需**: 关于如何使用技能的详细信息。

### 调用代码

```json
{
  "toolset_name": "ToolsetRegistry.AgentSkillToolset",
  "tool_name": "UpdateSkill",
  "arguments": {}
}
```

---

## `ListSkills`

**完整名称:** `ToolsetRegistry.AgentSkillToolset.ListSkills`

**描述:** 获取项目中所有AgentSkills的摘要。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "ToolsetRegistry.AgentSkillToolset",
  "tool_name": "ListSkills",
  "arguments": {}
}
```

---

## `GetSkills`

**完整名称:** `ToolsetRegistry.AgentSkillToolset.GetSkills`

**描述:** 返回关于特定AgentSkills集的详细信息。

### 输入参数

- `skillPaths` (array) **必需**: 要检索的AgentSkills的路径列表。

### 调用代码

```json
{
  "toolset_name": "ToolsetRegistry.AgentSkillToolset",
  "tool_name": "GetSkills",
  "arguments": {}
}
```

---

## `CreateSkill`

**完整名称:** `ToolsetRegistry.AgentSkillToolset.CreateSkill`

**描述:** 创建一个新的AgentSkill。
这应该只在获得用户的明确指示或许可后才调用。

### 输入参数

- `folderPath` (string) **必需**: 在其中创建技能的文件夹。即/Game/Skills/。
- `assetName` (string) **必需**: 要创建的技能的名称，即MySkill。
- `description` (string) **必需**: 技能的简要描述。
- `details` (object) **必需**: 关于如何使用技能的详细信息。

### 调用代码

```json
{
  "toolset_name": "ToolsetRegistry.AgentSkillToolset",
  "tool_name": "CreateSkill",
  "arguments": {}
}
```

---
