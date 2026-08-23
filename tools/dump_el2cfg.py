
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("SELECT printf('0x%X',addr) a,name FROM funcs WHERE name LIKE '%config_set_el2%' OR name LIKE '%config_set_isa%' OR name LIKE '%config_set_vhe%'")["results"][0]["rows"]
for a,n in rows: print(a,n[:80])
for a,n in rows:
    env=post("SELECT disasm_range(%s,%s);"%(a,hex(int(a,16)+0xc0)))
    print("="*12,n)
    print("\n".join(r[0] for r in env["results"][0]["rows"])[:1500])
