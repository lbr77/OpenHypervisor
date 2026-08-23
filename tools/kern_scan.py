
import subprocess, struct
K="/System/Library/Kernels/kernel.release.t8132"
data=open(K,"rb").read()
print("size:",len(data))
# MOVZ Wn,#0x4004 : 0x52800800 | Rd ; MOVK Wn,#0xFAE9,LSL#16 : 0x72A1F5D2 | Rd
hits=[]
for rd in range(0,32):
    a=struct.pack("<I",0x52800800|rd)
    b=struct.pack("<I",0x72A1F5D2|rd)
    i=data.find(a+b)
    while i!=-1:
        hits.append((i,rd)); i=data.find(a+b,i+4)
    i=data.find(a)
    while i!=-1:
        j=data.find(b,i+4,i+40)
        if j!=-1 and (j-i)%4==0: 
            if (i,j) not in [(h[0],h[0]) for h in hits]: pass
        i=data.find(a,i+4)
# dedupe close pairs incl non-adjacent
pairs=[]
for rd in range(32):
    a=struct.pack("<I",0x52800800|rd); b=struct.pack("<I",0x72A1F5D2|rd)
    pos=0
    while True:
        i=data.find(a,pos)
        if i==-1: break
        j=data.find(b,i,i+64)
        if j!=-1: pairs.append((i,j-i,rd))
        pos=i+4
print("ILLEGAL_GUEST_STATE sites:",len(pairs))
for off,d,rd in pairs[:24]:
    print(hex(off), "gap",d, "rd",rd)
