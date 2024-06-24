/* rcuhashbash-resize: test module for RCU hash-table resize alorithms.
 * Written by Josh Triplett
 * Mostly lockless random number generator rcu_random from rcutorture, by Paul
 * McKenney and Josh Triplett.
 * ddds implementation based on work by Nick Piggin.
 */
#include <asm/byteorder.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_AUTHOR("Josh Triplett <josh@kernel.org>");
MODULE_DESCRIPTION("RCU hash table resizing algorithm test module.");
MODULE_LICENSE("GPL");

static char *test = "rcu"; /* Hash table implementation to benchmark */
static int readers = -1;
static bool insert = false;
static int insertct = 0;
static bool resize = false;
static u8 shift1 = 13;
static u8 shift2 = 14;
static unsigned long entries = 65536;

module_param(test, charp, 0444);
MODULE_PARM_DESC(test, "Hash table implementation");
module_param(readers, int, 0444);
MODULE_PARM_DESC(readers,
                 "Number of reader threads (default: number of online CPUs)");
module_param(resize, bool, 0444);
MODULE_PARM_DESC(resize, "Whether to run a resize thread (default: true)");
module_param(shift1, byte, 0444);
MODULE_PARM_DESC(shift1, "Initial number of hash buckets, log 2");
module_param(shift2, byte, 0444);
MODULE_PARM_DESC(shift2, "Number of hash buckets after resize, log 2");
module_param(entries, ulong, 0444);
MODULE_PARM_DESC(entries, "Number of hash table entries");

struct rcuhashbash_table {
    unsigned long mask;
    struct hlist_head buckets[];
};

struct stats {
    u64 read_hits;          /* Primary table hits */
    u64 read_hits_fallback; /* Fallback (secondary table) hits */
    u64 read_hits_slowpath; /* Slowpath primary table hits (if applicable) */
    u64 read_misses;
    u64 resizes;
    u64 insertions;
};

struct rcuhashbash_ops {
    const char *test;
    int (*read)(u32 value, struct stats *stats);
    int (*resize)(u8 new_buckets_shift, struct stats *stats);
    int (*insert)(u32 value, struct stats *stats);
};

static struct rcuhashbash_ops *ops;

static struct rcuhashbash_table *table;
static struct rcuhashbash_table *table2;

static seqcount_t seqcount;
static DEFINE_RWLOCK(rwlock);

struct rcuhashbash_entry {
    struct hlist_node node;
    struct rcu_head rcu_head;
    u32 value;
};

static struct kmem_cache *entry_cache;

static struct task_struct **tasks;

struct stats *thread_stats;

struct rcu_random_state {
    unsigned long rrs_state;
    long rrs_count;
};

#define RCU_RANDOM_MULT 39916801 /* prime */
#define RCU_RANDOM_ADD 479001701 /* prime */
#define RCU_RANDOM_REFRESH 10000

#define DEFINE_RCU_RANDOM(name) struct rcu_random_state name = {0, 0}

s64 start, end;

DEFINE_MUTEX(table_mutex);

/*
 * Crude but fast random-number generator.  Uses a linear congruential
 * generator, with occasional help from cpu_clock().
 */
static unsigned long rcu_random(struct rcu_random_state *rrsp)
{
    if (--rrsp->rrs_count < 0) {
        rrsp->rrs_state += (unsigned long) cpu_clock(raw_smp_processor_id());
        rrsp->rrs_count = RCU_RANDOM_REFRESH;
    }
    rrsp->rrs_state = rrsp->rrs_state * RCU_RANDOM_MULT + RCU_RANDOM_ADD;
    return swahw32(rrsp->rrs_state);
}

static int rhashtable_insert(u32 value, struct stats *stats)
{
    struct rcuhashbash_entry *entry;
    entry = kmem_cache_zalloc(entry_cache, GFP_KERNEL);

    if (!entry)
        return -ENOMEM;

    entry->value = value;
    hlist_add_head_rcu(&entry->node, &table->buckets[value & table->mask]);
    stats->insertions++;
    return 0;
}

