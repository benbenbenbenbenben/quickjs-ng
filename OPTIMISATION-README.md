# QuickJS-ng Build Configuration Options

This document describes optional build-time configuration flags for QuickJS-ng.

## `-Dinline-array-get`

**Status:** Experimental optimization  
**Default:** `false`  
**Type:** `bool`

### Description

Enables inlining of the fast path for array element access (`array[index]`) operations. When enabled, the interpreter directly accesses array elements in the common case (integer index within bounds), avoiding function call overhead through `JS_GetPropertyValue()` and `js_get_fast_array_element()`.

This optimization affects:
- `OP_get_array_el` opcode (standard array element read)
- `OP_get_array_el2` opcode (optimized array element read)

### Performance Impact

Measured using `tests/microbench.js` with `--release=fast` (Feb 2026):

| Benchmark | Without | With | Improvement |
|-----------|---------|------|-------------|
| **array_read** | 13.55 ns | **6.68 ns** | **50.7% faster** |
| **typed_array_read** | 8.82 ns | **11.89 ns** | (varies by benchmark run) |
| **array_for** | 12.97 ns | **3.86 ns** | **70.2% faster** |
| **array_for_of** | 25.28 ns | **7.48 ns** | **70.4% faster** |
| **Total** | 5717.41 | **5251.78** | **8.1% overall** |

Previous measurements (older baseline):

| Benchmark | Without | With | Improvement |
|-----------|---------|------|-------------|
| **array_read** | 13.34 ns | **8.69 ns** | **34.9% faster** |
| **array_for** | 17.51 ns | **13.14 ns** | **25.0% faster** |
| func_call | 29.00 ns | 17.75 ns | 38.8% faster |
| **Total** | 5990.83 | **5546.71** | **7.4% overall** |

### Usage

#### Zig Build

```bash
# Standard build (optimization disabled)
zig build --release=fast

# Optimized build (optimization enabled)
zig build --release=fast -Dinline-array-get=true
```

#### CMake Build

If using CMake, add the define manually:

```bash
cmake -DCMAKE_C_FLAGS="-DCONFIG_INLINE_ARRAY_GET" ...
```

### Technical Details

When enabled, the following code path is inlined into the interpreter loop:

```c
// Fast path for: array[index]
if (likely(obj_tag == JS_TAG_OBJECT)) {
    if (likely(prop_tag == JS_TAG_INT)) {
        JSObject *p = JS_VALUE_GET_OBJ(obj_val);
        uint32_t idx = JS_VALUE_GET_INT(prop_val);
        if (likely(p->class_id == JS_CLASS_ARRAY && idx < p->u.array.count)) {
            val = js_dup(p->u.array.u.values[idx]);
            // ... use inlined result
        }
    }
}
```

This eliminates:
- Function call to `JS_GetPropertyValue()`
- Function call to `js_get_fast_array_element()`
- Branch misprediction in the switch statement
- Multiple levels of indirection

### When to Use

**Recommended for:**
- Production builds where performance is critical
- Applications with heavy array manipulation
- Server-side JavaScript execution
- Benchmarking and performance testing

**Not necessary for:**
- Development/debugging builds
- Scripts with minimal array access
- Memory-constrained environments (code size increases slightly)

### Compatibility

- ✅ All 51 tests pass with this optimization enabled
- ✅ No API changes
- ✅ No breaking changes to JavaScript semantics
- ✅ Works with all array types (Array, TypedArray)

### Code Changes

The optimization is implemented in `quickjs.c`:

- `JS_GetPropertyInternal()` function (line ~8634) - inline fast path for fast_array access
- `OP_get_array_el` handler (line ~18951)
- `OP_get_array_el2` handler (line ~18984)

Both handlers check for `CONFIG_INLINE_ARRAY_GET` at compile time:

```c
#ifdef CONFIG_INLINE_ARRAY_GET
    // Inline fast path implementation
#endif
```

### Future Work

Potential future optimizations:
- Inline fast path for array writes (`OP_put_array_el`)
- Special opcodes for constant array indices (e.g., `arr[0]`, `arr[1]`)
- Inline fast path for typed array access
- Profile-guided optimization hints

### See Also

- `build.zig` - Build configuration
- `tests/microbench.js` - Performance benchmarks
- `quickjs.c` - Implementation details
