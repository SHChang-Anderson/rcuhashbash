set title "Insertion Performance: Bucket Lock vs Table Mutex"
set xlabel "Time (seconds)"
set ylabel "Insertions"
set terminal png font "Times_New_Roman, 12"
set key right

set output "insertion_performance_comparison.png"

plot \
"output_bucket_lock.txt" using 1:2 with linespoints linewidth 2 title "Bucket Lock", \
"output_table_mutex.txt" using 1:2 with linespoints linewidth 2 title "Table Mutex" \
