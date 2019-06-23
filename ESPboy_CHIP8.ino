/*
ESPboy chip8/schip emulator by RomanS
based on ideas of Alvaro Alea Fernandez Chip-8 emulator
Special thanks to Igor (corax69), DmitryL (Plague) and John Earnest (https://github.com/JohnEarnest/Octo/tree/gh-pages/docs) for help
*/


/*
2do:
<<<<<<< Updated upstream
DONE- timers correct attach/detach
DONE implememnt load and set from ".k" game file:
=======
DONE - timers correct attach/detach
DONE - implememnt load and set from ".k" game file: 
>>>>>>> Stashed changes
DONE - optimize TFT output
DONE - implement return keeping key pressed time
DONW - implement reset/exit pressing side buttons
DONE - implemet PROGMEM for fonts
DONE - clear screen using memset
DONE - implement help:
DONE - print game description/help
- implement super chip
- implement "hires" emulation
- NO is regs and stack should be in ram?
- add games + ".k" files to each game
DONE implement compatibility optimisation
*/

#include <FS.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_MCP23017.h>
#include <Adafruit_MCP4725.h>
#include <ESP8266WiFi.h>
#include "ESPboyLogo.h"
#include <pgmspace.h>
#include <Ticker.h>

//system
#define fontchip_OFFSET       0x38
#define NLINEFILES            14 //no of files in menu
#define csTFTMCP23017pin      8
#define LEDquantity           1
#define MCP23017address       0 // actually it's 0x20 but in <Adafruit_MCP23017.h> lib there is (x|0x20) :)
#define MCP4725dacresolution  8
#define MCP4725address        0

/*compatibility_emu var
8XY6/8XYE opcode
Bit shifts a register by 1, VIP: shifts rY by one and places in rX, SCHIP: ignores rY field, shifts existing value in rX.
bit1 = 1    <<= amd >>= takes vx, shifts, puts to vx, ignore vy
bit1 = 0    <<= and >>= takes vy, shifts, puts to vx

FX55/FX65 opcode
Saves/Loads registers up to X at I pointer - VIP: increases I by X, SCHIP: I remains static.
bit2 = 1    load and store operations leave i unchaged
bit2 = 0    I is set to I + X + 1 after operation

8XY4/8XY5/8XY7/ ??8XY6??and??8XYE??
bit3 = 1    arithmetic results write to vf after status flag
bit3 = 0    vf write only status flag

DXYN
bit4 = 1    wrapping sprites
bit4 = 0    clip sprites at screen edges instead of wrapping

BNNN (aka jump0)
Sets PC to address NNN + v0 - VIP: correctly jumps based on value in v0. SCHIP: also uses highest nibble of address to select register, instead of v0 (high nibble pulls double duty). Effectively, making it jumpN where target memory address is N##. Very awkward quirk.
bit5 = 1    Jump to CurrentAddress+NN ;4 high bits of target address determines the offset register of jump0 instead of v0.
bit5 = 0    Jump to address NNN+V0

DXYN check bit 8
bit6 = 1    drawsprite returns number of collised rows of the sprite + rows out of the screen lines of the sprite (check for bit8)
bit6 = 0    drawsprite returns 1 if collision/s and 0 if no collision/s

EMULATOR TFT DRAW
bit7 = 1    draw to TFT just after changing pixel by drawsprite() not on timer
bit7 = 0    redraw all TFT from display on timer

DXYN OUT OF SCREEN check bit 6
bit8 = 1    drawsprite does not add "number of out of the screen lines of the sprite" in returned value
bit8 = 0    drawsprite add "number of out of the screen lines of the sprite" in returned value
*/

