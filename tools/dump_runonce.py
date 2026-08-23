
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
env=post("SELECT disasm_range(0x21B3786E8, 0x21B3787F0);")
txt="\n".join(r[0] for r in env["results"][0]["rows"])
# 只打印含 0x138/0x139/0x13A/0x920/0x988/0xA00/ORR 高位常量 的行及其上下文
lines=txt.splitlines()
for i,ln in enumerate(lines):
    if any(k in ln for k in ("0x138","0x139","0x13A","0x920","0xA00","ORR","#0x10","LDRB")):
        print(lines[max(0,i-1):i+2][0].strip()[:100])
        print("  >>",ln.strip()[:100])
