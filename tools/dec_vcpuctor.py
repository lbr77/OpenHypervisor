
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
a="0x21B377CE0"
env=post("SELECT decompile(%s);"%a)
txt=env["results"][0]["rows"][0][0]
# 只输出与 flag/ctx 写入相关的片段
import re
keep=[]
for ln in txt.splitlines():
    if re.search(r'0x138|0x139|0x13[aA]|0x140\]|v27|v26|v25|param|el2|vhe|gic', ln, re.I):
        keep.append(ln)
print("\n".join(keep[:70])[:4800])
