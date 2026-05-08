const childProcess = require("child_process");
const fs = require("fs");
const net = require("net");
const path = require("path");

function repoRootPath() {
  return path.resolve(__dirname, "..", "..");
}

function bundledBuildDirectory() {
  return path.join(repoRootPath(), "build-shared");
}

function allocatePort(host = "127.0.0.1") {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once("error", reject);
    server.listen(0, host, () => {
      const address = server.address();
      server.close((closeError) => {
        if (closeError) {
          reject(closeError);
          return;
        }
        resolve(address.port);
      });
    });
  });
}

function defaultRuntimePath() {
  return path.join(bundledBuildDirectory(), "qjs");
}

function normalizeEnv(extraEnv) {
  if (!extraEnv || typeof extraEnv !== "object") {
    return { ...process.env };
  }
  const merged = { ...process.env };
  for (const [key, value] of Object.entries(extraEnv)) {
    if (value === undefined || value === null) {
      continue;
    }
    merged[key] = String(value);
  }
  return merged;
}

function waitForServer({ host, port, child, timeoutMs = 5000 }) {
  const start = Date.now();

  return new Promise((resolve, reject) => {
    let settled = false;

    function finish(err) {
      if (settled) {
        return;
      }
      settled = true;
      child.off("exit", onExit);
      child.off("error", onError);
      if (err) {
        reject(err);
      } else {
        resolve();
      }
    }

    function onExit(code, signal) {
      finish(new Error(`QuickJS exited before the DAP server was ready (code=${code}, signal=${signal})`));
    }

    function onError(error) {
      finish(error);
    }

    function tryConnect() {
      if (Date.now() - start > timeoutMs) {
        finish(new Error(`Timed out waiting for QuickJS DAP server on ${host}:${port}`));
        return;
      }

      const socket = net.connect({ host, port });
      socket.once("connect", () => {
        socket.end();
        socket.destroy();
        finish();
      });
      socket.once("error", () => {
        socket.destroy();
        setTimeout(tryConnect, 50);
      });
    }

    child.once("exit", onExit);
    child.once("error", onError);
    tryConnect();
  });
}

function runCommand(command, args, options, outputChannel) {
  return new Promise((resolve, reject) => {
    const child = childProcess.spawn(command, args, {
      cwd: options.cwd,
      env: options.env || process.env,
      stdio: ["ignore", "pipe", "pipe"]
    });

    if (outputChannel) {
      outputChannel.appendLine(`[quickjs-dap] run: ${command} ${args.join(" ")}`);
      child.stdout.on("data", (chunk) => outputChannel.append(chunk.toString()));
      child.stderr.on("data", (chunk) => outputChannel.append(chunk.toString()));
    }

    child.once("error", reject);
    child.once("exit", (code, signal) => {
      if (code === 0) {
        resolve();
        return;
      }
      reject(new Error(`${command} failed (code=${code}, signal=${signal})`));
    });
  });
}

async function ensureBundledRuntime(runtimePath, outputChannel) {
  if (fs.existsSync(runtimePath)) {
    return runtimePath;
  }

  const repoRoot = repoRootPath();
  const buildDir = bundledBuildDirectory();
  const env = normalizeEnv();

  if (outputChannel) {
    outputChannel.appendLine(`[quickjs-dap] bundled runtime missing, building ${runtimePath}`);
  }

  await runCommand(
    "cmake",
    ["-S", repoRoot, "-B", buildDir, "-DQJS_ENABLE_DAP=ON", "-DBUILD_SHARED_LIBS=ON"],
    { cwd: repoRoot, env },
    outputChannel
  );
  await runCommand(
    "cmake",
    ["--build", buildDir, "--target", "qjs_exe", "--parallel"],
    { cwd: repoRoot, env },
    outputChannel
  );

  if (!fs.existsSync(runtimePath)) {
    throw new Error(`QuickJS DAP runtime was built but not found at ${runtimePath}`);
  }
  return runtimePath;
}

function spawnQuickJSProcess({ runtime, runtimeArgs, host, port, cwd, dapLog, env, outputChannel }) {
  const args = [...runtimeArgs, `--dap=tcp:${port}`];
  if (dapLog) {
    args.push(`--dap-log=${dapLog}`);
  }

  if (outputChannel) {
    outputChannel.appendLine(`[quickjs-dap] spawn: ${runtime} ${args.join(" ")}`);
  }

  const child = childProcess.spawn(runtime, args, {
    cwd,
    env: normalizeEnv(env),
    stdio: ["ignore", "pipe", "pipe"]
  });

  if (outputChannel) {
    child.stdout.on("data", (chunk) => {
      outputChannel.append(chunk.toString());
    });
    child.stderr.on("data", (chunk) => {
      outputChannel.append(chunk.toString());
    });
    child.on("exit", (code, signal) => {
      outputChannel.appendLine(`[quickjs-dap] exit: code=${code} signal=${signal}`);
    });
  }

  return child;
}

class QuickJSDebugConfigurationProvider {
  constructor(vscodeApi) {
    this.vscode = vscodeApi;
  }

