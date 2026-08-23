
import subprocess, json, re, struct
K="/System/Library/Kernels/kernel.release.t8132"
S={int(k):v for k,v in json.load(open("/Users/libr/Desktop/life/vphone-qemu/.scratch/hvf-ida/kernel/kernel.release.t8132.symbols.json")).items()}
d=open(K,"rb").read()
ncmds=struct.unpack_from("<I",d,16)[0]
off=32; segs=[]
for _ in range(ncmds):
    cmd,sz=struct.unpack_from("<II",d,off)
    if cmd==0x19:
        nm=d[off+8:off+24].rstrip(b"\0").decode()
        vm,msz,fo,fsz=struct.unpack_from("<QQQQ",d,off+24)
        segs.append((vm,msz,fo,fsz))
    off+=sz
def vm2fo(a):
    for vm,msz,fo,fsz in segs:
        if vm<=a<vm+msz: return fo+(a-vm)
    return None
def fo2vm(f):
    for vm,msz,fo,fsz in segs:
        if fo<=f<fo+fsz: return vm+(f-fo)
    return None
d=open(K,"rb").read()
r=subprocess.run(["xcrun","llvm-objdump","-d","--start-address=0xfffffe000750f6b0","--stop-address=0xfffffe0007515700",K],capture_output=True,text=True)
targets={}
for ln in r.stdout.splitlines():
    m=re.search(r"\bbl\s+0x([0-9a-f]+)",ln)
    if m:
        a=int(m.group(1),16)
        targets.setdefault(a,0); targets[a]+=1
def nearest(a):
    best=None;bd=1<<62
    for sa,sn in S.items():
        if sa<=a and a-sa<bd: bd=a-sa;best=(sa,sn)
    return best
print("== 未命名 BL 目标中的 ERET 检测 ==")
for a,c in sorted(targets.items()):
    if S.get(a): continue
    fo=vm2fo(a)
    if fo is None: continue
    w=d[fo:fo+0x600]
    n_eret=w.count(b"\xe0\x03\x9f\xd6")
    if n_eret:
        na=nearest(a)
        print(f"  {hex(a)} calls={c} eret={n_eret} nearest={na[1][:50]}+{a-na[0]}")