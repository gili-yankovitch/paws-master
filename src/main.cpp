#include <stdint.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Keyboard.h>
#include <NeoPixelBus.h>

#define MAX_KEY_COUNT 128
#define MAX_BUFFER_DATA (16)

#define EEPROM_ADDR_IS_CONFIG 0x0
#define EEPROM_ADDR_CONFIG_SIZE 0x1
#define EEPROM_ADDR_CONFIG_START 0x3

#define SERIAL_RECV_MAGIC 0x4141
struct serial_config_s
{
	uint16_t magic;
	uint16_t size;
	uint8_t data[0];
};

#define LEDS_PIN 6
#define TOKEN_RECV_PIN 4
#define TOKEN_SEND_PIN 5

#ifndef O_NONBLOCK
#define O_NONBLOCK 00004000
#endif

unsigned int parse_state = 0;

#define CONFIG_MAGIC_IDX (0)
#define CONFIG_MAGIC_SIZE (2)
#define CONFIG_OBJNUM_IDX (CONFIG_MAGIC_IDX + CONFIG_MAGIC_SIZE)
#define CONFIG_OBJNUM_SIZE (2)
#define CONFIG_OBJARR_IDX (CONFIG_OBJNUM_IDX + CONFIG_OBJNUM_SIZE)
#define CONFIG_OBJ_SIZE (8)
#define CONFIG_OBJ_TYPE_OFFSET (0)
#define CONFIG_OBJ_TYPE_SIZE (1)
#define CONFIG_OBJ_BTN_IDX_OFFSET (CONFIG_OBJ_TYPE_OFFSET + CONFIG_OBJ_TYPE_SIZE)
#define CONFIG_OBJ_BTN_IDX_SIZE (1)
#define CONFIG_OBJ_DATA_OFFSET (CONFIG_OBJ_BTN_IDX_OFFSET + CONFIG_OBJ_BTN_IDX_SIZE)

#define CONFIG_OBJ_KEYVAL_IDX (0)

#define CONFIG_OBJ_LED_R_IDX (0)
#define CONFIG_OBJ_LED_G_IDX (1)
#define CONFIG_OBJ_LED_B_IDX (2)

#define CONFIG_OBJ_LED_ANIMATION (3)

#define CONFIG_BEGIN 0x4242
#define CONFIG_KEY 0x01
#define CONFIG_LED 0x02
#define CONFIG_ANIMATION 0x03

enum btn_state_e
{
	BTN_STATE_RELEASED = 0,
	BTN_STATE_PRESSED
};

#define BASE_ASSIGN_ADDR 2

static enum btn_state_e *btnStates = NULL;
static uint8_t assignAddr = BASE_ASSIGN_ADDR;

struct key_obj_s
{
	uint8_t keyValue;
	struct key_obj_s *next;
};

struct led_obj_s
{
	uint8_t ledR;
	uint8_t ledG;
	uint8_t ledB;
};

enum animation_type
{
	ANIMATION_GRADIENT = 0,
	ANIMATION_PULSE = 1,
	ANIMATION_STILL = 2
};

struct animation_obj_s
{
	enum animation_type type;

	struct led_obj_s color;
};

struct config_obj_s
{
	uint8_t type;
	uint8_t btnIdx;

	union
	{
		struct key_obj_s key;

		struct led_obj_s clickColor;

		struct animation_obj_s animation;

		uint8_t tmp[CONFIG_OBJ_SIZE];
	} data;
};

struct config_s
{
	uint16_t configMagic;
	uint16_t configObjNum;
	struct config_obj_s objects[0];
} *config = NULL;

// ***** CONFIG PROTOCOL DEFINITION *****
// Protocol is little-endian ints
// | NAME             | VALUE     | SIZE (bytes) | REMARK                       |
// | CONFIG_BEGIN     | 0x42 0x42 | 2            | Magic number - Config begin  |
// | CONFIG_SIZE      | 0xXX 0xXX | 2            | Number of objects in config  |

