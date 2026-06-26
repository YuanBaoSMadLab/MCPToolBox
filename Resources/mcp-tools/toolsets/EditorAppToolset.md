# EditorToolset.EditorAppToolset

21 个工具

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