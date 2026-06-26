# UE5 MCP 工具集目录

> 自动生成: 2026-06-26 11:09:03 | 工具集: 6 | 工具: 55

### EditorToolset.EditorAppToolset

| 工具名 | 说明 |
|--------|------|
| `WorldPosToScreenCoords` | 基于编辑器视口摄像机，将世界空间位置转换为归一化的屏幕空间坐标。 |
| `StopPIE` | 停止当前运行的游戏会话（PIE或模拟）。
如果没有正在运行的游玩会话，则会引发错误。 |
| `StartPIE` | 开始一个使用当前关卡的Play-In-Editor或Simulate-In-Editor会话。在引擎触发PostPIEStarted（会话已完全启动，
BeginPlay已调用）之后完成，并在 Options.WarmupSeconds经过后生效，从而在代理检查状态或记录日志之前，为项目
特定的初始化（如服务、认证、插件预热）
提供稳定的收尾时间。
如果游戏会话已在运行，则会引发错误。 |
| `SetContentBrowserPath` | 将活动的内容浏览器导航到指定的文件夹路径。 |
| `SetCameraTransform` | 设置关卡视口摄像机的位置和旋转。 |
| `SelectAssets` | 在内容浏览器中选择指定资产。
内容浏览器应用该选项后即为完成。 |
| `SelectActors` | 选择当前场景中的指定Actor。 |
| `SearchCVars` | 查找所有包含给定名称的控制台变量。 |
| `ScreenCoordsToWorld` | 在给定的归一化视图空间坐标处，找到最近实心Object的世界位置。 |
| `OpenEditorForAsset` | 为指定资产打开资产编辑器。 |
| `IsPIERunning` | 返回当前是否正在运行编辑器会话。 |
| `GetVisibleActors` | 返回当前关卡中边界与视口视锥体相交的所有Actor。 |
| `GetSelectedAssets` | 获取在内容浏览器中选择的资产列表。 |
| `GetSelectedActors` | 获取关卡编辑器中当前选定的Actor。 |
| `GetOpenAssets` | 获取当前在资产编辑器中打开的资产列表。 |
| `GetContentBrowserPath` | 获取激活的内容浏览器的当前路径。 |
| `GetCameraTransform` | 返回关卡视口摄像机的位置和旋转。 |
| `FocusOnActors` | 重新定位关卡编辑器摄像机，使其聚焦于指定Actor。
PIE激活时无法调用。 |
| `CaptureViewport` | 捕获带可选注释的关卡视口。

注释渲染在可见Actor上叠加一个投影的3D世界空间网格，
以及名称和位置标签。网格绘制在可配置的
地面平面Z处，并通过摄像机投影，交点处
有坐标数字（以米为单位显示）。每个带标签的Actor在其
投影的屏幕位置处得到一个准星，并放置一个引导线标注以避免重叠。这赋予了
具备视觉能力的代理空间感知能力：它可以引用网格
坐标来指导放置，并通过标签识别场景内容。 |
| `CaptureEditorImage` | 捕获整个编辑器应用程序的用户可见图像。 |
| `CaptureAssetImage` | 为指定资产渲染缩略图（例如静态网格体、骨骼网格体、
骨架、动画、蒙太奇、材质、纹理）。 |

### EditorToolset.LogsToolset

| 工具名 | 说明 |
|--------|------|
| `SetVerbosity` | 设置日志类别的冗长度级别。 |
| `GetVerbosity` | 返回日志类别的当前冗长度级别。 |
| `GetLogEntries` | 返回来自当前会话的日志文件的日志条目。 |
| `GetLogCategories` | 返回已注册日志类别的排序列表。 |

### GameFeaturesToolset.GameFeaturesToolset

