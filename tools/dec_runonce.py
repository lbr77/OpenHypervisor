
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","300","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
env=post("SELECT decompile(0x21B3786E8);")
txt=env["results"][0]["rows"][0][0]
print(txt[:5200])