// ***** Config objects *****
// *** Key object ***
// | CONFIG_KEY       | 0x01      | 1            | Type number - Key val obj    |
// | CONFIG_KEY_VAL   | 0xXX      | 1            | Value for i-th key           |
// | EMPTY            | 0xXX      | 2            | Alignment for 4-bytes object |

// *** LED object ***
// | CONFIG_LED       | 0x02      | 1            | Type number - LED val obj    |
// | CONFIG_LED_R_VAL | 0xXX      | 1            | Red value for i-th key       |
// | CONFIG_LED_G_VAL | 0xXX      | 1            | Green value for i-th key     |
// | CONFIG_LED_B_VAL | 0xXX      | 1            | Blue value for i-th key      |

bool initDone;

static unsigned tokenRecvCnt;
static unsigned tokenSentCnt;

static size_t btnNum = 0;
static uint16_t animationCycle = 0;
static struct key_obj_s **keyMap = NULL;
static struct led_obj_s **ledsMap = NULL;
static struct animation_obj_s **animationMap = NULL;

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> ledStrip(MAX_KEY_COUNT, LEDS_PIN);

static void eepromWriteByte(unsigned addr, uint8_t data)
{
	EEPROM.write(addr, data);
}

static uint8_t eepromReadByte(unsigned addr)
{
	return EEPROM.read(addr);
}

static void eepromWriteHWord(unsigned addr, uint16_t data)
{
	EEPROM.write(addr + 0, (data & 0x00ff) >> 0);
	EEPROM.write(addr + 1, (data & 0xff00) >> 8);
}

static uint16_t eepromReadHWord(unsigned addr)
{
	uint16_t data = 0;

	data |= EEPROM.read(addr + 0) << 0;
	data |= EEPROM.read(addr + 1) << 8;

	return data;
}

static void eepromDumpConfig(uint8_t *config, uint16_t size)
{
	unsigned i;

	eepromWriteByte(EEPROM_ADDR_IS_CONFIG, 1);

	// Write the config size
	eepromWriteHWord(EEPROM_ADDR_CONFIG_SIZE, size);

	// Dump the config
	for (i = 0; i < size; ++i)
	{
		eepromWriteByte(EEPROM_ADDR_CONFIG_START + i, config[i]);
	}
}

static bool isConfigured()
{
	static bool isConf = false;

	if (!isConf)
		isConf = (eepromReadByte(EEPROM_ADDR_IS_CONFIG) == 1) ? true : false;

	return isConf;
}

static uint16_t eepromLoadConfig(uint8_t **config)
{
	uint16_t size = 0;
	unsigned i;

	if (!isConfigured())
	{
		Serial.println("Could not load config. Not initialized.");

		goto error;
	}

	// Read the size
	size = eepromReadHWord(EEPROM_ADDR_CONFIG_SIZE);

	// Allocate data
	*config = (uint8_t *)malloc(size);

	// Read the data
	for (i = 0; i < size; ++i)
	{
		(*config)[i] = eepromReadByte(EEPROM_ADDR_CONFIG_START + i);
	}

error:
	return size;
}

static size_t serialRecv(uint8_t *buf, size_t size, int flags)
{
	size_t read = 0;

	if (flags & O_NONBLOCK)
	{
		return Serial.readBytes(buf, size);
	}

	while (read != size)
	{
		while (!Serial.available())
			;

		read += Serial.readBytes(buf + read, size - read);
	}

	return read;
}

