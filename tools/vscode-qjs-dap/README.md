# QuickJS VS Code DAP demo extension

This is a small in-repo VS Code extension that exposes QuickJS as a normal VS Code debugger using the existing `qjs --dap=tcp:PORT` support.

It is intentionally **demo-first**:

- minimal build/setup
- plain JavaScript extension entrypoint
- supports both **launch** and **attach**

## What it gives you

Once loaded into VS Code, the extension contributes a `qjs-dap` debug type so you can:

- launch a local script through a DAP-enabled `qjs`
- attach to an already-running QuickJS DAP TCP port
- use VS Code’s normal debugger UI for breakpoints, stepping, stack traces, scopes, and variable inspection

## Prerequisites

Launch mode can build and use the bundled repo runtime automatically. If you want to build it yourself first, a convenient default is:

```bash
cmake -B build-shared -DQJS_ENABLE_DAP=ON -DBUILD_SHARED_LIBS=ON
cmake --build build-shared --target qjs_exe
```

The bundled runtime path is:

```text
<repo-root>/build-shared/qjs
```

## Running the extension locally

The repo uses **two separate workspaces**:

- the **repository root** is for extension development
- `tools/vscode-qjs-dap/example-workspace/` is the demo consumer workspace

The repository root `.vscode/launch.json` now contains only the **Extension Host** launcher. The `qjs-dap` debug configurations live in the example workspace, where they actually make sense.

1. Open the **repository root** in VS Code.
2. Select **QuickJS DAP: Run Extension Host** in Run and Debug.
3. Press `F5`.
4. VS Code opens an Extension Development Host window rooted at `tools/vscode-qjs-dap/example-workspace/`.
5. In that new window, use the preconfigured **QuickJS: Launch demo script** or **QuickJS: Attach to TCP port** configurations.

The extension host window is the simulated “other repo” where a developer consumes the extension.

## Example `launch.json`

If you want to customize the demo configs, start from:

`tools/vscode-qjs-dap/example-workspace/.vscode/launch.json`

### Launch

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "QuickJS: Launch demo script",
      "type": "qjs-dap",
      "request": "launch",
      "program": "${workspaceFolder}/demo.js",
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

## Demo workspace contents

The example workspace includes:

- `demo.js` — a tiny breakpoint/step/scope-friendly sample script
- `.vscode/launch.json` — the `qjs-dap` launch and attach configurations used in the extension host window

## Notes

- Launch mode spawns QuickJS with `--dap=tcp:PORT`.
- If no `runtime` is specified, launch mode uses the bundled repo runtime and builds it automatically if needed.
- Attach mode connects VS Code to an existing QuickJS TCP DAP endpoint.
- If you set `"dapLog": "/tmp/qjs-dap.log"` in a launch config, the extension passes it through as `--dap-log=/tmp/qjs-dap.log`.
