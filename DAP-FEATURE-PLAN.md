# DAP Debugging Feature Plan for QuickJS-ng

## Overview

Add opt-in, DAP-based step-through debugging to QuickJS-ng. The feature is
controlled by the `QJS_ENABLE_DAP` build flag. When enabled, the `qjs` CLI
gains a `--dap` flag that starts a Debug Adapter Protocol server, allowing
VS Code and other DAP-compliant clients to attach and debug JavaScript
execution with breakpoints, stepping, variable inspection, and stack traces.

The DAP implementation will be written in **pure C** (no C++ dependency) to
match the rest of the codebase. A minimal JSON reader/writer and the DAP
message framing will be implemented directly. This avoids introducing cppdap
as a dependency (C++11, nlohmann/json) that would be at odds with the C11
codebase and its C-only build targets (WASI, Emscripten).

### Transport Modes

| Mode | Transport | Use Case |
|------|-----------|----------|
| **stdio** | DAP messages over stdin/stdout | Single session; VS Code launch mode |
| **tcp** | Listen on a TCP port | Multi-session; external tool attachment |

Selected via CLI flags: `--dap` (stdio) or `--dap=tcp:PORT` (TCP).

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  DAP Client (VS Code / Neovim / etc.)               │
└─────────────────┬───────────────────────────────────┘
                  │  JSON-RPC (DAP protocol)
                  ▼
┌─────────────────────────────────────────────────────┐
│  DAP Server (dap-server.c)                          │
│  ┌─────────────┐  ┌────────────┐  ┌──────────────┐ │
│  │ Transport   │  │ Protocol   │  │ Debug        │ │
│  │ (stdio/tcp) │  │ Handler    │  │ Controller   │ │
│  └─────────────┘  └─────┬──────┘  └──────┬───────┘ │
│                         │                │          │
│  ┌──────────────────────▼────────────────▼────────┐ │
│  │         QuickJS Debug Instrumentation          │ │
│  │  (new OP_debugger, breakpoint check, step)     │ │
│  │  hooks into JS_CallInternal via interrupt      │ │
│  │  handler and per-opcode callbacks              │ │
│  └────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

### New Source Files

| File | Purpose |
|------|---------|
| `qjs-dap.h` | Public DAP debug API (types, functions) |
| `qjs-dap.c` | DAP server: transport, protocol, session management |
| `qjs-debug.h` | Internal debug instrumentation API |
| `qjs-debug.c` | Debug hooks: breakpoints, stepping, variable inspection |
| `tests/test_dap.py` | Integration tests (Python script using DAP client) |

### Modified Source Files

| File | Changes |
|------|---------|
| `quickjs.c` | New `OP_debugger` opcode handling; expose `find_line_num` and `JSStackFrame` accessors; conditional per-opcode callback |
| `quickjs.h` | New public APIs: `JS_SetDebugHandler`, `JS_GetStackFrames`, `JS_GetLocalVariables`, `JS_EvaluateAtFrame` |
| `qjs.c` | New `--dap` / `--dap=tcp:PORT` CLI flags; DAP server lifecycle |
| `CMakeLists.txt` | `QJS_ENABLE_DAP` option; conditional source inclusion |
| `build.zig` | `dap` option; conditional source inclusion |
| `meson.build` | `dap` option; conditional source inclusion |
| `.github/workflows/ci.yml` | New DAP test CI job |

---

## Phase 1: Core Debug Instrumentation

**Goal:** Add the low-level hooks inside QuickJS that allow an external
debugger to pause execution, inspect state, and control stepping.

### 1.1 New `OP_debugger` Bytecode

Currently the `debugger` statement is parsed and ignored
(`quickjs.c:29038`). Change it to emit a new `OP_debugger` bytecode.

**In `quickjs-opcode.h`:**
```c
DEF(debugger, 1, 0, 0, none)        /* debugger; statement */
```

