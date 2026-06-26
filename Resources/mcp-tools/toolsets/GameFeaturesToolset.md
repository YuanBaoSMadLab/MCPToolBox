# GameFeaturesToolset.GameFeaturesToolset

> 7 个工具 | 更新时间: 2026-06-26 11:30:56

## `RequestDeactivateGameFeature`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.RequestDeactivateGameFeature`

**描述:** 请求停用游戏功能插件。
如果停用请求成功提交，则返回true。实际的停用
是异步发生的——轮询GetGameFeatureState()以确认完成。
如果子系统不可用或未找到插件，则会引发错误。

### 输入参数

- `pluginName` (string) **必需**: GFP的名称。

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "RequestDeactivateGameFeature",
  "arguments": {}
}
```

---

## `RequestActivateGameFeature`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.RequestActivateGameFeature`

**描述:** 请求激活游戏功能插件。
如果激活请求成功提交，则返回true。实际激活是异步
发生的——通过轮询GetGameFeatureState()或IsGameFeatureActive()来确认完成。如果子系统不可用或未找到插件，
则会引发错误。

### 输入参数

- `pluginName` (string) **必需**: GFP的名称。

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "RequestActivateGameFeature",
  "arguments": {}
}
```

---

## `ListEnabledGameFeaturePlugins`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.ListEnabledGameFeaturePlugins`

**描述:** 列出所有已启用的游戏功能插件，按名称排序。除了判断一个插件是否为
游戏功能插件之外，已启用的插件是游戏功能系统唯一了解的插件。
使用插件工具集执行常规插件启用/禁用任务。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "ListEnabledGameFeaturePlugins",
  "arguments": {}
}
```

---

## `ListDiscoveredGameFeaturePlugins`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.ListDiscoveredGameFeaturePlugins`

**描述:** 列出所有已发现的游戏功能插件，按名称排序。这包括已启用和已禁用的
插件。除了判断一个插件是否为
游戏功能插件之外，游戏功能系统只了解已启用的插件。
使用插件工具集执行常规插件启用/禁用任务。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "ListDiscoveredGameFeaturePlugins",
  "arguments": {}
}
```

---

## `IsGameFeaturePlugin`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.IsGameFeaturePlugin`

**描述:** 返回插件是否为游戏功能插件。如果插件管理器找不到此
名称的插件，将会报错。

### 输入参数

- `pluginName` (string) **必需**: 插件的名称

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "IsGameFeaturePlugin",
  "arguments": {}
}
```

---

## `IsGameFeatureActive`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.IsGameFeatureActive`

**描述:** 检查游戏功能插件是否处于激活状态。如果子系统不可用或未找到插件，
则会引发错误。
如果需要在插件未激活时获取当前状态，请使用GetGameFeatureState。

### 输入参数

- `pluginName` (string) **必需**: 游戏功能插件的名称。

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "IsGameFeatureActive",
  "arguments": {}
}
```

---

## `GetGameFeatureState`

**完整名称:** `GameFeaturesToolset.GameFeaturesToolset.GetGameFeatureState`

**描述:** 获取游戏功能插件的当前状态。

### 输入参数

- `pluginName` (string) **必需**: 游戏功能插件的名称。

### 调用代码

```json
{
  "toolset_name": "GameFeaturesToolset.GameFeaturesToolset",
  "tool_name": "GetGameFeatureState",
  "arguments": {}
}
```

---