//0b01000011 for AstroDodge
//0b01000011 for SpaceIviders
//0b11110111 for BLITZ
//0b01000000 for BRIX
#define DEFAULTCOMPATIBILITY    0b01000000 //bit bit8,bit7...bit1;
#define DEFAULTOPCODEPERFRAME   40
#define DEFAULTTIMERSFREQ       60 // freq herz
#define DEFAULTBACKGROUND       0  // check colors []
#define DEFAULTDELAY            1
#define DEFAULTSOUNDTONE        300
#define LORESDISPLAY            0
#define HIRESDISPLAY            1

//buttons
#define LEFT_BUTTON   (buttonspressed & 1)
#define UP_BUTTON     (buttonspressed & 2)
#define DOWN_BUTTON   (buttonspressed & 4)
#define RIGHT_BUTTON  (buttonspressed & 8)
#define ACT_BUTTON    (buttonspressed & 16)
#define ESC_BUTTON    (buttonspressed & 32)
#define LFT_BUTTON    (buttonspressed & 64)
#define RGT_BUTTON    (buttonspressed & 128)
#define LEFT_BUTTONn  0
#define UP_BUTTONn    1
#define DOWN_BUTTONn  2
#define RIGHT_BUTTONn 3
#define ACT_BUTTONn   4
#define ESC_BUTTONn   5
#define LFT_BUTTONn   6
#define RGT_BUTTONn   7

//pins
#define LEDPIN    D4
#define SOUNDPIN  D3

//lib colors
uint16_t colors[] = { TFT_BLACK, TFT_NAVY, TFT_DARKGREEN, TFT_DARKCYAN, TFT_MAROON,
					 TFT_PURPLE, TFT_OLIVE, TFT_LIGHTGREY, TFT_DARKGREY, TFT_BLUE, TFT_GREEN, TFT_CYAN,
					 TFT_RED, TFT_MAGENTA, TFT_YELLOW, TFT_WHITE, TFT_ORANGE, TFT_GREENYELLOW, TFT_PINK };

static const uint8_t PROGMEM fontchip[16 * 5] = {
	0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
	0x20, 0x60, 0x20, 0x20, 0x70, // 1
	0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
	0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
	0x90, 0x90, 0xF0, 0x10, 0x10, // 4
	0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
	0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
	0xF0, 0x10, 0x20, 0x40, 0x40, // 7
	0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
	0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
	0xF0, 0x90, 0xF0, 0x90, 0x90, // A
	0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
	0xF0, 0x80, 0x80, 0x80, 0xF0, // C
	0xE0, 0x90, 0x90, 0x90, 0xE0, // D
	0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
	0xF0, 0x80, 0xF0, 0x80, 0x80, // F
};


uint8_t   mem[0x1000]; 
uint8_t   reg[0x10];    // ram 0x0 - 0xF
uint8_t   stack[0x10];  // ram 0x16 - 0x36???  EA0h-EFFh
uint8_t   sp; 
volatile uint8_t stimer, dtimer;
uint16_t  pc,i;
uint8_t   display[128*64];// ram F00h-FFFh

static uint8_t   keys[8]={0};
static uint8_t   foreground_emu;
static uint8_t   background_emu;
static uint8_t   compatibility_emu;   // look above
static uint8_t   delay_emu;           // delay in microseconds before next opcode done
static uint8_t   opcodesperframe_emu; // how many opcodes should be done before screen updates
static uint8_t   timers_emu;          // freq of timers. standart 60hz
String           description_emu;     // file description
static uint16_t  soundtone_emu;

static uint16_t buttonspressed;


enum EMUSTATE {
  APP_HELP,
  APP_SHOW_DIR,
  APP_CHECK_KEY,
  APP_EMULATE
};
EMUSTATE emustate = APP_HELP;


TFT_eSPI tft = TFT_eSPI();
Adafruit_MCP23017 mcp;
Adafruit_NeoPixel pixels(LEDquantity, LEDPIN);
Adafruit_MCP4725 dac;
Ticker timers;