| 工具名 | 说明 |
|--------|------|
| `RequestDeactivateGameFeature` | 请求停用游戏功能插件。
如果停用请求成功提交，则返回true。实际的停用
是异步发生的——轮询GetGameFeatureState()以确认完成。
如果子系统不可用或未找到插件，则会引发错误。 |
| `RequestActivateGameFeature` | 请求激活游戏功能插件。
如果激活请求成功提交，则返回true。实际激活是异步
发生的——通过轮询GetGameFeatureState()或IsGameFeatureActive()来确认完成。如果子系统不可用或未找到插件，
则会引发错误。 |
| `ListEnabledGameFeaturePlugins` | 列出所有已启用的游戏功能插件，按名称排序。除了判断一个插件是否为
游戏功能插件之外，已启用的插件是游戏功能系统唯一了解的插件。
使用插件工具集执行常规插件启用/禁用任务。 |
| `ListDiscoveredGameFeaturePlugins` | 列出所有已发现的游戏功能插件，按名称排序。这包括已启用和已禁用的
插件。除了判断一个插件是否为
游戏功能插件之外，游戏功能系统只了解已启用的插件。
使用插件工具集执行常规插件启用/禁用任务。 |
| `IsGameFeaturePlugin` | 返回插件是否为游戏功能插件。如果插件管理器找不到此
名称的插件，将会报错。 |
| `IsGameFeatureActive` | 检查游戏功能插件是否处于激活状态。如果子系统不可用或未找到插件，
则会引发错误。
如果需要在插件未激活时获取当前状态，请使用GetGameFeatureState。 |
| `GetGameFeatureState` | 获取游戏功能插件的当前状态。 |

### PhysicsToolsets.PhysicsAssetToolset

| 工具名 | 说明 |
|--------|------|
| `SetSphere` | 在形体上添加或替换Sphere碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。 |
| `SetConstraintLimits` | 更新现有约束的角限制。 |
| `SetCapsule` | 在形体上添加或替换胶囊体碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。
应用旋转之后，胶囊体的长轴是其局部Z轴。 |
| `SetBox` | 在形体上添加或替换盒体碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。 |
| `SetBodyPhysicsMode` | 设置给定形体的物理模拟模式。 |
| `SetBodyMassScale` | 设置给定形体的质量比例乘数。 |
| `RemoveShape` | 按名称从形体中移除碰撞图元。 |
| `RemoveConstraint` | 移除两个形体之间的约束。 |
| `RemoveBody` | 移除给定骨骼的形体以及引用它的所有约束。
如果PhysicsAsset为空或BoneName不存在形体，则引发脚本错误。 |
| `GetConstraints` | 返回物理资产中的所有约束及其当前角限制。 |
| `GetBodyShapes` | 返回分配给形体的所有碰撞形状。 |
| `GetBodyPhysicsMode` | 返回给定形体的物理模拟模式。 |
| `GetBodyNames` | 返回物理资产中每个刚体的骨骼名称。 |
| `GetBodyMassScale` | 返回给定形体的质量缩放倍数。 |
| `CreateFromMesh` | 从骨骼网格体创建物理资产，为每个骨骼自动生成
碰撞体。资产放置在与网格体相同的文件夹中，后缀为
“_PhysicsAsset”。 |
| `AddConstraint` | 在两个形体之间添加一个新约束。两个形体必须已经存在。 |
| `AddBody` | 为给定骨骼添加一个新的空形体。 |

### ToolsetRegistry.AgentSkillToolset

| 工具名 | 说明 |
|--------|------|
| `UpdateSkill` | 更新现有AgentSkill。
这应该只在获得用户的明确指示或许可后才调用。 |
| `ListSkills` | 获取项目中所有AgentSkills的摘要。 |
| `GetSkills` | 返回关于特定AgentSkills集的详细信息。 |
| `CreateSkill` | 创建一个新的AgentSkill。
这应该只在获得用户的明确指示或许可后才调用。 |

### editor_toolset.toolsets.programmatic.ProgrammaticToolset

| 工具名 | 说明 |
|--------|------|
| `execute_tool_script` | Execute a Python script against the toolset APIs.

        Use this to batch multiple tool calls into a single script execution,
        reducing round-trips and context usage.

        IMPORTANT: Available modules and usage instructions are described by the
        value returned by `get_execution_environment`. You MUST call
        `get_execution_environment` once in the conversation before using this
        tool. Read the value in the `instructions` field in the returned
        environment info prior to calling this function, so that you understand
        what APIs are available and how to use them.

        Before writing a script that calls multiple tools, look up the output
        schemas (if available) for any tools you plan to use. This returns the
        JSON schema describing each tool's return value, so you know how to
        parse results and pass data between calls.

        Args:
            script: Python script to execute. Must define a `run()` function
                that returns a `Dict[str, Any]`.

        Returns:
            JSON-encoded dict returned by the script's `run()` function.

        Raises:
            SyntaxError: If the script has invalid syntax.
            ValueError: If the script imports a disallowed module or does not
                define a `run()` function.
            TypeError: If `run()` does not return a dict.
            Exception: Any unhandled exception raised by the script. |
| `get_execution_environment` | Get details about execution environment.

        This includes instructions on how to write scripts, and constraints,
        such as what modules may be imported and the script entrypoint and
        function signature.

        Returns:
            Returns the current execution environment. |
