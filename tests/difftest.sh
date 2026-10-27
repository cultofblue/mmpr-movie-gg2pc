#!/bin/bash
# difftest.sh SCRIPTFILE FRAMES -> compares interpreter vs recompiled hashes
S="$(cat $1)"; N=$2; T=$(mktemp -d)
${HEADLESS:-./headless} "${ROM:-mmpr_movie.gg}" $N -i -k "$S" -h $T/i.txt 2>$T/i.log
( cd $T && ${HEADLESS_RC:-./headless_rc} "${ROM:-mmpr_movie.gg}" $N -k "$S" -h $T/r.txt 2>$T/r.log )
python3 - $T $1 <<'P'
import sys
T=sys.argv[1]
a=open(T+'/i.txt').read().split('\n'); b=open(T+'/r.txt').read().split('\n')
for i,(x,y) in enumerate(zip(a,b)):
    if x!=y: print(sys.argv[2],'DIFF at frame',i,x,'|',y); break
else: print(sys.argv[2],'identical frames:',len(a)-1)
P
cat $T/r.log; [ -f $T/recomp_misses.txt ] && echo "misses:" && cat $T/recomp_misses.txt
