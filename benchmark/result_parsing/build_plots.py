#!/usr/bin/env python

import argparse
import os
import sys

import pandas as pd

sys.path.insert(0, os.path.realpath(os.path.join(__file__, os.pardir, os.pardir)))
from result_parsing.helpers import plot_bars

def parse_argv():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", help="Bench result directory")
    return parser.parse_args()


def main():
    args = parse_argv()
    result_dir = args.results
    if not os.path.isdir(result_dir):
        raise Exception('No directory {} found'.format(result_dir))

    df = pd.read_csv(os.path.join(result_dir, 'sfs_stats.csv'))
    plot_bars(df, result_dir)


if __name__ == '__main__':
    main()
