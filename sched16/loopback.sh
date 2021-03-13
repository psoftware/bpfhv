#!/bin/sh

IF1=enp1s0f0
IF2=enp1s0f1

set -x

# clean up
sudo ip -s -s neigh flush all

sudo iptables -t nat -F
sudo ip addr flush dev $IF1
sudo ip addr flush dev $IF2

sudo ip link set $IF1 up
sudo ip link set $IF2 up

sudo ip addr add 10.50.0.1/24 dev $IF1
sudo ip addr add 10.50.1.1/24 dev $IF2 

# nat source IP 10.50.0.1 -> 10.60.0.1 when going to 10.60.1.1
sudo iptables -t nat -A POSTROUTING -s 10.50.0.1 -d 10.60.1.1 -j SNAT --to-source 10.60.0.1

# nat inbound 10.60.0.1 -> 10.50.0.1
sudo iptables -t nat -A PREROUTING -d 10.60.0.1 -j DNAT --to-destination 10.50.0.1

# nat source IP 10.50.1.1 -> 10.60.1.1 when going to 10.60.0.1
sudo iptables -t nat -A POSTROUTING -s 10.50.1.1 -d 10.60.0.1 -j SNAT --to-source 10.60.1.1

# nat inbound 10.60.1.1 -> 10.50.1.1
sudo iptables -t nat -A PREROUTING -d 10.60.1.1 -j DNAT --to-destination 10.50.1.1

sudo ip route add 10.60.1.1 dev $IF1
sudo arp -i $IF1 -s 10.60.1.1 68:05:ca:2e:74:31 # $IF2's mac address

sudo ip route add 10.60.0.1 dev $IF2
sudo arp -i $IF2 -s 10.60.0.1 68:05:ca:2e:74:30 # $IF1's mac address
