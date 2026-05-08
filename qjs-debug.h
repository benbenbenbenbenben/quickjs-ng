#ifndef QJS_DEBUG_H
#define QJS_DEBUG_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JSBreakpoint {
    JSAtom filename;
    int line;
    int id;
    bool verified;
    char *condition;     /* optional JS expression */
} JSBreakpoint;

typedef struct JSDebugState {
    JSContext *ctx;
    JSBreakpoint *breakpoints;
    int bp_count;
    int bp_capacity;
    int next_bp_id;

    /* Stepping state */
    enum { STEP_NONE, STEP_INTO, STEP_OVER, STEP_OUT } step_mode;
    int step_depth;      /* call depth at step start */
    uint32_t step_pc;    /* PC to step past (for step-over within same frame) */
    
    bool pause_on_exceptions;

    /* Current pause state */
    bool paused;
    bool is_evaluating;              /* prevent re-entrancy during evaluate */
    bool stop_on_entry;
    bool has_stop_state;
    bool stop_exception_uncatchable;
    JSDebugReason stop_reason;
    int stop_breakpoint_id;
    JSValue stop_exception;
    JSDebugHandler *pause_callback;  /* called to resume */
    void *user_opaque;               /* opaque passed to pause_callback */
    
    /* Cached state for fast breakpoint checking */
    JSValueConst last_func;
    uint32_t last_pc;
    int last_line;
} JSDebugState;

JS_EXTERN JSDebugState *JS_NewDebugState(JSContext *ctx);
JS_EXTERN void JS_FreeDebugState(JSDebugState *ds);

/* Debug handler implementation hooked to JSRuntime */
JS_EXTERN int js_debug_handler(JSRuntime *rt, void *opaque, JSDebugReason reason, const uint8_t *pc);

/* Breakpoint management */
JS_EXTERN int JS_AddBreakpoint(JSDebugState *ds, const char *filename, int line, const char *condition);
JS_EXTERN void JS_ClearBreakpoints(JSDebugState *ds, const char *filename);

/* Stepping control */
JS_EXTERN void JS_DebugStepInto(JSDebugState *ds);
JS_EXTERN void JS_DebugStepOver(JSDebugState *ds);
JS_EXTERN void JS_DebugStepOut(JSDebugState *ds);
JS_EXTERN void JS_DebugContinue(JSDebugState *ds);

/* Trigger pause manually */
JS_EXTERN void JS_DebugPause(JSDebugState *ds);

#ifdef __cplusplus
}
#endif

#endif /* QJS_DEBUG_H */