//keymapping 0-LEFT, 1-UP, 2-DOWN, 3-RIGHT, 4-ACT, 5-ESC, 6-LFT side button, 7-RGT side button
static uint8_t default_buttons[8] = { 4, 2, 8, 6, 5, 11, 4, 6 };
//1 2 3 C
//4 5 6 D
//7 8 9 E
//A 0 B F

void chip8timers()
{
	if (dtimer)
		dtimer--;
	if (stimer)
		stimer--;
}

uint16_t checkbuttons()
{
	buttonspressed = ~mcp.readGPIOAB() & 255;
	return (buttonspressed);
}

void updatedisplay()
{
	static uint32_t i, j;
	// static unsigned long tme;
	// tme=millis();
	for (i = 0; i < 32; i++)
		for (j = 0; j < 64; j++)
			if (display[(i << 6) + j])
				tft.fillRect(j << 1, (i << 1) + 16, 2, 2, colors[foreground_emu]);
			else
				tft.fillRect(j << 1, (i << 1) + 16, 2, 2, colors[background_emu]);
	// Serial.println(millis()-tme);
}

uint8_t iskeypressed(uint8_t key)
{
	uint8_t ret;
	ret = 0;
	checkbuttons();
	if (LEFT_BUTTON && keys[LEFT_BUTTONn] == key)
		ret++;
	if (UP_BUTTON && keys[UP_BUTTONn] == key)
		ret++;
	if (DOWN_BUTTON && keys[DOWN_BUTTONn] == key)
		ret++;
	if (RIGHT_BUTTON && keys[RIGHT_BUTTONn] == key)
		ret++;
	if (ACT_BUTTON && keys[ACT_BUTTONn] == key)
		ret++;
	if (ESC_BUTTON && keys[ESC_BUTTONn] == key)
		ret++;
	if (LFT_BUTTON && keys[LFT_BUTTONn] == key)
		ret++;
	if (RGT_BUTTON && keys[RGT_BUTTONn] == key)
		ret++;
	return (ret ? 1 : 0);
}

uint8_t waitanykey()
{
	uint8_t ret;
	while (!checkbuttons())
		delay(5);
	if (LEFT_BUTTON)
		ret = keys[LEFT_BUTTONn];
	if (UP_BUTTON)
		ret = keys[UP_BUTTONn];
	if (DOWN_BUTTON)
		ret = keys[DOWN_BUTTONn];
	if (RIGHT_BUTTON)
		ret = keys[RIGHT_BUTTONn];
	if (ACT_BUTTON)
		ret = keys[ACT_BUTTONn];
	if (ESC_BUTTON)
		ret = keys[ESC_BUTTONn];
	if (RGT_BUTTON)
		ret = keys[RGT_BUTTONn];
	if (LFT_BUTTON)
		ret = keys[LFT_BUTTONn];
	return ret;
}

uint16_t waitkeyunpressed()
{
	unsigned long starttime = millis();
	while (checkbuttons())
		delay(5);
	return (millis() - starttime);
}

uint8_t readkey()
{
	checkbuttons();
	if (LEFT_BUTTON)
		return LEFT_BUTTONn;
	if (UP_BUTTON)
		return UP_BUTTONn;
	if (DOWN_BUTTON)
		return DOWN_BUTTONn;
	if (RIGHT_BUTTON)
		return RIGHT_BUTTONn;
	if (ACT_BUTTON)
		return ACT_BUTTONn;
	if (ESC_BUTTON)
		return ESC_BUTTONn;
	if (LFT_BUTTON)
		return LFT_BUTTONn;
	if (RGT_BUTTON)
		return RGT_BUTTONn;
	if (!buttonspressed)
		return 255;
}

