#include "qjs-debug.h"
#include <string.h>
#include <stdlib.h>

static int js_debug_get_top_frame_line(JSContext *ctx, int *line)
{
    JSStackFrameInfo *frames = NULL;
    int num_frames = JS_GetStackTrace(ctx, &frames, 1);
    if (num_frames <= 0)
        return -1;

    *line = frames[0].line;
    JS_FreeValue(ctx, frames[0].func);
    if (frames[0].filename != JS_ATOM_NULL)
        JS_FreeAtom(ctx, frames[0].filename);
    if (frames[0].func_name != JS_ATOM_NULL)
        JS_FreeAtom(ctx, frames[0].func_name);
    js_free(ctx, frames);
    return 0;
}

static void js_debug_clear_stop_state(JSDebugState *ds)
{
    if (JS_IsException(ds->stop_exception) || JS_IsUninitialized(ds->stop_exception))
        ds->stop_exception = JS_UNDEFINED;
    if (!JS_IsUndefined(ds->stop_exception))
        JS_FreeValue(ds->ctx, ds->stop_exception);
    ds->stop_exception = JS_UNDEFINED;
    ds->stop_exception_uncatchable = false;
    ds->stop_breakpoint_id = 0;
    ds->stop_reason = JS_DEBUG_REASON_POLL;
    ds->has_stop_state = false;
}

static void js_debug_capture_exception_state(JSContext *ctx, JSDebugState *ds)
{
    JSValue exception;

    if (!JS_HasException(ctx))
        return;

    exception = JS_PeekException(ctx);
    if (JS_IsUndefined(exception))
        return;
    ds->stop_exception_uncatchable = JS_IsUncatchableError(exception);
    ds->stop_exception = exception;
}

JSDebugState *JS_NewDebugState(JSContext *ctx) {
    JSDebugState *ds = js_mallocz(ctx, sizeof(JSDebugState));
    if (!ds) return NULL;
    ds->ctx = ctx;
    ds->step_mode = STEP_NONE;
    ds->breakpoints = NULL;
    ds->bp_count = 0;
    ds->bp_capacity = 0;
    ds->next_bp_id = 1;
    ds->is_evaluating = false;
    ds->pause_on_exceptions = false;
    ds->stop_on_entry = false;
    ds->last_func = JS_UNDEFINED;
    ds->last_pc = 0;
    ds->last_line = -1;
    ds->stop_exception = JS_UNDEFINED;
    ds->stop_reason = JS_DEBUG_REASON_POLL;
    return ds;
}

void JS_FreeDebugState(JSDebugState *ds) {
    if (!ds) return;
    js_debug_clear_stop_state(ds);
    for (int i = 0; i < ds->bp_count; i++) {
        JS_FreeAtom(ds->ctx, ds->breakpoints[i].filename);
        if (ds->breakpoints[i].condition) {
            js_free(ds->ctx, ds->breakpoints[i].condition);
        }
    }
    if (ds->breakpoints) {
        js_free(ds->ctx, ds->breakpoints);
    }
    js_free(ds->ctx, ds);
}

int JS_AddBreakpoint(JSDebugState *ds, const char *filename, int line, const char *condition) {
    if (ds->bp_count >= ds->bp_capacity) {
        int new_cap = ds->bp_capacity == 0 ? 8 : ds->bp_capacity * 2;
        JSBreakpoint *new_bp = js_realloc(ds->ctx, ds->breakpoints, new_cap * sizeof(JSBreakpoint));
        if (!new_bp) return -1;
        ds->breakpoints = new_bp;
        ds->bp_capacity = new_cap;
    }
    JSBreakpoint *bp = &ds->breakpoints[ds->bp_count++];
    bp->filename = JS_NewAtom(ds->ctx, filename);
    bp->line = line;
    bp->id = ds->next_bp_id++;
    bp->verified = true;
    bp->condition = condition ? js_strdup(ds->ctx, condition) : NULL;
    return bp->id;
}

void JS_ClearBreakpoints(JSDebugState *ds, const char *filename) {
    JSAtom fn_atom = JS_NewAtom(ds->ctx, filename);
    int write_idx = 0;
    for (int i = 0; i < ds->bp_count; i++) {
        if (ds->breakpoints[i].filename == fn_atom) {
            JS_FreeAtom(ds->ctx, ds->breakpoints[i].filename);
            if (ds->breakpoints[i].condition) {
                js_free(ds->ctx, ds->breakpoints[i].condition);
            }
        } else {
            ds->breakpoints[write_idx++] = ds->breakpoints[i];
        }
    }
    ds->bp_count = write_idx;
    JS_FreeAtom(ds->ctx, fn_atom);
}

void JS_DebugStepInto(JSDebugState *ds) {
    js_debug_clear_stop_state(ds);
    ds->step_mode = STEP_INTO;
    ds->step_depth = JS_GetStackDepth(ds->ctx);
    ds->step_pc = 0;
    ds->paused = false;
}

void JS_DebugStepOver(JSDebugState *ds) {
    js_debug_clear_stop_state(ds);
    ds->stop_on_entry = false;
    ds->step_mode = STEP_OVER;
    ds->step_depth = JS_GetStackDepth(ds->ctx);
    ds->step_pc = 0; // The actual PC is handled dynamically in poll
    ds->paused = false;
}

