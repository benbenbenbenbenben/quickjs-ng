#include "qjs-dap.h"
#include "qjs-debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

struct DAPServer {
    JSContext *ctx;
    DAPTransport *transport;
    JSDebugState *debug_state;
    bool configuration_done;
    bool is_disconnecting;
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
static void stdio_close(DAPTransport *t) {
    js_free(NULL, t);
}
DAPTransport *DAP_NewStdioTransport(void) {
    DAPTransport *t = malloc(sizeof(DAPTransport));
    t->recv = stdio_recv;
    t->send = stdio_send;
    t->close = stdio_close;
    return t;
}

/* --- TCP Transport implementation --- */
struct TCPTransportData {
    int server_fd;
    int client_fd;
};

static int tcp_recv(DAPTransport *t, char *buf, size_t len) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
    if (d->client_fd < 0) return -1;
    ssize_t r = recv(d->client_fd, buf, len, 0);
    return r == 0 ? -1 : (int)r;
}

static int tcp_send(DAPTransport *t, const char *buf, size_t len) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
    if (d->client_fd < 0) return -1;
    ssize_t w = send(d->client_fd, buf, len, 0);
    return w == len ? 0 : -1;
}

static void tcp_close(DAPTransport *t) {
    struct TCPTransportData *d = (struct TCPTransportData *)t->opaque;
#ifdef _WIN32
    if (d->client_fd >= 0) closesocket(d->client_fd);
    if (d->server_fd >= 0) closesocket(d->server_fd);
    WSACleanup();
#else
    if (d->client_fd >= 0) close(d->client_fd);
    if (d->server_fd >= 0) close(d->server_fd);
#endif
    free(d);
    free(t);
}

DAPTransport *DAP_NewTCPTransport(int port) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
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
    
    fprintf(stderr, "Waiting for DAP client on port %d...\n", port);
    
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
#ifdef _WIN32
        closesocket(server_fd);
#else
        close(server_fd);
#endif
        return NULL;
    }
    
    fprintf(stderr, "DAP client connected.\n");
    
    DAPTransport *t = malloc(sizeof(DAPTransport));
    struct TCPTransportData *d = malloc(sizeof(struct TCPTransportData));
    d->server_fd = server_fd;
    d->client_fd = client_fd;
    
    t->opaque = d;
    t->recv = tcp_recv;
    t->send = tcp_send;
    t->close = tcp_close;
    
    return t;
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

/* --- Message processing --- */
static void handle_initialize(DAPServer *s, JSValue request) {
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "supportsConfigurationDoneRequest", JS_NewBool(s->ctx, true));
    JS_SetPropertyStr(s->ctx, body, "supportsEvaluateForHovers", JS_NewBool(s->ctx, true));
    dap_send_response(s, request, body);
    dap_send_event(s, "initialized", JS_UNDEFINED);
}

