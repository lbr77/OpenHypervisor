
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","180","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
env=post("SELECT disasm_range(0x21B3F1D98, 0x21B3F2060);")
print("\n".join(r[0] for r in env["results"][0]["rows"])[:5200])
