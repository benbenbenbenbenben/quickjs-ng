#include "qjs-dap.h"
#include "qjs-debug.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/select.h>
#endif
#if QJS_DAP_HAVE_SOCKETS && defined(_WIN32)
#include <winsock2.h>
#elif QJS_DAP_HAVE_SOCKETS
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

struct DAPServer {
    JSContext *ctx;
    DAPTransport *transport;
    JSDebugState *debug_state;
    char *launch_program;
    char **launch_args;
    int launch_argc;
    int launch_module;
    bool launch_stop_on_entry;
    bool entry_stop_pending;
    bool initialized;
    bool launch_received;
    bool configuration_done;
    bool is_disconnecting;
    bool pause_requested;
    bool terminate_requested;
    bool execution_started;
    FILE *log_fp;
    int seq;
};

/* --- JSON Helpers using QuickJS --- */
static int js_get_int(JSContext *ctx, JSValue obj, const char *prop, int default_val) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsException(v)) {
        JS_FreeValue(ctx, v);
        return default_val;
    }
    int32_t i;
    if (JS_ToInt32(ctx, &i, v) < 0) i = default_val;
    JS_FreeValue(ctx, v);
    return i;
}

static int js_get_bool(JSContext *ctx, JSValue obj, const char *prop, int default_val) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    int ret = default_val;
    if (!JS_IsUndefined(v) && !JS_IsException(v))
        ret = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return ret < 0 ? default_val : ret;
}

static const char *js_get_str(JSContext *ctx, JSValue obj, const char *prop) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsString(v)) {
        return JS_ToCString(ctx, v);
    }
    JS_FreeValue(ctx, v);
    return NULL;
}

/* --- Transport implementation --- */
static int stdio_recv(DAPTransport *t, char *buf, size_t len) {
    size_t r = fread(buf, 1, len, stdin);
    return r == 0 ? -1 : (int)r;
}
static int stdio_send(DAPTransport *t, const char *buf, size_t len) {
    size_t w = fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return w == len ? 0 : -1;
}
static int stdio_poll(DAPTransport *t, int timeout_ms) {
    (void)t;
#ifdef _WIN32
    (void)timeout_ms;
    return 0;
#else
    int fd = fileno(stdin);
    struct timeval tv;
    fd_set rfds;
    if (fd < 0)
        return -1;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &rfds, NULL, NULL, &tv);
#endif
}
static void stdio_close(DAPTransport *t) {
    js_free(NULL, t);
}
DAPTransport *DAP_NewStdioTransport(void) {
    DAPTransport *t = malloc(sizeof(DAPTransport));
    t->recv = stdio_recv;
    t->send = stdio_send;
    t->poll = stdio_poll;
    t->close = stdio_close;
    return t;
}

/* --- TCP Transport implementation --- */
#if QJS_DAP_HAVE_SOCKETS
struct TCPTransportData {
    int server_fd;
    int client_fd;
};

static void tcp_close_client(struct TCPTransportData *d) {
    if (d->client_fd < 0)
        return;
#ifdef _WIN32
    closesocket(d->client_fd);
#else
    close(d->client_fd);
#endif
    d->client_fd = -1;
}

static int tcp_accept_client(DAPTransport *t) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
    int client_fd;

    if (d->client_fd >= 0)
        return 0;
    client_fd = accept(d->server_fd, NULL, NULL);
    if (client_fd < 0)
        return -1;
    d->client_fd = client_fd;
    fprintf(stderr, "DAP client connected.\n");
    return 0;
}

static int tcp_recv(DAPTransport *t, char *buf, size_t len) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
    ssize_t r;

    if (tcp_accept_client(t) < 0)
        return -1;
    for (;;) {
        r = recv(d->client_fd, buf, len, 0);
        if (r > 0)
            return (int)r;
        if (r == 0) {
            tcp_close_client(d);
            return -1;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEINTR)
            continue;
#else
        if (errno == EINTR)
            continue;
#endif
        tcp_close_client(d);
        return -1;
    }
}

static int tcp_send(DAPTransport *t, const char *buf, size_t len) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
    size_t written = 0;

    if (d->client_fd < 0)
        return 0;
    while (written < len) {
        ssize_t w = send(d->client_fd, buf + written, len - written,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (w > 0) {
            written += (size_t)w;
            continue;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEINTR)
            continue;
#else
        if (errno == EINTR)
            continue;
#endif
        tcp_close_client(d);
        return 0;
    }
    return 0;
}

static int tcp_poll(DAPTransport *t, int timeout_ms) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
    fd_set rfds;
    struct timeval tv;
    int fd;

    FD_ZERO(&rfds);
    fd = d->client_fd >= 0 ? d->client_fd : d->server_fd;
    if (fd < 0)
        return -1;
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &rfds, NULL, NULL, &tv);
}

static void tcp_close(DAPTransport *t) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
#ifdef _WIN32
    tcp_close_client(d);
    if (d->server_fd >= 0) closesocket(d->server_fd);
    WSACleanup();
#else
    tcp_close_client(d);
    if (d->server_fd >= 0) close(d->server_fd);
#endif
    free(d);
    free(t);
}

DAPTransport *DAP_NewTCPTransport(int port) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return NULL;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(server_fd);
#else
        close(server_fd);
#endif
        return NULL;
    }

    if (listen(server_fd, 1) < 0) {
#ifdef _WIN32
        closesocket(server_fd);
#else
        close(server_fd);
#endif
        return NULL;
    }

    DAPTransport *t = malloc(sizeof(DAPTransport));
    struct TCPTransportData *d = malloc(sizeof(struct TCPTransportData));
    d->server_fd = server_fd;
    d->client_fd = -1;

    fprintf(stderr, "Waiting for DAP client on port %d...\n", port);

    t->opaque = d;
    t->recv = tcp_recv;
    t->send = tcp_send;
    t->poll = tcp_poll;
    t->close = tcp_close;

    return t;
}
#else
DAPTransport *DAP_NewTCPTransport(int port) {
    (void)port;
    fprintf(stderr, "qjs: TCP DAP transport is not supported on this target\n");
    return NULL;
}
#endif

static bool dap_is_stopped(DAPServer *s) {
    return s->debug_state->paused && s->debug_state->has_stop_state;
}

