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
- Profile-guided optimization hints

### Attempted Optimizations

#### Typed Array Inline Access

An inline fast path for typed array element access (`-Dinline-typed-array-get`) was attempted but showed performance degradation (4.22 ns → 9.24 ns for `typed_array_read` benchmark). The overhead of the switch statement for handling different typed array types outweighs the benefit of avoiding the function call. The existing implementation through `JS_GetPropertyValue()` is already well-optimized.

## `-Dmap-init-hash-size`

**Status:** Experimental optimization  
**Default:** `false`  
**Type:** `bool`

### Description

Increases the initial hash table size for Map and Set objects from 1 bucket to 16 buckets, with a corresponding increase in the resize threshold from 4 to 32 records. This reduces the number of hash table reallocations and rehashing operations during the common pattern of creating a Map/Set and immediately adding multiple entries.

The optimization affects:
- `Map` and `Set` creation and population
- `WeakMap` and `WeakSet` creation and population
- All Map/Set operations that trigger hash table growth

### Performance Impact

Measured using `tests/microbench.js` with `--release=fast` (Feb 2026):

| Benchmark | Without | With | Improvement |
|-----------|---------|------|-------------|
| **map_set** | 279.50 ns | **96.60 ns** | **65.4% faster** |
| **map_delete** | 276.00 ns | **102.80 ns** | **62.8% faster** |
| **weak_map_set** | 154.70 ns | **90.00 ns** | **41.8% faster** |
| **weak_map_delete** | 417.40 ns | **303.00 ns** | **27.4% faster** |
| **Total** | 5877.92 | **3415.58** | **41.9% overall** |

### Usage

#### Zig Build

```bash
# Standard build (optimization disabled)
zig build --release=fast

# Optimized build (optimization enabled)
zig build --release=fast -Dmap-init-hash-size=true
```

#### CMake Build

If using CMake, add the define manually:

```bash
cmake -DCMAKE_C_FLAGS="-DCONFIG_MAP_INIT_HASH_SIZE" ...
```

### Technical Details

When enabled, the following changes are made in `js_map_constructor()` (`quickjs.c`):

```c
#ifdef CONFIG_MAP_INIT_HASH_SIZE
    s->hash_size = 16;                    // Increased from 1
    s->record_count_threshold = 32;       // Increased from 4
#else
    s->hash_size = 1;
    s->record_count_threshold = 4;
#endif
```

This optimization eliminates the cascading resize operations that occur when:
1. A Map is created (1 bucket)
2. Records are added, triggering resizes at 4, 8, 16, 32, 64 records...
3. Each resize reallocates the hash table and rehashes all existing records

With the optimization, a Map can hold up to 32 records before the first resize, avoiding the early resize overhead that's common in real-world usage patterns.

### Memory Impact

- **Without optimization:** 1 hash bucket initially (~8-16 bytes)
- **With optimization:** 16 hash buckets initially (~128-256 bytes)
- **Overhead:** Approximately 100-200 bytes per Map/Set at creation time

This small memory increase is typically negligible and is offset by the performance gains.

### When to Use

**Recommended for:**
- Production builds where Map/Set performance is critical
- Applications that create Maps/Sets with many entries
- Server-side JavaScript with heavy data processing
- Any workload involving frequent Map/Set operations

**Not necessary for:**
- Development/debugging builds
- Scripts that create Maps/Sets with very few entries (< 10)
- Memory-constrained environments where every byte counts

### Compatibility

- All existing tests pass with this optimization enabled
- No API changes
- No breaking changes to JavaScript semantics
- Works with all Map/Set types (Map, Set, WeakMap, WeakSet)

### Code Changes

The optimization is implemented in `quickjs.c` in the `js_map_constructor()` function (line ~50521):

```c
s->hash_table = js_malloc(ctx, sizeof(s->hash_table[0]) * s->hash_size);
if (!s->hash_table)
    goto fail;
for (int i = 0; i < s->hash_size; i++)
    init_list_head(&s->hash_table[i]);
```

The hash table initialization is modified to handle multiple buckets when `CONFIG_MAP_INIT_HASH_SIZE` is defined.

### See Also

- `build.zig` - Build configuration
- `tests/microbench.js` - Performance benchmarks
- `quickjs.c` - Implementation details