  provideDebugConfigurations(folder) {
    return [
      {
        name: "QuickJS: Launch current file",
        type: "qjs-dap",
        request: "launch",
        program: "${file}",
        runtime: defaultRuntimePath(),
        cwd: folder ? folder.uri.fsPath : "${workspaceFolder}",
        stopOnEntry: true
      },
      {
        name: "QuickJS: Attach to TCP port",
        type: "qjs-dap",
        request: "attach",
        host: "127.0.0.1",
        port: 4711
      }
    ];
  }

  resolveDebugConfiguration(folder, config) {
    if (!config.type && !config.request && !config.name) {
      const activeEditor = this.vscode.window.activeTextEditor;
      if (activeEditor) {
        config.type = "qjs-dap";
        config.request = "launch";
        config.name = "QuickJS: Launch current file";
        config.program = activeEditor.document.uri.fsPath;
      }
    }

    if (config.type !== "qjs-dap") {
      return config;
    }

    if (!config.request) {
      config.request = "launch";
    }

    if (config.request === "launch") {
      if (!config.program) {
        const activeEditor = this.vscode.window.activeTextEditor;
        if (activeEditor) {
          config.program = activeEditor.document.uri.fsPath;
        }
      }
      if (!config.program) {
        this.vscode.window.showErrorMessage("QuickJS launch requires a 'program' file.");
        return null;
      }
      config.runtime = config.runtime || defaultRuntimePath();
      config.cwd = config.cwd || path.dirname(config.program);
      config.host = config.host || "127.0.0.1";
      config.runtimeArgs = Array.isArray(config.runtimeArgs) ? config.runtimeArgs : [];
      config.args = Array.isArray(config.args) ? config.args : [];
      return config;
    }

    if (config.request === "attach") {
      config.host = config.host || "127.0.0.1";
      if (!config.port) {
        this.vscode.window.showErrorMessage("QuickJS attach requires a TCP 'port'.");
        return null;
      }
      return config;
    }

    this.vscode.window.showErrorMessage(`Unsupported QuickJS debug request '${config.request}'.`);
    return null;
  }
}

class QuickJSDebugAdapterDescriptorFactory {
  constructor(vscodeApi, outputChannel) {
    this.vscode = vscodeApi;
    this.outputChannel = outputChannel;
    this.sessions = new Map();
  }

  async createDebugAdapterDescriptor(session) {
    if (session.configuration.request === "attach") {
      return new this.vscode.DebugAdapterServer(session.configuration.port, session.configuration.host || "127.0.0.1");
    }

    const host = session.configuration.host || "127.0.0.1";
    const port = await allocatePort(host);
    const runtime = session.configuration.runtime || defaultRuntimePath();
    const resolvedRuntime = session.configuration.runtime ? runtime : await ensureBundledRuntime(runtime, this.outputChannel);
    const child = spawnQuickJSProcess({
      runtime: resolvedRuntime,
      runtimeArgs: session.configuration.runtimeArgs || [],
      host,
      port,
      cwd: session.configuration.cwd || (session.workspaceFolder ? session.workspaceFolder.uri.fsPath : undefined),
      dapLog: session.configuration.dapLog,
      env: session.configuration.env,
      outputChannel: this.outputChannel
    });

    this.sessions.set(session.id, { child, ownsProcess: true });

    try {
      await waitForServer({ host, port, child });
    } catch (error) {
      this.stopSession(session.id);
      this.vscode.window.showErrorMessage(error.message);
      throw error;
    }

    return new this.vscode.DebugAdapterServer(port, host);
  }

  stopSession(sessionId) {
    const entry = this.sessions.get(sessionId);
    if (!entry) {
      return;
    }
    this.sessions.delete(sessionId);
    if (!entry.ownsProcess || !entry.child || entry.child.exitCode !== null || entry.child.killed) {
      return;
    }
    entry.child.kill("SIGTERM");
    setTimeout(() => {
      if (entry.child.exitCode === null) {
        entry.child.kill("SIGKILL");
      }
    }, 1000);
  }

  dispose() {
    for (const sessionId of this.sessions.keys()) {
      this.stopSession(sessionId);
    }
  }
}

function activate(context) {
  const vscode = require("vscode");
  const outputChannel = vscode.window.createOutputChannel("QuickJS DAP");
  const provider = new QuickJSDebugConfigurationProvider(vscode);
  const factory = new QuickJSDebugAdapterDescriptorFactory(vscode, outputChannel);

  context.subscriptions.push(outputChannel);
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider(
      "qjs-dap",
      provider,
      vscode.DebugConfigurationProviderTriggerKind.Dynamic
    )
  );
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("qjs-dap", factory)
  );
  context.subscriptions.push(
    vscode.debug.onDidTerminateDebugSession((session) => {
      if (session.type === "qjs-dap") {
        factory.stopSession(session.id);
      }
    })
  );
  context.subscriptions.push(factory);
}

function deactivate() {}

module.exports = {
  activate,
  deactivate,
  allocatePort,
  bundledBuildDirectory,
  defaultRuntimePath,
  ensureBundledRuntime,
  repoRootPath,
  runCommand,
  waitForServer,
  spawnQuickJSProcess,
  QuickJSDebugConfigurationProvider,
  QuickJSDebugAdapterDescriptorFactory
};