static int dap_pause_callback(JSRuntime *rt, void *opaque, JSDebugReason reason, const uint8_t *pc);

enum {
    DAP_SCOPE_REF_GLOBALS = 1,
    DAP_SCOPE_REF_BASE = 1000,
    DAP_SCOPE_REF_STRIDE = 8,
    DAP_SCOPE_KIND_LOCAL = 1,
    DAP_SCOPE_KIND_CLOSURE = 2,
};

static int dap_make_scope_ref(int frame_id, int kind) {
    return DAP_SCOPE_REF_BASE + frame_id * DAP_SCOPE_REF_STRIDE + kind;
}

static int dap_decode_scope_ref(int ref, int *frame_id, int *kind) {
    int value;

    if (ref == DAP_SCOPE_REF_GLOBALS) {
        *frame_id = -1;
        *kind = 0;
        return 0;
    }
    if (ref < DAP_SCOPE_REF_BASE)
        return -1;
    value = ref - DAP_SCOPE_REF_BASE;
    *frame_id = value / DAP_SCOPE_REF_STRIDE;
    *kind = value % DAP_SCOPE_REF_STRIDE;
    if (*kind != DAP_SCOPE_KIND_LOCAL && *kind != DAP_SCOPE_KIND_CLOSURE)
        return -1;
    return 0;
}

static void dap_clear_launch_config(DAPServer *s) {
    int i;
    free(s->launch_program);
    s->launch_program = NULL;
    if (s->launch_args) {
        for (i = 0; i < s->launch_argc; i++)
            free(s->launch_args[i]);
        free(s->launch_args);
    }
    s->launch_args = NULL;
    s->launch_argc = 0;
    s->launch_module = -1;
    s->launch_stop_on_entry = false;
}

static void dap_free_var_info_array(JSContext *ctx, JSVarInfo *vars, int count) {
    int i;

    if (!vars)
        return;
    for (i = 0; i < count; i++) {
        JS_FreeAtom(ctx, vars[i].name);
        JS_FreeValue(ctx, vars[i].value);
    }
    js_free(ctx, vars);
}

static void transport_disconnect_client(DAPTransport *t) {
#if QJS_DAP_HAVE_SOCKETS
    if (t && t->recv == tcp_recv)
        tcp_close_client((struct TCPTransportData *)t->opaque);
#else
    (void)t;
#endif
}

static void dap_reset_client_session(DAPServer *s) {
    s->initialized = false;
    s->pause_requested = false;
    s->is_disconnecting = false;
    if (!s->execution_started) {
        s->launch_received = false;
        s->configuration_done = false;
        dap_clear_launch_config(s);
    }
}

static void dap_detach_client(DAPServer *s) {
    transport_disconnect_client(s->transport);
    dap_reset_client_session(s);
    if (s->execution_started && s->debug_state->paused)
        JS_DebugContinue(s->debug_state);
}

static void dap_log_json(DAPServer *s, const char *direction, const char *json, size_t len) {
    if (!s->log_fp)
        return;
    fprintf(s->log_fp, "%s ", direction);
    fwrite(json, 1, len, s->log_fp);
    fputc('\n', s->log_fp);
    fflush(s->log_fp);
}

static JSValue js_print_dap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    DAPServer *s = JS_GetContextOpaque(ctx);
    if (!s) return JS_UNDEFINED;

    const char *category = magic == 0 ? "stdout" : "stderr";

    // We concatenate all arguments with space
    size_t total_len = 0;
    const char **strs = js_malloc(ctx, sizeof(const char *) * argc);
    for(int i = 0; i < argc; i++) {
        strs[i] = JS_ToCString(ctx, argv[i]);
        if (strs[i]) {
            total_len += strlen(strs[i]) + (i > 0 ? 1 : 0);
        }
    }

    if (total_len > 0) {
        char *buf = js_malloc(ctx, total_len + 2); // + '\n' + '\0'
        buf[0] = '\0';
        for (int i = 0; i < argc; i++) {
            if (strs[i]) {
                if (i > 0) strcat(buf, " ");
                strcat(buf, strs[i]);
                JS_FreeCString(ctx, strs[i]);
            }
        }
        strcat(buf, "\n");
        DAP_SendOutput(s, category, buf);
        js_free(ctx, buf);
    }
    js_free(ctx, strs);
    return JS_UNDEFINED;
}

