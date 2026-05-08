# QuickJS VS Code DAP demo extension

This is a small in-repo VS Code extension that exposes QuickJS as a normal VS Code debugger using the existing `qjs --dap=tcp:PORT` support.

It is intentionally **demo-first**:

- no build step
- plain JavaScript extension entrypoint
- supports both **launch** and **attach**

## What it gives you

Once loaded into VS Code, the extension contributes a `qjs-dap` debug type so you can:

- launch a local script through a DAP-enabled `qjs`
- attach to an already-running QuickJS DAP TCP port
- use VS Code’s normal debugger UI for breakpoints, stepping, stack traces, scopes, and variable inspection

## Prerequisites

Build a DAP-enabled QuickJS binary first. A convenient default is:

```bash
cmake -B build-shared -DQJS_ENABLE_DAP=ON -DBUILD_SHARED_LIBS=ON
cmake --build build-shared --target qjs_exe
```

The sample launch config below assumes the runtime path is:

```text
${workspaceFolder}/build-shared/qjs
```

## Running the extension locally

1. Open this repository in VS Code.
2. Open the folder `tools/vscode-qjs-dap/`.
3. Press `F5` to launch an Extension Development Host.
4. In the extension host window, open a JavaScript file from the repo (or any workspace).
5. Create a `.vscode/launch.json` using one of the examples below.
6. Start debugging with the new **QuickJS DAP** configurations.

## Example `launch.json`

### Launch

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "QuickJS: Launch current file",
      "type": "qjs-dap",
      "request": "launch",
      "program": "${file}",
      "runtime": "${workspaceFolder}/build-shared/qjs",
      "cwd": "${workspaceFolder}",
      "stopOnEntry": true
    }
  ]
}
```

### Attach

Start QuickJS yourself first:

```bash
./build-shared/qjs --dap=tcp:4711
```

Then use:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "QuickJS: Attach to TCP port",
      "type": "qjs-dap",
      "request": "attach",
      "host": "127.0.0.1",
      "port": 4711
    }
  ]
}
```

The attach flow is meant for reconnecting to a **running** DAP session. If the target has not started execution yet, QuickJS will reject the attach request and tell you to use `launch`.

## Notes

- Launch mode spawns QuickJS with `--dap=tcp:PORT`.
- Attach mode connects VS Code to an existing QuickJS TCP DAP endpoint.
- If you set `"dapLog": "/tmp/qjs-dap.log"` in a launch config, the extension passes it through as `--dap-log=/tmp/qjs-dap.log`.