**In `quickjs.c` parser** (replace the `TOK_DEBUGGER` case):
```c
case TOK_DEBUGGER:
    if (next_token(s))
        goto fail;
    emit_op(s, OP_debugger);
    if (js_parse_expect_semi(s))
        goto fail;
    break;
```

**In `quickjs.c` `JS_CallInternal`** main loop, add handling:
```c
CASE(OP_debugger):
    if (rt->debug_handler) {
        int ret = rt->debug_handler(rt, rt->debug_opaque,
                                    JS_DEBUG_REASON_DEBUGGER_STMT,
                                    NULL);
        if (ret)
            goto exception;
    }
    BREAK;
```

### 1.2 Per-Opcode Debug Callback

Add an optional per-instruction callback to the main interpreter loop.
When enabled, this callback is invoked before each opcode executes.

**New `JSDebugHandler` callback type** (in `quickjs.h`):
```c
typedef enum {
    JS_DEBUG_REASON_BREAKPOINT = 1,
    JS_DEBUG_REASON_STEP = 2,
    JS_DEBUG_REASON_DEBUGGER_STMT = 3,
    JS_DEBUG_REASON_EXCEPTION = 4,
    JS_DEBUG_REASON_ENTRY = 5,
} JSDebugReason;

typedef int JSDebugHandler(JSRuntime *rt, void *opaque,
                           JSDebugReason reason,
                           const uint8_t *pc);

void JS_SetDebugHandler(JSRuntime *rt, JSDebugHandler *cb, void *opaque);
```

**In `JS_CallInternal`**, after the opcode dispatch at the top of the main
loop, add (guarded by a runtime flag `rt->debug_enabled`):

```c
if (__builtin_expect(rt->debug_enabled, 0)) {
    int ret = rt->debug_handler(rt, rt->debug_opaque,
                                JS_DEBUG_REASON_STEP, pc - 1);
    if (ret)
        goto exception;
}
```

The `__builtin_expect` ensures zero overhead when debugging is disabled.
The flag `rt->debug_enabled` is a new `uint8_t` field in `JSRuntime`.

### 1.3 Breakpoint Management

Implement breakpoint checking inside the debug handler (not in the hot
path of every opcode). The debug handler maintains a sorted array of
breakpoints keyed by `(filename_atom, line_number)`.

**Breakpoint data structure** (in `qjs-debug.h`):
```c
typedef struct JSBreakpoint {
    JSAtom filename;
    int line;
    int id;
    bool verified;
    char *condition;     /* optional JS expression */
} JSBreakpoint;

typedef struct JSDebugState {
    JSBreakpoint *breakpoints;
    int bp_count;
    int bp_capacity;
    int next_bp_id;

    /* Stepping state */
    enum { STEP_NONE, STEP_INTO, STEP_OVER, STEP_OUT } step_mode;
    int step_depth;      /* call depth at step start */
    uint32_t step_pc;    /* PC to step past (for step-over within same frame) */

    /* Current pause state */
    bool paused;
    JSDebugHandler *pause_callback;  /* called to resume */
} JSDebugState;
```

When the per-opcode callback fires, it:
1. Gets the current source location via `find_line_num()`.
2. Checks if `(filename, line)` matches a breakpoint.
3. Checks step conditions (depth change for step-in/out, same-frame for step-over).
4. If a match, enters the **paused** state and blocks waiting for a DAP
   continue/step command.

### 1.4 Variable Inspection API

Add public APIs for inspecting variables at a paused stack frame.

