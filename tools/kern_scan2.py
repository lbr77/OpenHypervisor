
import struct
data=open("/System/Library/Kernels/kernel.release.t8132","rb").read()
def find_all(pat,limit=64):
    out=[];i=data.find(pat)
    while i!=-1 and len(out)<limit:
        out.append(i); i=data.find(pat,i+1)
    return out
full=find_all(struct.pack("<I",0xFAE94004))
print("0xFAE94004 literal:",[hex(x) for x in full])
base=find_all(struct.pack("<I",0xFAE94000),20)
print("0xFAE94000 literal:",[hex(x) for x in base])
# also the QUOTA/other hv consts to confirm hv code region markers
for c,n in [(0xFae94fff,"QUOTA"),(0xfae94008,"FAULT"),(0xfae9400f,"UNSUPPORTED")]:
    print(n,[hex(x) for x in find_all(struct.pack("<I",c),12)])
