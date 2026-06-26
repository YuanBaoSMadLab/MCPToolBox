# UE5 MCP Toolset Catalog

> Generated: 2026-06-26 11:20:33 | Loaded: 6 sets | Tools: 55

## Loaded Toolsets

### EditorToolset.EditorAppToolset

| Tool | Description |
|------|-------------|
| WorldPosToScreenCoords | 基于编辑器视口摄像机，将世界空间位置转换为归一化的屏幕空间坐标。 |
| StopPIE | 停止当前运行的游戏会话（PIE或模拟）。 |
| StartPIE | 开始一个使用当前关卡的Play-In-Editor或Simulate-In-Editor会话。在引擎触发PostPIEStarted（会话已完全启动， |
| SetContentBrowserPath | 将活动的内容浏览器导航到指定的文件夹路径。 |
| SetCameraTransform | 设置关卡视口摄像机的位置和旋转。 |
| SelectAssets | 在内容浏览器中选择指定资产。 |
| SelectActors | 选择当前场景中的指定Actor。 |
| SearchCVars | 查找所有包含给定名称的控制台变量。 |
| ScreenCoordsToWorld | 在给定的归一化视图空间坐标处，找到最近实心Object的世界位置。 |
| OpenEditorForAsset | 为指定资产打开资产编辑器。 |
| IsPIERunning | 返回当前是否正在运行编辑器会话。 |
| GetVisibleActors | 返回当前关卡中边界与视口视锥体相交的所有Actor。 |
| GetSelectedAssets | 获取在内容浏览器中选择的资产列表。 |
| GetSelectedActors | 获取关卡编辑器中当前选定的Actor。 |
| GetOpenAssets | 获取当前在资产编辑器中打开的资产列表。 |
| GetContentBrowserPath | 获取激活的内容浏览器的当前路径。 |
| GetCameraTransform | 返回关卡视口摄像机的位置和旋转。 |
| FocusOnActors | 重新定位关卡编辑器摄像机，使其聚焦于指定Actor。 |
| CaptureViewport | 捕获带可选注释的关卡视口。 |
| CaptureEditorImage | 捕获整个编辑器应用程序的用户可见图像。 |
| CaptureAssetImage | 为指定资产渲染缩略图（例如静态网格体、骨骼网格体、 |

### EditorToolset.LogsToolset

| Tool | Description |
|------|-------------|
| SetVerbosity | 设置日志类别的冗长度级别。 |
| GetVerbosity | 返回日志类别的当前冗长度级别。 |
| GetLogEntries | 返回来自当前会话的日志文件的日志条目。 |
| GetLogCategories | 返回已注册日志类别的排序列表。 |

### GameFeaturesToolset.GameFeaturesToolset

| Tool | Description |
|------|-------------|
| RequestDeactivateGameFeature | 请求停用游戏功能插件。 |
| RequestActivateGameFeature | 请求激活游戏功能插件。 |
| ListEnabledGameFeaturePlugins | 列出所有已启用的游戏功能插件，按名称排序。除了判断一个插件是否为 |
| ListDiscoveredGameFeaturePlugins | 列出所有已发现的游戏功能插件，按名称排序。这包括已启用和已禁用的 |
| IsGameFeaturePlugin | 返回插件是否为游戏功能插件。如果插件管理器找不到此 |
| IsGameFeatureActive | 检查游戏功能插件是否处于激活状态。如果子系统不可用或未找到插件， |
| GetGameFeatureState | 获取游戏功能插件的当前状态。 |

### PhysicsToolsets.PhysicsAssetToolset

| Tool | Description |
|------|-------------|
| SetSphere | 在形体上添加或替换Sphere碰撞图元。 |
| SetConstraintLimits | 更新现有约束的角限制。 |
| SetCapsule | 在形体上添加或替换胶囊体碰撞图元。 |
| SetBox | 在形体上添加或替换盒体碰撞图元。 |
| SetBodyPhysicsMode | 设置给定形体的物理模拟模式。 |
| SetBodyMassScale | 设置给定形体的质量比例乘数。 |
| RemoveShape | 按名称从形体中移除碰撞图元。 |
| RemoveConstraint | 移除两个形体之间的约束。 |
| RemoveBody | 移除给定骨骼的形体以及引用它的所有约束。 |
| GetConstraints | 返回物理资产中的所有约束及其当前角限制。 |
| GetBodyShapes | 返回分配给形体的所有碰撞形状。 |
| GetBodyPhysicsMode | 返回给定形体的物理模拟模式。 |
| GetBodyNames | 返回物理资产中每个刚体的骨骼名称。 |
| GetBodyMassScale | 返回给定形体的质量缩放倍数。 |
| CreateFromMesh | 从骨骼网格体创建物理资产，为每个骨骼自动生成 |
| AddConstraint | 在两个形体之间添加一个新约束。两个形体必须已经存在。 |
| AddBody | 为给定骨骼添加一个新的空形体。 |

### ToolsetRegistry.AgentSkillToolset

| Tool | Description |
|------|-------------|
| UpdateSkill | 更新现有AgentSkill。 |
| ListSkills | 获取项目中所有AgentSkills的摘要。 |
| GetSkills | 返回关于特定AgentSkills集的详细信息。 |
| CreateSkill | 创建一个新的AgentSkill。 |

### editor_toolset.toolsets.programmatic.ProgrammaticToolset

| Tool | Description |
|------|-------------|
| execute_tool_script | Execute a Python script against the toolset APIs. |
| get_execution_environment | Get details about execution environment. |

---

## Known Toolsets (Not Loaded - Enable Plugin)

These toolsets are documented but not loaded in the current UE instance.
Go to Edit > Plugins, search for the plugin, enable it, restart UE.

| Plugin | Function |
|--------|----------|
| MVCBlueprintToolset | 蓝图工具 - 创建/编辑蓝图、节点图、变量、组件、GAS |
| ScriptBlueprintToolset | 脚本蓝图工具 |
| EditorScriptingToolset | 编辑器脚本 |
| StaticMeshToolset | 静态网格工具 |
| GeometryScriptingToolset | 几何体脚本 |
| ModelingModeToolset | 建模模式 |
| PCGToolset | PCG 程序化生成 |
| AnimationAssistantToolset | 动画工具 |
| ConversationToolset | 对话系统 |
| GameplayCueToolset | GameplayCue |
| LayerToolset | 图层管理 |
| SmartObjectToolset | SmartObject |
| WorldConditionToolset | 世界条件 |
| RigVMBlueprintToolset | RigVM 蓝图 |
| SlateUICalloutToolset | UI 工具 |
| UIFrontendToolset | UI 前端 |
| MVCComponentToolset | 组件工具 |
| AIModuleToolset | AI 模块 |

## Requirement -> Plugin Map

| User Need | Required Plugin |
|-----------|-----------------|
| 创建/编辑蓝图 | MVCBlueprintToolset |
| 创建关卡 | EditorAppToolset (built-in) |
| 创建/编辑静态网格 | StaticMeshToolset |
| 编辑器脚本/Python | EditorScriptingToolset |
| 几何体操作 | GeometryScriptingToolset |
| 建模 | ModelingModeToolset |
| PCG 程序化生成 | PCGToolset |
| 动画 | AnimationAssistantToolset |
| AI 技能管理 | AgentSkillToolset (built-in) |
| 材质创建/编辑 | editor_toolset.toolsets.material (MaterialTools plugin) |