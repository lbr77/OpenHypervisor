
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("""SELECT DISTINCT f.addr fa, f.name FROM instructions i JOIN funcs f ON f.addr=i.func_addr
WHERE (i.disasm LIKE '%0x5C%' OR i.disasm LIKE '%0x5D%')
AND (f.name LIKE '%Hv4Vcpu%' OR f.name LIKE '%Hv2Vm%') LIMIT 60""")["results"][0]["rows"]
print("== 含 0x5C/0x5D 立即数的 Hv 函数 ==")
for a,n in rows: print(" ",a,n[:105])
print("count:",len(rows))
