# EditorToolset.EditorAppToolset

> 21 个工具 | 更新时间: 2026-06-26 11:30:56

## `WorldPosToScreenCoords`

**完整名称:** `EditorToolset.EditorAppToolset.WorldPosToScreenCoords`

**描述:** 基于编辑器视口摄像机，将世界空间位置转换为归一化的屏幕空间坐标。

### 输入参数

- `position` (object) **必需**: 要转换的世界空间位置。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "WorldPosToScreenCoords",
  "arguments": {}
}
```

---

## `StopPIE`

**完整名称:** `EditorToolset.EditorAppToolset.StopPIE`

**描述:** 停止当前运行的游戏会话（PIE或模拟）。
如果没有正在运行的游玩会话，则会引发错误。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "StopPIE",
  "arguments": {}
}
```

---

## `StartPIE`

**完整名称:** `EditorToolset.EditorAppToolset.StartPIE`

**描述:** 开始一个使用当前关卡的Play-In-Editor或Simulate-In-Editor会话。在引擎触发PostPIEStarted（会话已完全启动，
BeginPlay已调用）之后完成，并在 Options.WarmupSeconds经过后生效，从而在代理检查状态或记录日志之前，为项目
特定的初始化（如服务、认证、插件预热）
提供稳定的收尾时间。
如果游戏会话已在运行，则会引发错误。

### 输入参数

- `options` (object) **必需**: 会话配置：PIE与Simulate模式、游玩模式、可选的 生成变换重载、预热时长。请参阅FPIESessionOptions。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "StartPIE",
  "arguments": {}
}
```

---

## `SetContentBrowserPath`

**完整名称:** `EditorToolset.EditorAppToolset.SetContentBrowserPath`

**描述:** 将活动的内容浏览器导航到指定的文件夹路径。

### 输入参数

- `path` (string) **必需**: 要导航到的内部路径，例如：“/Game/Meshes”。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "SetContentBrowserPath",
  "arguments": {}
}
```

---

## `SetCameraTransform`

**完整名称:** `EditorToolset.EditorAppToolset.SetCameraTransform`

**描述:** 设置关卡视口摄像机的位置和旋转。

### 输入参数

- `transform` (object) **必需**: 表示带有可选位置、旋转和缩放的3D变换。
未设置的字段在创建Object时表示“使用默认/恒等值”，在修改现有Object时表示“保持不变”。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "SetCameraTransform",
  "arguments": {}
}
```

---

## `SelectAssets`

**完整名称:** `EditorToolset.EditorAppToolset.SelectAssets`

**描述:** 在内容浏览器中选择指定资产。
内容浏览器应用该选项后即为完成。

### 输入参数

- `assetPaths` (array) **必需**: 要选择的资产的包路径。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "SelectAssets",
  "arguments": {}
}
```

---

## `SelectActors`

**完整名称:** `EditorToolset.EditorAppToolset.SelectActors`

**描述:** 选择当前场景中的指定Actor。

### 输入参数

- `actors` (array) **必需**: 要选择的Actor。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "SelectActors",
  "arguments": {}
}
```

---

## `SearchCVars`

**完整名称:** `EditorToolset.EditorAppToolset.SearchCVars`

**描述:** 查找所有包含给定名称的控制台变量。

### 输入参数

- `name` (string) **必需**: 要搜索的部分或完整名称。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "SearchCVars",
  "arguments": {}
}
```

---

## `ScreenCoordsToWorld`

**完整名称:** `EditorToolset.EditorAppToolset.ScreenCoordsToWorld`

**描述:** 在给定的归一化视图空间坐标处，找到最近实心Object的世界位置。

### 输入参数

- `coords` (object) **必需**: 要追踪的归一化屏幕空间坐标。
- `traceDistance` (number): 场景中要追踪的最大距离。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "ScreenCoordsToWorld",
  "arguments": {}
}
```

---

## `OpenEditorForAsset`

**完整名称:** `EditorToolset.EditorAppToolset.OpenEditorForAsset`

**描述:** 为指定资产打开资产编辑器。

### 输入参数

- `assetPath` (string) **必需**: 要打开的资产的路径，例如“/Game/Meshes/SM_Cube”。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "OpenEditorForAsset",
  "arguments": {}
}
```