static int parseConfig(uint8_t *buf, size_t size)
{
	int err = -1;
	uint16_t magic;
	uint16_t objnum;
	size_t i;
	unsigned configObjId;

	parse_state = __LINE__;

	// Check magic
	magic = (buf[CONFIG_MAGIC_IDX + 0] << 0) | (buf[CONFIG_MAGIC_IDX + 1] << 8);

	if (magic != CONFIG_BEGIN)
	{
		Serial.println("Error parsing config: Invalid magic.");

		goto error;
	}

	parse_state = __LINE__;

	// Get number of objects
	objnum = (buf[CONFIG_OBJNUM_IDX + 0] << 0) | (buf[CONFIG_OBJNUM_IDX + 1] << 8);

	// Check that recv size correlates to number of objects
	if (size != (CONFIG_MAGIC_SIZE + CONFIG_OBJNUM_SIZE + CONFIG_OBJ_SIZE * objnum))
	{
		Serial.println("Error parsing config: Invalid config sizes. Expected: " +
					   String(CONFIG_MAGIC_SIZE + CONFIG_OBJNUM_SIZE + CONFIG_OBJ_SIZE * objnum) + " Got: " + String(size));

		goto error;
	}

	parse_state = __LINE__;

	// Free previous config
	if (config)
	{
		free(config);
	}

	// Allocate config
	config = (struct config_s *)malloc(sizeof(struct config_s) + sizeof(struct config_obj_s) * objnum);

	// Populate fields
	config->configMagic = magic;
	config->configObjNum = objnum;

	// Populate objects
	for (i = 0, configObjId = 0; i < config->configObjNum; ++i)
	{
		uint8_t configType = buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_TYPE_OFFSET];
		uint8_t btnIdx = buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_BTN_IDX_OFFSET];

		parse_state = __LINE__;

		if (btnIdx >= btnNum)
		{
			// Ignore this config - No such button idx.
			continue;
		}

		switch (configType)
		{
		case CONFIG_KEY:
		{
			uint8_t keyValue = buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_KEYVAL_IDX];

			parse_state = __LINE__;

			config->objects[configObjId].data.key.keyValue = keyValue;
			config->objects[configObjId].data.key.next = NULL;

			// Map keys
			if (keyMap[btnIdx] == NULL)
			{
				keyMap[btnIdx] = &config->objects[configObjId].data.key;
			}
			else
			{
				struct key_obj_s *last = keyMap[btnIdx];

				// Find the last object to append to
				while (last->next)
				{
					last = last->next;
				}

				last->next = &config->objects[configObjId].data.key;
			}

			break;
		}

		case CONFIG_LED:
		{
			config->objects[configObjId].data.clickColor.ledR =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_R_IDX];
			config->objects[configObjId].data.clickColor.ledG =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_G_IDX];
			config->objects[configObjId].data.clickColor.ledB =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_B_IDX];

			ledsMap[btnIdx] = &config->objects[configObjId].data.clickColor;

			break;
		}

		case CONFIG_ANIMATION:
		{
			config->objects[configObjId].data.animation.color.ledR =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_R_IDX];
			config->objects[configObjId].data.animation.color.ledG =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_G_IDX];
			config->objects[configObjId].data.animation.color.ledB =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_B_IDX];
			config->objects[configObjId].data.animation.type = (enum animation_type)
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_DATA_OFFSET + CONFIG_OBJ_LED_ANIMATION];
			break;
		}

		default:
			break;
		}

		config->objects[configObjId].btnIdx = btnIdx;
		config->objects[configObjId].type = configType;

		// Valid config. Advance...
		configObjId++;
	}

	err = 0;
error:
	return err;
}

