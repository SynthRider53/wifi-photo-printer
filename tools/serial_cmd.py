"""Usage: serial_cmd.py SECONDS [command ...]  - sends each command, then prints console output for SECONDS."""
import sys, time, serial

secs = float(sys.argv[1])
p = serial.Serial()
p.port = 'COM14'
p.baudrate = 115200
p.timeout = 0.2
p.dtr = False
p.rts = False
p.open()
for c in sys.argv[2:]:
    p.write((c + '\r\n').encode())
    time.sleep(0.3)
end = time.time() + secs
while time.time() < end:
    d = p.read(512)
    if d:
        sys.stdout.write(d.decode('utf-8', 'replace'))
        sys.stdout.flush()
