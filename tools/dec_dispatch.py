
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","240","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
for tag,a in [("GET dispatch","0x21B3755D0"),("SET dispatch","0x21B376CC8")]:
    env=post("SELECT decompile(%s);"%a)
    txt=env["results"][0]["rows"][0][0]
    print("="*14,tag)
    print(txt[:3400])