**New functions in `quickjs.h`:**
```c
/* Walk the call stack. Returns array of JSStackFrameInfo. */
typedef struct JSStackFrameInfo {
    JSValue func;           /* Function object */
    JSAtom filename;        /* Source file */
    int line;               /* Current line */
    int col;                /* Current column */
    JSAtom func_name;       /* Function name */
} JSStackFrameInfo;

int JS_GetStackTrace(JSContext *ctx, JSStackFrameInfo **frames, int max_frames);

/* Enumerate local variables and arguments for a stack frame index. */
typedef struct JSVarInfo {
    JSAtom name;
    JSValue value;
    int frame_index;        /* 0 = top frame */
} JSVarInfo;

int JS_GetFrameLocals(JSContext *ctx, int frame_index,
                      JSVarInfo **vars);

/* Evaluate an expression in the context of a paused frame. */
JSValue JS_EvaluateAtFrame(JSContext *ctx, int frame_index,
                           const char *expr, size_t expr_len);

/* Get global scope variables. */
int JS_GetGlobalVariables(JSContext *ctx, JSVarInfo **vars);
```

These are implemented by walking `rt->current_stack_frame` and reading
`sf->arg_buf[]`, `sf->var_buf[]` using the `JSFunctionBytecode.vardefs`
metadata.

### 1.5 Expose `find_line_num` Internally

The existing `find_line_num()` function (`quickjs.c:7642`) is `static`.
Add a non-static wrapper and make the `JSFunctionBytecode` pc2line fields
accessible through a new accessor:

```c
int JS_GetPCLineNumber(JSContext *ctx, JSValueConst func, uint32_t pc);
```

### 1.6 Interrupt Handler Integration

The existing `JS_SetInterruptHandler` mechanism (`quickjs.c:8189`) calls
the handler every ~10000 opcodes. The debug system will use this for:

- **Async pause**: Set a flag that the per-opcode callback checks, so the
  next opcode triggers a pause. This allows a DAP client to send a "pause"
  request while the program is running.
- **Timeout detection**: Not for debugging, but to ensure the debug handler
  doesn't block forever.

---

## Phase 2: DAP Protocol Implementation

**Goal:** Implement the DAP message protocol in pure C.

### 2.1 JSON Layer (`qjs-dap.c` internal)

A minimal JSON serializer/deserializer for DAP messages. This does **not**
need to be a general-purpose JSON library — it only needs to handle:

- Parsing DAP request objects (flat + one level of nesting)
- Constructing DAP response and event objects
- No streaming parser needed; full messages fit in memory

Implementation approach:
- Write helpers for building JSON strings into a buffer
- Parse incoming JSON using a simple recursive-descent parser
- ~500-800 lines of C

### 2.2 DAP Message Framing

DAP uses a simple framing protocol:
```
Content-Length: <N>\r\n\r\n<payload>
```

Implement read/write functions for this framing over both stdio and TCP.

### 2.3 Transport Layer

**Stdio transport:**
- Read DAP frames from stdin, write to stdout
- Binary-safe using `Content-Length` framing
- `qjs` redirects its own JS stdout/stderr to DAP `output` events

**TCP transport:**
- Listen on specified port
- Accept a single connection (per session)
- Read/write DAP frames over the socket
- For multi-session: listen loop that accepts, serves, then accepts again

**Transport interface:**
```c
typedef struct DAPTransport {
    int (*recv)(struct DAPTransport *t, char *buf, size_t len);
    int (*send)(struct DAPTransport *t, const char *buf, size_t len);
    void (*close)(struct DAPTransport *t);
} DAPTransport;
```

### 2.4 DAP Session State Machine

```
                    ┌──────────┐
    initialize ──▶  │ UNINIT   │
                    └────┬─────┘
                    init │
                    ┌────▼─────┐   initialized event
                    │  INIT    │──────────────────▶ client
                    └────┬─────┘
              launch/attach │
                    ┌──────▼──────┐
                    │ CONFIGURING │◀── setBreakpoints, etc.
                    └──────┬──────┘
            configDone event│
                    ┌──────▼──────┐
                    │  RUNNING    │◀─────────────────┐
                    └──────┬──────┘                   │
                  hit bp / │                          │
                  step end │                          │
                    ┌──────▼──────┐    continue/step  │
                    │  STOPPED    │───────────────────┘
                    └──────┬──────┘
                  terminate │
                    ┌──────▼──────┐
                    │ TERMINATED  │
                    └─────────────┘
```

