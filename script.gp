set title "Fixed hashtable size v.s resize between two sizes"
set xlabel "Reader threads"
set ylabel "Lookups/second (millions)"
set terminal png font "Times_New_Roman, 12"
set key right

set output "8k_16k_rwlock_resize.png"


plot \
"8k_16k_rwlock_resize.txt" using 1:2 with linespoints linewidth 2 title "8k (fixed)", \
"8k_16k_rwlock_resize.txt" using 1:3 with linespoints linewidth 2 title "16k (fixed)", \
"8k_16k_rwlock_resize.txt" using 1:4 with linespoints linewidth 2 title "resize (rwlock)"\