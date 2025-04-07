#!/usr/bin/env python3
import sys
import os
import re
import datetime
import matplotlib.pyplot as plt
from collections import OrderedDict


def parse_updatetip_line(line):
    match = re.match(
        r'^([\d\-:TZ]+) UpdateTip: new best.+height=(\d+).+tx=(\d+).+cache=([\d.]+)MiB\((\d+)txo\)',
        line
    )
    if not match:
        return None
    iso_str, height_str, tx_str, cache_size_mb_str, cache_coins_count_str = match.groups()
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime, int(height_str), int(tx_str), float(cache_size_mb_str), int(cache_coins_count_str)


def parse_leveldb_compact_line(line):
    match = re.match(r'^([\d\-:TZ]+) \[leveldb] Compacting.*files', line)
    if not match:
        return None
    iso_str = match.groups()[0]
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime


def parse_leveldb_generated_table_line(line):
    match = re.match(r'^([\d\-:TZ]+) \[leveldb] Generated table.*: (\d+) keys, (\d+) bytes', line)
    if not match:
        return None
    iso_str, keys_count_str, bytes_count_str = match.groups()
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime, int(keys_count_str), int(bytes_count_str)


def parse_validation_txadd_line(line):
    match = re.match(r'^([\d\-:TZ]+) \[validation] TransactionAddedToMempool: txid=.+wtxid=.+', line)
    if not match:
        return None
    iso_str = match.groups()[0]
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime


def parse_coindb_write_batch_line(line):
    match = re.match(r'^([\d\-:TZ]+) \[coindb] Writing (partial|final) batch of ([\d.]+) MiB', line)
    if not match:
        return None
    iso_str, is_partial_str, size_mb_str = match.groups()
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime, is_partial_str, float(size_mb_str)


def parse_coindb_commit_line(line):
    match = re.match(r'^([\d\-:TZ]+) \[coindb] Committed (\d+) changed transaction outputs', line)
    if not match:
        return None
    iso_str, txout_count_str = match.groups()
    parsed_datetime = datetime.datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
    return parsed_datetime, int(txout_count_str)


def parse_log_file(log_file):
    with open(log_file, 'r', encoding='utf-8') as f:
        update_tip_data = []
        leveldb_compact_data = []
        leveldb_gen_table_data = []
        validation_txadd_data = []
        coindb_write_batch_data = []
        coindb_commit_data = []

        for line in f:
            if result := parse_updatetip_line(line):
                update_tip_data.append(result)
            elif result := parse_leveldb_compact_line(line):
                leveldb_compact_data.append(result)
            elif result := parse_leveldb_generated_table_line(line):
                leveldb_gen_table_data.append(result)
            elif result := parse_validation_txadd_line(line):
                validation_txadd_data.append(result)
            elif result := parse_coindb_write_batch_line(line):
                coindb_write_batch_data.append(result)
            elif result := parse_coindb_commit_line(line):
                coindb_commit_data.append(result)

        if not update_tip_data:
            print("No UpdateTip entries found.")
            sys.exit(0)

        assert all(update_tip_data[i][0] <= update_tip_data[i + 1][0] for i in
                   range(len(update_tip_data) - 1)), "UpdateTip entries are not sorted by time"

    return update_tip_data, leveldb_compact_data, leveldb_gen_table_data, validation_txadd_data, coindb_write_batch_data, coindb_commit_data


