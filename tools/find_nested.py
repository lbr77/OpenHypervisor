
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("SELECT printf('0x%X',addr) a,name FROM funcs WHERE name LIKE '%Nested%' OR name LIKE '%Vhe%' OR name LIKE '%vhe%'")["results"][0]["rows"]
seen=set()
for a,n in rows:
    if n in seen: continue
    seen.add(n)
    print(a,n[:110])
print("total:",len(seen))