void loadrom(String filename)
{
	File f;
	String data;
	char *desc;
	uint16_t c;
	f = SPIFFS.open(filename, "r");
	f.read(&mem[0x200], f.size());
	f.close();
	filename.setCharAt(filename.length() - 3, 'k');
	filename = filename.substring(0, filename.length() - 2);
	if (SPIFFS.exists(filename))
	{
		f = SPIFFS.open(filename, "r");
		for (c = 0; c < 7; c++)
		{
			data = f.readStringUntil(' ');
			keys[c] = data.toInt();
		}
		data = f.readStringUntil('\n');
		keys[8] = data.toInt();
		data = f.readStringUntil('\n');
		foreground_emu = data.toInt();
		data = f.readStringUntil('\n');
		background_emu = data.toInt();
		data = f.readStringUntil('\n');
		delay_emu = data.toInt();
		data = f.readStringUntil('\n');
		compatibility_emu = data.toInt();
		data = f.readStringUntil('\n');
		opcodesperframe_emu = data.toInt();
		data = f.readStringUntil('\n');
		timers_emu = data.toInt();
		data = f.readStringUntil('\n');
		soundtone_emu = data.toInt();
		description_emu = f.readStringUntil('\n');
		description_emu = description_emu.substring(0, 314);
		f.close();
		if (foreground_emu == 255)
			foreground_emu = random(17) + 1;
		//compatibility_emu = DEFAULTCOMPATIBILITY;
	}
	else
	{
		for (c = 0; c < 8; c++)
			keys[c] = default_buttons[c];
		foreground_emu = random(10) + 9;
		background_emu = DEFAULTBACKGROUND;
		compatibility_emu = DEFAULTCOMPATIBILITY;
		delay_emu = DEFAULTDELAY;
		opcodesperframe_emu = DEFAULTOPCODEPERFRAME;
		timers_emu = DEFAULTTIMERSFREQ;
		soundtone_emu = DEFAULTSOUNDTONE;
		desc = (char *)"No configuration\nfile found\n\nUsing unoptimal\ndefault parameters";
		description_emu = desc;
	}
	chip8_reset();
}

void buzz()
{
	static uint8_t flagbuzz = 0;
	if (stimer > 1 && !flagbuzz)
	{
		flagbuzz++;
		tone(SOUNDPIN, soundtone_emu);
		pixels.setPixelColor(0, pixels.Color(30, 0, 0));
		pixels.show();
	}
	if (stimer < 1 && flagbuzz)
	{
		flagbuzz = 0;
		noTone(SOUNDPIN);
		pixels.setPixelColor(0, pixels.Color(0, 0, 0));
		pixels.show();
	}
}

uint8_t drawsprite(uint8_t x, uint8_t y, uint8_t size)
{
	uint8_t data, mask, c, d, ret, preret;
	uint32_t addrdisplay;
	ret = 0;
	preret = 0;
	if (!size)
		size = 16;
	for (c = 0; c < size; c++)
	{
		data = mem[i + c];
		mask = 128;
		preret = 0;
		for (d = 0; d < 8; d++)
		{
			addrdisplay = x + d + ((y + c) << 6);
			if ((x + d) < 64 && (y + c) < 32)
			{
				if ((data & mask) && display[addrdisplay])
					preret++;
				if (compatibility_emu & 64)
					if ((display[addrdisplay] && !(data & mask)) || (!display[addrdisplay] && (data & mask)))
						tft.fillRect((x + d) << 1, ((y + c) << 1) + 16, 2, 2, colors[foreground_emu]);
					else
						tft.fillRect((x + d) << 1, ((y + c) << 1) + 16, 2, 2, colors[background_emu]);
				display[addrdisplay] ^= (data & mask);
			}
			else
			{
				if (compatibility_emu & 8)
				{
					if ((x + d) > 63)
						addrdisplay -= 64;
					if ((y + c) > 31)
						addrdisplay -= (32 << 6);
					if ((display[addrdisplay] && !(data & mask)) || (!display[addrdisplay] && (data & mask)))
						tft.fillRect((x + d) << 1, ((y + c) << 1) + 16, 2, 2, colors[foreground_emu]);
					else
						tft.fillRect((x + d) << 1, ((y + c) << 1) + 16, 2, 2, colors[background_emu]);
					display[addrdisplay] ^= (data & mask);
				}
			}
			mask >>= 1;
		}
		if (preret)
			ret++;
		if ((y + c) > 31 && !(compatibility_emu & 64))
			ret++;
	}
	if (compatibility_emu & 32)
		return (ret);
	else
		return (ret ? 1 : 0);
}

