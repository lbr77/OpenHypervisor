
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
print("== RegisterCollection ==")
rows=post("SELECT printf('0x%X',addr) a,name FROM funcs WHERE name LIKE '%RegisterCollection%'")["results"][0]["rows"]
for a,n in rows[:12]: print(a,n[:100])
print("== is_vhe_enabled 的调用者 ==")
rows=post("SELECT printf('0x%X',from_addr) site, f.name FROM xrefs x JOIN funcs f ON f.addr=x.from_func WHERE x.to_addr=0x21B379138 LIMIT 20")["results"][0]["rows"]
for a,n in rows: print(a,n[:95])