---

## `IsPIERunning`

**完整名称:** `EditorToolset.EditorAppToolset.IsPIERunning`

**描述:** 返回当前是否正在运行编辑器会话。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "IsPIERunning",
  "arguments": {}
}
```

---

## `GetVisibleActors`

**完整名称:** `EditorToolset.EditorAppToolset.GetVisibleActors`

**描述:** 返回当前关卡中边界与视口视锥体相交的所有Actor。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "GetVisibleActors",
  "arguments": {}
}
```

---

## `GetSelectedAssets`

**完整名称:** `EditorToolset.EditorAppToolset.GetSelectedAssets`

**描述:** 获取在内容浏览器中选择的资产列表。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "GetSelectedAssets",
  "arguments": {}
}
```

---

## `GetSelectedActors`

**完整名称:** `EditorToolset.EditorAppToolset.GetSelectedActors`

**描述:** 获取关卡编辑器中当前选定的Actor。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "GetSelectedActors",
  "arguments": {}
}
```

---

## `GetOpenAssets`

**完整名称:** `EditorToolset.EditorAppToolset.GetOpenAssets`

**描述:** 获取当前在资产编辑器中打开的资产列表。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "GetOpenAssets",
  "arguments": {}
}
```

---

## `GetContentBrowserPath`

**完整名称:** `EditorToolset.EditorAppToolset.GetContentBrowserPath`

**描述:** 获取激活的内容浏览器的当前路径。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "GetContentBrowserPath",
  "arguments": {}
}
```

---

## `GetCameraTransform`

**完整名称:** `EditorToolset.EditorAppToolset.GetCameraTransform`

**描述:** 返回关卡视口摄像机的位置和旋转。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "GetCameraTransform",
  "arguments": {}
}
```

---

## `FocusOnActors`

**完整名称:** `EditorToolset.EditorAppToolset.FocusOnActors`

**描述:** 重新定位关卡编辑器摄像机，使其聚焦于指定Actor。
PIE激活时无法调用。

### 输入参数

- `actors` (array) **必需**: 要聚焦关卡摄像机的Actor。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "FocusOnActors",
  "arguments": {}
}
```

---

## `CaptureViewport`

**完整名称:** `EditorToolset.EditorAppToolset.CaptureViewport`

**描述:** 捕获带可选注释的关卡视口。

注释渲染在可见Actor上叠加一个投影的3D世界空间网格，
以及名称和位置标签。网格绘制在可配置的
地面平面Z处，并通过摄像机投影，交点处
有坐标数字（以米为单位显示）。每个带标签的Actor在其
投影的屏幕位置处得到一个准星，并放置一个引导线标注以避免重叠。这赋予了
具备视觉能力的代理空间感知能力：它可以引用网格
坐标来指导放置，并通过标签识别场景内容。

### 输入参数

- `captureTransform` (object): 可选的捕获姿势。如果未设置，则使用视口 当前摄像机。
- `annotations` (object): 可选的注释覆层配置。仅在你需要 此信息以执行空间操作时使用。
- `bShowUI` (boolean): 如果为false（默认），则在捕获的图像中隐藏编辑器UI覆层， 例如变换小工具和选择轮廓。设为true以精确 捕获屏幕上的内容，包括小工具。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "CaptureViewport",
  "arguments": {}
}
```

---

## `CaptureEditorImage`

**完整名称:** `EditorToolset.EditorAppToolset.CaptureEditorImage`

**描述:** 捕获整个编辑器应用程序的用户可见图像。

### 输入参数

(无参数)

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "CaptureEditorImage",
  "arguments": {}
}
```

---

## `CaptureAssetImage`

**完整名称:** `EditorToolset.EditorAppToolset.CaptureAssetImage`

**描述:** 为指定资产渲染缩略图（例如静态网格体、骨骼网格体、
骨架、动画、蒙太奇、材质、纹理）。

### 输入参数

- `assetPath` (string) **必需**: 资产的路径，例如'/Game/Meshes/SM_Cube'。

### 调用代码

```json
{
  "toolset_name": "EditorToolset.EditorAppToolset",
  "tool_name": "CaptureAssetImage",
  "arguments": {}
}
```

---
