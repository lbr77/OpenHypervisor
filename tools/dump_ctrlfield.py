
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","120","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
# 1) public control-field wrapper
env=post("SELECT disasm_range(0x21B3F2390, 0x21B3F23F0);")
print("=== __hv_vcpu_get_control_field ===")
print("\n".join(r[0] for r in env["results"][0]["rows"])[:2600])
