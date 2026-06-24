# 🌌 Port OS (Open-Source)

[![C++](https://img.shields.io/badge/Language-C%2B%2B11-blue.svg?style=for-the-badge&logo=c%2B%2B)](https://en.wikipedia.org/wiki/C%2B%2B11)
[![Platform](https://img.shields.io/badge/Platform-Windows%20Win32-0078D6.svg?style=for-the-badge&logo=windows)](https://en.wikipedia.org/wiki/Windows_API)
[![Graphics](https://img.shields.io/badge/Graphics-GDI%2B-brightgreen.svg?style=for-the-badge)](https://en.wikipedia.org/wiki/GDI%2B)
[![AI Engine](https://img.shields.io/badge/AI%20Engine-Super--Nova%201.0-orange.svg?style=for-the-badge)](https://github.com/google-gemini)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

**Port OS** is an experimental, **AI-Native virtual operating system application** built entirely from scratch in C++. Powered by a **robotic AI kernel** running the **Super-Nova 1.0 engine**, Port OS transforms natural language prompts into autonomous system actions within a secure, sandboxed desktop environment.

Rather than being a traditional bare-metal operating system, Port OS runs as a high-performance virtual desktop environment on top of the Windows host. It is designed to demonstrate the future of **AI-first computer interfaces**, where the user interacts with the OS through a conversational search and command bar, and the robotic kernel handles the rest.

---

## 🚀 Key Architectural Pillars

### 🤖 Robotic AI Kernel (Super-Nova 1.0 Engine)
* **Autonomous Task Planning**: Converts natural language prompts (e.g., *"create a folder named Projects and download the source code"*) into step-by-step shell and filesystem operations.
* **Live System Execution**: The robotic kernel runs commands in real-time, modifying the sandboxed filesystem dynamically.
* **Gemini API Integration**: Built-in support for the Google Gemini API via native Windows internet sockets (`WinINet`). It loads API keys dynamically from local environment variables or `gemini_key.txt`.

### 🖥️ Premium GDI+ Virtual Desktop
* **Glassmorphic Aesthetics**: Designed with a premium, sleek dark-mode aesthetic, transparent window controls, and modern typography using high-quality anti-aliased **Segoe UI ClearType** fonts.
* **Butter-Smooth Interactions**: Double-buffered memory device contexts (`memDC`) ensure 100% flicker-free desktop drag-selection and icon movement.
* **Smart Grid & Dynamic Folders**: The desktop automatically scans the sandboxed `sandbox/Desktop/` directory and renders real-time folder icons. Creating or deleting folders in the sandbox updates the UI instantly without needing a manual refresh.
* **Live Trash Bin**: Features a custom right-click context menu (*Empty Trash*, *Rename*, *Properties*) and dynamically changes its icon state (empty/full) based on sandbox contents.

### 🔒 Secured Sandboxed Filesystem
* All operations executed by the AI kernel or the user are safely contained inside a localized `sandbox/` directory, preventing any unwanted modifications to the host system.

---

## 📂 Repository Structure

```text
├── src/
│   ├── kernel/           # AI Kernel Core (Super-Nova 1.0 implementation)
│   │   ├── ai_client.cpp # Google Gemini API client & offline fallback plan generator
│   │   └── ai_kernel.cpp # Command execution routing and state machine
│   ├── desktop/          # Win32 & GDI+ graphical desktop environment
│   │   └── main.cpp      # Window procedures, double-buffered rendering, subclassed controls
│   └── terminal/         # FOX-terminal console interface
├── include/
│   └── fox/              # Public headers (ai_kernel, ai_client, command_router)
├── sandbox/              # Local sandboxed virtual drive (created on boot)
├── CMakeLists.txt        # Build configuration file
└── README.md             # Project documentation
```

---

## 🛠️ Build and Compilation Guide

Port OS is extremely lightweight and has **zero external package dependencies** other than standard Windows system libraries.

### Requirements
* **Operating System**: Windows 10 / 11
* **Compiler**: A C++11 compliant compiler (MSVC, MinGW, or Clang)
* **Build System**: CMake 3.20 or newer

### Building the Project

1. Open your terminal (PowerShell, Command Prompt, or Git Bash) and navigate to the project directory.
2. Generate the build files and compile the project:

```powershell
# Configure the build directory
cmake -S . -B build

# Compile the executables in Release mode (recommended for maximum performance)
cmake --build build --config Release
```

### Running Port OS

After a successful build, you can launch the virtual desktop application:

```powershell
# Run the virtual desktop application
.\build\Release\port-desktop.exe
```

You can also run the command-line terminal interface:

```powershell
# Run the terminal interface
.\build\Release\fox-terminal.exe
```

---

## 🔑 Activating the AI Kernel (Google Gemini)

To unlock the full potential of the **Super-Nova 1.0** robotic kernel, you can hook it up to the Gemini API:

1. Obtain an API key from the Google AI Studio.
2. Place the key in a file named `gemini_key.txt` in the root directory of the project, or set it as an environment variable:
   ```powershell
   $env:GEMINI_API_KEY="your_api_key_here"
   ```
3. If no API key is provided, the kernel automatically falls back to its highly optimized **offline plan compiler**, allowing you to use basic system commands without an internet connection.

---

## 🤝 Contributing & Star the Repo!

Port OS is an open-source project. If you love the idea of an **AI-Native Operating System** built from scratch in pure C++, feel free to fork the repository, submit pull requests, or open issues.

**Don't forget to star ⭐ this repository if you find it interesting!**
