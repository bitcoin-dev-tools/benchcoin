#!/usr/bin/env python3
import sys
import os
import re
import datetime
import matplotlib.pyplot as plt


def parse_line(line):
    match = re.match(
        r'^([\d\-:TZ]+) UpdateTip: new best.+height=(\d+).+tx=(\d+).+cache=([\d.]+)MiB\((\d+)txo\)',
        line
    )
    if not match:
        return None
    iso_str, height_str, tx_str, cache_size_mb_str, cache_coins_count_str = match.groups()
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime, int(height_str), int(tx_str), float(cache_size_mb_str), int(cache_coins_count_str)


def parse_log_file(log_file):
    with open(log_file, 'r', encoding='utf-8') as f:
        data = [result for line in f if (result := parse_line(line))]
    if not data:
        print("No UpdateTip entries found.")
        sys.exit(0)
    assert all(data[i][0] <= data[i + 1][0] for i in range(len(data) - 1)), "Entries are not sorted by time"
    return data


def generate_plot(x, y, x_label, y_label, title, output_file):
    plt.figure(figsize=(20, 10))
    plt.plot(x, y)
    plt.title(title)
    plt.xlabel(x_label)
    plt.ylabel(y_label)
    plt.grid(True)
    plt.xticks(rotation=90)
    plt.tight_layout()
    plt.savefig(output_file)
    plt.close()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <log_directory> <png_directory>")
        sys.exit(1)

    log_file = sys.argv[1]
    if not os.path.isfile(log_file):
        print(f"File not found: {log_file}")
        sys.exit(1)

    png_dir = sys.argv[2]
    os.makedirs(png_dir, exist_ok=True)

    times, heights, tx_counts, cache_size, cache_count = zip(*parse_log_file(log_file))
    float_minutes = [(t - times[0]).total_seconds() / 60 for t in times]

    generate_plot(float_minutes, heights, "Elapsed minutes", "Block Height", "Block Height vs Time", os.path.join(png_dir, "height_vs_time.png"))
    generate_plot(heights, cache_size, "Block Height", "Cache Size (MiB)", "Cache Size vs Block Height", os.path.join(png_dir, "cache_vs_height.png"))
    generate_plot(float_minutes, cache_size, "Elapsed minutes", "Cache Size (MiB)", "Cache Size vs Time", os.path.join(png_dir, "cache_vs_time.png"))
    generate_plot(heights, tx_counts, "Block Height", "Transaction Count", "Transactions vs Block Height", os.path.join(png_dir, "tx_vs_height.png"))
    generate_plot(heights, cache_count, "Block Height", "Coins Cache Size", "Coins Cache Size vs Block Height", os.path.join(png_dir, "coins_cache_vs_height.png"))

    print("Plots saved!")
