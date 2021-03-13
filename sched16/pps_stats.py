#!/usr/bin/env python

import re
import sys
import argparse
import numpy


description = "Python script to compute mean and standard deviation"
epilog = "2016 Vincenzo Maffione"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('-d', '--data-file',
                       help = "Path to file containing data", type=str,
                       required = True)
argparser.add_argument('-t', '--num-trials',
                       help = "Number of samples for each point", type=int,
                       default = 10)

args = argparser.parse_args()

x = dict()
n = 1
k = 0

fin = open(args.data_file)
while 1:
    line = fin.readline()
    if line == '':
        break

    if k == 0:
        x[n] = []

    m = re.search(r'(\d\.\d\d\de[+-]\d\d) pps', line)
    if m == None:
        continue

    val = float(m.group(1))/1000000.0
    x[n].append(val)

    k = k + 1
    if k == args.num_trials:
        k = 0
        n = n + 1

fin.close()

for num in x:
    mean = numpy.mean(x[num])
    stddev = numpy.std(x[num])
    print("%d %.4f %.4f" % (num, mean, stddev))
