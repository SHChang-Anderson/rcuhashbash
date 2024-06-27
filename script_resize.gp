set title "8k buckets without resizing"
set xlabel "Reader threads"
set ylabel "Lookups"
set terminal png font "Times_New_Roman, 12"
set key right

set output "8k_without_resize.png"


plot \
"8k_buckets_without_resizing_rcu.txt" using 1:2 with linespoints linewidth 2 title "rcu", \
"8k_buckets_without_resizing_ddds.txt" using 1:2 with linespoints linewidth 2 title "ddds", \
"8k_buckets_without_resizing_rwlock.txt" using 1:2 with linespoints linewidth 2 title "rwlock", \