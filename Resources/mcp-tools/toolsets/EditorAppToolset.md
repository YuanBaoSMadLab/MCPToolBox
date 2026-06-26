# EditorToolset.EditorAppToolset

21 tools

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