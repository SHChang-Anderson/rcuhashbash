#!/bin/bash
TARGET_MODULE=(rcuhashbash-resize rcuhashbash-resize_wob_lock)
sleep_time=(5 10 15 20 25)
tests="rcu"
readers=1
insert=true

sudo dmesg -C

for((sleept = 0; sleept < 5; sleept = sleept + 1))
do    
    sudo insmod ${TARGET_MODULE[0]}.ko \
            shift1=13 \
            resize=true \
            insert=true
    sleep ${sleep_time[sleept]}
    sudo rmmod ${TARGET_MODULE[0]}
done

mapfile -t inserts < <(sudo dmesg | grep "rcuhashbash summary: inserts" | awk '{print $4}')

for i in "${!inserts[@]}"; do
    echo "${sleep_time[i]} ${inserts[i]}"
done > output_bucket_lock.txt

sudo dmesg -C

for((sleept = 0; sleept < 5; sleept = sleept + 1))
do    
    sudo insmod ${TARGET_MODULE[1]}.ko \
            shift1=13 \
            resize=true \
            insert=true
    sleep ${sleep_time[sleept]}
    sudo rmmod ${TARGET_MODULE[1]}
done

mapfile -t inserts < <(sudo dmesg | grep "rcuhashbash summary: inserts" | awk '{print $4}')

for i in "${!inserts[@]}"; do
    echo "${sleep_time[i]} ${inserts[i]}"
done > output_table_mutex.txt

gnuplot script.gp