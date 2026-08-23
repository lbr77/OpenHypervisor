
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
rows=post("""SELECT DISTINCT f.addr, f.name FROM instructions i JOIN funcs f ON f.addr=i.func_addr
WHERE i.mnemonic='LDRB' AND (i.disasm LIKE '%, #0x5D]%' OR i.disasm LIKE '%, #0x5C]%')
AND (f.name LIKE '%2Hv%' OR f.name LIKE '%HvCore%') LIMIT 40""")["results"][0]["rows"]
print("== 读 vm+0x5c/0x5d 的函数 ==")
for a,n in rows: print(" ",a,n[:100])
rows=post("SELECT printf('0x%X',from_addr) a, f.name FROM xrefs x JOIN funcs f ON f.addr=x.from_func WHERE x.to_addr=0x21B379140 LIMIT 15")["results"][0]["rows"]
print("== is_vhe_enabled thunk 调用者 ==")
for a,n in rows: print(" ",a,n[:95])
