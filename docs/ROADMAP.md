# Port OS Roadmap

## Completed foundation

- C++23 layered kernel and native desktop.
- Sandboxed filesystem with Trash Bin and audit log.
- Structured command plans and approval gates.
- Gemini provider, context engine, offline fallback, and asynchronous UI flow.
- Session history and automated kernel/security/AI tests.

## Next: desktop stabilization

- Split the large desktop source into focused UI modules.
- Add automated Win32 interaction tests for selection, drag, DPI, and resize.
- Add cancellation and visible timeout/error details for AI requests.
- Embed or install image assets instead of relying on the working directory.

## Next: controlled tools

- Replace URLMon downloads with a cancellable client and progress reporting.
- Add signed/hash-verified package manifests.
- Add a configurable program allowlist and stronger process isolation.
- Add provider integration tests against a local mock HTTP server.

## Later research

- Evaluate a QEMU or Hyper-V guest backend.
- Route high-risk execution into the guest environment.
- Research bootloader and bare-metal kernel experiments separately from the
  Windows virtual desktop product.
