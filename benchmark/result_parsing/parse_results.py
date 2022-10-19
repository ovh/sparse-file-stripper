#!/usr/bin/env python

import argparse
import json
import os
import re
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.realpath(os.path.join(__file__, os.pardir, os.pardir)))
from result_parsing.helpers import plot_bars

compress_pattern = re.compile(r'^(.*)\.compress$', re.IGNORECASE)
inflate_pattern = re.compile(r'^(.*)\.inflate$', re.IGNORECASE)
volume_pattern = re.compile(r'^(.*)\.volume$', re.IGNORECASE)
real_time_extract = re.compile(r'^real\s+(\d+)m(\d+\.\d+)s$', re.IGNORECASE)
volume_extract = re.compile(r'^(?:[^\s]+\s+){4}(\d+)\s+.*$', re.IGNORECASE)


def parse_argv():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", help="Bench result directory")
    return parser.parse_args()


def main():
    args = parse_argv()
    result_dir = args.results
    if not os.path.isdir(result_dir):
        raise Exception('No directory {} found'.format(result_dir))
    output_dir = os.path.join(result_dir, 'output')
    os.makedirs(output_dir, exist_ok=True, mode=0o755)

    result_map = {}

    for root, dirs, files in os.walk(result_dir):
        for fname in files:
            for k, pattern, method in [('compress', compress_pattern, extract_real_times_from_file),
                                       ('inflate', inflate_pattern, extract_real_times_from_file),
                                       ('volume', volume_pattern, extract_volumes_from_file)]:
                m = pattern.match(fname)
                if m:
                    algo = m.group(1)
                    algo_dict = result_map.setdefault(algo, {})
                    algo_dict[k] = method(os.path.join(root, fname))
                    break
        # Stop at depth 1
        break

    with open(os.path.join(output_dir, 'result_map.json'), 'w') as f:
        json.dump(result_map, f)

    df = build_df(result_map)
    print(df)
    df.to_csv(os.path.join(output_dir, 'sfs_stats.csv'), index=False)

    plot_bars(df, output_dir)


def extract_real_times_from_file(filename):
    all_times = []
    with open(filename, 'r') as f:
        for line in f.readlines():
            m = real_time_extract.match(line)
            if not m:
                continue
            minutes = float(m.group(1))
            seconds = float(m.group(2))
            total_secs = seconds + minutes * 60
            all_times.append(total_secs)
    return np.mean(all_times), np.std(all_times)


def extract_volumes_from_file(filename):
    all_volumes = []

    with open(filename, 'r') as f:
        for line in f.readlines():
            line = line.strip()
            if not line:
                continue
            m = volume_extract.match(line)
            if not m:
                raise Exception('Unexpected line format: file {} line {}'.format(filename, line))
            all_volumes.append(int(m.group(1)))
    reference = all_volumes[0]
    if len(all_volumes) == 0 or any(map(lambda x: x != reference, all_volumes)):
        raise Exception('Unexpected volume list: {}'.format(all_volumes))
    return reference


def build_df(result_map):
    rows = []
    volume_ref = result_map['raw']['volume']
    for algo, data in result_map.items():
        # For the std deviation of the total (compress+inflate) we just assume variable independence between both operations
        # This choice could be challenged, but nevermind, this is not essential
        row = [algo, *data['compress'], *data['inflate'], data['volume'] / volume_ref,
               data['compress'][0] + data['inflate'][0],
               np.sqrt(data['compress'][1] ** 2 + data['inflate'][1] **2)]
        rows.append(row)

    return pd.DataFrame(
        rows, columns=['method', 'compress_mean', 'compress_std', 'inflate_mean', 'inflate_std', 'ratio',
                       'total', 'total_std']).sort_values('inflate_mean', ascending=False)


if __name__ == '__main__':
    main()
