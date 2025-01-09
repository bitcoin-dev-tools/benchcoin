const std = @import("std");
const zine = @import("zine");

pub fn build(b: *std.Build) void {
    const step = b.step("gen-content", "Generate content files from PR data");
    const genContent = b.allocator.create(GenContentStep) catch unreachable;
    genContent.* = GenContentStep.init(b);
    step.dependOn(&genContent.step);

    zine.website(b, .{
        .title = "benchcoin",
        .host_url = "localhost.com",
        .content_dir_path = "content",
        .layouts_dir_path = "layouts",
        .assets_dir_path = "assets",
    });
}

const GenContentStep = struct {
    step: std.Build.Step,
    b: *std.Build,

    fn init(b: *std.Build) GenContentStep {
        var self = GenContentStep{
            .step = std.Build.Step.init(.{
                .id = .custom,
                .name = "gen-content",
                .owner = b,
            }),
            .b = b,
        };
        self.step.makeFn = make;
        return self;
    }

    fn make(step: *std.Build.Step, prog: std.Progress.Node) !void {
        _ = prog;
        const self = @as(*GenContentStep, @fieldParentPtr("step", step));

        const cwd = std.fs.cwd();
        const log = std.log.info;

        var assets_dir = try cwd.openDir("assets", .{ .iterate = true });
        defer assets_dir.close();

        var walker = try assets_dir.walk(self.b.allocator);
        defer walker.deinit();

        while (try walker.next()) |entry| {
            if (std.mem.startsWith(u8, entry.basename, "pr-")) {
                const pr_num = std.fmt.parseInt(u32, entry.basename[3..], 10) catch continue;
                if (pr_num < 100) continue;
                log("Found PR {d}", .{pr_num});

                // Open the PR directory & list runs inside
                var pr_dir = try assets_dir.openDir(entry.basename, .{ .iterate = true });
                defer pr_dir.close();

                var iter = pr_dir.iterate();
                while (try iter.next()) |run_entry| {
                    if (run_entry.kind != .directory) continue;
                    log("  Run: {s}", .{run_entry.name});
                }
            }
        }
    }
};