static void handle_launch(DAPServer *s, JSValue request) {
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
    s->configuration_done = true;
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_continue(DAPServer *s, JSValue request) {
    JS_DebugContinue(s->debug_state);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_next(DAPServer *s, JSValue request) {
    JS_DebugStepOver(s->debug_state);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_step_in(DAPServer *s, JSValue request) {
    JS_DebugStepInto(s->debug_state);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_step_out(DAPServer *s, JSValue request) {
    JS_DebugStepOut(s->debug_state);
    dap_send_response(s, request, JS_UNDEFINED);
}

static void handle_stack_trace(DAPServer *s, JSValue request) {
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
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    int frame_id = js_get_int(s->ctx, args, "frameId", 0);
    JS_FreeValue(s->ctx, args);
    
    JSValue scopes = JS_NewArray(s->ctx);
    
    // Locals scope (variableReference = frame_id + 1000)
    JSValue loc_scope = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, loc_scope, "name", JS_NewString(s->ctx, "Locals"));
    JS_SetPropertyStr(s->ctx, loc_scope, "variablesReference", JS_NewInt32(s->ctx, frame_id + 1000));
    JS_SetPropertyStr(s->ctx, loc_scope, "expensive", JS_NewBool(s->ctx, false));
    JS_SetPropertyUint32(s->ctx, scopes, 0, loc_scope);
    
    // Globals scope (variableReference = 1)
    JSValue glb_scope = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, glb_scope, "name", JS_NewString(s->ctx, "Globals"));
    JS_SetPropertyStr(s->ctx, glb_scope, "variablesReference", JS_NewInt32(s->ctx, 1));
    JS_SetPropertyStr(s->ctx, glb_scope, "expensive", JS_NewBool(s->ctx, true));
    JS_SetPropertyUint32(s->ctx, scopes, 1, glb_scope);
    
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "scopes", scopes);
    dap_send_response(s, request, body);
}

static void handle_variables(DAPServer *s, JSValue request) {
    JSValue args = JS_GetPropertyStr(s->ctx, request, "arguments");
    int var_ref = js_get_int(s->ctx, args, "variablesReference", 0);
    JS_FreeValue(s->ctx, args);
    
    JSValue vars = JS_NewArray(s->ctx);
    
    if (var_ref == 1) { // Globals
        JSVarInfo *vinfo = NULL;
        int count = JS_GetGlobalVariables(s->ctx, &vinfo);
        if (count > 0) {
            for (int i = 0; i < count; i++) {
                JSValue out_var = JS_NewObject(s->ctx);
                const char *vname = JS_AtomToCString(s->ctx, vinfo[i].name);
                JS_SetPropertyStr(s->ctx, out_var, "name", JS_NewString(s->ctx, vname));
                JS_FreeCString(s->ctx, vname);
                
                JSValue str_val = JS_ToString(s->ctx, vinfo[i].value);
                const char *val_cstr = JS_ToCString(s->ctx, str_val);
                JS_SetPropertyStr(s->ctx, out_var, "value", JS_NewString(s->ctx, val_cstr ? val_cstr : ""));
                if (val_cstr) JS_FreeCString(s->ctx, val_cstr);
                JS_FreeValue(s->ctx, str_val);
                
                JS_SetPropertyStr(s->ctx, out_var, "variablesReference", JS_NewInt32(s->ctx, 0));
                JS_SetPropertyUint32(s->ctx, vars, i, out_var);
                
                JS_FreeAtom(s->ctx, vinfo[i].name);
                JS_FreeValue(s->ctx, vinfo[i].value);
            }
            js_free(s->ctx, vinfo);
        }
    } else if (var_ref >= 1000) { // Locals
        int frame_id = var_ref - 1000;
        JSVarInfo *vinfo = NULL;
        int count = JS_GetFrameLocals(s->ctx, frame_id, &vinfo);
        if (count > 0) {
            for (int i = 0; i < count; i++) {
                JSValue out_var = JS_NewObject(s->ctx);
                const char *vname = JS_AtomToCString(s->ctx, vinfo[i].name);
                JS_SetPropertyStr(s->ctx, out_var, "name", JS_NewString(s->ctx, vname));
                JS_FreeCString(s->ctx, vname);
                
                JSValue str_val = JS_ToString(s->ctx, vinfo[i].value);
                const char *val_cstr = JS_ToCString(s->ctx, str_val);
                JS_SetPropertyStr(s->ctx, out_var, "value", JS_NewString(s->ctx, val_cstr ? val_cstr : ""));
                if (val_cstr) JS_FreeCString(s->ctx, val_cstr);
                JS_FreeValue(s->ctx, str_val);
                
                JS_SetPropertyStr(s->ctx, out_var, "variablesReference", JS_NewInt32(s->ctx, 0));
                JS_SetPropertyUint32(s->ctx, vars, i, out_var);
                
                JS_FreeAtom(s->ctx, vinfo[i].name);
                JS_FreeValue(s->ctx, vinfo[i].value);
            }
            js_free(s->ctx, vinfo);
        }
    }
    
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "variables", vars);
    dap_send_response(s, request, body);
}

