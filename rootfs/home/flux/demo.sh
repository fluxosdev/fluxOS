#!/bin/fsh
# A small tour of fsh scripting.

fib() {
    a=0
    b=1
    i=0
    while [ $i -lt $1 ]; do
        printf "%d " $a
        t=$((a + b))
        a=$b
        b=$t
        i=$((i + 1))
    done
    echo
}

echo "fluxOS $(uname -r) on $(uname -m), running as pid $$"
echo "First 15 Fibonacci numbers:"
fib 15

echo "Programs in /bin: $(ls /bin | wc -l)"

for f in /etc/*; do
    if [ -f $f ]; then
        echo "  $f: $(wc -l < $f) lines"
    fi
done

echo "Starting three CPU-bound workers..."
spin one 2 &
spin two 2 &
spin three 2 &
wait
echo "All workers finished."
