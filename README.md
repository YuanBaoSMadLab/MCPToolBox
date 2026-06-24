# MCPToolBox

An Unreal Engine 5.8 plugin that integrates Large Language Models (LLM) with the editor through the Model Context Protocol (MCP).

## Features

- **AI-Powered Assistance**: Integrate LLMs directly into the Unreal Engine editor
- **Tool Calling**: Support for custom tools including screenshot capture, file operations, and more
- **Visual Mode**: Toggle visual capabilities for image input/output
- **Session Management**: Organize conversations by project with persistent storage
- **Skill System**: Extensible plugin architecture for custom skills
- **Memory System**: Automatic context injection from memory files

## Requirements

- Unreal Engine 5.8 or later
- Windows 10/11
- Internet connection for LLM API access

## Installation

1. Clone this repository to your project's `Plugins/` directory
2. Enable the plugin in Unreal Engine's Plugin Manager
3. Build the project

## Usage

1. Open the MCP Toolbox from the Window menu
2. Configure your API settings in the settings panel
3. Start conversing with the AI assistant

## Project Structure

```
MCPToolbox/
├── Config/                  # Plugin configuration
├── Source/
│   ├── MCPToolbox/          # Main plugin code
│   │   ├── Private/         # Implementation files
│   │   └── Public/          # Header files
│   └── MCPToolboxScreenshot/ # Screenshot tool
├── ThirdParty/              # Third-party libraries
└── MCPToolbox.uplugin       # Plugin descriptor
```

## Key Components

- **MCPToolboxChatWidget**: Main chat interface
- **MCPToolboxMCPClient**: MCP protocol client
- **MCPToolboxAPIManager**: API request management
- **MCPToolboxMemoryManager**: Memory system
- **MCPToolboxChatSession**: Session management

## License

Apache License 2.0

## Contributing

Contributions are welcome! Please submit pull requests to the main branch.