static bool rcuhashbash_try_lookup(struct rcuhashbash_table *t, u32 value)
{
    struct rcuhashbash_entry *entry;
    struct hlist_node *node __attribute__((unused));

    hlist_for_each_entry_rcu(entry, &t->buckets[value & t->mask],
                             node) if (entry->value == value) return true;
    return false;
}

static int rcuhashbash_read_rcu(u32 value, struct stats *stats)
{
    rcu_read_lock();
    if (rcuhashbash_try_lookup(rcu_dereference(table), value))
        stats->read_hits++;
    else
        stats->read_misses++;
    rcu_read_unlock();

    return 0;
}

static struct hlist_node **hlist_advance_last_next(
    struct hlist_node **old_last_next)
{
    struct hlist_head h = {.first = *old_last_next};
    struct hlist_node *node;

    hlist_for_each(node, &h) if (!node->next) return &node->next;
    /* If we get here, we had an empty list. */
    return old_last_next;
}

static bool rcuhashbash_verify_insert(u32 value)
{
    bool found = false;

    rcu_read_lock();
    if (rcuhashbash_try_lookup(rcu_dereference(table), value)) {
        found = true;
    }
    rcu_read_unlock();

    return found;
}

static int rcuhashbash_resize(u8 new_buckets_shift, struct stats *stats)
{
    unsigned long len2 = 1UL << new_buckets_shift;
    unsigned long mask2 = len2 - 1;
    unsigned long i, j;

    if (mask2 < table->mask) {
        /* Shrink. */
        const struct rcuhashbash_table *new_table;
        for (i = 0; i <= mask2; i++) {
            struct hlist_node **last_next = &table->buckets[i].first;
            for (j = i + len2; j <= table->mask; j += len2) {
                if (hlist_empty(&table->buckets[j]))
                    continue;
                last_next = hlist_advance_last_next(last_next);
                *last_next = table->buckets[j].first;
                (*last_next)->pprev = last_next;
            }
        }
        /* Force the readers to see the new links before the
         * new mask. smp_wmb() does not suffice since the
         * readers do not smp_rmb(). */
        synchronize_rcu();
        table->mask = mask2;
        synchronize_rcu();
        /* Assume (and assert) that __krealloc shrinks in place. */
        new_table =
            krealloc(table, sizeof(*table) + len2 * sizeof(table->buckets[0]),
                     GFP_KERNEL);
        BUG_ON(new_table != table);
    } else if (mask2 > table->mask) {
        /* Grow. */
        struct rcuhashbash_table *temp_table, *old_table;
        bool moved_one;

        /* Explicitly avoid in-place growth, to simplify the algorithm. */
        temp_table = kzalloc(sizeof(*table) + len2 * sizeof(table->buckets[0]),
                             GFP_KERNEL);
        temp_table->mask = mask2;
        for (i = 0; i <= mask2; i++) {
            struct rcuhashbash_entry *entry;
            struct hlist_node *node __attribute__((unused));
            hlist_for_each_entry (entry, &table->buckets[i & table->mask], node)
                if ((entry->value & mask2) == i) {
                    temp_table->buckets[i].first = &entry->node;
                    entry->node.pprev = &temp_table->buckets[i].first;
                    break;
                }
        }
        /* We now have a valid hash table, albeit with buckets zipped together.
         */
        old_table = table;
        rcu_assign_pointer(table, temp_table);
        synchronize_rcu();

        /* Unzip the buckets */
        do {
            moved_one = false;
            for (i = 0; i <= old_table->mask; i++) {
                struct rcuhashbash_entry *entry_prev, *entry;
                struct hlist_node *node __attribute__((unused));
                if (hlist_empty(&old_table->buckets[i]))
                    continue;
                entry_prev = hlist_entry(old_table->buckets[i].first,
                                         struct rcuhashbash_entry, node);
                hlist_for_each_entry (entry, &old_table->buckets[i], node) {
                    if ((entry->value & mask2) != (entry_prev->value & mask2))
                        break;
                    entry_prev = entry;
                }
                if (!entry) {
                    old_table->buckets[i].first = NULL;
                    continue;
                }
                old_table->buckets[i].first = &entry->node;
                moved_one = true;
                hlist_for_each_entry (entry, &old_table->buckets[i], node)
                    if ((entry->value & mask2) == (entry_prev->value & mask2))
                        break;
                entry_prev->node.next = &entry->node;
                if (!entry) {
                    entry_prev->node.next = NULL;
                    continue;
                }
                entry->node.pprev = &entry_prev->node.next;
            }
            synchronize_rcu();
        } while (moved_one);

        kfree(old_table);
    }

    stats->resizes++;
    return 0;
}