void DAP_SetupOutputRedirection(DAPServer *server) {
    JSValue global_obj = JS_GetGlobalObject(server->ctx);
    JSValue console = JS_NewObject(server->ctx);
    JS_SetPropertyStr(server->ctx, console, "log", JS_NewCFunctionMagic(server->ctx, js_print_dap, "log", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(server->ctx, console, "error", JS_NewCFunctionMagic(server->ctx, js_print_dap, "error", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(server->ctx, console, "warn", JS_NewCFunctionMagic(server->ctx, js_print_dap, "warn", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(server->ctx, global_obj, "console", console);
    JS_SetPropertyStr(server->ctx, global_obj, "print", JS_NewCFunctionMagic(server->ctx, js_print_dap, "print", 1, JS_CFUNC_generic_magic, 0));
    JS_FreeValue(server->ctx, global_obj);
}

/* --- Core DAP Framing --- */
static void dap_send_json(DAPServer *s, JSValue obj) {
    JSValue str = JS_JSONStringify(s->ctx, obj, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsString(str)) {
        size_t len;
        const char *cstr = JS_ToCStringLen(s->ctx, &len, str);
        dap_log_json(s, "->", cstr, len);
        char header[128];
        int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
        s->transport->send(s->transport, header, hlen);
        s->transport->send(s->transport, cstr, len);
        JS_FreeCString(s->ctx, cstr);
    }
    JS_FreeValue(s->ctx, str);
}

static void dap_send_event(DAPServer *s, const char *event, JSValue body) {
    JSValue msg = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, msg, "seq", JS_NewInt32(s->ctx, ++s->seq));
    JS_SetPropertyStr(s->ctx, msg, "type", JS_NewString(s->ctx, "event"));
    JS_SetPropertyStr(s->ctx, msg, "event", JS_NewString(s->ctx, event));
    if (!JS_IsUndefined(body)) {
        JS_SetPropertyStr(s->ctx, msg, "body", body);
    }
    dap_send_json(s, msg);
    JS_FreeValue(s->ctx, msg);
}

static void dap_send_response(DAPServer *s, JSValue request, JSValue body) {
    JSValue msg = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, msg, "seq", JS_NewInt32(s->ctx, ++s->seq));
    JS_SetPropertyStr(s->ctx, msg, "type", JS_NewString(s->ctx, "response"));
    JSValue req_seq = JS_GetPropertyStr(s->ctx, request, "seq");
    JS_SetPropertyStr(s->ctx, msg, "request_seq", req_seq);
    JSValue command = JS_GetPropertyStr(s->ctx, request, "command");
    JS_SetPropertyStr(s->ctx, msg, "command", command);
    JS_SetPropertyStr(s->ctx, msg, "success", JS_NewBool(s->ctx, true));
    if (!JS_IsUndefined(body)) {
        JS_SetPropertyStr(s->ctx, msg, "body", body);
    }
    dap_send_json(s, msg);
    JS_FreeValue(s->ctx, msg);
}

static void dap_send_error_response(DAPServer *s, JSValue request, const char *message) {
    JSValue msg = JS_NewObject(s->ctx);
    JSValue body = JS_NewObject(s->ctx);
    JSValue err = JS_NewObject(s->ctx);

    JS_SetPropertyStr(s->ctx, msg, "seq", JS_NewInt32(s->ctx, ++s->seq));
    JS_SetPropertyStr(s->ctx, msg, "type", JS_NewString(s->ctx, "response"));
    JS_SetPropertyStr(s->ctx, msg, "request_seq", JS_GetPropertyStr(s->ctx, request, "seq"));
    JS_SetPropertyStr(s->ctx, msg, "command", JS_GetPropertyStr(s->ctx, request, "command"));
    JS_SetPropertyStr(s->ctx, msg, "success", JS_NewBool(s->ctx, false));
    JS_SetPropertyStr(s->ctx, msg, "message", JS_NewString(s->ctx, message));
    JS_SetPropertyStr(s->ctx, err, "format", JS_NewString(s->ctx, message));
    JS_SetPropertyStr(s->ctx, body, "error", err);
    JS_SetPropertyStr(s->ctx, msg, "body", body);
    dap_send_json(s, msg);
    JS_FreeValue(s->ctx, msg);
}

static void dap_send_continued_event(DAPServer *s) {
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "threadId", JS_NewInt32(s->ctx, 1));
    JS_SetPropertyStr(s->ctx, body, "allThreadsContinued", JS_NewBool(s->ctx, true));
    dap_send_event(s, "continued", body);
}

static const char *dap_stop_reason_string(DAPServer *s) {
    if (!s->debug_state->has_stop_state)
        return "pause";
    switch (s->debug_state->stop_reason) {
    case JS_DEBUG_REASON_BREAKPOINT:
        return "breakpoint";
    case JS_DEBUG_REASON_STEP:
        return "step";
    case JS_DEBUG_REASON_EXCEPTION:
        return "exception";
    case JS_DEBUG_REASON_DEBUGGER:
        return "debugger_statement";
    case JS_DEBUG_REASON_ENTRY:
        return "entry";
    case JS_DEBUG_REASON_POLL:
    case JS_DEBUG_REASON_EXIT:
    default:
        return "pause";
    }
}

static JSValue dap_stringify_value(JSContext *ctx, JSValueConst value) {
    if (JS_IsUninitialized(value))
        return JS_NewString(ctx, "<uninitialized>");
    JSValue str_val = JS_ToString(ctx, value);
    if (JS_IsException(str_val)) {
        JS_GetException(ctx);
        return JS_NewString(ctx, "");
    }
    return str_val;
}

/* --- Message processing --- */
static void handle_initialize(DAPServer *s, JSValue request) {
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "supportsConfigurationDoneRequest", JS_NewBool(s->ctx, true));
    JS_SetPropertyStr(s->ctx, body, "supportsEvaluateForHovers", JS_NewBool(s->ctx, true));
    JS_SetPropertyStr(s->ctx, body, "supportsSetVariable", JS_NewBool(s->ctx, true));
    JS_SetPropertyStr(s->ctx, body, "supportsConditionalBreakpoints", JS_NewBool(s->ctx, true));
    JS_SetPropertyStr(s->ctx, body, "supportsExceptionInfoRequest", JS_NewBool(s->ctx, true));
    JS_SetPropertyStr(s->ctx, body, "supportsTerminateRequest", JS_NewBool(s->ctx, true));
    dap_send_response(s, request, body);
    s->initialized = true;
    dap_send_event(s, "initialized", JS_UNDEFINED);
}

static void handle_launch(DAPServer *s, JSValue request) {
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    JSValue argv = JS_GetPropertyStr(s->ctx, args, "args");
    const char *program = js_get_str(s->ctx, args, "program");
    int64_t argc = 0;

    if (s->execution_started) {
        JS_FreeValue(s->ctx, argv);
        JS_FreeValue(s->ctx, args);
        if (program)
            JS_FreeCString(s->ctx, program);
        dap_send_error_response(s, request, "launch is only valid before the target starts");
        return;
    }

    dap_clear_launch_config(s);
    if (program) {
        s->launch_program = strdup(program);
        JS_FreeCString(s->ctx, program);
    }
    if (!JS_IsUndefined(argv))
        JS_GetLength(s->ctx, argv, &argc);
    if (argc > 0) {
        s->launch_args = calloc((size_t)argc, sizeof(*s->launch_args));
        if (!s->launch_args) {
            JS_FreeValue(s->ctx, argv);
            JS_FreeValue(s->ctx, args);
            dap_send_error_response(s, request, "failed to allocate launch arguments");
            return;
        }
        s->launch_argc = (int)argc;
        for (int64_t i = 0; i < argc; i++) {
            JSValue arg = JS_GetPropertyInt64(s->ctx, argv, i);
            const char *arg_str = JS_ToCString(s->ctx, arg);
            if (arg_str) {
                s->launch_args[i] = strdup(arg_str);
                JS_FreeCString(s->ctx, arg_str);
            }
            JS_FreeValue(s->ctx, arg);
        }
    }
    s->launch_stop_on_entry = js_get_bool(s->ctx, args, "stopOnEntry", false);
    s->launch_module = js_get_bool(s->ctx, args, "module", -1);
    s->launch_received = true;
    JS_FreeValue(s->ctx, argv);
    JS_FreeValue(s->ctx, args);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_attach(DAPServer *s, JSValue request) {
    if (!s->execution_started) {
        dap_send_error_response(s, request, "attach is only valid for a running target");
        return;
    }
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_set_breakpoints(DAPServer *s, JSValue request) {
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    JSValue source = JS_GetPropertyStr(s->ctx, args, "source");
    const char *path = js_get_str(s->ctx, source, "path");

    if (path) {
        JS_ClearBreakpoints(s->debug_state, path);
        JSValue bps = JS_GetPropertyStr(s->ctx, args, "breakpoints");
        int64_t len = 0;
        JS_GetLength(s->ctx, bps, &len);

        JSValue resp_bps = JS_NewArray(s->ctx);
        for (int64_t i = 0; i < len; i++) {
            JSValue bp = JS_GetPropertyInt64(s->ctx, bps, i);
            int line = js_get_int(s->ctx, bp, "line", 0);
            const char *cond = js_get_str(s->ctx, bp, "condition");

            int id = JS_AddBreakpoint(s->debug_state, path, line, cond);

            JSValue out_bp = JS_NewObject(s->ctx);
            JS_SetPropertyStr(s->ctx, out_bp, "id", JS_NewInt32(s->ctx, id));
            JS_SetPropertyStr(s->ctx, out_bp, "verified", JS_NewBool(s->ctx, true));
            JS_SetPropertyStr(s->ctx, out_bp, "line", JS_NewInt32(s->ctx, line));
            JS_SetPropertyUint32(s->ctx, resp_bps, i, out_bp);

            if (cond) JS_FreeCString(s->ctx, cond);
            JS_FreeValue(s->ctx, bp);
        }

        JSValue body = JS_NewObject(s->ctx);
        JS_SetPropertyStr(s->ctx, body, "breakpoints", resp_bps);
        dap_send_response(s, request, body);
        JS_FreeCString(s->ctx, path);
        JS_FreeValue(s->ctx, bps);
    } else {
        dap_send_response(s, request, JS_UNDEFINED);
    }
    JS_FreeValue(s->ctx, source);
    JS_FreeValue(s->ctx, args);
}

static void handle_set_exception_breakpoints(DAPServer *s, JSValue request) {
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    JSValue filters = JS_GetPropertyStr(s->ctx, args, "filters");

    bool pause_on_exceptions = false;
    int64_t len = 0;
    if (!JS_IsUndefined(filters)) {
        JS_GetLength(s->ctx, filters, &len);
        for (int64_t i = 0; i < len; i++) {
            JSValue filter = JS_GetPropertyInt64(s->ctx, filters, i);
            const char *str = JS_ToCString(s->ctx, filter);
            if (str) {
                if (strcmp(str, "all") == 0 || strcmp(str, "uncaught") == 0) {
                    pause_on_exceptions = true;
                }
                JS_FreeCString(s->ctx, str);
            }
            JS_FreeValue(s->ctx, filter);
        }
        JS_FreeValue(s->ctx, filters);
    }

    s->debug_state->pause_on_exceptions = pause_on_exceptions;

    JS_FreeValue(s->ctx, args);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_configuration_done(DAPServer *s, JSValue request) {
    if (!s->initialized) {
        dap_send_error_response(s, request, "initialize must be called before configurationDone");
        return;
    }
    s->configuration_done = true;
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_threads(DAPServer *s, JSValue request) {
    JSValue threads = JS_NewArray(s->ctx);
    JSValue thread = JS_NewObject(s->ctx);
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, thread, "id", JS_NewInt32(s->ctx, 1));
    JS_SetPropertyStr(s->ctx, thread, "name", JS_NewString(s->ctx, "main"));
    JS_SetPropertyUint32(s->ctx, threads, 0, thread);
    JS_SetPropertyStr(s->ctx, body, "threads", threads);
    dap_send_response(s, request, body);
}

static void handle_continue(DAPServer *s, JSValue request) {
    JS_DebugContinue(s->debug_state);
    s->pause_requested = false;
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "allThreadsContinued", JS_NewBool(s->ctx, true));
    dap_send_response(s, request, body);
    dap_send_continued_event(s);
}

static void handle_next(DAPServer *s, JSValue request) {
    JS_DebugStepOver(s->debug_state);
    s->pause_requested = false;
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "allThreadsContinued", JS_NewBool(s->ctx, true));
    dap_send_response(s, request, body);
    dap_send_continued_event(s);
}

static void handle_step_in(DAPServer *s, JSValue request) {
    JS_DebugStepInto(s->debug_state);
    s->pause_requested = false;
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "allThreadsContinued", JS_NewBool(s->ctx, true));
    dap_send_response(s, request, body);
    dap_send_continued_event(s);
}

static void handle_step_out(DAPServer *s, JSValue request) {
    JS_DebugStepOut(s->debug_state);
    s->pause_requested = false;
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "allThreadsContinued", JS_NewBool(s->ctx, true));
    dap_send_response(s, request, body);
    dap_send_continued_event(s);
}

static void handle_stack_trace(DAPServer *s, JSValue request) {
    if (!dap_is_stopped(s)) {
        dap_send_error_response(s, request, "stackTrace is only available while paused");
        return;
    }
    JSStackFrameInfo *frames = NULL;
    int count = JS_GetStackTrace(s->ctx, &frames, 50);

    JSValue out_frames = JS_NewArray(s->ctx);
    for (int i = 0; i < count; i++) {
        JSValue out_frame = JS_NewObject(s->ctx);
        JS_SetPropertyStr(s->ctx, out_frame, "id", JS_NewInt32(s->ctx, i));

        const char *func_name = frames[i].func_name == JS_ATOM_NULL ? "(anonymous)" : JS_AtomToCString(s->ctx, frames[i].func_name);
        JS_SetPropertyStr(s->ctx, out_frame, "name", JS_NewString(s->ctx, func_name));
        if (frames[i].func_name != JS_ATOM_NULL) JS_FreeCString(s->ctx, func_name);

        JS_SetPropertyStr(s->ctx, out_frame, "line", JS_NewInt32(s->ctx, frames[i].line > 0 ? frames[i].line : 1));
        JS_SetPropertyStr(s->ctx, out_frame, "column", JS_NewInt32(s->ctx, frames[i].col > 0 ? frames[i].col : 1));

        if (frames[i].filename != JS_ATOM_NULL) {
            JSValue source = JS_NewObject(s->ctx);
            const char *file_cstr = JS_AtomToCString(s->ctx, frames[i].filename);
            JS_SetPropertyStr(s->ctx, source, "path", JS_NewString(s->ctx, file_cstr));
            JS_SetPropertyStr(s->ctx, source, "name", JS_NewString(s->ctx, file_cstr));
            JS_SetPropertyStr(s->ctx, out_frame, "source", source);
            JS_FreeCString(s->ctx, file_cstr);
        }

        JS_SetPropertyUint32(s->ctx, out_frames, i, out_frame);

        JS_FreeValue(s->ctx, frames[i].func);
        if (frames[i].filename != JS_ATOM_NULL) JS_FreeAtom(s->ctx, frames[i].filename);
        if (frames[i].func_name != JS_ATOM_NULL) JS_FreeAtom(s->ctx, frames[i].func_name);
    }
    if (frames) js_free(s->ctx, frames);

    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "stackFrames", out_frames);
    JS_SetPropertyStr(s->ctx, body, "totalFrames", JS_NewInt32(s->ctx, count));
    dap_send_response(s, request, body);
}

