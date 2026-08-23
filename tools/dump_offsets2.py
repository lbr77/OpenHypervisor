
import subprocess, json, re
def post(sql):
    p=subprocess.run(["curl","-s","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("SELECT addr,name FROM funcs")["results"][0]["rows"]
addr={}
for a,n in rows: addr.setdefault(n,int(a))
want=[n for n in addr if re.search(r"(set_vtimer_is_masked|get_execution_time|get_virtual_timer_offset|18get_sp_gl1)",n)]
for n in want[:5]:
    env=post("SELECT disasm_range(%d,%d);"%(addr[n],addr[n]+0xa0))
    txt="\n".join(r[0] for r in env["results"][0]["rows"])
    print("="*16,n.split("(")[0])
    for ln in txt.splitlines():
        m=re.search(r"(LDR|STR|ADD|MOVK|MOV)[A-Z]?\s.*#(0x[0-9A-Fa-f]+|\d+)",ln)
        if m and ("X9" in ln or "X8" in ln or "X10" in ln or "#0x" in ln): print("  ",ln.split(":")[-1].strip()[:90])
