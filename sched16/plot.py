#!/usr/bin/env python

from matplotlib import pyplot as plt
import argparse
import re


description = "Python script to create histograms"
epilog = "2016 Vincenzo Maffione"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('-d', '--data-file',
                       help = "Path to file containing data", action = 'append',
                       default = [])
argparser.add_argument('-o', '--out-file',
                       help = "Path to output PDF file", type = str)
argparser.add_argument('--log-x', action='store_true', help="Logarithmic scale for X")
argparser.add_argument('--log-y', action='store_true', help="Logarithmic scale for Y")
argparser.add_argument('--title', help = "Title", type = str, default="title")
argparser.add_argument('--xlabel', help = "Title", type = str, default="xlabel")
argparser.add_argument('--ylabel', help = "Title", type = str, default="ylabel")
argparser.add_argument('--legend-loc', type = str,
                       help = "Location of the legend", default = 'upper left')
argparser.add_argument('-i', '--interactive', action='store_true', help="Interactive mode")
argparser.add_argument('-c', '--cumulative', action='store_true', help="Cumulative mode")
argparser.add_argument('-x', '--cutoff', type = int, help="Cut off value")

args = argparser.parse_args()

xs = []
ys = []

for data_file in args.data_file:
    x = []
    y = []
    fin = open(data_file)
    while 1:
        line = fin.readline()
        if line == '':
            break

        if line.startswith('#'):
            continue

        m = re.match(r'(\d+) ([-+]?\d*\.\d+|\d+)', line)
        if m == None:
            continue

        x0 = int(m.group(1))
        y0 = float(m.group(2))

        if args.cutoff == None or x0 <= args.cutoff:
            x.append(x0)
            y.append(y0)
    fin.close()
    xs.append(x)
    ys.append(y)

#Plotting to our canvas
for idx in range(len(xs)):
    plt.plot(xs[idx], ys[idx])

#plt.axis([0, 10, 0, 10]) # [xmin, xmax, ymin, ymax]
plt.ylabel(args.ylabel)
plt.xlabel(args.xlabel)
plt.title(args.title)
plt.grid(True)
plt.legend(loc=args.legend_loc)

if args.log_x:
    plt.xscale('log')
if args.log_y:
    plt.yscale('log')

#Saving what we plotted
if args.out_file:
    plt.savefig(args.out_file)

if args.interactive:
    plt.show()