static void handle_scopes(DAPServer *s, JSValue request) {
    JSVarInfo *closures = NULL;
    if (!dap_is_stopped(s)) {
        dap_send_error_response(s, request, "scopes is only available while paused");
        return;
    }
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    int frame_id = js_get_int(s->ctx, args, "frameId", 0);
    JS_FreeValue(s->ctx, args);

    JSValue scopes = JS_NewArray(s->ctx);

    JSValue loc_scope = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, loc_scope, "name", JS_NewString(s->ctx, "Locals"));
    JS_SetPropertyStr(s->ctx, loc_scope, "variablesReference",
                      JS_NewInt32(s->ctx, dap_make_scope_ref(frame_id, DAP_SCOPE_KIND_LOCAL)));
    JS_SetPropertyStr(s->ctx, loc_scope, "expensive", JS_NewBool(s->ctx, false));
    JS_SetPropertyUint32(s->ctx, scopes, 0, loc_scope);

    int closure_count = JS_GetFrameClosures(s->ctx, frame_id, &closures);
    if (closure_count > 0) {
        JSValue closure_scope = JS_NewObject(s->ctx);
        JS_SetPropertyStr(s->ctx, closure_scope, "name", JS_NewString(s->ctx, "Closure"));
        JS_SetPropertyStr(s->ctx, closure_scope, "variablesReference",
                          JS_NewInt32(s->ctx, dap_make_scope_ref(frame_id, DAP_SCOPE_KIND_CLOSURE)));
        JS_SetPropertyStr(s->ctx, closure_scope, "expensive", JS_NewBool(s->ctx, false));
        JS_SetPropertyUint32(s->ctx, scopes, 1, closure_scope);
    }
    dap_free_var_info_array(s->ctx, closures, closure_count > 0 ? closure_count : 0);

    JSValue glb_scope = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, glb_scope, "name", JS_NewString(s->ctx, "Globals"));
    JS_SetPropertyStr(s->ctx, glb_scope, "variablesReference", JS_NewInt32(s->ctx, DAP_SCOPE_REF_GLOBALS));
    JS_SetPropertyStr(s->ctx, glb_scope, "expensive", JS_NewBool(s->ctx, true));
    JS_SetPropertyUint32(s->ctx, scopes, closure_count > 0 ? 2 : 1, glb_scope);

    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "scopes", scopes);
    dap_send_response(s, request, body);
}