static noinline bool ddds_lookup_slowpath(struct rcuhashbash_table *cur,
                                          struct rcuhashbash_table *old,
                                          u32 value,
                                          struct stats *stats)
{
    unsigned seq;

    do {
        seq = read_seqcount_begin(&seqcount);
        if (rcuhashbash_try_lookup(cur, value)) {
            stats->read_hits_slowpath++;
            return true;
        }
        if (rcuhashbash_try_lookup(old, value)) {
            stats->read_hits_fallback++;
            return true;
        }
    } while (read_seqcount_retry(&seqcount, seq));

    stats->read_misses++;
    return false;
}

static int rcuhashbash_read_ddds(u32 value, struct stats *stats)
{
    struct rcuhashbash_table *cur, *old;

    rcu_read_lock();
    cur = table;
    old = table2;
    if (unlikely(old))
        ddds_lookup_slowpath(cur, old, value, stats);
    else if (rcuhashbash_try_lookup(cur, value))
        stats->read_hits++;
    else
        stats->read_misses++;
    rcu_read_unlock();

    return 0;
}

static void ddds_move_nodes(struct rcuhashbash_table *old,
                            struct rcuhashbash_table *new)
{
    unsigned long i, oldsize;

    oldsize = old->mask + 1;
    for (i = 0; i < oldsize; i++) {
        struct hlist_head *head = &old->buckets[i];

        while (!hlist_empty(head)) {
            struct rcuhashbash_entry *entry;

            /* don't get preempted while holding resize_seq */
            preempt_disable();
            write_seqcount_begin(&seqcount);
            entry = hlist_entry(head->first, struct rcuhashbash_entry, node);
            hlist_del_rcu(&entry->node);
            hlist_add_head_rcu(&entry->node,
                               &new->buckets[entry->value & new->mask]);
            write_seqcount_end(&seqcount);
            preempt_enable();
            cond_resched();
            cpu_relax();
        }
    }
}

static int rcuhashbash_resize_ddds(u8 new_buckets_shift, struct stats *stats)
{
    /* table2 == d_hash_old, table == d_hash_cur */
    struct rcuhashbash_table *new, *old;
    new = kzalloc(
        sizeof(*table) + (1UL << new_buckets_shift) * sizeof(table->buckets[0]),
        GFP_KERNEL);
    if (!new)
        return -ENOMEM;
    new->mask = (1UL << new_buckets_shift) - 1;

    table2 = table;
    synchronize_rcu();
    table = new;
    synchronize_rcu();
    ddds_move_nodes(table2, table);
    synchronize_rcu();
    old = table2;
    table2 = NULL;
    synchronize_rcu();
    kfree(old);

    stats->resizes++;

    return 0;
}

static int rcuhashbash_read_rwlock(u32 value, struct stats *stats)
{
    read_lock(&rwlock);
    if (rcuhashbash_try_lookup(table, value))
        stats->read_hits++;
    else
        stats->read_misses++;
    read_unlock(&rwlock);

    return 0;
}

static int rcuhashbash_resize_rwlock(u8 new_buckets_shift, struct stats *stats)
{
    struct rcuhashbash_table *new;
    unsigned long i;

    new = kzalloc(
        sizeof(*table) + (1UL << new_buckets_shift) * sizeof(table->buckets[0]),
        GFP_KERNEL);
    if (!new)
        return -ENOMEM;
    new->mask = (1UL << new_buckets_shift) - 1;

    write_lock(&rwlock);
    for (i = 0; i <= table->mask; i++) {
        struct hlist_head *head = &table->buckets[i];

        while (!hlist_empty(head)) {
            struct rcuhashbash_entry *entry =
                hlist_entry(head->first, struct rcuhashbash_entry, node);
            hlist_del_rcu(&entry->node);
            hlist_add_head_rcu(&entry->node,
                               &new->buckets[entry->value & new->mask]);
        }
    }
    kfree(table);
    table = new;
    write_unlock(&rwlock);

    stats->resizes++;

    return 0;
}

