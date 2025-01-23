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
        log("Generating content files from PR data {s}", .{""});

        var assets_dir = try cwd.openDir("assets", .{ .iterate = true });
        defer assets_dir.close();

        var walker = try assets_dir.walk(self.b.allocator);
        defer walker.deinit();

        while (try walker.next()) |entry| {
            const pr_num = std.fmt.parseInt(u32, entry.basename, 10) catch continue;
            if (pr_num < 100) continue;
            log("Found PR {d}", .{pr_num});

            // Open the PR directory & list runs inside
            var pr_dir = try assets_dir.openDir(entry.basename, .{ .iterate = true });
            defer pr_dir.close();

            var iter = pr_dir.iterate();
            while (try iter.next()) |run_entry| {
                if (run_entry.kind != .directory) continue;
                log("  Run: {s}", .{run_entry.name});

                const pr_num_string = try std.fmt.allocPrint(self.b.allocator, "{d:0>6}", .{pr_num});
                defer self.b.allocator.free(pr_num_string);

                try writeContentFile(&self.b.allocator, pr_num_string, run_entry.name);

                try appendToIndex(self.b.allocator, "content/index.smd", pr_num_string, run_entry.name);
            }
        }
    }

    fn writeContentFile(allocator: *std.mem.Allocator, pr_number: []const u8, run_number: []const u8) !void {
        const output_file_name = try std.fmt.allocPrint(allocator.*, "content/{s}-{s}.smd", .{ pr_number, run_number });
        defer allocator.free(output_file_name);

        std.log.info("    Writing content file {s}", .{output_file_name});
        // Open the input file in read-only mode
        const input_file = try std.fs.cwd().openFile("templates/run.tsmd", .{ .mode = std.fs.File.OpenMode.read_only });
        defer input_file.close();

        // Open the output file in write-only mode
        const output_file = try std.fs.cwd().createFile(output_file_name, .{});
        defer output_file.close();

        // Get the file size
        const file_size = try input_file.getEndPos();

        // Allocate a buffer to hold the file's contents
        const file_contents = try allocator.alloc(u8, file_size);
        defer allocator.free(file_contents);

        // Read the entire file into a buffer
        _ = try input_file.reader().readAll(file_contents);

        const pr_replace_output = try replaceAll(allocator, file_contents, "{{PR}}", pr_number);
        defer allocator.free(pr_replace_output);

        const run_replace_output = try replaceAll(allocator, pr_replace_output, "{{RUN}}", run_number);
        defer allocator.free(run_replace_output);

        try output_file.writeAll(run_replace_output);
    }

    pub fn replaceAll(allocator: *std.mem.Allocator, input: []const u8, search: []const u8, replacement: []const u8) ![]const u8 {
        if (search.len == 0) return input; // Avoid infinite loop if search is empty

        var builder = std.ArrayList(u8).init(allocator.*);
        defer builder.deinit();

        var start: usize = 0;
        while (start < input.len) {
            const index = std.mem.indexOf(u8, input[start..], search);
            if (index) |i| {
                // Add up to the found instance
                try builder.appendSlice(input[start .. start + i]);
                // Add the replacement
                try builder.appendSlice(replacement);
                // Move start forward
                start += i + search.len;
            } else {
                // No more instances; append the rest of the input
                try builder.appendSlice(input[start..]);
                break;
            }
        }

        return builder.toOwnedSlice();
    }

    pub fn appendToIndex(allocator: std.mem.Allocator, filePath: []const u8, pr_num: []const u8, run_num: []const u8) !void {
        // Open the file in append mode
        var file = try std.fs.cwd().openFile(filePath, .{ .mode = std.fs.File.OpenMode.read_write });
        defer file.close();

        // | 108/12678565948 | [#108/12678565948](/108-12678565948/) |
        const link = try std.fmt.allocPrint(allocator, "| {s}/{s} | [#{s}/{s}](/{s}-{s}/) |\n", .{ pr_num, run_num, pr_num, run_num, pr_num, run_num });
        defer allocator.free(link);

        try file.seekFromEnd(0);
        // Write the content to the file
        try file.writer().print("{s}", .{link});
    }
};