### 2.5 DAP Requests to Implement

**Required for MVP (Phase 2):**

| Request | Description |
|---------|-------------|
| `initialize` | Exchange capabilities |
| `launch` | Start JS execution with program arguments |
| `setBreakpoints` | Set/clear source breakpoints |
| `setExceptionBreakpoints` | Configure exception breakpoints (caught/uncaught) |
| `configurationDone` | End configuration phase |
| `threads` | Return single thread |
| `stackTrace` | Return call stack frames |
| `scopes` | Return scopes for a frame (locals, globals, closure) |
| `variables` | Return variables in a scope |
| `continue` | Resume execution |
| `next` | Step over (next line in same frame) |
| `stepIn` | Step into function call |
| `stepOut` | Step out of current function |
| `evaluate` | Evaluate expression in frame context |
| `disconnect` | End session |
| `terminate` | Terminate debuggee |

**Events to emit:**

| Event | When |
|-------|------|
| `initialized` | After `initialize` response |
| `stopped` | Breakpoint hit, step complete, `debugger` stmt, exception |
| `continued` | After continue/step resumes execution |
| `output` | JS stdout/stderr, console messages |
| `terminated` | Program exits |
| `exited` | Process exit code |

**Capabilities to advertise:**

```json
{
  "supportsConfigurationDoneRequest": true,
  "supportsEvaluateForHovers": true,
  "supportsStepBack": false,
  "supportsSetVariable": true,
  "supportsConditionalBreakpoints": true,
  "supportsHitConditionalBreakpoints": false,
  "supportsExceptionInfoRequest": true,
  "supportsTerminateRequest": true,
  "supportsCancelRequest": true
}
```

### 2.6 Pause/Resume Mechanism

When the debug handler detects a breakpoint or step completion, it needs
to **block** the JS interpreter and wait for a DAP command.

Implementation: Use a mutex + condition variable (or a simple socket poll
loop since the DAP server is single-threaded).

```
JS thread (same thread as DAP server):
  1. DAP server starts, waits for launch
  2. JS execution begins
  3. Debug callback fires -> sends "stopped" event
  4. Blocks reading next DAP message from transport
  5. Processes continue/step/evaluate/variables requests
  6. On continue/step: returns from callback, JS resumes
```

Since QuickJS is single-threaded (one JS execution per runtime), the DAP
server and JS interpreter naturally alternate on the same thread. No
multi-threading needed.

---

## Phase 3: CLI Integration

**Goal:** Wire the DAP server into the `qjs` CLI.

### 3.1 New CLI Flags

```
--dap             Enable DAP debugging over stdio
--dap=tcp:4711    Enable DAP debugging over TCP port 4711
--dap-log=FILE    Log DAP protocol messages to file (for debugging)
```

### 3.2 CLI Flow with DAP

```
1. Parse CLI args, detect --dap
2. Create JSRuntime + JSContext
3. Initialize DAP server with chosen transport
4. Wait for DAP "initialize" request
5. Send "initialized" event
6. Wait for "launch" request (contains program path + args)
7. Wait for "setBreakpoints" and other config requests
8. Wait for "configurationDone"
9. Load and compile the JS program specified in "launch"
10. Install debug handler (breakpoints, stepping)
11. Run the JS program (JS_Eval or module evaluation)
    -> When paused, process DAP requests (evaluate, variables, etc.)
12. On completion, send "terminated" + "exited" events
13. Wait for "disconnect", clean up
```

### 3.3 Output Redirection

When DAP is active, redirect JS `console.log` / `print` output to DAP
`output` events (category `stdout`). Similarly for stderr.

---

## Phase 4: Build System Integration

### 4.1 CMake (`CMakeLists.txt`)

