#!/bin/bash
TARGET_MODULE=rcuhashbash-resize_wob_lock
sleep_time=10
insert=false
tests=("rcu" "ddds" "rwlock")
readers=(1 2 4 8 16)

sudo dmesg -C

for((readers_count = 0; readers_count < 5; readers_count = readers_count + 1))
do    
    sudo insmod ${TARGET_MODULE}.ko \
            test=${tests[0]} \
            readers=${readers[readers_count]} \
            shift1=13 \
            resize=false
    sleep ${sleep_time}
    sudo rmmod ${TARGET_MODULE}
done

mapfile -t reads < <(sudo dmesg | grep "rcuhashbash summary" | grep -oP "reads: \K\d+(?= primary hits)")

for i in "${!reads[@]}"; do
    echo "${readers[i]} ${reads[i]}"
done > 8k_buckets_without_resizing_rcu.txt


sudo dmesg -C

for((readers_count = 0; readers_count < 5; readers_count = readers_count + 1))
do    
    sudo insmod ${TARGET_MODULE}.ko \
            test=${tests[1]} \
            readers=${readers[readers_count]} \
            shift1=13 \
            resize=false
    sleep ${sleep_time}
    sudo rmmod ${TARGET_MODULE}
done

mapfile -t reads < <(sudo dmesg | grep "rcuhashbash summary" | grep -oP "reads: \K\d+(?= primary hits)")

for i in "${!reads[@]}"; do
    echo "${readers[i]} ${reads[i]}"
done > 8k_buckets_without_resizing_ddds.txt

sudo dmesg -C

for((readers_count = 0; readers_count < 5; readers_count = readers_count + 1))
do    
    sudo insmod ${TARGET_MODULE}.ko \
            test=${tests[2]} \
            readers=${readers[readers_count]} \
            shift1=13 \
            resize=false
    sleep ${sleep_time}
    sudo rmmod ${TARGET_MODULE}
done

mapfile -t reads < <(sudo dmesg | grep "rcuhashbash summary" | grep -oP "reads: \K\d+(?= primary hits)")

for i in "${!reads[@]}"; do
    echo "${readers[i]} ${reads[i]}"
done > 8k_buckets_without_resizing_rwlock.txt

gnuplot script_resize.gp 
