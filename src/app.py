#!/usr/bin/python3
import sys
import serial.tools.list_ports
from functools import reduce
from struct import pack, unpack
from serial import Serial

CONFIG_MAGIC = 0x4242
CONFIG_OBJ_TYPE_KEY = 0x1
CONFIG_OBJ_TYPE_LED = 0x2


class Config:
    def __init__(self, objects):
        self.objects = objects

    def objPad(self, obj):
        OBJ_SIZE = 8
        return obj + bytes([0] * (OBJ_SIZE - len(obj)))

    def encode(self):
        config = pack("<HH", CONFIG_MAGIC, len(self.objects)) + reduce(
            lambda x, y: x + y,
            [self.objPad(config.encode()) for config in self.objects],
        )
        return pack("<HH", 0x4141, len(config)) + config


class ConfigObj:
    def __init__(self, idx, type):
        self.btnIdx = idx
        self.type = type

    def encode(self):
        return pack("<BB", self.type, self.btnIdx)


class ConfigKey:
    def __init__(self, idx, key):
        ConfigObj.__init__(self, idx, CONFIG_OBJ_TYPE_KEY)

        self.key = key

    def encode(self):
        return ConfigObj.encode(self) + pack("<B", self.key)


class ConfigLED:
    def __init__(self, idx, r, g, b):
        ConfigObj.__init__(self, idx, CONFIG_OBJ_TYPE_LED)
        self.r = r
        self.g = g
        self.b = b

    def encode(self):
        return ConfigObj.encode(self) + pack("<BBB", self.r, self.g, self.b)


def portName(port):
    if sys.platform.startswith("win"):
        return port
    elif sys.platform == "linux":
        return "/dev/{PORT}".format(PORT=port)


def probePort():
    for p in serial.tools.list_ports.comports():
        try:
            s = Serial(portName(p.name), 115200, timeout=4)

            # Write something
            s.write(bytes([0x42]))

            # Try reading
            data = s.read(2)

            if data == bytes("\x42\x69", "ascii"):
                print("Found: {PORT}".format(PORT=p.name))
                return s

            s.close()

        except OSError as e:
            # Invalid
            pass

    return None


def main():
    # Create config
    c = Config([ConfigKey(0, ord("a")), ConfigLED(0, 0x66, 0x00, 0xCC)])

    conf_data = c.encode()

    data = ""

    for x in conf_data:
        data += "%02x " % x

    print(data)

    exit(1)

    s = probePort()

    if s is None:
        print("Could not find port")

        return

    print("Using port named: {PORT}".format(PORT=s.name))

    # Write config
    s.write(conf_data)

    # Try reading
    data = s.read(1024)

    s.close()

    if data != b"\xff":
        print("Error writing config: " + data)

        exit(1)

    print("Done")


if __name__ == "__main__":
    main()