static void dap_append_variables(DAPServer *s, JSValue vars, JSVarInfo *vinfo, int count) {
    int i;

    for (i = 0; i < count; i++) {
        JSValue out_var = JS_NewObject(s->ctx);
        JSValue str_val;
        const char *vname = JS_AtomToCString(s->ctx, vinfo[i].name);
        const char *val_cstr;

        JS_SetPropertyStr(s->ctx, out_var, "name", JS_NewString(s->ctx, vname));
        JS_FreeCString(s->ctx, vname);

        str_val = dap_stringify_value(s->ctx, vinfo[i].value);
        val_cstr = JS_ToCString(s->ctx, str_val);
        JS_SetPropertyStr(s->ctx, out_var, "value", JS_NewString(s->ctx, val_cstr ? val_cstr : ""));
        if (val_cstr)
            JS_FreeCString(s->ctx, val_cstr);
        JS_FreeValue(s->ctx, str_val);

        JS_SetPropertyStr(s->ctx, out_var, "variablesReference", JS_NewInt32(s->ctx, 0));
        JS_SetPropertyUint32(s->ctx, vars, i, out_var);
    }
}

static void handle_variables(DAPServer *s, JSValue request) {
    JSVarInfo *vinfo = NULL;
    int frame_id = -1;
    int scope_kind = 0;
    int count = 0;
    if (!dap_is_stopped(s)) {
        dap_send_error_response(s, request, "variables is only available while paused");
        return;
    }
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    int var_ref = js_get_int(s->ctx, args, "variablesReference", 0);
    JS_FreeValue(s->ctx, args);

    JSValue vars = JS_NewArray(s->ctx);

    if (var_ref == DAP_SCOPE_REF_GLOBALS) {
        count = JS_GetGlobalVariables(s->ctx, &vinfo);
    } else if (dap_decode_scope_ref(var_ref, &frame_id, &scope_kind) == 0) {
        if (scope_kind == DAP_SCOPE_KIND_LOCAL)
            count = JS_GetFrameLocals(s->ctx, frame_id, &vinfo);
        else if (scope_kind == DAP_SCOPE_KIND_CLOSURE)
            count = JS_GetFrameClosures(s->ctx, frame_id, &vinfo);
    }
    if (count > 0) {
        dap_append_variables(s, vars, vinfo, count);
        dap_free_var_info_array(s->ctx, vinfo, count);
    }

    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "variables", vars);
    dap_send_response(s, request, body);
}

