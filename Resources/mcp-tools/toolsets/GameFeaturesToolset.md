# GameFeaturesToolset.GameFeaturesToolset

7 个工具

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