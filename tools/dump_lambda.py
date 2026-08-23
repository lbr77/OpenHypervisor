
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("SELECT printf('0x%X',addr),name FROM funcs WHERE name LIKE '%get_capabilities%$_0%' OR name LIKE '%set_virtual_timer_offset%' OR name LIKE '%get_virtual_timer_offset%'")["results"][0]["rows"]
print("candidates:")
for a,n in rows: print(" ",a,n[:90])
# dump lambda
for a,n in rows:
    if "$_0" in n:
        env=post("SELECT disasm_range(%s,%s);"%(a,hex(int(a,16)+0x400)))
        print("="*10,n)
        print("\n".join(r[0] for r in env["results"][0]["rows"])[:5000])
