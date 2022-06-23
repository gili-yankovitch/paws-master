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


KEY_CODE_LCTRL = 0x80
KEY_CODE_LSHIFT = 0x81

KEY_CODE_UP_ARROW = 0xDA
KEY_CODE_DOWN_ARROW = 0xD9
KEY_CODE_LEFT_ARROW = 0xD8
KEY_CODE_RIGHT_ARROW = 0xD7
KEY_CODE_PLAYPAUSE = ord(" ")
KEY_CODE_MUTE = 0x74

KEY_CODE_VOLUME_UP = 72
KEY_CODE_VOLUME_DOWN = 73

lastPort = None


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
    PRESS_TYPE_ONCE = 0
    PRESS_TYPE_CONT = 1

    def __init__(self, idx, key, press_type=PRESS_TYPE_CONT):
        ConfigObj.__init__(self, idx, CONFIG_OBJ_TYPE_KEY)

        self.key = key
        self.press_type = press_type

    def encode(self):
        return ConfigObj.encode(self) + pack("<BB", self.key, self.press_type)


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
    global lastPort

    # Remember the last port I used
    if n is None and lastPort is not None:
        n = lastPort

    if n is not None:
        return tryProbing(Serial(portName(n), 115200, timeout=4))

    for p in serial.tools.list_ports.comports():
        try:
            print("Trying %s" % p.name)
            s = Serial(portName(p.name), 115200, timeout=4)

            if tryProbing(s) is not None:
                lastPort = s.name
                return s

            s.close()

        except OSError as e:
            # Invalid
            pass

    return None


def writeData(s, data):
    s.write(data)


def writeConfig(s, c):
    # Write config
    # s.write(c.encode())
    writeData(s, c.encode())

    # Try reading
    data = s.read(1024)

    print("Response:", data)

    if data != b"\xff":
        print(
            "Error writing config (%d): '%s'" % (len(data), data.decode("ascii") + "'")
        )

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


def functions():
    s = probePort("COM22")

    if s is None:
        print("Could not find port")

        return

    print("Using port named: {PORT}".format(PORT=s.name))

    num = readNumberOfModules(s)

    print("Number of modules: %d" % num)

    s.close()

    return

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


def main():
    # s = probePort("ttyS22")
    s = probePort("COM22")

    # Create config
    c = Config(
        [
            ConfigKey(0, KEY_CODE_PLAYPAUSE),
            ConfigLED(0, ConfigColor(0xCC, 0x00, 0x55)),
            ConfigAnimation(0, ConfigAnimation.GRADIENT),
            ConfigKey(1, KEY_CODE_LCTRL, press_type=ConfigKey.PRESS_TYPE_ONCE),
            ConfigKey(1, KEY_CODE_LSHIFT, press_type=ConfigKey.PRESS_TYPE_ONCE),
            ConfigKey(1, ord("m"), press_type=ConfigKey.PRESS_TYPE_ONCE),
            ConfigLED(1, ConfigColor(0xCC, 0x00, 0x55)),
            ConfigAnimation(1, ConfigAnimation.GRADIENT),
            ConfigKey(2, KEY_CODE_UP_ARROW),
            ConfigLED(2, ConfigColor(0xCC, 0x00, 0x55)),
            ConfigAnimation(2, ConfigAnimation.GRADIENT),
            ConfigKey(3, KEY_CODE_DOWN_ARROW),
            ConfigLED(3, ConfigColor(0xCC, 0x00, 0x55)),
            ConfigAnimation(3, ConfigAnimation.GRADIENT),
            ConfigKey(4, KEY_CODE_RIGHT_ARROW),
            ConfigLED(4, ConfigColor(0xCC, 0x00, 0x55)),
            ConfigAnimation(4, ConfigAnimation.GRADIENT),
            ConfigKey(5, KEY_CODE_LEFT_ARROW),
            ConfigLED(5, ConfigColor(0xCC, 0x00, 0x55)),
            ConfigAnimation(5, ConfigAnimation.GRADIENT),
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
    # functions()