static void handle_evaluate(DAPServer *s, JSValue request) {
    if (!dap_is_stopped(s)) {
        dap_send_error_response(s, request, "evaluate is only available while paused");
        return;
    }
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    const char *expr = js_get_str(s->ctx, args, "expression");
    int frame_id = js_get_int(s->ctx, args, "frameId", 0);

    JSValue body = JS_NewObject(s->ctx);
    if (expr) {
        s->debug_state->is_evaluating = true;
        JSValue result = JS_EvaluateAtFrame(s->ctx, frame_id, expr, strlen(expr));
        s->debug_state->is_evaluating = false;

        JSValue str_val = JS_ToString(s->ctx, result);
        const char *val_cstr = JS_ToCString(s->ctx, str_val);
        JS_SetPropertyStr(s->ctx, body, "result", JS_NewString(s->ctx, val_cstr ? val_cstr : ""));
        if (val_cstr) JS_FreeCString(s->ctx, val_cstr);
        JS_FreeValue(s->ctx, str_val);

        JS_SetPropertyStr(s->ctx, body, "variablesReference", JS_NewInt32(s->ctx, 0));
        JS_FreeValue(s->ctx, result);
        JS_FreeCString(s->ctx, expr);
    }
    JS_FreeValue(s->ctx, args);

    dap_send_response(s, request, body);
}

static void handle_set_variable(DAPServer *s, JSValue request) {
    JSValue args;
    JSValue eval_result;
    JSValue body;
    JSValue stringified;
    const char *name;
    const char *value_expr;
    const char *value_cstr;
    int frame_id = -1;
    int scope_kind = 0;
    int var_ref;
    int ret;

    if (!dap_is_stopped(s)) {
        dap_send_error_response(s, request, "setVariable is only available while paused");
        return;
    }

    args = JS_GetPropertyStr(s->ctx, request, "arguments");
    name = js_get_str(s->ctx, args, "name");
    value_expr = js_get_str(s->ctx, args, "value");
    var_ref = js_get_int(s->ctx, args, "variablesReference", 0);
    if (!name || !value_expr) {
        if (name) JS_FreeCString(s->ctx, name);
        if (value_expr) JS_FreeCString(s->ctx, value_expr);
        JS_FreeValue(s->ctx, args);
        dap_send_error_response(s, request, "setVariable requires name and value");
        return;
    }

    if (var_ref != DAP_SCOPE_REF_GLOBALS &&
        dap_decode_scope_ref(var_ref, &frame_id, &scope_kind) < 0) {
        JS_FreeCString(s->ctx, name);
        JS_FreeCString(s->ctx, value_expr);
        JS_FreeValue(s->ctx, args);
        dap_send_error_response(s, request, "unsupported variablesReference");
        return;
    }

    eval_result = JS_EvaluateAtFrame(s->ctx, frame_id >= 0 ? frame_id : 0,
                                     value_expr, strlen(value_expr));
    if (JS_IsException(eval_result)) {
        JS_FreeCString(s->ctx, name);
        JS_FreeCString(s->ctx, value_expr);
        JS_FreeValue(s->ctx, args);
        JS_FreeValue(s->ctx, JS_GetException(s->ctx));
        dap_send_error_response(s, request, "failed to evaluate setVariable value");
        return;
    }

    if (var_ref == DAP_SCOPE_REF_GLOBALS) {
        ret = JS_SetGlobalVariable(s->ctx, name, eval_result);
    } else if (scope_kind == DAP_SCOPE_KIND_LOCAL) {
        ret = JS_SetFrameLocal(s->ctx, frame_id, name, eval_result);
    } else if (scope_kind == DAP_SCOPE_KIND_CLOSURE) {
        ret = JS_SetFrameClosure(s->ctx, frame_id, name, eval_result);
    } else {
        ret = -1;
    }

    JS_FreeCString(s->ctx, name);
    JS_FreeCString(s->ctx, value_expr);
    JS_FreeValue(s->ctx, args);
    if (ret < 0) {
        JS_FreeValue(s->ctx, eval_result);
        dap_send_error_response(s, request, "failed to set variable");
        return;
    }

    body = JS_NewObject(s->ctx);
    stringified = dap_stringify_value(s->ctx, eval_result);
    value_cstr = JS_ToCString(s->ctx, stringified);
    JS_SetPropertyStr(s->ctx, body, "value", JS_NewString(s->ctx, value_cstr ? value_cstr : ""));
    JS_SetPropertyStr(s->ctx, body, "variablesReference", JS_NewInt32(s->ctx, 0));
    if (value_cstr)
        JS_FreeCString(s->ctx, value_cstr);
    JS_FreeValue(s->ctx, stringified);
    JS_FreeValue(s->ctx, eval_result);
    dap_send_response(s, request, body);
}

static int dap_refresh_stop_exception(DAPServer *s) {
    if (!JS_IsUndefined(s->debug_state->stop_exception))
        return 0;
    s->debug_state->stop_exception = JS_PeekException(s->ctx);
    if (JS_IsUndefined(s->debug_state->stop_exception))
        return -1;
    s->debug_state->stop_exception_uncatchable =
        JS_IsUncatchableError(s->debug_state->stop_exception);
    return 0;
}

