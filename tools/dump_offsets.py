
import subprocess, json, re
def post(sql):
    p=subprocess.run(["curl","-s","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("SELECT addr,name FROM funcs")["results"][0]["rows"]
addr={}
for a,n in rows: addr.setdefault(n,int(a))
targets=[n for n in addr if re.search(r"2Hv4Vcpu\d+(get_register|get_simd_fp_register|set_vtimer_is_masked|get_execution_time)",n) and "Thn" not in n]
print("targets:",targets[:6])
for n in targets[:4]:
    env=post("SELECT disasm_range(%d,%d);"%(addr[n],addr[n]+0x120))
    txt="\n".join(r[0] for r in env["results"][0]["rows"])
    print("="*20,n)
    # print only instruction lines with immediates
    for ln in txt.splitlines():
        if re.search(r"#0x|LSL|LDR|STR",ln): print(" ",ln.split(":")[-1].strip())
