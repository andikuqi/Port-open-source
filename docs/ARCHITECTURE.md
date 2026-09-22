# Port OS 2.0 Architecture

Port OS is a layered Windows desktop environment written in C++23.

## Runtime flow

1. Port Desktop receives a command or natural-language prompt.
2. Explicit commands go to the Command Router and AI Kernel.
3. Natural language runs asynchronously through Prompt Runtime.
4. Context Engine supplies conversation history to the Gemini provider.
5. Provider output is filtered to the supported Port command grammar.
6. The kernel pauses protected steps for approval.
7. Approved tools execute through the sandbox and write audit/session logs.

## Layers

- `core`: events, logging, configuration, metrics, and services.
- `security`: sandbox path policy and audit log.
- `storage`: persistent session history.
- `ai`: providers, context, prompt runtime, and tool definitions.
- `kernel`: plans, approval state, routing, and foundation drivers.
- `desktop`: Win32/GDI+ shell and user interaction.

## Security boundaries

- Relative filesystem paths are resolved beneath the sandbox root.
- Existing ancestors are resolved to their real Windows paths to block
  symlink/reparse-point escapes.
- Downloads require HTTPS and always target a sandbox path.
- Program execution is limited to `.exe` files already inside the sandbox.
- Protected actions require explicit approval and are audited after execution.

Port OS is not a security boundary against a hostile Windows process and does
not yet use a VM or operating-system-level process isolation.
