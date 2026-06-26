## 已加载 MCP 工具 — 快速索引

> 生成时间: 2026-06-26 11:30:56

### 顶层工具

- `list_toolsets` — List all available toolsets with names and descriptions.
- `describe_toolset` — Get detailed information about a toolset including all tool names, descriptions, and input schemas.
- `call_tool` — Call a tool by name. Provide toolset_name to call a toolset tool, or omit it to call a top-level MCP tool. Use list_toolsets and describe_toolset to discover available tools and their input schemas.

- **EditorToolset.EditorAppToolset**: WorldPosToScreenCoords, StopPIE, StartPIE, SetContentBrowserPath, SetCameraTransform, SelectAssets, SelectActors, SearchCVars, ScreenCoordsToWorld, OpenEditorForAsset, IsPIERunning, GetVisibleActors, GetSelectedAssets, GetSelectedActors, GetOpenAssets, GetContentBrowserPath, GetCameraTransform, FocusOnActors, CaptureViewport, CaptureEditorImage, CaptureAssetImage
- **EditorToolset.LogsToolset**: SetVerbosity, GetVerbosity, GetLogEntries, GetLogCategories
- **GameFeaturesToolset.GameFeaturesToolset**: RequestDeactivateGameFeature, RequestActivateGameFeature, ListEnabledGameFeaturePlugins, ListDiscoveredGameFeaturePlugins, IsGameFeaturePlugin, IsGameFeatureActive, GetGameFeatureState
- **PhysicsToolsets.PhysicsAssetToolset**: SetSphere, SetConstraintLimits, SetCapsule, SetBox, SetBodyPhysicsMode, SetBodyMassScale, RemoveShape, RemoveConstraint, RemoveBody, GetConstraints, GetBodyShapes, GetBodyPhysicsMode, GetBodyNames, GetBodyMassScale, CreateFromMesh, AddConstraint, AddBody
- **ToolsetRegistry.AgentSkillToolset**: UpdateSkill, ListSkills, GetSkills, CreateSkill
- **editor_toolset.toolsets.programmatic.ProgrammaticToolset**: execute_tool_script, get_execution_environment

### 调用方式

```
call_tool(toolset_name="ToolsetName", tool_name="ToolName", arguments={})
```

### 已知未加载工具集 (启用插件后可用)

| 插件 | 功能 |
|------|------|
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
