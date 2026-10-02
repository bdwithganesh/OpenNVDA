#!/bin/zsh
# every filter's GPU + CPU output into $2 (ci_sweep dump), one process each
L=${1:?list}; D=${2:?dir}; mkdir -p $D
while read f; do perl -e 'alarm 60; exec @ARGV' ./ci_sweep dump $f $D/$f > $D/$f.txt 2>/dev/null; done < $L
echo DONE > $D/DONE
