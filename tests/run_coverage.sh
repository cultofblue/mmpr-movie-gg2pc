#!/bin/bash
cd "$(dirname "$0")"
run() { m=$1; s=$2; n=$3; ${HEADLESS:-./headless} "${ROM:-mmpr_movie.gg}" $n -i -k "$(cat s_${m}$s.txt)" -c v3/cov_${m}$s.txt > v3/log_${m}$s.txt 2>&1; }
run story 1 30000 & run story 2 60000; wait
run story 3 60000 & run story 4 60000; wait
run story 5 60000 & run vs 1 40000; wait
run vs 2 40000 & run option 1 15000 & run option 2 15000; wait
run link 1 8000 & run idle 0 12000; wait
echo ALLDONE
