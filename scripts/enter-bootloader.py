# Reboot the running firmware into the ROM bootloader via the USB CDC 1200-baud touch.
# No-op when no ACM port exists (already in bootloader, or ESP not attached).
import glob, sys, time
try:
    import serial
except ImportError:
    sys.exit(0)
for p in glob.glob("/dev/ttyACM*"):
    try:
        serial.Serial(p, 1200).close()
        time.sleep(3)
    except Exception as e:
        print("bootloader touch failed on %s: %s" % (p, e))