static int handleSerialConfig()
{
	int err = -1;
	struct serial_config_s *data;
	uint16_t magic;
	uint16_t size;

	if (!Serial.available())
	{
		goto done;
	}

	// Read data request
	if (Serial.read() != 0x42)
	{
		Serial.println("Error receiving read request magic");

		goto done;
	}

	// Write magic number so desktop can identify this as the correct port
	Serial.write("\x42\x69");

	// Receive magic
	if (serialRecv((uint8_t *)&magic, sizeof(magic), O_NONBLOCK) != sizeof(uint16_t))
	{
		Serial.println("Error receiving magic number");

		goto error;
	}

	// Verify magic
	if (magic != SERIAL_RECV_MAGIC)
	{
		Serial.println("Invalid serial magic number");

		goto error;
	}

	// Receive size
	if (serialRecv((uint8_t *)&size, sizeof(uint16_t), O_NONBLOCK) != sizeof(uint16_t))
	{
		Serial.println("Error recveing config size");

		goto error;
	}

	data = (struct serial_config_s *)malloc(sizeof(struct serial_config_s) + size);

	// Populate data
	if (serialRecv(data->data, size, O_NONBLOCK) != size)
	{
		Serial.println("Error receiving config data");

		goto error;
	}

	data->magic = magic;
	data->size = size;

	// Parse the config
	if (parseConfig(data->data, data->size) < 0)
	{
		Serial.println("Error parsing config");

		goto error;
	}

	Serial.print("\xFF");

	// Dump config to eeprom
	eepromDumpConfig(data->data, data->size);

	free(data);

done:
	err = 0;
error:
	return err;
}

static int configStartup()
{
	int err = -1;
	uint8_t *config;
	uint16_t size;

	// Try and get config
	if ((size = eepromLoadConfig(&config)) < 0)
	{
		Serial.println("Failed loading config on startup");

		// No config available
		goto error;
	}

	// Parse the config
	if (parseConfig(config, size) < 0)
	{
		Serial.println("Error parsing config on startup");

		goto error;
	}

	// Free buffer
	free(config);

	err = 0;
error:
	return err;
}

static void dataHandler(int size)
{
	// Wait for the data
	while (Wire.available() < 1)
		;

	// Get the addr
	uint8_t data = Wire.read();
	uint8_t addrRecvd = data & 0b01111111;
	enum btn_state_e recvState = (data & 0b10000000) == 0 ? BTN_STATE_RELEASED : BTN_STATE_PRESSED;

	if (btnStates[addrRecvd] == recvState)
		return;

	// Serial.println("Received: " + String(addrRecvd));
	// Serial.println("State: " + String(recvState));

	btnStates[addrRecvd] = recvState;
}

#define I2C_BCAST_ADDR (0)
#define I2C_MASTER_ADDR (1)
#define MAX_ADDR_ASSIGN_RETRIES (50)

#define REQUESTS

void I2CAddrAsignReq()
{
	Serial.println("Assigning addr...");
	// Send address on bus
	Wire.write(assignAddr);
}

