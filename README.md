# UnrealAgentLink

<p align="center">
  <strong>Bridge plugin that connects Unreal Engine Editor to <a href="https://uebox.ai">Unreal Box</a> AI Agent.</strong>
</p>

<p align="center">
  <img alt="Version" src="https://img.shields.io/badge/version-1.2.7-blue" />
  <img alt="UE" src="https://img.shields.io/badge/Unreal%20Engine-5.0%20~%205.7-blueviolet" />
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green" />
</p>

---

## Overview

**UnrealAgentLink** is an Editor-only plugin that enables [Unreal Box](https://uebox.ai) to communicate with the Unreal Engine Editor in real time. It exposes a set of command handlers over a local WebSocket connection, allowing an external AI Agent to inspect, create, and modify assets, actors, materials, widgets, and more — all without leaving the editor.

## Features

| Module | Capabilities |
|--------|-------------|
| **Actor Commands** | Spawn, transform, query, and delete actors in the level |
| **Blueprint Commands** | Create and modify Blueprints programmatically |
| **Content Browser Commands** | Browse, import, and manage assets |
| **Editor Commands** | Control editor viewport, play mode, and general editor state |
| **Level Commands** | Load, save, and manipulate levels and sub-levels |
| **Material Commands** | Create and edit materials and material instances |
| **Widget Commands** | Generate and modify UMG widgets and widget Blueprints |
| **System Commands** | Query engine/project info, execute console commands |

## Architecture

```
┌─────────────────────┐         WebSocket          ┌──────────────────┐
│   Unreal Box App    │ ◄──────────────────────────►│ UnrealAgentLink  │
│   (AI Agent)        │        localhost            │ (UE Editor)      │
└─────────────────────┘                             └──────────────────┘
                                                           │
                                                    ┌──────┴──────┐
                                                    │ Command     │
                                                    │ Handlers    │
                                                    ├─────────────┤
                                                    │ Actor       │
                                                    │ Blueprint   │
                                                    │ Content     │
                                                    │ Editor      │
                                                    │ Level       │
                                                    │ Material    │
                                                    │ Widget      │
                                                    │ System      │
                                                    └─────────────┘
```

## Requirements

- **Unreal Engine** 5.0 – 5.7
- **Python Script Plugin** (bundled with UE, enabled automatically)
- **Unreal Box** desktop app — [uebox.ai](https://uebox.ai)

## Installation

The plugin is automatically installed and managed by the **Unreal Box** application. No manual setup is required.

If you prefer manual installation:

1. Copy the `UnrealAgentLink` folder into your project's `Plugins/` directory.
2. Regenerate project files and open the project in Unreal Editor.
3. Enable the plugin via **Edit → Plugins → UnrealAgentLink**.

## Project Structure

```
UnrealAgentLink/
├── Config/                  # Plugin configuration
├── Content/                 # Plugin content assets
├── Resources/               # Icons and resources
├── Source/
│   └── UnrealAgentLink/
│       ├── Private/
│       │   ├── Commands/    # Command handler implementations
│       │   ├── Core/        # Plugin module & lifecycle
│       │   ├── Extensions/  # Editor extensions
│       │   ├── Network/     # WebSocket server
│       │   └── Utils/       # Utility helpers
│       ├── Public/          # Public headers
│       └── UnrealAgentLink.Build.cs
└── UnrealAgentLink.uplugin
```

## How It Works

1. When the plugin loads, it starts a lightweight **WebSocket server** on a local port.
2. The **Unreal Box** desktop app discovers and connects to the editor instance.
3. The AI Agent sends JSON command messages over the WebSocket.
4. The appropriate **Command Handler** executes the operation inside the editor and returns the result.

## License

MIT License — see [LICENSE](LICENSE) for details.

## Links

- 🌐 Website: [uebox.ai](https://uebox.ai)
- 🐛 Issues: [GitHub Issues](https://github.com/ueboxai/unreal-agent-link/issues)
