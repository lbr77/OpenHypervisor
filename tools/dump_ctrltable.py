
import subprocess, json
def post(sql):
    p=subprocess.run(["curl","-s","-m","180","-X","POST","http://127.0.0.1:8091/query","--data-binary",sql],capture_output=True,text=True)
    return json.loads(p.stdout)
# 1) offset table contents
env=post("SELECT hex(blob_concat(value)) FROM bytes WHERE start_addr=0x21B3F4858 AND n=144;")
h=env["results"][0]["rows"][0][0]
print("ctrl table hex:", h)
offs=[int(h[i:i+16],16) for i in range(0,len(h),16)]
print("offsets:", [hex(o) for o in offs])
# 2) find capabilities filler
rows=post("SELECT printf('0x%X',addr),name FROM funcs WHERE name LIKE '%Capabilities%' OR name LIKE 'get_capabilities'")["results"][0]["rows"]
print("caps funcs:", rows[:6])