static int rcuhashbash_read_thread(void *arg)
{
    int err;
    struct stats *stats_ret = arg;
    struct stats stats = {};
    DEFINE_RCU_RANDOM(rand);

    set_user_nice(current, 19);

    do {
        cond_resched();
        err = ops->read(rcu_random(&rand) % entries, &stats);
    } while (!kthread_should_stop() && !err);

    *stats_ret = stats;

    while (!kthread_should_stop())
        schedule_timeout_interruptible(1);
    return err;
}

static int rcuhashbash_resize_thread(void *arg)
{
    int err;
    struct stats *stats_ret = arg;
    struct stats stats = {};

    set_user_nice(current, 19);

    do {
        cond_resched();
        mutex_lock(&table_mutex);
        err = ops->resize(table->mask == (1UL << shift1) - 1 ? shift2 : shift1,
                          &stats);
        mutex_unlock(&table_mutex);
    } while (!kthread_should_stop() && !err);

    *stats_ret = stats;

    while (!kthread_should_stop())
        schedule_timeout_interruptible(1);
    return err;
}

static int rcuhashbash_insert_thread(void *arg)
{
    int i = 0;
    int err;
    struct stats *stats_ret = arg;
    struct stats stats = {};

    set_user_nice(current, 19);

    do {
        cond_resched();
        mutex_lock(&table_mutex);
        err = ops->insert((entries + i), &stats);
        mutex_unlock(&table_mutex);
        if (!rcuhashbash_verify_insert(entries + i))
            printk(KERN_ALERT "insert failure\n");
        i++;
    } while (!kthread_should_stop() && !err && i < insertct);

    *stats_ret = stats;

    while (!kthread_should_stop())
        schedule_timeout_interruptible(1);
    return err;
}

static struct rcuhashbash_ops all_ops[] = {
    {
        .test = "rcu",
        .read = rcuhashbash_read_rcu,
        .resize = rcuhashbash_resize,
        .insert = rhashtable_insert,
    },
    {
        .test = "ddds",
        .read = rcuhashbash_read_ddds,
        .resize = rcuhashbash_resize_ddds,
    },
    {
        .test = "rwlock",
        .read = rcuhashbash_read_rwlock,
        .resize = rcuhashbash_resize_rwlock,
    },
};

static struct rcuhashbash_ops *ops;

static void rcuhashbash_print_stats(void)
{
    int i;
    struct stats s = {};

    if (!thread_stats) {
        printk(KERN_ALERT "rcuhashbash stats unavailable\n");
        return;
    }

    for (i = 0; i < readers + resize + insert; i++) {
        s.read_hits += thread_stats[i].read_hits;
        s.read_hits_slowpath += thread_stats[i].read_hits_slowpath;
        s.read_hits_fallback += thread_stats[i].read_hits_fallback;
        s.read_misses += thread_stats[i].read_misses;
        s.resizes += thread_stats[i].resizes;
        s.insertions += thread_stats[i].insertions;
    }

    printk(KERN_ALERT
           "rcuhashbash summary: test=%s readers=%d resize=%s\n"
           "rcuhashbash summary: insert=%s insert counts=%d\n"
           "rcuhashbash summary: entries=%lu shift1=%u (%lu buckets) shift2=%u "
           "(%lu buckets)\n"
           "rcuhashbash summary: reads: %llu primary hits, %llu slowpath "
           "primary hits, %llu secondary hits, %llu misses\n"
           "rcuhashbash summary: resizes: %llu\n"
           "rcuhashbash summary: inserts: %llu\n"
           "rcuhashbash summary: %s\n"
           "rcuhashbash summary: total time: %llu ns\n",
           test, readers, resize ? "true" : "false", insert ? "true" : "false",
           insert ? insertct : 0, entries, shift1, 1UL << shift1, shift2,
           1UL << shift2, s.read_hits, s.read_hits_slowpath,
           s.read_hits_fallback, s.read_misses, s.resizes, s.insertions,
           s.read_misses == 0 ? "PASS" : "FAIL", end - start);
}

