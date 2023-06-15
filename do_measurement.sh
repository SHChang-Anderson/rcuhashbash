tests=("rcu" "ddds" "rwlock")
readers=(1 2 4 8 16)
sleep_time=10
TARGET_MODULE=rcuhashbash-resize

sudo rmmod ${TARGET_MODULE} || true >/dev/null

# test 1: no resizing and 2^13 buckets
sudo dmesg -C
for((tcount = 1; tcount <= 10; tcount = tcount + 1))
do
    # iterate algorithm
    for((tests_count = 0; tests_count < 3; tests_count = tests_count + 1))
    do
        for((readers_count = 0; readers_count < 5; readers_count = readers_count + 1))
        do
            sudo insmod ${TARGET_MODULE}.ko \
                    test=${tests[tests_count]} \
                    readers=${readers[readers_count]} \
                    shift1=13 \
                    resize=false
            sleep ${sleep_time}
            sudo rmmod ${TARGET_MODULE}
        done
    done
    echo test1 ${tcount} test done
done
sudo dmesg > test1result.txt

# test 2: no resizing and 2^14 buckets
sudo dmesg -C
for((tcount = 1; tcount <= 10; tcount = tcount + 1))
do
    # iterate algorithm
    for((tests_count = 0; tests_count < 3; tests_count = tests_count + 1))
    do
        for((readers_count = 0; readers_count < 5; readers_count = readers_count + 1))
        do
            sudo insmod ${TARGET_MODULE}.ko \
                    test=${tests[tests_count]} \
                    readers=${readers[readers_count]} \
                    shift1=14 \
                    resize=false
            sleep ${sleep_time}
            sudo rmmod ${TARGET_MODULE}
        done
    done
    echo test2 ${tcount} test done
done
sudo dmesg > test2result.txt

# test 3: continuous resizing between $2^{13}$ and $2^{14}$ buckets
sudo dmesg -C
for((tcount = 1; tcount <= 10; tcount = tcount + 1))
do
    # iterate algorithm
    for((tests_count = 0; tests_count < 3; tests_count = tests_count + 1))
    do
        for((readers_count = 0; readers_count < 5; readers_count = readers_count + 1))
        do
            sudo insmod ${TARGET_MODULE}.ko \
                    test=${tests[tests_count]} \
                    readers=${readers[readers_count]} \
                    resize=true
            sleep ${sleep_time}
            sudo rmmod ${TARGET_MODULE}
        done
    done
    echo test3 ${tcount} test done
done
sudo dmesg > test3result.txt
