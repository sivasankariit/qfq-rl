#!/bin/bash

dev=eth4

tc qdisc del dev $dev root
rmmod sch_qfq
make

insmod ./sch_qfq.ko
tc qdisc add dev $dev root handle 1: qfq
tc class add dev $dev parent 1: classid 1:1 qfq weight 1 maxpkt 2048
tc class add dev $dev parent 1: classid 1:2 qfq weight 2 maxpkt 2048
tc class add dev $dev parent 1: classid 1:3 qfq weight 1 maxpkt 2048
tc filter add dev $dev parent 1: protocol all prio 1 u32 match ip dport 5001 0xffff  flowid 1:1
tc filter add dev $dev parent 1: protocol all prio 1 u32 match ip dport 5002 0xffff  flowid 1:2
tc filter add dev $dev parent 1: protocol all prio 2 u32 match u32 0 0 flowid 1:3
