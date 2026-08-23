
import subprocess, json, re
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
a="0x21B377F60"
env=post("SELECT disasm_range(%s,%s);"%(a,hex(int(a,16)+0x2a0)))
txt="\n".join(r[0] for r in env["results"][0]["rows"])
lines=txt.splitlines()
out=[]
for i,ln in enumerate(lines):
    if re.search(r"#0x138|#0x139|#0x13[Aa]", ln, re.I):
        out.append("\n".join(l.strip()[:96] for l in lines[max(0,i-3):i+3]))
        out.append("----")
print("\n".join(out)[:4600])