```cmake
xoption(QJS_ENABLE_DAP "Enable DAP debugging support" OFF)

if(QJS_ENABLE_DAP)
    set(qjs_sources ${qjs_sources} qjs-dap.c qjs-debug.c)
endif()
```

### 4.2 Zig (`build.zig`)

```zig
const build_dap = b.option(bool, "dap", "Enable DAP debugging") orelse false;
// ... conditionally add qjs-dap.c and qjs-debug.c to sources
```

### 4.3 Meson (`meson.build`, `meson_options.txt`)

```meson
option('dap', type: 'boolean', value: false, description: 'Enable DAP debugging')
```

### 4.4 Conditional Compilation

DAP code is excluded from non-DAP builds using `#ifdef QJS_ENABLE_DAP`.
The `OP_debugger` opcode is always emitted by the parser (since it's a
valid JS statement) but the runtime handler is a no-op when DAP is
disabled.

---

## Phase 5: Testing

### 5.1 Unit Tests (C)

Add DAP-specific tests to `api-test.c` (guarded by `#ifdef QJS_ENABLE_DAP`):

- **Breakpoint set/clear**: Verify breakpoints trigger at correct locations
- **Step modes**: Verify step-in, step-over, step-out behavior
- **Variable inspection**: Verify locals, arguments, closures are readable
- **Stack trace**: Verify frame enumeration matches expected call depth
- **`debugger` statement**: Verify it triggers a pause
- **Exception breakpoints**: Verify pause on thrown exceptions
- **Conditional breakpoints**: Verify expression evaluation at breakpoint

### 5.2 Integration Tests (Python)

A Python test script `tests/test_dap.py` that launches `qjs --dap` and
communicates via the DAP protocol. Uses a simple DAP client implementation
(or the `debugpy` DAP client).

**Test scenarios:**

| Test | Description |
|------|-------------|
| `test_launch_and_terminate` | Basic lifecycle: init → launch → terminate |
| `test_breakpoint` | Set breakpoint, run, verify stop at line |
| `test_conditional_breakpoint` | BP with condition that evaluates to true/false |
| `test_step_over` | Step over function calls, stay in same frame |
| `test_step_in` | Step into function call |
| `test_step_out` | Step out of function back to caller |
| `test_stack_trace` | Verify stack frames at nested call site |
| `test_locals` | Inspect local variables at breakpoint |
| `test_evaluate` | Evaluate expressions while paused |
| `test_debugger_statement` | Pause at `debugger;` |
| `test_exception_breakpoint` | Pause on thrown exceptions |
| `test_output_events` | Verify stdout/stderr captured as output events |
| `test_set_variable` | Modify a variable while paused |
| `test_disconnect_while_running` | Disconnect while JS is executing |
| `test_tcp_transport` | All of the above over TCP transport |
| `test_multi_session_tcp` | Connect, disconnect, reconnect on TCP |

### 5.3 CI Integration

Add to `.github/workflows/ci.yml`:

```yaml
dap:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v6
    - name: setup python
      uses: actions/setup-python@v5
      with:
        python-version: '3.12'
    - name: build with DAP
      run: |
        make QJS_ENABLE_DAP=ON
    - name: test DAP
      run: |
        python3 tests/test_dap.py ./build/qjs
    - name: test DAP (api-test)
      run: |
        ./build/api-test
```

### 5.4 Manual Testing with VS Code

