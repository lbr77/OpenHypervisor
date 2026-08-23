
import subprocess, json, re
S={int(k):v for k,v in json.load(open("/Users/libr/Desktop/life/vphone-qemu/.scratch/hvf-ida/kernel/kernel.release.t8132.symbols.json")).items()}
r=subprocess.run(["xcrun","llvm-objdump","-d","--start-address=0xfffffe000750f6b0","--stop-address=0xfffffe0007515700","/System/Library/Kernels/kernel.release.t8132"],capture_output=True,text=True)
targets={}
order=[]
for ln in r.stdout.splitlines():
    m=re.search(r"\bbl\s+0x([0-9a-f]+)",ln)
    if m:
        a=int(m.group(1),16)
        if a not in targets: order.append(a)
        targets[a]=targets.get(a,0)+1
for a in order[:30]:
    print(hex(a), S.get(a,"?")[:70], "x%d"%targets[a])
