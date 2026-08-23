
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","120","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
for name,a in [("get_control_field","0x21B374788"),("set_control_field","0x21B3747EC")]:
    env=post("SELECT disasm_range(%s, %s);"%(a,hex(int(a,16)+0x64)))
    print("===",name)
    print("\n".join(r[0] for r in env["results"][0]["rows"])[:2400])