Provide a `.vscode/launch.json` example for manual testing:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Debug with QuickJS",
      "type": "qjs-dap",
      "request": "launch",
      "program": "${file}",
      "runtime": "./build/qjs",
      "runtimeArgs": ["--dap"],
      "stopOnEntry": true
    }
  ]
}
```

A companion VS Code extension (out of scope for initial implementation) would
register the `qjs-dap` debug type. Until then, manual testing uses the
`debugpy` adapter or a generic DAP test client.

---

## Implementation Order and Effort Estimates

| Phase | Description | Files | Estimated LOC | Effort |
|-------|-------------|-------|---------------|--------|
| 1.1 | `OP_debugger` bytecode | `quickjs.c`, `quickjs-opcode.h` | 30 | S |
| 1.2 | Per-opcode debug callback | `quickjs.c`, `quickjs.h` | 80 | S |
| 1.3 | Breakpoint management | `qjs-debug.c`, `qjs-debug.h` | 400 | M |
| 1.4 | Variable inspection API | `quickjs.c`, `quickjs.h` | 300 | M |
| 2.1 | JSON layer | `qjs-dap.c` | 600 | M |
| 2.2 | DAP framing | `qjs-dap.c` | 100 | S |
| 2.3 | Transport (stdio + TCP) | `qjs-dap.c` | 200 | S |
| 2.4 | Session state machine | `qjs-dap.c` | 200 | M |
| 2.5 | DAP request handlers | `qjs-dap.c` | 1500 | L |
| 2.6 | Pause/resume mechanism | `qjs-debug.c` | 200 | M |
| 3 | CLI integration | `qjs.c` | 200 | S |
| 4 | Build system | 4 build files | 100 | S |
| 5.1 | C unit tests | `api-test.c` | 400 | M |
| 5.2 | Python integration tests | `tests/test_dap.py` | 800 | L |
| 5.3 | CI integration | `ci.yml` | 30 | S |
| **Total** | | | **~5140** | |

### Phase dependencies

```
1.1 ─┐
1.2 ─┤
     ├──▶ 1.3 ──▶ 1.4 ──▶ 2.6
     │
2.1 ─┤
2.2 ─┤
2.3 ─┼──▶ 2.4 ──▶ 2.5 ──▶ 3 ──▶ 4 ──▶ 5
     │
     └──────────────────────────────▶ 5
```

Phases 1 (instrumentation) and 2.1-2.3 (protocol/transport) can proceed in
parallel. They converge at 2.5 (DAP request handlers that use the
instrumentation API).

---

## Design Decisions and Rationale

### Why pure C instead of cppdap?

1. **Build system consistency**: QuickJS builds with four build systems, all
   targeting pure C. Adding a C++ dependency would require all four to handle
   C++ compilation, linking, and the nlohmann/json transitive dependency.
2. **WASI/Emscripten support**: These targets have limited C++ support.
3. **Minimal scope**: DAP uses a simple JSON-RPC framing over a single
   transport. A full SDK is overkill; ~700 lines of JSON handling suffices.
4. **No external dependencies**: QuickJS has zero runtime dependencies today.
   Adding one would be a philosophical change.

### Why per-opcode callback instead of only interrupt handler?

The interrupt handler fires every ~10000 opcodes. For responsive stepping
and breakpoints, we need to check every opcode. The `__builtin_expect`
branch hint ensures the hot path is unaffected when debugging is off.

### Why single-threaded?

QuickJS is inherently single-threaded per runtime. The DAP server naturally
alternates with JS execution on the same thread (pause → process requests →
resume). No thread synchronization needed, no race conditions.

### Why not `evaluate` for conditional breakpoints in the handler?

Conditional breakpoint expressions need to be evaluated in the JS context.
This is done by calling `JS_Eval()` within the debug handler while the
interpreter is paused. The result determines whether to actually pause or
continue.

---

## Future Enhancements (Out of Scope)

- **VS Code extension**: Proper extension with `qjs-dap` debug type
- **Source map support**: For debugging transpiled/bundled code
- **Watch expressions**: Persistent evaluate-on-every-stop
- **Logpoints**: Breakpoints that log instead of pause
- **Data breakpoints**: Break when a variable changes value
- **Async stack traces**: Show async causal chain
- **Multi-thread debugging**: Support workers with separate DAP sessions
- **Bytecode disassembly view**: Via DAP `disassemble` request
