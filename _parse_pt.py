import struct, sys
for fn in ["_com9_head.bin","_com9_app_full.bin","C:/Users/mohut/xiaozhi-car/backup_com9/com9_boot_part_app.bin","C:/Users/mohut/xiaozhi-car/backup_com9/com9_assets.bin"]:
    try:
        d=open(fn,"rb").read()
    except Exception as e:
        print(fn, "ERR", e); continue
    print("==",fn,len(d),hex(len(d)))
    if len(d)>0x8000+32*9:
        off=0x8000
        for i in range(95):
            e=d[off+i*32:off+i*32+32]
            if len(e)<32: break
            if e[:2]==b"\xaa\x50":
                t,st,po,sz=e[2],e[3],struct.unpack("<I",e[4:8])[0],struct.unpack("<I",e[8:12])[0]
                lab=e[12:28].split(b"\x00")[0].decode("utf8","ignore")
                print("   ",lab,hex(po),hex(sz), ("enc" if t&1 else ""), ("app" if t==0 else ""))
            elif e==b"\xff"*32:
                break
