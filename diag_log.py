import subprocess, sys, time, serial

# 用法：python diag_log.py [端口] [抓取秒数]
#   python diag_log.py COM16
#   python diag_log.py COM16 20
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM19"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

# 用 flash_id + hard_reset 触发复位（不烧录，速度快，复位后立刻开串口抓全量开机日志）
r = subprocess.run([sys.executable, "-m", "esptool", "--port", PORT,
                    "--chip", "esp32-c3", "--after", "hard_reset", "flash_id"],
                   capture_output=True, text=True)
print("reset rc =", r.returncode)

s = serial.Serial(PORT, 115200, timeout=1)
t0 = time.time()
buf = b""
while time.time() - t0 < DURATION:
    d = s.read(8192)
    if d:
        buf += d

txt = buf.decode("utf-8", "ignore")
for line in txt.splitlines():
    print(line)
print("===== LOG END =====")