static void rcuhashbash_exit(void)
{
    unsigned long i;
    int ret;
    if (tasks) {
        for (i = 0; i < readers + resize + insert; i++)
            if (tasks[i]) {
                ret = kthread_stop(tasks[i]);
                if (ret)
                    printk(KERN_ALERT "rcuhashbash task returned error %d\n",
                           ret);
            }
        kfree(tasks);
    }
    end = ktime_get_ns();

    /* Wait for all RCU callbacks to complete. */
    rcu_barrier();

    if (table2) {
        printk(
            KERN_ALERT
            "rcuhashbash FAIL: secondary table still exists during cleanup.\n");
        kfree(table2);
    }

    if (table) {
        for (i = 0; i <= table->mask; i++) {
            struct hlist_head *head = &table->buckets[i];
            while (!hlist_empty(head)) {
                struct rcuhashbash_entry *entry;
                entry =
                    hlist_entry(head->first, struct rcuhashbash_entry, node);
                head->first = entry->node.next;
                kmem_cache_free(entry_cache, entry);
            }
        }
        kfree(table);
    }

    if (entry_cache)
        kmem_cache_destroy(entry_cache);

    rcuhashbash_print_stats();

    kfree(thread_stats);

    printk(KERN_ALERT "rcuhashbash done\n");
}

static __init int rcuhashbash_init(void)
{
    int ret;
    unsigned long i;

    for (i = 0; i < ARRAY_SIZE(all_ops); i++)
        if (strcmp(test, all_ops[i].test) == 0) {
            ops = &all_ops[i];
        }
    if (!ops) {
        printk(KERN_ALERT "rcuhashbash: No implementation with test=%s\n",
               test);
        return -EINVAL;
    }

    if (readers < 0)
        readers = num_online_cpus();

    if (!ops->read) {
        printk(KERN_ALERT "rcuhashbash: Internal error: read function NULL\n");
        return -EINVAL;
    }
    if (!ops->resize) {
        printk(KERN_ALERT
               "rcuhashbash: Internal error: resize function NULL\n");
        return -EINVAL;
    }

    entry_cache = KMEM_CACHE(rcuhashbash_entry, 0);
    if (!entry_cache)
        goto enomem;

    table =
        kzalloc(sizeof(*table) + (1UL << shift1) * sizeof(table->buckets[0]),
                GFP_KERNEL);
    if (!table)
        goto enomem;

    table->mask = (1UL << shift1) - 1;

    for (i = 0; i < entries; i++) {
        struct rcuhashbash_entry *entry;
        entry = kmem_cache_zalloc(entry_cache, GFP_KERNEL);
        if (!entry)
            goto enomem;
        entry->value = i;
        hlist_add_head(&entry->node, &table->buckets[i & table->mask]);
    }

    thread_stats =
        kcalloc(readers + resize + insert, sizeof(thread_stats[0]), GFP_KERNEL);
    if (!thread_stats)
        goto enomem;

    tasks = kcalloc(readers + resize + insert, sizeof(tasks[0]), GFP_KERNEL);
    if (!tasks)
        goto enomem;

    printk(KERN_ALERT "rcuhashbash starting threads\n");
    start = ktime_get_ns();
    for (i = 0; i < readers + resize + insert; i++) {
        struct task_struct *task;
        if (i < readers)
            task = kthread_run(rcuhashbash_read_thread, &thread_stats[i],
                               "rcuhashbash_read");
        else if (i < readers + resize)
            task = kthread_run(rcuhashbash_resize_thread, &thread_stats[i],
                               "rcuhashbash_resize");
        else
            task = kthread_run(rcuhashbash_insert_thread, &thread_stats[i],
                               "rcuhashbash_insert");
        if (IS_ERR(task)) {
            ret = PTR_ERR(task);
            goto error;
        }
        tasks[i] = task;
    }

    return 0;

enomem:
    ret = -ENOMEM;
error:
    rcuhashbash_exit();
    return ret;
}

module_init(rcuhashbash_init);
module_exit(rcuhashbash_exit);
