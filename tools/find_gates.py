
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("""SELECT DISTINCT f.addr fa, f.name FROM instructions i JOIN funcs f ON f.addr=i.func_addr
WHERE (i.disasm LIKE '%#0x138%' OR i.disasm LIKE '%#0x139%' OR i.disasm LIKE '%#0x13A%')
AND (f.name LIKE '%Hv4Vcpu%' OR f.name LIKE '%HvCore%' OR f.name LIKE '%Hv2Vm%') LIMIT 60""")["results"][0]["rows"]
print("== EL2/VHE/GIC 门控函数 ==")
for a,n in rows: print(" ",a,n[:108])
print("count:",len(rows))