static void handle_evaluate(DAPServer *s, JSValue request) {
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

static void handle_disconnect(DAPServer *s, JSValue request) {
    s->is_disconnecting = true;
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
            else if (strcmp(cmd, "setBreakpoints") == 0) handle_set_breakpoints(s, msg);
            else if (strcmp(cmd, "setExceptionBreakpoints") == 0) handle_set_exception_breakpoints(s, msg);
            else if (strcmp(cmd, "configurationDone") == 0) handle_configuration_done(s, msg);
            else if (strcmp(cmd, "continue") == 0) handle_continue(s, msg);
            else if (strcmp(cmd, "next") == 0) handle_next(s, msg);
            else if (strcmp(cmd, "stepIn") == 0) handle_step_in(s, msg);
            else if (strcmp(cmd, "stepOut") == 0) handle_step_out(s, msg);
            else if (strcmp(cmd, "stackTrace") == 0) handle_stack_trace(s, msg);
            else if (strcmp(cmd, "scopes") == 0) handle_scopes(s, msg);
            else if (strcmp(cmd, "variables") == 0) handle_variables(s, msg);
            else if (strcmp(cmd, "evaluate") == 0) handle_evaluate(s, msg);
            else if (strcmp(cmd, "disconnect") == 0) handle_disconnect(s, msg);
            else dap_send_response(s, msg, JS_UNDEFINED); // Ignore unknown
            JS_FreeCString(s->ctx, cmd);
        }
    }
    if (type) JS_FreeCString(s->ctx, type);
}

static int read_message(DAPServer *s) {
    char header[256];
    int hidx = 0;
    while (hidx < sizeof(header) - 1) {
        if (s->transport->recv(s->transport, &header[hidx], 1) <= 0) return -1;
        if (hidx >= 3 && header[hidx] == '\n' && header[hidx-1] == '\r' && header[hidx-2] == '\n' && header[hidx-3] == '\r') {
            break;
        }
        hidx++;
    }
    header[hidx+1] = '\0';
    
    char *cl_ptr = strstr(header, "Content-Length: ");
    if (!cl_ptr) return -1;
    size_t len = strtol(cl_ptr + 16, NULL, 10);
    if (len == 0 || len > 1024 * 1024 * 10) return -1; // 10MB limit
    
    char *buf = malloc(len + 1);
    if (!buf) return -1;
    
    size_t read_len = 0;
    while (read_len < len) {
        int r = s->transport->recv(s->transport, buf + read_len, len - read_len);
        if (r <= 0) {
            free(buf);
            return -1;
        }
        read_len += r;
    }
    buf[len] = '\0';
    
    JSValue msg = JS_ParseJSON(s->ctx, buf, len, "<dap>");
    free(buf);
    
    if (JS_IsException(msg)) {
        JS_GetException(s->ctx); // clear
        return -1;
    }
    
    process_message(s, msg);
    JS_FreeValue(s->ctx, msg);
    return 0;
}

static int dap_pause_callback(JSRuntime *rt, void *opaque, JSDebugReason reason, const uint8_t *pc) {
    DAPServer *s = (DAPServer*)opaque;
    if (s->is_disconnecting) return 0;
    
    JSValue body = JS_NewObject(s->ctx);
    JS_SetPropertyStr(s->ctx, body, "reason", JS_NewString(s->ctx, "step"));
    JS_SetPropertyStr(s->ctx, body, "threadId", JS_NewInt32(s->ctx, 1));
    dap_send_event(s, "stopped", body);
    
    while (s->debug_state->paused && !s->is_disconnecting) {
        if (read_message(s) < 0) {
            s->is_disconnecting = true;
            break;
        }
    }
    return 0;
}

DAPServer *DAP_NewServer(JSContext *ctx, DAPTransport *transport) {
    DAPServer *s = js_mallocz(ctx, sizeof(DAPServer));
    s->ctx = ctx;
    s->transport = transport;
    s->debug_state = JS_NewDebugState(ctx);
    s->debug_state->pause_callback = dap_pause_callback;
    s->debug_state->user_opaque = s;
    JS_SetContextOpaque(ctx, s);
    JS_SetDebugHandler(JS_GetRuntime(ctx), js_debug_handler, s->debug_state);
    return s;
}

void DAP_FreeServer(DAPServer *server) {
    if (server->transport && server->transport->close) {
        server->transport->close(server->transport);
    }
    JS_FreeDebugState(server->debug_state);
    js_free(server->ctx, server);
}

int DAP_WaitForLaunch(DAPServer *server) {
    while (!server->configuration_done && !server->is_disconnecting) {
        if (read_message(server) < 0) return -1;
    }
    return 0;
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
