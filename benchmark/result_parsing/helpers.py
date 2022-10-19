import os
import re

import numpy as np
import matplotlib
import matplotlib.pyplot as plt

matplotlib.rc('xtick', labelsize=8)
matplotlib.rc('ytick', labelsize=8)


def plot_bars(df, output_dir):

    # For the std deviation of the total (compress+inflate) we just assume variable independence 
    # between both operations
    # This choice could be challenged, but nevermind, this is not essential
    if 'total_std' not in df.columns:
        df['total_std'] = df['compress_std'] ** 2 + df['inflate_std'] ** 2
        df['total_std'] = df['total_std'].pow(0.5)

    df = df.sort_values('compress_mean', ascending=False)
    plt.title('Formatting time (s)')
    plt.bar(df['method'], df['compress_mean'], color="b",
            yerr=df['compress_std'])
    plt.xticks(rotation=45, ha='right')
    plt.gcf().subplots_adjust(bottom=0.17)
    plt.savefig(os.path.join(output_dir, 'compress.jpg'))

    plt.figure()
    df = df.sort_values('inflate_mean', ascending=False)
    plt.title('Inflatting time (s)')
    plt.bar(df['method'], df['inflate_mean'], color="b",
            yerr=df['inflate_std'])
    plt.xticks(rotation=45, ha='right')
    plt.gcf().subplots_adjust(bottom=0.17)
    plt.savefig(os.path.join(output_dir, 'inflate.jpg'))

    plt.figure()
    df = df.sort_values('total', ascending=False)
    plt.title('Formatting+Inflatting time (s)')
    plt.bar(df['method'], df['total'], color="b",
            yerr=df['total_std'])
    plt.xticks(rotation=45, ha='right')
    plt.gcf().subplots_adjust(bottom=0.17)
    plt.savefig(os.path.join(output_dir, 'total.jpg'))

    plt.figure()
    df = df.sort_values('ratio', ascending=False)
    plt.title('Compression ratios')
    plt.bar(df['method'], df['ratio'], color="b")
    plt.xticks(rotation=45, ha='right')
    plt.gcf().subplots_adjust(bottom=0.17)
    plt.savefig(os.path.join(output_dir, 'ratios.jpg'))

    plt.figure()
    plt.title('SFS is a restore booster')
    # df = df.sort_values('inflate_mean', ascending=False)
    aux1 = df[df['method'].isin(['gzip', 'pigz', 'xz', 'pixz', 'lz4'])].copy()
    aux2 = df[df['method'].isin(['sfs+gzip', 'sfs+pigz', 'sfs+xz', 'sfs+pixz', 'sfs+lz4'])].copy()
    x1 = np.arange(len(aux1['method']))
    w = 0.4
    x2 = [i + w for i in x1]
    pattern = re.compile(r'^sfs\+(.*)$')
    aux2['method'] = aux2['method'].apply(lambda x: pattern.match(x).group(1))
    plt.bar(x1, aux1['inflate_mean'], w, color="b", yerr=aux1['inflate_std'], label='no sfs')
    plt.bar(x2, aux2['inflate_mean'], w, color="g", yerr=aux2['inflate_std'], label='sfs')
    plt.xticks(x1, aux1['method'], rotation=45)
    plt.legend(loc='best')
    plt.gcf().subplots_adjust(bottom=0.17)
    plt.savefig(os.path.join(output_dir, 'sfs_as_booster.jpg'))

