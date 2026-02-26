const std = @import("std");

// QuickJS-ng build configuration
pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Options (similar to CMake options)
    const build_shared = b.option(bool, "shared", "Build shared library instead of static") orelse false;
    const build_werror = b.option(bool, "werror", "Build with -Werror") orelse false;
    const build_examples = b.option(bool, "examples", "Build examples") orelse false;
    const build_libc = b.option(bool, "libc", "Build standard library modules as part of the library") orelse false;
    const static_cli = b.option(bool, "static-cli", "Build a static qjs executable") orelse false;
    const disable_parser = b.option(bool, "no-parser", "Disable JS source code parser") orelse false;

    // Determine version from quickjs.h
    const version = .{
        .major = 0,
        .minor = 12,
        .patch = 1,
    };

    // Base C flags
    var c_flags: [30][]const u8 = undefined;
    var c_flags_len: usize = 0;

    c_flags[c_flags_len] = "-std=c11";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wall";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wformat=2";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-implicit-fallthrough";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-sign-compare";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-missing-field-initializers";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-unused-parameter";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-unused-but-set-variable";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-unused-result";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-stringop-truncation";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-Wno-array-bounds";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-funsigned-char";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-D_GNU_SOURCE";
    c_flags_len += 1;
    c_flags[c_flags_len] = "-DQUICKJS_NG_BUILD";
    c_flags_len += 1;

    if (build_werror) {
        c_flags[c_flags_len] = "-Werror";
        c_flags_len += 1;
    }

    if (disable_parser) {
        c_flags[c_flags_len] = "-DQJS_DISABLE_PARSER";
        c_flags_len += 1;
    }

    if (build_libc) {
        c_flags[c_flags_len] = "-DQJS_BUILD_LIBC";
        c_flags_len += 1;
    }

    // Platform-specific flags
    const os_tag = target.result.os.tag;
    const is_windows = os_tag == .windows;
    const is_wasi = os_tag == .wasi;

    if (is_windows) {
        c_flags[c_flags_len] = "-DWIN32_LEAN_AND_MEAN";
        c_flags_len += 1;
        c_flags[c_flags_len] = "-D_WIN32_WINNT=0x0601";
        c_flags_len += 1;
    }

    const flags_slice = c_flags[0..c_flags_len];

    // Create root module for the library
    const qjs_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    qjs_mod.addIncludePath(b.path("."));

    // Create the QuickJS library
    const qjs_lib = b.addLibrary(.{
        .name = "qjs",
        .linkage = if (build_shared) .dynamic else .static,
        .root_module = qjs_mod,
        .version = .{ .major = version.major, .minor = version.minor, .patch = version.patch },
    });

    // Core library sources
    const qjs_sources = [_][]const u8{
        "dtoa.c",
        "libregexp.c",
        "libunicode.c",
        "quickjs.c",
    };

    for (&qjs_sources) |src| {
        qjs_lib.root_module.addCSourceFile(.{
            .file = b.path(src),
            .flags = flags_slice,
        });
    }

    if (build_libc) {
        qjs_lib.root_module.addCSourceFile(.{
            .file = b.path("quickjs-libc.c"),
            .flags = flags_slice,
        });
    }

    qjs_lib.linkLibC();

    if (!is_wasi) {
        qjs_lib.linkSystemLibrary("pthread");
    }
    qjs_lib.linkSystemLibrary("m");

    if (build_shared) {
        qjs_lib.root_module.addCMacro("BUILDING_QJS_SHARED", "1");
    }

    b.installArtifact(qjs_lib);

    // Install headers
    b.installFile("quickjs.h", "include/quickjs.h");
    if (build_libc) {
        b.installFile("quickjs-libc.h", "include/quickjs-libc.h");
    }

    // Build qjs-libc as separate static library if not included in main lib
    const qjs_libc = if (!build_libc) blk: {
        const libc_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
        });
        libc_mod.addIncludePath(b.path("."));

        const libc_lib = b.addLibrary(.{
            .name = "qjs-libc",
            .linkage = .static,
            .root_module = libc_mod,
        });
        libc_lib.root_module.addCSourceFile(.{
            .file = b.path("quickjs-libc.c"),
            .flags = flags_slice,
        });
        libc_lib.linkLibC();
        if (!is_wasi) {
            libc_lib.linkSystemLibrary("pthread");
        }
        libc_lib.linkSystemLibrary("m");
        break :blk libc_lib;
    } else null;

    if (qjs_libc) |libc| {
        b.installArtifact(libc);
    }

    // Helper to link qjs libc if needed
    const addQjsLibc = struct {
        fn add(libc_opt: ?*std.Build.Step.Compile, exe: *std.Build.Step.Compile) void {
            if (libc_opt) |libc| {
                exe.linkLibrary(libc);
            }
        }
    }.add;

    // Helper for static linking
    const addStatic = struct {
        fn add(exe: *std.Build.Step.Compile, should_static: bool) void {
            if (should_static) {
                exe.linkage = .static;
            }
        }
    }.add;

    // qjsc - QuickJS bytecode compiler
    const qjsc_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    qjsc_mod.addIncludePath(b.path("."));

    const qjsc = b.addExecutable(.{
        .name = "qjsc",
        .root_module = qjsc_mod,
    });
    qjsc.root_module.addCSourceFile(.{
        .file = b.path("qjsc.c"),
        .flags = flags_slice,
    });
    qjsc.linkLibC();
    qjsc.linkLibrary(qjs_lib);
    addQjsLibc(qjs_libc, qjsc);
    addStatic(qjsc, static_cli);
    b.installArtifact(qjsc);

    // qjs - QuickJS CLI
    const qjs_exe_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    qjs_exe_mod.addIncludePath(b.path("."));

    const qjs_exe = b.addExecutable(.{
        .name = "qjs",
        .root_module = qjs_exe_mod,
    });

    const qjs_exe_sources = [_][]const u8{
        "gen/repl.c",
        "gen/standalone.c",
        "qjs.c",
    };

    for (&qjs_exe_sources) |src| {
        qjs_exe.root_module.addCSourceFile(.{
            .file = b.path(src),
            .flags = flags_slice,
        });
    }

    qjs_exe.linkLibC();
    qjs_exe.linkLibrary(qjs_lib);
    addQjsLibc(qjs_libc, qjs_exe);
    addStatic(qjs_exe, static_cli);

    // Windows stack size
    if (is_windows) {
        qjs_exe.linker_allow_shlib_undefined = true;
    }

    b.installArtifact(qjs_exe);

    // run-test262 - Test262 runner
    const run_test262_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    run_test262_mod.addIncludePath(b.path("."));

    const run_test262 = b.addExecutable(.{
        .name = "run-test262",
        .root_module = run_test262_mod,
    });
    run_test262.root_module.addCSourceFile(.{
        .file = b.path("run-test262.c"),
        .flags = flags_slice,
    });
    run_test262.linkLibC();
    run_test262.linkLibrary(qjs_lib);
    addQjsLibc(qjs_libc, run_test262);
    b.installArtifact(run_test262);

    // api-test - API test
    const api_test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    api_test_mod.addIncludePath(b.path("."));

    const api_test = b.addExecutable(.{
        .name = "api-test",
        .root_module = api_test_mod,
    });
    api_test.root_module.addCSourceFile(.{
        .file = b.path("api-test.c"),
        .flags = flags_slice,
    });
    api_test.linkLibC();
    api_test.linkLibrary(qjs_lib);
    b.installArtifact(api_test);

    // unicode_gen - Unicode generator
    const unicode_gen_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    unicode_gen_mod.addIncludePath(b.path("."));

    const unicode_gen = b.addExecutable(.{
        .name = "unicode_gen",
        .root_module = unicode_gen_mod,
    });
    unicode_gen.root_module.addCSourceFile(.{
        .file = b.path("libunicode.c"),
        .flags = flags_slice,
    });
    unicode_gen.root_module.addCSourceFile(.{
        .file = b.path("unicode_gen.c"),
        .flags = flags_slice,
    });
    unicode_gen.linkLibC();
    b.installArtifact(unicode_gen);

    // function_source - Generated test executable
    const function_source_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    function_source_mod.addIncludePath(b.path("."));

    const function_source = b.addExecutable(.{
        .name = "function_source",
        .root_module = function_source_mod,
    });
    function_source.root_module.addCSourceFile(.{
        .file = b.path("gen/function_source.c"),
        .flags = flags_slice,
    });
    function_source.linkLibC();
    function_source.linkLibrary(qjs_lib);
    addQjsLibc(qjs_libc, function_source);
    b.installArtifact(function_source);

    // Examples
    if (build_examples) {
        // hello
        const hello_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
        });
        hello_mod.addIncludePath(b.path("."));

        const hello = b.addExecutable(.{
            .name = "hello",
            .root_module = hello_mod,
        });
        hello.root_module.addCSourceFile(.{
            .file = b.path("gen/hello.c"),
            .flags = flags_slice,
        });
        hello.linkLibC();
        hello.linkLibrary(qjs_lib);
        addQjsLibc(qjs_libc, hello);
        b.installArtifact(hello);

        // hello_module
        const hello_module_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
        });
        hello_module_mod.addIncludePath(b.path("."));

        const hello_module = b.addExecutable(.{
            .name = "hello_module",
            .root_module = hello_module_mod,
        });
        hello_module.root_module.addCSourceFile(.{
            .file = b.path("gen/hello_module.c"),
            .flags = flags_slice,
        });
        hello_module.linkLibC();
        hello_module.linkLibrary(qjs_lib);
        addQjsLibc(qjs_libc, hello_module);
        b.installArtifact(hello_module);

        // fib module
        const fib_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
        });
        fib_mod.addIncludePath(b.path("."));

        const fib = b.addLibrary(.{
            .name = "fib",
            .linkage = .dynamic,
            .root_module = fib_mod,
        });
        fib.root_module.addCSourceFile(.{
            .file = b.path("examples/fib.c"),
            .flags = flags_slice,
        });
        fib.root_module.addCMacro("QUICKJS_NG_MODULE_BUILD", "1");
        fib.linkLibC();
        fib.linkLibrary(qjs_lib);
        b.installArtifact(fib);

        // point module
        const point_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
        });
        point_mod.addIncludePath(b.path("."));

        const point = b.addLibrary(.{
            .name = "point",
            .linkage = .dynamic,
            .root_module = point_mod,
        });
        point.root_module.addCSourceFile(.{
            .file = b.path("examples/point.c"),
            .flags = flags_slice,
        });
        point.root_module.addCMacro("QUICKJS_NG_MODULE_BUILD", "1");
        point.linkLibC();
        point.linkLibrary(qjs_lib);
        b.installArtifact(point);

        // test_fib
        const test_fib_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
        });
        test_fib_mod.addIncludePath(b.path("."));

        const test_fib = b.addExecutable(.{
            .name = "test_fib",
            .root_module = test_fib_mod,
        });
        test_fib.root_module.addCSourceFile(.{
            .file = b.path("gen/test_fib.c"),
            .flags = flags_slice,
        });
        test_fib.linkLibC();
        test_fib.linkLibrary(qjs_lib);
        addQjsLibc(qjs_libc, test_fib);
        b.installArtifact(test_fib);
    }

    // WASI reactor (only for WASI target)
    if (is_wasi) {
        const build_reactor = b.option(bool, "wasi-reactor", "Build WASI reactor") orelse false;
        if (build_reactor) {
            const qjs_wasi_mod = b.createModule(.{
                .target = target,
                .optimize = optimize,
            });
            qjs_wasi_mod.addIncludePath(b.path("."));

            const qjs_wasi = b.addExecutable(.{
                .name = "qjs-wasi",
                .root_module = qjs_wasi_mod,
            });
            const wasi_sources = [_][]const u8{
                "quickjs-libc.c",
                "qjs-wasi-reactor.c",
            };
            for (&wasi_sources) |src| {
                qjs_wasi.root_module.addCSourceFile(.{
                    .file = b.path(src),
                    .flags = flags_slice,
                });
            }
            qjs_wasi.linkLibC();
            qjs_wasi.linkLibrary(qjs_lib);
            b.installArtifact(qjs_wasi);
        }
    }

    // Run commands
    const run_cmd = b.addRunArtifact(qjs_exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run the qjs CLI");
    run_step.dependOn(&run_cmd.step);

    // Test step
    const test_step = b.step("test", "Run tests");
    const run_tests = b.addRunArtifact(api_test);
    test_step.dependOn(&run_tests.step);

    // Print configuration
    std.debug.print("\nQuickJS-ng Zig Build Configuration:\n", .{});
    std.debug.print("  Version: {}.{}.{}{s}\n", .{ version.major, version.minor, version.patch, "" });
    std.debug.print("  Target: {s}\n", .{@tagName(target.result.cpu.arch)});
    std.debug.print("  OS: {s}\n", .{@tagName(os_tag)});
    std.debug.print("  Build type: {s}\n", .{@tagName(optimize)});
    std.debug.print("  Shared library: {}\n", .{build_shared});
    std.debug.print("  Werror: {}\n", .{build_werror});
    std.debug.print("  Examples: {}\n", .{build_examples});
    std.debug.print("  Build libc into lib: {}\n", .{build_libc});
    std.debug.print("\n", .{});
}