void JS_DebugStepOut(JSDebugState *ds) {
    js_debug_clear_stop_state(ds);
    ds->stop_on_entry = false;
    ds->step_mode = STEP_OUT;
    ds->step_depth = JS_GetStackDepth(ds->ctx);
    ds->step_pc = 0;
    ds->paused = false;
}

void JS_DebugContinue(JSDebugState *ds) {
    js_debug_clear_stop_state(ds);
    ds->stop_on_entry = false;
    ds->step_mode = STEP_NONE;
    ds->paused = false;
}

void JS_DebugPause(JSDebugState *ds) {
    ds->paused = true;
}

static bool evaluate_condition(JSContext *ctx, JSDebugState *ds, const char *condition) {
    ds->is_evaluating = true;
    JSValue ret = JS_EvaluateAtFrame(ctx, 0, condition, strlen(condition));
    ds->is_evaluating = false;
    bool is_truthy = JS_ToBool(ctx, ret);
    JS_FreeValue(ctx, ret);
    return is_truthy;
}

int js_debug_handler(JSRuntime *rt, void *opaque, JSDebugReason reason, const uint8_t *pc) {
    JSDebugState *ds = (JSDebugState *)opaque;
    if (!ds || !ds->ctx || ds->is_evaluating) return 0;
    JSContext *ctx = ds->ctx;

    bool should_pause = false;
    JSDebugReason pause_reason = reason;
    int stop_breakpoint_id = 0;

    if (reason == JS_DEBUG_REASON_DEBUGGER) {
        should_pause = true;
        js_debug_get_top_frame_line(ctx, &ds->last_line);
    } else if (reason == JS_DEBUG_REASON_EXCEPTION) {
        if (ds->pause_on_exceptions) {
            should_pause = true;
            js_debug_get_top_frame_line(ctx, &ds->last_line);
        }
    } else if (reason == JS_DEBUG_REASON_POLL || reason == JS_DEBUG_REASON_STEP) {
        // To get the line number, we need the current frame's function.
        // We can get it via JS_GetStackTrace with 1 frame.
        JSStackFrameInfo *frames = NULL;
        int num_frames = JS_GetStackTrace(ctx, &frames, 1);
        if (num_frames > 0) {
            JSStackFrameInfo *frame = &frames[0];
            int current_depth = JS_GetStackDepth(ctx);
            
            // Check step conditions
            if (ds->step_mode != STEP_NONE) {
                if (ds->step_mode == STEP_INTO) {
                    if (frame->line != ds->last_line || current_depth != ds->step_depth) {
                        should_pause = true;
                        pause_reason = ds->stop_on_entry ? JS_DEBUG_REASON_ENTRY : JS_DEBUG_REASON_STEP;
                    }
                } else if (ds->step_mode == STEP_OVER) {
                    if (current_depth < ds->step_depth) {
                        should_pause = true;
                        pause_reason = JS_DEBUG_REASON_STEP;
                    } else if (current_depth == ds->step_depth && frame->line != ds->last_line) {
                        should_pause = true;
                        pause_reason = JS_DEBUG_REASON_STEP;
                    }
                } else if (ds->step_mode == STEP_OUT) {
                    if (current_depth < ds->step_depth) {
                        should_pause = true;
                        pause_reason = JS_DEBUG_REASON_STEP;
                    }
                }
            }

            // Check breakpoints if line changed
            if (!should_pause && frame->line != ds->last_line && frame->filename != JS_ATOM_NULL) {
                for (int i = 0; i < ds->bp_count; i++) {
                    JSBreakpoint *bp = &ds->breakpoints[i];
                    if (bp->filename == frame->filename && bp->line == frame->line) {
                        if (bp->condition) {
                            if (evaluate_condition(ctx, ds, bp->condition)) {
                                should_pause = true;
                                pause_reason = JS_DEBUG_REASON_BREAKPOINT;
                                stop_breakpoint_id = bp->id;
                                break;
                            }
                        } else {
                            should_pause = true;
                            pause_reason = JS_DEBUG_REASON_BREAKPOINT;
                            stop_breakpoint_id = bp->id;
                            break;
                        }
                    }
                }
            }
            
            ds->last_line = frame->line;
            JS_FreeValue(ctx, frame->func);
            if (frame->filename != JS_ATOM_NULL) JS_FreeAtom(ctx, frame->filename);
            if (frame->func_name != JS_ATOM_NULL) JS_FreeAtom(ctx, frame->func_name);
            js_free(ctx, frames);
        }
    }

    if (ds->paused || should_pause) {
        ds->paused = true;
        ds->step_mode = STEP_NONE; // Reset stepping once paused
        ds->stop_on_entry = false;
        js_debug_clear_stop_state(ds);
        ds->has_stop_state = true;
        ds->stop_reason = pause_reason;
        ds->stop_breakpoint_id = stop_breakpoint_id;
        if (pause_reason == JS_DEBUG_REASON_EXCEPTION)
            js_debug_capture_exception_state(ctx, ds);
        if (ds->pause_callback) {
            ds->pause_callback(rt, ds->user_opaque ? ds->user_opaque : opaque, pause_reason, pc);
        }
    }

    return 0;
}