void initializeI2CAddrs()
{
	// Reset assignAddr
	assignAddr = BASE_ASSIGN_ADDR;

	// Reset buttons number
	btnNum = 0;

	Serial.println("Initiating address distribution...");

	// Initalize I2C as master
	Wire.begin(I2C_MASTER_ADDR);

	Serial.println("Setting TOKEN_SEND to LOW");

	// Reset send pin
	digitalWrite(TOKEN_SEND_PIN, LOW);

	Serial.println("Waiting for TOKEN_RECV to be LOW...");

	// Wait for the last chip to initialize... (Assume LOW requirement)
	while (digitalRead(TOKEN_RECV_PIN) == HIGH)
		;

	Serial.println("Done. Kicking first chain member...");
#ifdef REQUESTS
	// Prepare request
	Wire.onRequest(I2CAddrAsignReq);
#endif
	// Signal to the first chip it is its time for address allocation
	digitalWrite(TOKEN_SEND_PIN, HIGH);

	unsigned retries = 0;

	// While last chip did not return the token,
	// distribute addresses
	while (1)
	{
		Serial.println("Assigning address: " + String(assignAddr) + "...");

		ledStrip.SetPixelColor((assignAddr - BASE_ASSIGN_ADDR), RgbColor(0, 0, 255));
		ledStrip.Show();

#ifdef REQUESTS
		// Wait for message from this address
		Wire.beginTransmission(I2C_BCAST_ADDR);
		Wire.write(assignAddr);
		Wire.endTransmission();
		Wire.requestFrom(assignAddr, (uint8_t)1);
#else
		Wire.beginTransmission(I2C_BCAST_ADDR);
		Wire.write(assignAddr);
		Wire.endTransmission();
#endif
		// Wait for 100ms
		delay(100);

		if (!Wire.available())
		{
			// In case something in the return path failed for some reason
			if ((++retries > MAX_ADDR_ASSIGN_RETRIES) && (assignAddr != BASE_ASSIGN_ADDR))
			{
				break;
			}

			Serial.println("Did not receive response. Retrying (" + String(retries) + ")...");
		}
		else
		{
			uint8_t ack = Wire.read();

			// Demand the ack to be the same address assigned
			if (ack == assignAddr)
			{
				Serial.println("Received ACK from " + String(ack));

				btnStates = (enum btn_state_e *)realloc(btnStates, sizeof(enum btn_state_e) * (assignAddr + 1));
				keyMap = (struct key_obj_s **)realloc(keyMap, sizeof(struct key_obj_s *) * (btnNum + 1));
				ledsMap = (struct led_obj_s **)realloc(keyMap, sizeof(struct led_obj_s *) * (btnNum + 1));
				animationMap = (struct animation_obj_s **)realloc(keyMap, sizeof(struct animation_obj_s *) * (btnNum + 1));

				// Reset this new cell
				keyMap[btnNum] = NULL;
				ledsMap[btnNum] = NULL;
				animationMap[btnNum] = NULL;

				// Increase number of buttons
				btnNum++;

				btnStates[assignAddr] = BTN_STATE_RELEASED;

				ledStrip.SetPixelColor((assignAddr - BASE_ASSIGN_ADDR), RgbColor(0, 255, 0));
				ledStrip.Show();

				// Increase addr assign
				assignAddr += 1;

				retries = 0;

				// Finish it up
				if (digitalRead(TOKEN_RECV_PIN) == HIGH)
				{
					break;
				}
			}
			else
			{
				Serial.println("Received something: " + String(ack));
			}
		}
	}

	// Finish setup, drive token LOW
	digitalWrite(TOKEN_SEND_PIN, LOW);

	Serial.println("Address distribution done.");
}

bool requested[MAX_ADDR_ASSIGN_RETRIES];

void setup()
{
	// Initialize EEPROM
	EEPROM.begin();

	// Initialize serial
	Serial.begin(115200);

	// Initialize init boolean
	initDone = false;

	// Reset token counters
	tokenRecvCnt = 0;
	tokenSentCnt = 0;

	Serial.println("Board booted.");

	// Setup token pins
	pinMode(TOKEN_SEND_PIN, OUTPUT);
	pinMode(TOKEN_RECV_PIN, INPUT); // INPUT_PULLUP ?
	// attachInterrupt(digitalPinToInterrupt(TOKEN_RECV_PIN), tokenRecv, RISING);

	// Reset LEDs
	for (int i = 0; i < MAX_KEY_COUNT; ++i)
	{
		ledStrip.SetPixelColor(i, RgbColor(0, 0, 0));
	}

	ledStrip.Show();

	// Assign all addresses
	initializeI2CAddrs();

	// Load config - Only now I know how many buttons there are
	if ((isConfigured()) && (configStartup() < 0))
	{
		Serial.println("Error loading config. Is it initialized?");
	}

	// Start questioning all the modules
	Wire.onReceive(dataHandler);

	for (int i = 0; i < MAX_KEY_COUNT; ++i)
	{
		ledStrip.SetPixelColor(i, RgbColor(0, 0, 255));
	}

	ledStrip.Show();

	// Initialize keyboard
	// Keyboard.begin();

	for (int i = 0; i < MAX_ADDR_ASSIGN_RETRIES; ++i)
	{
		requested[i] = false;
	}

	// Now act as slave
	Wire.begin(I2C_BCAST_ADDR);
}

