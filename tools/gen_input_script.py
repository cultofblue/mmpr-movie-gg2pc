import random,sys
# gen.py MODE SEED FRAMES -> input script on stdout
mode,seed,frames=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
r=random.Random(seed)
ev={}
def press(f,k,hold=5):
    ev[f]=k; ev[f+hold]='-'
for f in (700,820,940): press(f,'S')
t=1060
if mode=='story':
    press(t,'S')
elif mode=='vs':
    press(t,'D'); press(t+20,'S')
elif mode=='link':
    press(t,'D'); press(t+20,'D'); press(t+40,'S')
elif mode=='option':
    press(t,'D'); press(t+20,'D'); press(t+40,'D'); press(t+60,'S')
elif mode=='idle':
    ev.clear()
t+=120
# random play
f=t
while f<frames:
    if mode=='idle': break
    if f%700<12:   # periodic start double tap (advance text, pause+unpause)
        press(f,'S',4); press(f+10,'S',4); f+=20; continue
    if mode=='option' and r.random()<0.3:
        k=r.choice(['U','D','L','R','1','2','S'])
    else:
        k=''.join(sorted(set(r.choice(['','L','R','U','D','LU','RU','LD','RD']) + r.choice(['','1','2','12','1','2']))))
    if not k: k='-'
    dur=r.choice([3,6,10,20,40])
    ev[f]=k; f+=dur
print(';'.join(f'{k}:{v}' for k,v in sorted(ev.items())))
