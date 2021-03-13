#!/bin/bash
sudo modprobe netmap
sudo vale-ctl -n 0
sudo vale-ctl -a vale0:0
sudo vale-ctl -n 1
sudo vale-ctl -a vale0:1