static RgbColor Gradient(uint8_t btnIdx)
{
	uint8_t WheelPos = 255 - (((btnIdx * 256 / btnNum) + animationCycle) & 0xFF);

	if (WheelPos < 85)
	{
		return RgbColor(255 - WheelPos * 3, 0, WheelPos * 3);
	}
	else if (WheelPos < 170)
	{
		WheelPos -= 85;
		return RgbColor(0, WheelPos * 3, 255 - WheelPos * 3);
	}
	else
	{
		WheelPos -= 170;
		return RgbColor(WheelPos * 3, 255 - WheelPos * 3, 0);
	}
}

static RgbColor Pulse(uint8_t btnIdx)
{
	uint8_t cycle;

	if ((animationCycle % 512) < 256)
	{
		cycle = animationCycle & 0xff;
	}
	else
	{
		cycle = 255 - (animationCycle & 0xff);
	}

	uint32_t r = ledsMap[btnIdx]->ledR;
	uint32_t g = ledsMap[btnIdx]->ledG;
	uint32_t b = ledsMap[btnIdx]->ledB;

	r *= cycle;
	g *= cycle;
	b *= cycle;

	r /= 255;
	b /= 255;
	r /= 255;

	return RgbColor(r, g, b);
}

static RgbColor Still(uint8_t btnIdx)
{
	return RgbColor(ledsMap[btnIdx]->ledR, ledsMap[btnIdx]->ledG, ledsMap[btnIdx]->ledB);
}

void loop()
{
	unsigned i = 0;
	static unsigned long prevReconfigMillis = 0;

	for (i = BASE_ASSIGN_ADDR; i < assignAddr; i++)
	{
		uint8_t btnIdx = i - BASE_ASSIGN_ADDR;

		if (btnStates[i] == BTN_STATE_PRESSED)
		{
			if (isConfigured())
			{
				led_obj_s *color = ledsMap[btnIdx];

				if (color)
				{
					ledStrip.SetPixelColor(btnIdx, RgbColor(color->ledR, color->ledG, color->ledB));

					continue;
				}
			}

			ledStrip.SetPixelColor(btnIdx, RgbColor(0, 255, 0));
		}
		else
		{
			if (isConfigured())
			{
				struct animation_obj_s *animation = animationMap[btnIdx];

				if (animation)
				{
					switch (animation->type)
					{
					case ANIMATION_GRADIENT:
					{
						ledStrip.SetPixelColor(btnIdx, Gradient(btnIdx));

						continue;
					}

					case ANIMATION_PULSE:
					{
						ledStrip.SetPixelColor(btnIdx, Pulse(btnIdx));

						continue;
					}

					case ANIMATION_STILL:
					{
						ledStrip.SetPixelColor(btnIdx, Still(btnIdx));
						continue;
					}

					default:
					{
						break;
					}
					}
				}
			}

			ledStrip.SetPixelColor(btnIdx, RgbColor(0, 0, 255));
		}
	}

	animationCycle++;

	ledStrip.Show();

	// Always try and update config
	if (millis() - prevReconfigMillis >= 1000)
	{
		handleSerialConfig();

		// Update millis
		prevReconfigMillis = millis();
	}

	// If init is not done, don't execute main logic yet
	if (!isConfigured())
	{
		return;
	}

	for (i = BASE_ASSIGN_ADDR; i < assignAddr; i++)
	{
		key_obj_s *obj = keyMap[i - BASE_ASSIGN_ADDR];

		if (btnStates[i] == BTN_STATE_PRESSED)
		{
			// Serial.println("Btn #" + String(i) + " PRESSED: " + String(keyMap[0]) + " Map size: " + String(keyMapSize) + " parseState: " + String(parse_state) + " Config objects: " + String(config ? config->configObjNum : 0));

			// Press all buttons
			while (obj)
			{
				Keyboard.press(obj->keyValue);

				obj = obj->next;
			}
		}
		else
		{
			// Release all buttons
			while (obj)
			{
				Keyboard.release(obj->keyValue);

				obj = obj->next;
			}
		}
	}
}
