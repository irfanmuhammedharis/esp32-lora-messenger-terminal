#!/usr/bin/env python3
"""Reset the ESP32 (normal boot) and capture serial output from the first byte."""
import serial
import sys
import time

PORT = '/dev/ttyUSB0'
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0

ser = serial.Serial(PORT, BAUD, timeout=0.1)

# ESP32 devkit auto-reset circuit: RTS -> EN, DTR -> GPIO0 (both through
# transistors, so asserted = inverted at the chip). Transistor on = pull low:
#   DTR=True  -> GPIO0 LOW  (download strap)
#   DTR=False -> GPIO0 HIGH (normal boot)
# Normal boot: hold EN low with RTS, keep DTR False, then release RTS.
ser.setDTR(False)  # GPIO0 HIGH -> normal boot strap
ser.setRTS(True)   # EN low  -> hold in reset
time.sleep(0.2)
ser.setRTS(False)  # EN high -> release, boots normally
ser.reset_input_buffer()

start = time.time()
while time.time() - start < DURATION:
    n = ser.in_waiting
    if n:
        data = ser.read(n)
        sys.stdout.write(data.decode('utf-8', 'replace'))
        sys.stdout.flush()
    else:
        time.sleep(0.01)