void chip8_reset()
{
	tft.fillScreen(TFT_BLACK);
	memset(&display[0], 0, 64 * 32);
	pc = 0x200;
	sp = 0;
}

enum
{
	CHIP8_JMP = 0x1,
	CHIP8_JSR = 0x2,
	CHIP8_SKEQx = 0x3,
	CHIP8_SKNEx = 0x4,
	CHIP8_SKEQr = 0x5,
	CHIP8_MOVx = 0x6,
	CHIP8_ADDx = 0x7,
	CHIP8_MOVr = 0x8,
	CHIP8_SKNEr = 0x9,
	CHIP8_MVI = 0xa,
	CHIP8_JMI = 0xb,
	CHIP8_RAND = 0xc,
	CHIP8_DRYS = 0xd,

	CHIP8_EXT0 = 0x0,
	CHIP8_EXT0_CLS = 0xE0,
	CHIP8_EXT0_RTS = 0xEE,

	CHIP8_EXTF = 0xF,
	CHIP8_EXTF_GDELAY = 0x07,
	CHIP8_EXTF_KEY = 0x0a,
	CHIP8_EXTF_SDELAY = 0x15,
	CHIP8_EXTF_SSOUND = 0x18,
	CHIP8_EXTF_ADI = 0x1e,
	CHIP8_EXTF_FONT = 0x29,
	CHIP8_EXTF_XFONT = 0x30,
	CHIP8_EXTF_BCD = 0x33,
	CHIP8_EXTF_STR = 0x55,
	CHIP8_EXTF_LDR = 0x65,

	CHIP8_MATH = 0x8,
	CHIP8_MATH_MOV = 0x0,
	CHIP8_MATH_OR = 0x1,
	CHIP8_MATH_AND = 0x2,
	CHIP8_MATH_XOR = 0x3,
	CHIP8_MATH_ADD = 0x4,
	CHIP8_MATH_SUB = 0x5,
	CHIP8_MATH_SHR = 0x6,
	CHIP8_MATH_RSB = 0x7,
	CHIP8_MATH_SHL = 0xe,

	CHIP8_SK = 0xe,
	CHIP8_SK_RP = 0x9e,
	CHIP8_SK_UP = 0xa1,

};

