# Port OS Architecture

Port OS starts as a virtual AI-powered operating environment.

The long-term idea is to build an operating system where users can control work
through prompts instead of manually navigating every application. The first
implementation is intentionally smaller: a terminal, an AI kernel layer, and a
sandboxed runtime.

## Main Components

### FOX-terminal

FOX-terminal is the first user interface. It receives commands and prompts from
the user, sends them to the AI kernel layer, and prints the response.

### Port Desktop

Port Desktop is the first graphical virtual operating environment. It starts as a
native desktop application with FOX Terminal, My Port, and Trash Bin entries.
The desktop uses the same AI Kernel core as the command-line terminal.

### AI Kernel

The AI Kernel is the core control layer. In the first milestone it parses
commands, returns status, and accepts prompts. Later it will:

- create task plans;
- request user approval for risky operations;
- control sandboxed file operations;
- connect to network search and download tools;
- communicate with a virtual machine backend.

### Sandbox

The sandbox will isolate files, processes, and network operations from the host
system. This is required before the AI layer can safely execute real tasks.

### Foundation Drivers

Foundation drivers are virtual runtime modules, not host operating system
drivers. The first command, `/port ans-install-fd`, installs these virtual
drivers inside the Port runtime:

- System Driver Registry
- Compiler Toolchain Detector
- Sandbox Filesystem Driver
- Network Access Controller
- AI Execution Policy Engine

### VM Backend

The VM backend is a later milestone. It may use QEMU, VirtualBox, or another
virtualization layer. The first version does not depend on a real VM.

## Language Strategy

- C and C++ are used for the first kernel core and terminal runtime.
- C++11 is used first so the project can build on older compilers.
- C can be added for lower-level modules when the project moves closer to a
  real operating system kernel.
- All source code, comments, documentation, and UI text must be written in
  English.

## First Milestone

The first milestone is a working local executable:

```text
FOX-terminal -> AI Kernel -> command response
```

This proves the command loop, project structure, and kernel API before adding
AI model integration or sandbox execution.
