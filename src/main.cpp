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

#define CONFIG_MAGIC_IDX (0)
#define CONFIG_MAGIC_SIZE (2)
#define CONFIG_OBJNUM_IDX (CONFIG_MAGIC_IDX + CONFIG_MAGIC_SIZE)
#define CONFIG_OBJNUM_SIZE (2)
#define CONFIG_OBJARR_IDX (CONFIG_OBJNUM_IDX + CONFIG_OBJNUM_SIZE)
#define CONFIG_OBJ_SIZE (4)
#define CONFIG_OBJ_MAGIC_SIZE (1)
#define CONFIG_OBJ_KEYVAL_IDX (0)
#define CONFIG_OBJ_LED_R_IDX (0)
#define CONFIG_OBJ_LED_G_IDX (1)
#define CONFIG_OBJ_LED_B_IDX (2)

#define CONFIG_BEGIN 0x4242
#define CONFIG_KEY 0x01
#define CONFIG_LED 0x02

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
};

struct led_obj_s
{
	uint8_t ledR;
	uint8_t ledG;
	uint8_t ledB;
};

struct config_obj_s
{
	uint8_t magic;
	union
	{
		struct key_obj_s key;

		struct led_obj_s led;

		uint8_t tmp[3];
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
// | CONFIG_KEY       | 0x01      | 1            | Magic number - Key val obj   |
// | CONFIG_KEY_VAL   | 0xXX      | 1            | Value for i-th key           |
// | EMPTY            | 0xXX      | 2            | Alignment for 4-bytes object |

// *** LED object ***
// | CONFIG_LED       | 0x02      | 1            | Magic number - LED val obj   |
// | CONFIG_LED_R_VAL | 0xXX      | 1            | Red value for i-th key       |
// | CONFIG_LED_G_VAL | 0xXX      | 1            | Green value for i-th key     |
// | CONFIG_LED_B_VAL | 0xXX      | 1            | Blue value for i-th key      |

bool initDone;

static unsigned tokenRecvCnt;
static unsigned tokenSentCnt;

static unsigned dataIdx;
static int dataBuffer[MAX_BUFFER_DATA];

static size_t keysNum = 0;
static int *keyIds = NULL;

static size_t keyMapSize = 0;
static uint8_t *keyMap = NULL;

static size_t ledMapSize = 0;
static struct led_obj_s *ledsMap = NULL;

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
	return (eepromReadByte(EEPROM_ADDR_IS_CONFIG) == 1) ? true : false;
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
		*config[i] = eepromReadByte(EEPROM_ADDR_CONFIG_START + i);
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

	// Check magic
	magic = (buf[CONFIG_MAGIC_IDX + 0] << 0) | (buf[CONFIG_MAGIC_IDX + 1] << 8);

	if (magic != CONFIG_BEGIN)
	{
		Serial.println("Error parsing config: Invalid magic.");

		goto error;
	}

	// Get number of objects
	objnum = (buf[CONFIG_OBJNUM_IDX + 0] << 0) | (buf[CONFIG_OBJNUM_IDX + 1] << 8);

	// Check that recv size correlates to number of objects
	if (size != (CONFIG_MAGIC_SIZE + CONFIG_OBJNUM_SIZE + CONFIG_OBJ_SIZE * objnum))
	{
		Serial.println("Error parsing config: Invalid config sizes. Expected: " +
					   String(CONFIG_MAGIC_SIZE + CONFIG_OBJNUM_SIZE + CONFIG_OBJ_SIZE * objnum) + " Got: " + String(size));

		goto error;
	}

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

	// Reset key map array
	keyMapSize = 0;

	if (keyMap)
	{
		free(keyMap);
	}

	// Reset led map array
	ledMapSize = 0;

	if (ledsMap)
	{
		free(ledsMap);
	}

	// Populate objects
	for (i = 0; i < config->configObjNum; ++i)
	{
		config->objects[i].magic = buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE];

		switch (config->objects[i].magic)
		{
		case CONFIG_KEY:
		{
			config->objects[i].data.key.keyValue = buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_MAGIC_SIZE + CONFIG_OBJ_KEYVAL_IDX];

			// Map keys
			keyMap = (uint8_t *)realloc(keyMap, sizeof(uint8_t) * ++keyMapSize);
			keyMap[keyMapSize - 1] = config->objects[i].data.key.keyValue;

			break;
		}