uint8_t do_cpu()
{
	int16_t inst, in2, c, r;
	uint8_t in1, x, y, zz;

	inst = (mem[pc++] << 8) + mem[pc++];
	in1 = (inst >> 12) & 0xF;
	x = (inst >> 8) & 0xF;
	y = (inst >> 4) & 0xF;
	zz = inst & 0x00FF;
	switch (in1)
	{
	case CHIP8_JSR: // jsr xyz
		stack[sp] = pc;
		sp = ++sp & 0x0F;
		pc = inst & 0xFFF;
		break;

	case CHIP8_JMP: // jmp xyz
		pc = inst & 0x0FFF;
		break;

	case CHIP8_SKEQx: // skeq:  skip next opcode if r(x)=zz
		if (reg[x] == zz)
			pc += 2;
		break;

	case CHIP8_SKNEx: //skne: skip next opcode if r(x)<>zz
		if (reg[x] != zz)
			pc += 2;
		break;

	case CHIP8_SKEQr: //skeq: skip next opcode if r(x)=r(y)
		if (reg[x] == reg[y])
			pc += 2;
		break;

	case CHIP8_MOVx: // mov xxx
		reg[x] = zz;
		break;

	case CHIP8_ADDx: // add xxx
		reg[x] = reg[x] + zz;
		break;

	case CHIP8_SKNEr: //skne: skip next opcode if r(x)<>r(y)
		if (reg[x] != reg[y])
			pc += 2;
		break;

	case CHIP8_MVI: // mvi xxx
		i = inst & 0xFFF;
		break;

	case CHIP8_JMI: // jmi xxx
		if (!compatibility_emu & 16)
			pc = (inst & 0xFFF) + reg[0];
		else
			pc = (inst & 0xFFF) + reg[(inst >> 8) & 0xF];
		break;

	case CHIP8_RAND: //rand xxx
		reg[x] = random(256) & zz;
		break;

	case CHIP8_DRYS: //draw sprite
		reg[0xF] = drawsprite(reg[x], reg[y], inst & 0xF);
		break;

	case CHIP8_EXT0: //extended instruction
		switch (zz)
		{
		case CHIP8_EXT0_CLS:
			memset(&display[0], 0, 64 * 32);
			if (compatibility_emu & 64)
				tft.fillRect(0, 16, 128, 64, colors[background_emu]);
			break;
		case CHIP8_EXT0_RTS: // ret
			sp = --sp & 0xF;
			pc = stack[sp] & 0xFFF;
			break;
		}
		break;

	case CHIP8_MATH: //extended math instruction
		in2 = inst & 0xF;
		switch (in2)
		{
		case CHIP8_MATH_MOV: //mov r(x)=r(y)
			reg[x] = reg[y];
			break;
		case CHIP8_MATH_OR: //or
			reg[x] |= reg[y];
			break;
		case CHIP8_MATH_AND: //and
			reg[x] &= reg[y];
			break;
		case CHIP8_MATH_XOR: //xor
			reg[x] ^= reg[y];
			break;
		case CHIP8_MATH_ADD: //add
			r = reg[x] + reg[y];
			reg[x] = r & 0xFF;
			reg[0xf] = (((r & 0xF00) != 0) ? 1 : 0);
			if (x == 0xf && compatibility_emu & 4)
				reg[0xf] = r & 0xFF;
			break;
		case CHIP8_MATH_SUB: //sub
			r = reg[x] - reg[y];
			reg[x] = r & 0xff;
			reg[0xf] = ((r < 0) ? 0 : 1);
			if (x == 0xf && compatibility_emu & 4)
				reg[0xf] = r & 0xff;
			break;
		case CHIP8_MATH_SHR: //shr
			if (compatibility_emu & 1)
			{
				r = reg[x] & 0x1;
				reg[x] = reg[x] >> 1;
			}
			else
			{
				r = reg[y] & 0x1;
				reg[x] = reg[y] >> 1;
			}
			if (x == 0xf && !compatibility_emu & 4)
				reg[0xf] = r & 0xff;
			break;
		case CHIP8_MATH_RSB: //rsb
			r = reg[y] - reg[x];
			reg[x] = r & 0xFF;
			;
			reg[0xf] = ((r < 0) ? 0 : 1);
			if (x == 0xf && compatibility_emu & 4)
				reg[0xf] = r & 0xff;
			break;
		case CHIP8_MATH_SHL: //shl
			if (compatibility_emu & 1)
			{
				r = (reg[x] & 0x80) >> 8;
				reg[x] = (reg[x] << 1) & 0xFF;
			}
			else
			{
				r = (reg[y] & 0x80) >> 8;
				reg[x] = (reg[y] << 1) & 0xFF;
			}
			if (x == 0xf && !compatibility_emu & 4)
				reg[0xf] = r & 0xff;
			break;
		}
		break;

	case CHIP8_SK: //extended instruction
		switch (zz)
		{
		case CHIP8_SK_RP: // skipifkey
			if (iskeypressed(reg[x]))
				pc += 2;
			break;
		case CHIP8_SK_UP: // skipifnokey
			if (!iskeypressed(reg[x]))
				pc += 2;
			break;
		}
		break;

	case CHIP8_EXTF: //extended instruction
		switch (zz)
		{
		case CHIP8_EXTF_GDELAY: // getdelay
			reg[x] = dtimer;
			break;
		case CHIP8_EXTF_KEY: //waitkey
			reg[x] = waitanykey();
			break;
		case CHIP8_EXTF_SDELAY: //setdelay
			dtimer = reg[x];
			break;
		case CHIP8_EXTF_SSOUND: //setsound
			stimer = reg[x];
			break;
		case CHIP8_EXTF_ADI: //add i+r(x)
			i += reg[x];
			break;
		case CHIP8_EXTF_FONT: //fontchip i
			i = fontchip_OFFSET + (reg[x] * 5);
			break;
		case CHIP8_EXTF_BCD: //bcd
			mem[i] = reg[x] / 100;
			mem[i + 1] = (reg[x] / 10) % 10;
			mem[i + 2] = (reg[x]) % 10;
			break;
		case CHIP8_EXTF_STR: //save
			memcpy(&mem[i], &reg[0], x + 1);
			if (!(compatibility_emu & 2))
				i = i + x + 1;
			break;
		case CHIP8_EXTF_LDR: //load
			memcpy(&reg[0], &mem[i], x + 1);
			if (!(compatibility_emu & 2))
				i = i + x + 1;
			break;
		}
		break;
	}

	return (0);
}

