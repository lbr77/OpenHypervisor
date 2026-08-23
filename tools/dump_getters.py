
import subprocess, json, re
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
env=post("SELECT disasm_range(0x21B37C880, 0x21B37CA80);")
txt="\n".join(r[0] for r in env["results"][0]["rows"])
print(txt[:4600])
rows=post("SELECT printf('0x%X',addr),name FROM funcs WHERE name LIKE '%vtimer_offset%'")["results"][0]["rows"]
print("== vtimer_offset fns ==")
for a,n in rows: print(" ",a,n[:80])