static void handle_exception_info(DAPServer *s, JSValue request) {
    JSValue body;
    JSValue details;
    JSValue stringified;
    JSValue name_val;
    const char *description;
    const char *type_name;

    if (!dap_is_stopped(s) || s->debug_state->stop_reason != JS_DEBUG_REASON_EXCEPTION) {
        dap_send_error_response(s, request, "exceptionInfo is only available for exception stops");
        return;
    }
    if (JS_IsUndefined(s->debug_state->stop_exception) && dap_refresh_stop_exception(s) < 0) {
        body = JS_NewObject(s->ctx);
        details = JS_NewObject(s->ctx);
        JS_SetPropertyStr(s->ctx, body, "exceptionId", JS_NewString(s->ctx, "Error"));
        JS_SetPropertyStr(s->ctx, body, "breakMode",
                          JS_NewString(s->ctx, s->debug_state->stop_exception_uncatchable ? "uncaught" : "always"));
        JS_SetPropertyStr(s->ctx, body, "description", JS_NewString(s->ctx, ""));
        JS_SetPropertyStr(s->ctx, details, "message", JS_NewString(s->ctx, ""));
        JS_SetPropertyStr(s->ctx, details, "typeName", JS_NewString(s->ctx, "Error"));
        JS_SetPropertyStr(s->ctx, body, "details", details);
        dap_send_response(s, request, body);
        return;
    }

    body = JS_NewObject(s->ctx);
    details = JS_NewObject(s->ctx);
    stringified = dap_stringify_value(s->ctx, s->debug_state->stop_exception);
    description = JS_ToCString(s->ctx, stringified);
    name_val = JS_GetPropertyStr(s->ctx, s->debug_state->stop_exception, "name");
    if (JS_IsString(name_val)) {
        type_name = JS_ToCString(s->ctx, name_val);
    } else {
        type_name = NULL;
    }

    JS_SetPropertyStr(s->ctx, body, "exceptionId",
                      JS_NewString(s->ctx, type_name ? type_name : "Error"));
    JS_SetPropertyStr(s->ctx, body, "breakMode",
                      JS_NewString(s->ctx, s->debug_state->stop_exception_uncatchable ? "uncaught" : "always"));
    JS_SetPropertyStr(s->ctx, body, "description",
                      JS_NewString(s->ctx, description ? description : ""));
    JS_SetPropertyStr(s->ctx, details, "message",
                      JS_NewString(s->ctx, description ? description : ""));
    JS_SetPropertyStr(s->ctx, details, "typeName",
                      JS_NewString(s->ctx, type_name ? type_name : "Error"));
    JS_SetPropertyStr(s->ctx, body, "details", details);

    if (type_name)
        JS_FreeCString(s->ctx, type_name);
    if (description)
        JS_FreeCString(s->ctx, description);
    JS_FreeValue(s->ctx, name_val);
    JS_FreeValue(s->ctx, stringified);
    dap_send_response(s, request, body);
}

static void handle_pause(DAPServer *s, JSValue request) {
    s->pause_requested = true;
    dap_send_response(s, request, JS_UNDEFINED);
    if (dap_is_stopped(s) && !s->debug_state->is_evaluating) {
        dap_pause_callback(JS_GetRuntime(s->ctx), s, JS_DEBUG_REASON_POLL, NULL);
    }
}

static void handle_disconnect(DAPServer *s, JSValue request) {
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    bool terminate = js_get_bool(s->ctx, args, "terminateDebuggee", false);
    JS_FreeValue(s->ctx, args);

    if (terminate) {
        s->terminate_requested = true;
        s->is_disconnecting = true;
        if (s->debug_state->paused)
            JS_DebugContinue(s->debug_state);
        dap_send_response(s, request, JS_UNDEFINED);
        return;
    }
    dap_send_response(s, request, JS_UNDEFINED);
    dap_detach_client(s);
}

