
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
env=post("SELECT disasm_range(0x21B379D60, 0x21B379E48);")
rows=env["results"][0]["rows"]
print("\n".join(r[0] for r in rows)[:4200])