		case CONFIG_LED:
		{
			ledsMap = (struct led_obj_s *)malloc(sizeof(struct led_obj_s) * ++ledMapSize);
			ledsMap[ledMapSize - 1].ledR = config->objects[i].data.led.ledR =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_MAGIC_SIZE + CONFIG_OBJ_LED_R_IDX];
			ledsMap[ledMapSize - 1].ledG = config->objects[i].data.led.ledR =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_MAGIC_SIZE + CONFIG_OBJ_LED_G_IDX];
			ledsMap[ledMapSize - 1].ledB = config->objects[i].data.led.ledR =
				buf[CONFIG_OBJARR_IDX + i * CONFIG_OBJ_SIZE + CONFIG_OBJ_MAGIC_SIZE + CONFIG_OBJ_LED_B_IDX];

			ledStrip.SetPixelColor(ledMapSize - 1, RgbColor(ledsMap[ledMapSize - 1].ledR,
															ledsMap[ledMapSize - 1].ledG,
															ledsMap[ledMapSize - 1].ledB));

			break;
		}

		default:
			break;
		}
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

	// Receive magic
	if (serialRecv((uint8_t *)&magic, sizeof(magic), 0) != sizeof(uint16_t))
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
	if (serialRecv((uint8_t *)&size, sizeof(uint16_t), 0) != sizeof(uint16_t))
	{
		Serial.println("Error recveing config size");

		goto error;
	}

	data = (struct serial_config_s *)malloc(sizeof(struct serial_config_s) + size);

	// Populate data
	if (serialRecv(data->data, size, 0) != size)
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
	Serial.println("RECV DATA");

	// Wait for the data
	while (Wire.available() < 1)
		;

	// Get the addr
	uint8_t data = Wire.read();
	uint8_t addrRecvd = data & 0b01111111;

	Serial.println("Received: " + String(addrRecvd));
	Serial.println("State: " + String((data & 0b10000000) == 0 ? BTN_STATE_RELEASED : BTN_STATE_PRESSED));

	btnStates[addrRecvd] = (enum btn_state_e)((data & 0b10000000) == 0 ? BTN_STATE_RELEASED : BTN_STATE_PRESSED);
}

uint8_t idToKey(int id)
{
	size_t i;

	for (i = 0; i < keysNum; ++i)
	{
		if (id != keyIds[i])
		{
			continue;
		}

		// Maybe map is smaller than keyboard size?
		if (id >= keyMapSize)
		{
			return 0;
		}

		return keyMap[i];
	}
}

void xMitUSBData(int data)
{
	// TODO: Convert data to key strokes
	Keyboard.press(idToKey(data));
	delay(50);
	Keyboard.release(idToKey(data));
}

static void tokenRecv()
{
	if (!initDone)
	{
		initDone = true;
	}

	tokenRecvCnt++;
}

void tokenPass()
{
	digitalWrite(TOKEN_SEND_PIN, LOW);
	digitalWrite(TOKEN_SEND_PIN, HIGH);
	tokenSentCnt++;
}

#define I2C_BCAST_ADDR 0
#define I2C_MASTER_ADDR 1
#define MAX_ADDR_ASSIGN_RETRIES 40

#define REQUESTS

void I2CAddrAsignReq()
{
	Serial.println("Assigning addr...");
	// Send address on bus
	Wire.write(assignAddr);
}

void initializeI2CAddrs()
{
	// Start with addr '1' as 0 is reserved as 'broadcast'
	unsigned initTokenCount = tokenRecvCnt;
	uint8_t ack = assignAddr;

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
	// while (initTokenCount == tokenRecvCnt)
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
		Wire.requestFrom(assignAddr, 1);
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
#if 0
	// Load config
	if ((isConfigured()) && (configStartup() < 0))
	{
		Serial.println("Error loading config. Is it initialized?");
	}
#endif
	// Initialize init boolean
	initDone = false;

	// Reset token counters
	tokenRecvCnt = 0;
	tokenSentCnt = 0;

	// Reset data buffer index
	dataIdx = 0;

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

void loop()
{
	unsigned i = 0;

	for (i = BASE_ASSIGN_ADDR; i < assignAddr; i++)
	{
		if (btnStates[i] == BTN_STATE_PRESSED)
		{
			ledStrip.SetPixelColor(i - BASE_ASSIGN_ADDR, RgbColor(0, 255, 0));
		}
		else
		{
			ledStrip.SetPixelColor(i - BASE_ASSIGN_ADDR, RgbColor(0, 0, 255));
		}
	}

	ledStrip.Show();
#if 0
	unsigned i;

	// Always try and update config
	handleSerialConfig();

	// If init is not done, don't execute main logic yet
	if (!initDone)
	{
		return;
	}

	// Transmit data
	for (i = 0; i < dataIdx; ++i)
	{
		xMitUSBData(dataBuffer[i]);
	}

	// Reset data Idx
	dataIdx = 0;

	// Did I get the token? Pass it over
	if (tokenRecvCnt != tokenSentCnt)
	{
		tokenPass();
	}
#endif
}