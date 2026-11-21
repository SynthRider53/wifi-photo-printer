"""Usage: serial_reset.py SECONDS - resets the board via RTS and prints console output for SECONDS."""
import sys, time, serial

p = serial.Serial()
p.port = 'COM14'
p.baudrate = 115200
p.timeout = 0.2
p.dtr = False
p.rts = False
p.open()
p.rts = True
time.sleep(0.1)
p.rts = False
end = time.time() + float(sys.argv[1])
while time.time() < end:
    d = p.read(512)
    if d:
        sys.stdout.write(d.decode('utf-8', 'replace'))
        sys.stdout.flush()
