
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
targets={
 "get_vhe_el2":"0x21B3752C8",
 "set_vhe_el2":"0x21B3777C4",
 "get_nested_el1":"0x21B3753A0",
 "set_nested_el1":"0x21B377190",
 "is_vhe_enabled":"0x21B379138",
}
for tag,a in targets.items():
    rows=post("SELECT printf('0x%%X',x.from_addr) site, printf('0x%%X',x.from_func) fn FROM xrefs x WHERE x.to_addr=%s LIMIT 12"%a)["results"][0]["rows"]
    print("==",tag)
    names={}
    fns=set(r[1] for r in rows)
    if fns:
        q="SELECT addr,name FROM funcs WHERE addr IN (%s)"%(",".join(fns))
        for aa,nn in post(q)["results"][0]["rows"]: names[aa]=nn
    for r in rows:
        print("  ",r[0], names.get(r[1], r[1])[:85])
