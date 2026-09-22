# Port OS 2.0

**PORT Virtual Operating System — BUILD 1**

Port OS is an experimental AI-native desktop environment for Windows, built in
C++23 with Win32 and GDI+. It is a virtual operating environment, not a
bare-metal operating system.

The Super-Nova 2.0 kernel converts natural-language requests into a validated
plan of Port commands. Commands operate through a sandboxed filesystem and
protected actions require explicit user approval.

## Current features

- Native Win32/GDI+ desktop with draggable, selectable icons.
- My Port filesystem browser, Trash Bin, and local My Server view.
- Port Agents panel with a per-agent chat window and system prompt.
- Gemini provider with conversational context and offline fallback planning.
- Asynchronous AI requests so network work does not block the UI thread.
- Sandboxed file read, write, list, rename, directory, and trash operations.
- HTTPS downloads restricted to destinations inside the sandbox.
- Program execution restricted to approved `.exe` files inside the sandbox.
- Session history, audit logging, configuration, metrics, and automated tests.

## Architecture

```text
Port Desktop
    |
    v
AI Kernel ---- approval policy ---- audit/session logs
    |
    +---- Prompt Runtime ---- Context Engine ---- Gemini Provider
    |
    +---- Command Router ---- Sandboxed tools
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layer contract and the
security boundaries, and [docs/ROADMAP.md](docs/ROADMAP.md) for planned work.

## Project layout

```text
Port/
├── assets/              Images loaded at runtime
│   ├── agents/          Per-agent icons (agent-<name>.png)
│   ├── branding/        Wallpaper, app icon, dock logo
│   └── icons/           Desktop and file-browser icons
├── docs/                Architecture and roadmap
├── include/port/        Public headers, one folder per layer
│   ├── ai/  core/  kernel/  security/  storage/
├── src/                 Implementation, mirroring include/port
│   ├── ai/              Providers, context engine, prompt runtime, tools
│   ├── core/            Event bus, logger, config, metrics, services
│   ├── desktop/         Win32/GDI+ shell (entry point)
│   ├── kernel/          AI kernel, command router, foundation drivers
│   ├── security/        Sandbox and audit log
│   └── storage/         Session store
├── tests/               CTest suite, one executable per module
├── tools/               Developer scripts (asset generation)
├── CMakeLists.txt
└── LICENSE.txt
```

Everything else in the working tree — `build*/`, `sandbox/`, `tmp/`, logs and
key files — is generated at build or run time and is not tracked by Git.

## Requirements

- Windows 10 or 11
- CMake 3.20+
- Ninja or another CMake-supported build tool
- A C++23 compiler, such as current MSYS2 MinGW64 or MSVC 2022

## Build and run

```powershell
cmake -S . -B build-msys -G Ninja
cmake --build build-msys -j 4
.\build-msys\port-desktop.exe
```

The executable sets its working directory to the project root, so it finds
`assets/` whether it is started from the build directory or from a shortcut.

Run the test suite:

```powershell
ctest --test-dir build-msys --output-on-failure
```

With an MSYS2 toolchain the tests need the MinGW runtime DLLs on `PATH`:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
```

## Enable Gemini

The preferred configuration is an environment variable:

```powershell
$env:GEMINI_API_KEY="your-key"
$env:PORT_AI_MODEL="gemini-3.5-flash" # optional
.\build-msys\port-desktop.exe
```

For local development, a `gemini_key.txt` file is also supported. The file is
ignored by Git. If no key is available, Port OS uses its offline planner.

The AI may only emit the documented Port command grammar. Unknown output is
discarded, filesystem paths are checked against the sandbox, and write,
download, delete, empty-trash, and execute operations require approval.

## Project status

Port OS is an active prototype. The kernel, provider integration, sandbox,
approval flow, and desktop are functional. VM integration and bare-metal OS
research remain future work; see [docs/ROADMAP.md](docs/ROADMAP.md).

## License

MIT — see [LICENSE.txt](LICENSE.txt).