def generate_plot(x, y, x_label, y_label, title, output_file, is_height_based=False):
    if not x or not y:
        print(f"Skipping plot '{title}' as there is no data.")
        return

    plt.figure(figsize=(30, 10))
    plt.plot(x, y)
    plt.title(title, fontsize=20)
    plt.xlabel(x_label, fontsize=16)
    plt.ylabel(y_label, fontsize=16)
    plt.grid(True)

    # Make sure the x-axis covers the full data range
    min_x, max_x = min(x), max(x)
    plt.xlim(min_x, max_x)

    # Add vertical lines for major protocol upgrades if this is a height-based plot
    if is_height_based:
        # Define all notable heights from the chainparams file
        fork_heights = OrderedDict([
            ('BIP34', 227931),  # Block v2, coinbase includes height
            ('BIP66', 363725),  # Strict DER signatures
            ('BIP65', 388381),  # OP_CHECKLOCKTIMEVERIFY
            ('CSV', 419328),  # BIP68, 112, 113 - OP_CHECKSEQUENCEVERIFY
            ('Segwit', 481824),  # BIP141, 143, 144, 145 - Segregated Witness
            ('Taproot', 709632),  # BIP341, 342 - Schnorr signatures & Taproot
            ('Halving 1', 210000),  # First halving
            ('Halving 2', 420000),  # Second halving
            ('Halving 3', 630000),  # Third halving
            ('Halving 4', 840000),  # Fourth halving
        ])

        # Colors for the different types of events
        fork_colors = {
            'BIP34': 'blue',
            'BIP66': 'blue',
            'BIP65': 'blue',
            'CSV': 'blue',
            'Segwit': 'green',
            'Taproot': 'red',
            'Halving 1': 'purple',
            'Halving 2': 'purple',
            'Halving 3': 'purple',
            'Halving 4': 'purple',
        }

        # Line styles for different types of events
        fork_styles = {
            'BIP34': '--',
            'BIP66': '--',
            'BIP65': '--',
            'CSV': '--',
            'Segwit': '--',
            'Taproot': '--',
            'Halving 1': ':',
            'Halving 2': ':',
            'Halving 3': ':',
            'Halving 4': ':',
        }

        max_y = max(y)

        # Position text labels at different heights to avoid overlap
        text_positions = {}
        position_increment = max_y * 0.05
        current_position = max_y * 0.9

        # Add lines for forks that are in range
        for fork_name, height in fork_heights.items():
            if min_x <= height <= max_x:
                plt.axvline(x=height, color=fork_colors[fork_name],
                            linestyle=fork_styles[fork_name])

                # Avoid label overlaps by staggering vertical positions
                if height in text_positions:
                    # If this x position already has a label, adjust position
                    text_positions[height] -= position_increment
                else:
                    text_positions[height] = current_position
                    current_position -= position_increment
                    if current_position < max_y * 0.1:
                        current_position = max_y * 0.9  # Reset if we're too low

                plt.text(height, text_positions[height], f'{fork_name} ({height})',
                         rotation=90, verticalalignment='top',
                         color=fork_colors[fork_name])

    plt.xticks(rotation=90, fontsize=12)
    plt.yticks(fontsize=12)
    plt.tight_layout()
    plt.savefig(output_file)
    plt.close()
    print(f"Saved plot to {output_file}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <commit> <log_directory> <png_directory>")
        sys.exit(1)

    commit = sys.argv[1]

    log_file = sys.argv[2]
    if not os.path.isfile(log_file):
        print(f"File not found: {log_file}")
        sys.exit(1)

    png_dir = sys.argv[3]
    os.makedirs(png_dir, exist_ok=True)

    update_tip_data, leveldb_compact_data, leveldb_gen_table_data, validation_txadd_data, coindb_write_batch_data, coindb_commit_data = parse_log_file(log_file)
    times, heights, tx_counts, cache_size, cache_count = zip(*update_tip_data)
    float_minutes = [(t - times[0]).total_seconds() / 60 for t in times]

    generate_plot(float_minutes, heights, "Elapsed minutes", "Block Height", "Block Height vs Time", os.path.join(png_dir, f"{commit}-height_vs_time.png"))
    generate_plot(heights, cache_size, "Block Height", "Cache Size (MiB)", "Cache Size vs Block Height", os.path.join(png_dir, f"{commit}-cache_vs_height.png"), is_height_based=True)
    generate_plot(float_minutes, cache_size, "Elapsed minutes", "Cache Size (MiB)", "Cache Size vs Time", os.path.join(png_dir, f"{commit}-cache_vs_time.png"))
    generate_plot(heights, tx_counts, "Block Height", "Transaction Count", "Transactions vs Block Height", os.path.join(png_dir, f"{commit}-tx_vs_height.png"), is_height_based=True)
    generate_plot(heights, cache_count, "Block Height", "Coins Cache Size", "Coins Cache Size vs Height", os.path.join(png_dir, f"{commit}-coins_cache_vs_height.png"), is_height_based=True)

    # LevelDB Compaction and Generated Tables
    if leveldb_compact_data:
        leveldb_compact_times = [(t - times[0]).total_seconds() / 60 for t in leveldb_compact_data]
        leveldb_compact_y = [1 for _ in leveldb_compact_times]  # dummy y axis to mark compactions
        generate_plot(leveldb_compact_times, leveldb_compact_y, "Elapsed minutes", "LevelDB Compaction", "LevelDB Compaction Events vs Time", os.path.join(png_dir, f"{commit}-leveldb_compact_vs_time.png"))
    if leveldb_gen_table_data:
        leveldb_gen_table_times, leveldb_gen_table_keys, leveldb_gen_table_bytes = zip(*leveldb_gen_table_data)
        leveldb_gen_table_float_minutes = [(t - times[0]).total_seconds() / 60 for t in leveldb_gen_table_times]
        generate_plot(leveldb_gen_table_float_minutes, leveldb_gen_table_keys, "Elapsed minutes", "Number of keys", "LevelDB Keys Generated vs Time", os.path.join(png_dir, f"{commit}-leveldb_gen_keys_vs_time.png"))
        generate_plot(leveldb_gen_table_float_minutes, leveldb_gen_table_bytes, "Elapsed minutes", "Number of bytes", "LevelDB Bytes Generated vs Time", os.path.join(png_dir, f"{commit}-leveldb_gen_bytes_vs_time.png"))

    # validation mempool add transaction lines
    if validation_txadd_data:
        validation_txadd_times = [(t - times[0]).total_seconds() / 60 for t in validation_txadd_data]
        validation_txadd_y = [1 for _ in validation_txadd_times]  # dummy y axis to mark transaction additions
        generate_plot(validation_txadd_times, validation_txadd_y, "Elapsed minutes", "Transaction Additions", "Transaction Additions to Mempool vs Time", os.path.join(png_dir, f"{commit}-validation_txadd_vs_time.png"))

    # coindb write batch lines
    if coindb_write_batch_data:
        coindb_write_batch_times, is_partial_strs, sizes_mb = zip(*coindb_write_batch_data)
        coindb_write_batch_float_minutes = [(t - times[0]).total_seconds() / 60 for t in coindb_write_batch_times]
        generate_plot(coindb_write_batch_float_minutes, sizes_mb, "Elapsed minutes", "Batch Size MiB", "Coin Database Partial/Final Write Batch Size vs Time", os.path.join(png_dir, f"{commit}-coindb_write_batch_size_vs_time.png"))
    if coindb_commit_data:
        coindb_commit_times, txout_counts = zip(*coindb_commit_data)
        coindb_commit_float_minutes = [(t - times[0]).total_seconds() / 60 for t in coindb_commit_times]
        generate_plot(coindb_commit_float_minutes, txout_counts, "Elapsed minutes", "Transaction Output Count", "Coin Database Transaction Output Committed vs Time", os.path.join(png_dir, f"{commit}-coindb_commit_txout_vs_time.png"))

    print("Plots saved!")
