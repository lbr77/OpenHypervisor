
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
# who calls is_vhe_enabled
rows=post("SELECT printf('0x%X',from_addr) a, f.name FROM xrefs x JOIN funcs f ON f.addr=x.from_func WHERE x.to_addr=0x21B379138 LIMIT 25")["results"][0]["rows"]
print("== is_vhe_enabled callers ==")
for a,n in rows: print(" ",a,n[:95])
# all LDRB from offset 0x5c/0x5d of some reg (el2/vhe bytes) inside Hv:: code
rows=post("""SELECT i.func_addr, f.name, printf('0x%X',i.addr) site, i.disasm
FROM instructions i JOIN funcs f ON f.addr=i.func_addr
WHERE i.mnemonic='LDRB' AND (i.disasm LIKE '%, #0x5d]%' OR i.disasm LIKE '%, #0x5c]%')
AND f.name LIKE '%Hv%' LIMIT 40""")["results"][0]["rows"]
print("== Hv::* 读取 el2/vhe 字节 ==")
seen=set()
for fa,n,s,d in rows:
    if n in seen: continue
    seen.add(n); print(" ",s,n[:80],"|",d)
