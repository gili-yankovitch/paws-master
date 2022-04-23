#!/usr/bin/python3
import sys
import serial.tools.list_ports
from functools import reduce
from struct import pack, unpack
from serial import Serial

CONFIG_MAGIC = 0x4242
CONFIG_OBJ_TYPE_KEY = 0x1
CONFIG_OBJ_TYPE_LED = 0x2
CONFIG_OBJ_TYPE_ANIMATION = 0x3


class Config:
    OBJ_SIZE = 8
    SERIAL_WRITE_CONFIG_MAGIC = 0x4141

    def __init__(self, objects):
        self.objects = objects

    def objPad(self, obj):
        return obj + bytes([0] * (Config.OBJ_SIZE - len(obj)))

    def encode(self):
        config = pack("<HH", CONFIG_MAGIC, len(self.objects)) + reduce(
            lambda x, y: x + y,
            [self.objPad(config.encode()) for config in self.objects],
        )
        return pack("<HH", Config.SERIAL_WRITE_CONFIG_MAGIC, len(config)) + config


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


class ConfigColor:
    def __init__(self, r, g, b):
        self.r = r
        self.g = g
        self.b = b

    def encode(self):
        return pack("<BBB", self.r, self.g, self.b)


class ConfigLED:
    def __init__(self, idx, color):
        ConfigObj.__init__(self, idx, CONFIG_OBJ_TYPE_LED)
        self.color = color

    def encode(self):
        return ConfigObj.encode(self) + self.color.encode()


class ConfigAnimation:
    GRADIENT = 0
    PULSE = 1
    STILL = 2

    def __init__(self, idx, type, color=None):
        ConfigObj.__init__(self, idx, CONFIG_OBJ_TYPE_ANIMATION)
        self.animation_type = type
        self.color = color

    def encode(self):
        return (
            ConfigObj.encode(self)
            + pack("<B", self.animation_type)
            + (bytes() if self.color == None else self.color.encode())
        )


def portName(port):
    if sys.platform.startswith("win"):
        return port
    elif sys.platform == "linux":
        return "/dev/{PORT}".format(PORT=port)


def tryProbing(s):
    # Reset previous transaction if there was any
    s.readall()

    # Write something
    s.write(bytes([0x42]))

    # Try reading
    data = s.read(2)

    if data == bytes("\x42\x69", "ascii"):
        print("Found: {PORT}".format(PORT=s.name))
        return s

    print(data + s.read(1024))

    return None


def probePort(n=None):
    if n is not None:
        return tryProbing(Serial(portName(n), 115200, timeout=4))

    for p in serial.tools.list_ports.comports():
        try:
            s = Serial(portName(p.name), 115200, timeout=4)

            if tryProbing(s) is not None:
                return s

            s.close()

        except OSError as e:
            # Invalid
            pass

    return None


def writeConfig(s, c):
    # Write config
    s.write(c.encode())

    # Try reading
    data = s.read(1024)

    if data != b"\xff":
        print("Error writing config: " + data)

        return False

    return True


def toggleBtnPrompt(s):
    s.write(bytes([0x43, 0x43]))

    eod = s.read(1)

    if eod != b"\xff":
        print("Error toggeling button prompt")

        return False

    return True


def toggleBtnPromptRelease(s):
    s.write(bytes([0x44, 0x44]))

    eod = s.read(1)

    if eod != b"\xff":
        print("Error toggeling button prompt")

        return False

    return True


def readNumberOfModules(s):
    # Request modules
    s.write(bytes([0x42, 0x42]))

    # Read the number of modules
    num = s.read(1)

    eod = s.read(1)

    if eod != b"\xff":
        print("Error recving number of modules")

        return -1

    return int(num[0])


def main():
    s = probePort("ttyS22")

    if s is None:
        print("Could not find port")

        return

    print("Using port named: {PORT}".format(PORT=s.name))

    num = readNumberOfModules(s)

    print("Number of modules: %d" % num)

    s.close()

    s = probePort("ttyS22")

    if not toggleBtnPrompt(s):
        print("Error toggeling button prompt")

        exit(1)

    idx = 0

    while idx < 10:
        btn = s.read(1)

        print("Pressed: %d" % (int(btn[0])))

        idx += 1

    s.close()

    s = probePort("ttyS22")

    if not toggleBtnPromptRelease(s):
        print("Error releasing button prompt")

        exit(1)

    s.close()

    exit(1)

    # Create config
    c = Config(
        [
            ConfigKey(0, ord("a")),
            ConfigLED(0, ConfigColor(0xFF, 0x00, 0x00)),
            ConfigAnimation(0, ConfigAnimation.GRADIENT),
        ]
    )

    conf_data = c.encode()

    data = ""

    for x in conf_data:
        data += "%02x " % x

    print(data)

    if not writeConfig(s, c):
        print("Error writing config")

    s.close()

    print("Done")


if __name__ == "__main__":
    main()