static void handle_terminate(DAPServer *s, JSValue request) {
    s->terminate_requested = true;
    s->is_disconnecting = true;
    if (s->debug_state->paused)
        JS_DebugContinue(s->debug_state);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void process_message(DAPServer *s, JSValue msg) {
    const char *type = js_get_str(s->ctx, msg, "type");
    if (type && strcmp(type, "request") == 0) {
        const char *cmd = js_get_str(s->ctx, msg, "command");
        if (cmd) {
            if (strcmp(cmd, "initialize") == 0) handle_initialize(s, msg);
            else if (strcmp(cmd, "launch") == 0) handle_launch(s, msg);
            else if (strcmp(cmd, "attach") == 0) handle_attach(s, msg);
            else if (strcmp(cmd, "setBreakpoints") == 0) handle_set_breakpoints(s, msg);
            else if (strcmp(cmd, "setExceptionBreakpoints") == 0) handle_set_exception_breakpoints(s, msg);
            else if (strcmp(cmd, "configurationDone") == 0) handle_configuration_done(s, msg);
            else if (strcmp(cmd, "threads") == 0) handle_threads(s, msg);
            else if (strcmp(cmd, "continue") == 0) handle_continue(s, msg);
            else if (strcmp(cmd, "next") == 0) handle_next(s, msg);
            else if (strcmp(cmd, "stepIn") == 0) handle_step_in(s, msg);
            else if (strcmp(cmd, "stepOut") == 0) handle_step_out(s, msg);
            else if (strcmp(cmd, "stackTrace") == 0) handle_stack_trace(s, msg);
            else if (strcmp(cmd, "scopes") == 0) handle_scopes(s, msg);
            else if (strcmp(cmd, "variables") == 0) handle_variables(s, msg);
            else if (strcmp(cmd, "setVariable") == 0) handle_set_variable(s, msg);
            else if (strcmp(cmd, "evaluate") == 0) handle_evaluate(s, msg);
            else if (strcmp(cmd, "exceptionInfo") == 0) handle_exception_info(s, msg);
            else if (strcmp(cmd, "pause") == 0) handle_pause(s, msg);
            else if (strcmp(cmd, "disconnect") == 0) handle_disconnect(s, msg);
            else if (strcmp(cmd, "terminate") == 0) handle_terminate(s, msg);
            else dap_send_error_response(s, msg, "unsupported DAP request");
            JS_FreeCString(s->ctx, cmd);
        }
    }
    if (type) JS_FreeCString(s->ctx, type);
}

static int read_message(DAPServer *s) {
    char header[256];
    bool was_evaluating;
    int hidx = 0;
    while (hidx < sizeof(header) - 1) {
        if (s->transport->recv(s->transport, &header[hidx], 1) <= 0) {
            dap_detach_client(s);
            return 1;
        }
        if (hidx >= 3 && header[hidx] == '\n' && header[hidx-1] == '\r' && header[hidx-2] == '\n' && header[hidx-3] == '\r') {
            break;
        }
        hidx++;
    }
    header[hidx+1] = '\0';

    char *cl_ptr = strstr(header, "Content-Length: ");
    if (!cl_ptr) {
        dap_detach_client(s);
        return 1;
    }
    size_t len = strtol(cl_ptr + 16, NULL, 10);
    if (len == 0 || len > 1024 * 1024 * 10) {
        dap_detach_client(s);
        return 1;
    }

    char *buf = malloc(len + 1);
    if (!buf)
        return -1;

    size_t read_len = 0;
    while (read_len < len) {
        int r = s->transport->recv(s->transport, buf + read_len, len - read_len);
        if (r <= 0) {
            free(buf);
            dap_detach_client(s);
            return 1;
        }
        read_len += r;
    }
    buf[len] = '\0';
    dap_log_json(s, "<-", buf, len);

    was_evaluating = s->debug_state->is_evaluating;
    s->debug_state->is_evaluating = true;
    JSValue msg = JS_ParseJSON(s->ctx, buf, len, "<dap>");
    free(buf);

    if (JS_IsException(msg)) {
        s->debug_state->is_evaluating = was_evaluating;
        JS_GetException(s->ctx); // clear
        dap_detach_client(s);
        return 1;
    }

    process_message(s, msg);
    s->debug_state->is_evaluating = was_evaluating;
    JS_FreeValue(s->ctx, msg);
    return 0;
}

static int dap_interrupt_handler(JSRuntime *rt, void *opaque) {
    DAPServer *s = (DAPServer *)opaque;
    (void)rt;

    if (s->terminate_requested)
        return 1;

    if (!s->transport->poll || s->transport->poll(s->transport, 0) <= 0)
        return 0;

    if (read_message(s) < 0)
        return 1;
    if (s->terminate_requested)
        return 1;
    if (s->pause_requested)
        JS_DebugPause(s->debug_state);
    return 0;
}

static int dap_pause_callback(JSRuntime *rt, void *opaque, JSDebugReason reason, const uint8_t *pc) {
    DAPServer *s = (DAPServer*)opaque;
    const char *stop_reason;
    (void)rt;
    (void)pc;
    if (s->is_disconnecting || s->debug_state->is_evaluating)
        return 0;
    if (!s->initialized) {
        s->pause_requested = false;
        if (s->debug_state->paused)
            JS_DebugContinue(s->debug_state);
        return 0;
    }

    JSValue body = JS_NewObject(s->ctx);
    if (!s->debug_state->has_stop_state) {
        s->debug_state->has_stop_state = true;
        s->debug_state->stop_reason = reason;
    }
    if (s->entry_stop_pending) {
        s->entry_stop_pending = false;
        s->debug_state->stop_reason = JS_DEBUG_REASON_ENTRY;
    }
    stop_reason = dap_stop_reason_string(s);
    JS_SetPropertyStr(s->ctx, body, "reason", JS_NewString(s->ctx, stop_reason));
    JS_SetPropertyStr(s->ctx, body, "threadId", JS_NewInt32(s->ctx, 1));
    if (s->debug_state->stop_reason == JS_DEBUG_REASON_BREAKPOINT &&
        s->debug_state->stop_breakpoint_id > 0) {
        JSValue hits = JS_NewArray(s->ctx);
        JS_SetPropertyUint32(s->ctx, hits, 0, JS_NewInt32(s->ctx, s->debug_state->stop_breakpoint_id));
        JS_SetPropertyStr(s->ctx, body, "hitBreakpointIds", hits);
    }
    dap_send_event(s, "stopped", body);
    s->pause_requested = false;

    while (s->debug_state->paused && !s->is_disconnecting) {
        if (read_message(s) < 0)
            break;
    }
    return 0;
}

DAPServer *DAP_NewServer(JSContext *ctx, DAPTransport *transport) {
    DAPServer *s = js_mallocz(ctx, sizeof(DAPServer));
    s->ctx = ctx;
    s->transport = transport;
    s->debug_state = JS_NewDebugState(ctx);
    dap_clear_launch_config(s);
    s->debug_state->pause_callback = dap_pause_callback;
    s->debug_state->user_opaque = s;
    JS_SetContextOpaque(ctx, s);
    JS_SetDebugHandler(JS_GetRuntime(ctx), js_debug_handler, s->debug_state);
    JS_SetInterruptHandler(JS_GetRuntime(ctx), dap_interrupt_handler, s);
    return s;
}

void DAP_FreeServer(DAPServer *server) {
    JS_SetInterruptHandler(JS_GetRuntime(server->ctx), NULL, NULL);
    JS_SetDebugHandler(JS_GetRuntime(server->ctx), NULL, NULL);
    dap_clear_launch_config(server);
    if (server->log_fp)
        fclose(server->log_fp);
    if (server->transport && server->transport->close) {
        server->transport->close(server->transport);
    }
    JS_FreeDebugState(server->debug_state);
    js_free(server->ctx, server);
}

int DAP_WaitForLaunch(DAPServer *server) {
    while (!server->configuration_done && !server->terminate_requested) {
        if (read_message(server) < 0)
            return -1;
    }
    return (!server->terminate_requested && server->launch_received) ? 0 : -1;
}

void DAP_ProcessExited(DAPServer *server, int exit_code) {
    JSValue body = JS_NewObject(server->ctx);
    JS_SetPropertyStr(server->ctx, body, "exitCode", JS_NewInt32(server->ctx, exit_code));
    dap_send_event(server, "exited", body);
    dap_send_event(server, "terminated", JS_UNDEFINED);
}

void DAP_SendOutput(DAPServer *server, const char *category, const char *output) {
    JSValue body = JS_NewObject(server->ctx);
    JS_SetPropertyStr(server->ctx, body, "category", JS_NewString(server->ctx, category));
    JS_SetPropertyStr(server->ctx, body, "output", JS_NewString(server->ctx, output));
    dap_send_event(server, "output", body);
}

int DAP_SetLogFile(DAPServer *server, const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;
    server->log_fp = fp;
    return 0;
}

void DAP_PrepareExecution(DAPServer *server) {
    server->execution_started = true;
    if (server->launch_stop_on_entry) {
        server->entry_stop_pending = true;
        server->debug_state->stop_on_entry = true;
        JS_DebugStepInto(server->debug_state);
    }
}

const char *DAP_GetLaunchProgram(DAPServer *server) {
    return server->launch_program;
}

const char * const *DAP_GetLaunchArgs(DAPServer *server) {
    return (const char * const *)server->launch_args;
}

int DAP_GetLaunchArgc(DAPServer *server) {
    return server->launch_argc;
}

int DAP_GetLaunchModule(DAPServer *server) {
    return server->launch_module;
}
