import hashlib, re
P="C:/Users/mohut/xiaozhi-car/backup_com9/com9_boot_part_app.bin"
d=open(P,"rb").read()
app=d[0x20000:]
a=open("_com9_app_full.bin","rb").read()
def md5(b): return hashlib.md5(b).hexdigest()
print("dump10:33 app md5", md5(a), len(a))
print("dump10:57 app md5", md5(app), len(app))
print("same:", md5(a)==md5(app))
for name,buf in (("10:33",a),("10:57",app)):
    print("==",name)
    for pat in [b"XminiC3Board",b"es8311",b"ES8311",b"assets",b"Application",b"motor",b"ESP-ROM",b"IDF"]:
        print("   ",pat, buf.count(pat))
    m=re.findall(rb"\x1f\x8b\x08", buf[:200000])
    print("    gzip magic count(first200k):", len(m))