void do_emuation()
{
	uint16_t c = 0;
	timers.attach_ms((uint8_t)(1000.0f / (float)timers_emu), chip8timers);
	memcpy_P(&mem[fontchip_OFFSET], &fontchip[0], 16 * 5);
	while (1)
	{
		if (c < opcodesperframe_emu)
		{
			do_cpu();
			buzz();
			delay(delay_emu);
			c++;
		}
		else
		{
			if (!(compatibility_emu & 64))
				updatedisplay();
      
			c = 0;
		}
		if (LFT_BUTTON && RGT_BUTTON)
		{
			chip8_reset();
			if (waitkeyunpressed() > 200)
				break;
		}
	}
	timers.detach();
}

void setup()
{
	Serial.begin(115200); //serial init
	if (!SPIFFS.begin())
	{
		SPIFFS.format();
		SPIFFS.begin();
	}
	delay(100);
	WiFi.mode(WIFI_OFF); // to safe some battery power

	//LED init
	pinMode(LEDPIN, OUTPUT);
	pixels.begin();
	delay(100);
	pixels.setPixelColor(0, pixels.Color(0, 0, 0));
	pixels.show();

	//buttons on mcp23017 init
	mcp.begin(MCP23017address);
	delay(100);
	for (int i = 0; i < 8; i++)
	{
		mcp.pinMode(i, INPUT);
		mcp.pullUp(i, HIGH);
	}

	//TFT init
	mcp.pinMode(csTFTMCP23017pin, OUTPUT);
	mcp.digitalWrite(csTFTMCP23017pin, LOW);
	tft.init();
	tft.setRotation(0);
	tft.fillScreen(TFT_BLACK);

	//draw ESPboylogo
	tft.drawXBitmap(30, 24, ESPboyLogo, 68, 64, TFT_YELLOW);
	tft.setTextSize(1);
	tft.setTextColor(TFT_YELLOW);
	tft.setCursor(6, 102);
	tft.print("Chip8/Schip emulator");

	//sound init and test
	pinMode(SOUNDPIN, OUTPUT);
	tone(SOUNDPIN, 200, 100);
	delay(100);
	tone(SOUNDPIN, 100, 100);
	delay(100);
	noTone(SOUNDPIN);

	//DAC init
	dac.begin(0x60);
	delay(100);
	dac.setVoltage(4095, true);

	//clear TFT
	delay(2000);
	tft.fillScreen(TFT_BLACK);
}




