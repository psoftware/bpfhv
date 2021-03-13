#!/bin/bash

set -x

BW=500gbit
IF1=enp1s0f1
IF2=enp1s0f0

sudo tc qdisc del dev $IF1 root
sudo tc qdisc add dev $IF1 handle 1: root htb default 2
sudo tc class add dev $IF1 parent 1: classid 1:2 htb rate $BW
#sudo tc class add dev $IF1 parent 1:1 classid 1:11 htb rate $BW
