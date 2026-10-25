#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
xiaozhi-car C3 一键烧录脚本

用法:
    python flash_c3.py            # 自动找第一个串口
    python flash_c3.py COM19      # 指定串口

说明:
    - 烧录的是 build_c3/ 下的产物（idf.py -B build_c3 build 生成）
    - 分区表 0x270000(2.44MB) app + 0x280000 assets
    - 不用设 IDF 环境变量，只需要 pyserial（esptool 自带依赖）
"""
import os
import sys
import subprocess

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_c3")

FILES = [
    ("0x0",      os.path.join(BUILD, "bootloader", "bootloader.bin")),
    ("0x8000",   os.path.join(BUILD, "partition_table", "partition-table.bin")),
    ("0xd000",   os.path.join(BUILD, "ota_data_initial.bin")),
    ("0x10000",  os.path.join(BUILD, "xiaozhi.bin")),
    ("0x280000", os.path.join(BUILD, "generated_assets.bin")),
]


def pick_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    try:
        import serial.tools.list_ports as lp
        ports = [p.device for p in lp.comports()]
    except Exception:
        ports = []
    if not ports:
        print("没有找到任何串口。插上 USB 后再试，或用: python flash_c3.py COMxx")
        sys.exit(1)
    if len(ports) > 1:
        print("检测到多个串口 %s，用第一个；要换就: python flash_c3.py COMxx" % ports)
    return ports[0]


def main():
    for _, path in FILES:
        if not os.path.isfile(path):
            print("缺少文件: %s\n先跑一次 idf.py build" % path)
            sys.exit(1)

    port = pick_port()
    print("烧录端口: %s" % port)
    for _, path in FILES:
        print("  %-30s %6.1f KB" % (os.path.basename(path), os.path.getsize(path) / 1024.0))

    cmd = [sys.executable, "-m", "esptool",
           "--port", port, "--chip", "esp32-c3", "--baud", "460800",
           "write_flash"]
    for addr, path in FILES:
        cmd += [addr, path]

    rc = subprocess.call(cmd)
    if rc == 0:
        print("\n烧录完成，复位后日志: python diag_log.py")
    else:
        print("\n烧录失败(exit %d)。检查线/串口，或按住 BOOT 再上电进下载模式" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