void loop()
{
	static uint16_t selectedfilech8, countfilesonpage, countfilesch8, c, maxfilesch8;
	Dir dir;
	static String filename, selectedfilech8name;
	dir = SPIFFS.openDir("/");
	maxfilesch8 = 0;
	while (dir.next())
	{
		filename = String(dir.fileName());
		if (filename.lastIndexOf(String(".ch8")))
			maxfilesch8++;
	}

	switch (emustate)
	{
	case APP_HELP:
		tft.setTextColor(TFT_YELLOW);
		tft.setTextSize(1);
		tft.setCursor(0, 0);
		tft.print(
			"upload .ch8 to spiffs\n"
			"\n"
			"add config file for\n"
			" - keys remap\n"
			" - fore/back color\n"
			" - compatibility ops\n"
			" - opcodes per redraw\n"
			" - timers freq hz\n"
			" - sound tone\n"
			" - description\n"
			"\n"
			"during the play press\n"
			"both side buttons for\n"
			" - RESET - shortpress\n"
			" - EXIT  - longpress");
		waitkeyunpressed();
		waitanykey();
		waitkeyunpressed();
		selectedfilech8 = 1;
		if (maxfilesch8)
			emustate = APP_SHOW_DIR;
		break;
	case APP_SHOW_DIR:
		dir = SPIFFS.openDir("/");
		countfilesch8 = 0;
		c = 0;
		while (countfilesch8 < (selectedfilech8 / (NLINEFILES + 1)) * (NLINEFILES + 1) - 1)
		{
			dir.next();
			filename = String(dir.fileName());
			if (filename.lastIndexOf(String(".ch8")))
				countfilesch8++;
		}
		tft.fillScreen(TFT_BLACK);
		tft.setCursor(0, 0);
		tft.setTextColor(TFT_YELLOW);
		tft.print("SELECT GAME:");
		tft.setTextColor(TFT_MAGENTA);
		countfilesonpage = 0;
		while (dir.next() && (countfilesonpage < NLINEFILES))
		{
			filename = String(dir.fileName());
			if (filename.lastIndexOf(String(".ch8")))
			{
				if (filename.indexOf(".") < 17)
					filename = filename.substring(1, filename.indexOf("."));
				else
					filename = filename.substring(1, 16);
				filename = "  " + filename;
				countfilesch8++;
				if (countfilesch8 == selectedfilech8)
				{
					selectedfilech8name = filename;
					filename.trim();
					filename = String("* " + filename);
					tft.setTextColor(TFT_YELLOW);
				}
				else
				{
					tft.setTextColor(TFT_MAGENTA);
				}
				tft.setCursor(0, countfilesonpage * 8 + 12);
				tft.print(filename);
				countfilesonpage++;
			}
		}
		emustate = APP_CHECK_KEY;
		break;
	case APP_CHECK_KEY:
		waitkeyunpressed();
		waitanykey();
		if (UP_BUTTON && selectedfilech8 > 1)
			selectedfilech8--;
		if (DOWN_BUTTON && (selectedfilech8 < maxfilesch8))
			selectedfilech8++;
		if (ACT_BUTTON)
		{
			selectedfilech8name.trim();
			loadrom("/" + selectedfilech8name + ".ch8");
			tft.setCursor((((21 - selectedfilech8name.length())) / 2) * 6, 0);
			tft.setTextColor(TFT_YELLOW);
			tft.print(selectedfilech8name);
			tft.setCursor(0, 12);
			tft.setTextColor(TFT_MAGENTA);
			tft.print(description_emu);
			waitkeyunpressed();
			waitanykey();
			waitkeyunpressed();
			emustate = APP_EMULATE;
		}
		else
			emustate = APP_SHOW_DIR;
		waitkeyunpressed();
		break;
	case APP_EMULATE: //chip8 emulation
		tft.fillScreen(TFT_BLACK);
		do_emuation();
		emustate = APP_SHOW_DIR;
		break;
	}
}