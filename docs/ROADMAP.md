# Port OS Roadmap

## Milestone 1: Virtual Kernel Skeleton

- Create a C/C++ project structure.
- Build FOX-terminal as a command-line app.
- Add an AI Kernel interface.
- Add command parsing for `help`, `status`, `ask`, and `exit`.
- Add `/port ans-install-fd` as the first foundation setup command.
- Add Port Desktop as the first graphical virtual environment.
- Document the architecture.

## Milestone 2: Sandboxed Workspace

- Add a sandbox directory.
- Add safe file read/write/list operations.
- Log every operation requested by the AI kernel.
- Require confirmation for destructive actions.

## Milestone 3: AI Planning Layer

- Connect an AI model provider.
- Convert user prompts into structured task plans.
- Add approval gates for commands.
- Add persistent session history.

## Milestone 4: Network Tools

- Add controlled web search.
- Add controlled file downloads.
- Add source citation logs.

## Milestone 5: VM Integration

- Evaluate QEMU and VirtualBox backends.
- Boot a minimal guest environment.
- Route FOX-terminal operations into the guest sandbox.

## Milestone 6: Real Kernel Research

- Decide whether the real kernel path should use C, C++, Rust, or a hybrid.
- Build bootloader experiments.
- Build minimal memory, process, and driver prototypes.
