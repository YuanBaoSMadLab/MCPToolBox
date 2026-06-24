# MCPToolBox

一个通过模型上下文协议 (MCP) 将大型语言模型 (LLM) 集成到 Unreal Engine 编辑器中的插件。

## 功能特性

- **AI 辅助**: 将 LLM 直接集成到 Unreal Engine 编辑器中
- **工具调用**: 支持截图、文件操作等自定义工具
- **视觉模式**: 切换图像输入/输出的视觉功能
- **会话管理**: 按项目组织对话，支持持久化存储
- **技能系统**: 可扩展的插件架构，支持自定义技能
- **记忆系统**: 从记忆文件自动注入上下文

## 系统要求

- Unreal Engine 5.8 或更高版本
- Windows 10/11
- 访问 LLM API 需要互联网连接

## 安装步骤

1. 将此仓库克隆到项目的 `Plugins/` 目录
2. 在 Unreal Engine 的插件管理器中启用插件
3. 构建项目

## 使用方法

1. 从 Window 菜单打开 MCP Toolbox
2. 在设置面板中配置 API 设置
3. 开始与 AI 助手对话

## 项目结构

```
MCPToolbox/
├── Config/                  # 插件配置文件
├── Source/
│   ├── MCPToolbox/          # 主插件代码
│   │   ├── Private/         # 实现文件
│   │   └── Public/          # 头文件
│   └── MCPToolboxScreenshot/ # 截图工具
├── ThirdParty/              # 第三方库
└── MCPToolbox.uplugin       # 插件描述文件
```

## 主要组件

- **MCPToolboxChatWidget**: 主聊天界面
- **MCPToolboxMCPClient**: MCP 协议客户端
- **MCPToolboxAPIManager**: API 请求管理
- **MCPToolboxMemoryManager**: 记忆系统
- **MCPToolboxChatSession**: 会话管理

## 许可证

MIT License

## 贡献

欢迎贡献代码！请向 main 分支提交 pull requests。