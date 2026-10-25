import hashlib
def hdr(p,n=32):
    d=open(p,"rb").read()
    return d[:n], d[-n:], len(d)
for p in ["_com9_head.bin","_com9_app_full.bin","C:/Users/mohut/xiaozhi-car/backup_com9/com9_assets.bin","build/generated_assets.bin","build/xiaozhi.bin"]:
    a,b,l=hdr(p)
    print(p, l, hex(l))
    print("   head:", a.hex())
    print("   tail:", b.hex())
