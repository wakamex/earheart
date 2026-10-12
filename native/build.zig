// Earheart on gesso.
//   zig build                 build into zig-out/bin, with the engines beside the executable
//   zig build test            run the tests
// The speech and cleanup engines are shared libraries loaded at run time; fetch them once per
// target with tools/fetch-engines.sh.
const std = @import("std");
const zon = @import("build.zig.zon");

const flags: []const []const u8 = &.{ "-std=c11", "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };
const sources: []const []const u8 = &.{ "engine.c", "worker.c", "paths.c", "cores.c", "files.c", "jv.c", "settings.c", "loop.c", "engines.c", "remote.c", "preview.c", "pipeline.c", "history.c", "recorder.c", "app.c", "jw.c", "wav.c", "audio.c", "chunked.c", "text.c", "stt.c", "cleanup.c", "models.c", "download.c", "catalog.c", "fonts.c", "overlay.c", "ui.c", "hotkeys.c", "model_jobs.c", "commit.c", "hf.c" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const gesso = b.dependency("gesso", .{ .target = target, .optimize = optimize }).artifact("gesso");

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .strip = optimize != .debug });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = sources, .flags = flags });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{ "main.c", "dictate.c", "deliver.c", "run.c", "settings_window.c", "window_smoke.c", "wizard_window.c", "correction_window.c" }, .flags = flags });
    // Bringing the app forward when it shows a window: AppKit on macOS, nothing elsewhere.
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"activate.c"}, .flags = flags });
    if (target.result.os.tag == .macos) {
        mod.addCSourceFile(.{ .file = b.path("src/activate_macos.m"), .flags = &.{ "-fobjc-arc", "-Wall", "-Wextra", "-Wno-unused-parameter" } });
        mod.linkFramework("AppKit", .{});
    }
    mod.addIncludePath(b.path("vendor/include"));
    mod.addCMacro("EARHEART_VERSION", "\"" ++ zon.version ++ "\"");
    mod.linkLibrary(gesso);
    const exe = b.addExecutable(.{ .name = "earheart", .root_module = mod });
    b.installArtifact(exe);

    // The engines fetched for this target go beside the executable. -Dengines=false builds without
    // them: the app still runs and reports the missing engine when one is asked for.
    if (b.option(bool, "engines", "Install the engine libraries from engines/TARGET (default: true)") orelse true) {
        const t = target.result;
        const dir = b.fmt("engines/{s}-{s}", .{ @tagName(t.os.tag), if (t.cpu.arch == .aarch64) "arm64" else "x64" });
        b.installDirectory(.{ .source_dir = b.path(dir), .install_dir = .bin, .install_subdir = "engines" });
    }

    // A window to dictate into, for the end-to-end tests.
    const target_mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    target_mod.addCSourceFiles(.{ .root = b.path("tests"), .files = &.{"paste_target.c"}, .flags = flags });
    target_mod.linkLibrary(gesso);
    b.installArtifact(b.addExecutable(.{ .name = "paste-target", .root_module = target_mod }));
    const probe_mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    probe_mod.addCSourceFiles(.{ .root = b.path("tests"), .files = &.{"clipboard_probe.c"}, .flags = flags });
    probe_mod.linkLibrary(gesso);
    b.installArtifact(b.addExecutable(.{ .name = "clipboard-probe", .root_module = probe_mod }));

    const tests = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    tests.addCSourceFiles(.{ .root = b.path("src"), .files = sources, .flags = flags });
    tests.addCSourceFiles(.{ .root = b.path("tests"), .files = &.{ "main.c", "audio.c", "chunked.c", "text.c", "settings.c", "overlay.c", "hotkeys.c", "commit.c", "hf.c" }, .flags = flags });
    tests.addIncludePath(b.path("src"));
    tests.addIncludePath(b.path("vendor/include"));
    tests.linkLibrary(gesso);
    const test_run = b.addRunArtifact(b.addExecutable(.{ .name = "earheart-tests", .root_module = tests }));
    test_run.addDirectoryArg(b.path("tests"));
    // The prompt test opens the engines fetched for this target, when there are any.
    test_run.addDirectoryArg(b.path(b.fmt("engines/{s}-{s}", .{ @tagName(target.result.os.tag), if (target.result.cpu.arch == .aarch64) "arm64" else "x64" })));
    b.step("test", "Run the tests").dependOn(&test_run.step);
}
