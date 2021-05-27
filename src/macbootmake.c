// converted from basic7.
// source was macbootmake, version 8 from 2017-02-06
//
#define VERSION	"11"
#define DATE	"8 Sep"
#define YEAR	"2020"

#include <cbm.h>
#include <errno.h>	// only for _oserror
#include <peekpoke.h>
#include <stdbool.h>
#include <stdint.h>

// limits for device address:
#define DEVICE_MIN	4
#define DEVICE_MAX	30
#define ALTDEVICE_MIN	4
#define ALTDEVICE_MAX	31	// 30 is really the maximum for device numbers, but we use 31 as special value, see below
#define ALTDEVICE_NONE	31	// this value is used for "use boot device", i.e. "do NOT use an alternative device"
// convenience macros
#define printat(x, y, msg)	do { gotoxy(x, y); print(msg); } while (0)
#define CHROUT(c)		cbm_k_bsout(c)
#define ON_VDC			PEEK(215)
// logical file numbers
#define LFN_CMD		1	// drive's command channel
#define	LFN_BUF		2	// block buffer
#define LFN_RAWDIR	3	// raw directory to determine drive/partition type
// secondary addresses
#define SA_CMD		15	// drive's command channel is 15
#define SA_BUF		2	// could be anything in 2..14 range
#define SA_RAWDIR	3	// ...as above, but must be different of course
// needed to put number in string:
#define STR(x)	#x	// stringize
#define XSTR(x)	STR(x)	// expand, then stringize
// control codes in strings
#define COLOR_EMPH	"\x05"	// white
#define COLOR_STD	"\x9b"	// light gray
#define REVSON		"\x12"
#define REVSOFF		"\x92"
#define HOME		"\x13"
// control codes (cc65 seems to garble some of them when inside strings, so don't put them there)
#define c_CONTROL_D	4	// used in menu for "scan for next device"
#define c_BELL		0x07	// C128 only!
#define c_LOCK		0x0b	// C128 only! C64 uses 8!
#define c_UNLOCK	0x0c	// C128 only! C64 uses 9!
#define c_HOME		0x13
#define c_CLEAR		0x93
#define c_LOWERCASE	0x0e
#define c_UPPERCASE	0x8e
#define c_ESCAPE	0x1b	// C128 only!

// drive/partition types:
struct dpt {
	uint8_t	valid;	// so "unknown" entry forbids action
	uint8_t	fiddle_with_bam;	// set if allocation/freeing must be done
	char	*track_and_sector;	// where to find allocation byte
	char	*byte_offset;		// byte offset in sector (and then use its lsb)
	char	*name;	// symbolic name to display
};
struct dpt	dpt_1541	= {1, 1, "18 0", "5",  "1541/1571"};
struct dpt	dpt_ieee	= {1, 1, "38 0", "7",  "SFD/8050/8250"};
struct dpt	dpt_1581	= {1, 1, "40 1", "17", "1581"};
struct dpt	dpt_cmdnative	= {1, 0, NULL,   NULL, "CMD native"};
struct dpt	dpt_cmdextnat	= {1, 0, NULL,   NULL, "CMD extended native"};
struct dpt	dpt_rawsd2iec	= {1, 0, NULL,   NULL, "raw SD2IEC"};
struct dpt	dpt_unknown	= {0, 0, NULL,   NULL, "unknown"};
// TODO: add Slave2CBM

// globals:
uint8_t		chosen_device;
bool		bootblock_active;	// flag: contents start with "cbm"
enum as {	// ternary for allocation state:
	AS_FREE,	// free for files
	AS_ALLOCATED,	// marked as used in BAM
	AS_RESERVED	// drive/partition reserves T1S0, so no need to check/alloc/free!
};
enum as		allocation_state;	// ternary (free/allocated/dontcarebecausereserved)
struct dpt	*dpt;	// disk/partition type
bool		redraw_screen;
bool		quit_program;
#define FILENAME_BUF_LEN	17	// 16 chars plus terminator
char		filename_buf[FILENAME_BUF_LEN];
#define MSG_BUF_LEN	255	// 254 chars plus terminator
char		message_buffer[MSG_BUF_LEN];
uint8_t		message_len;
// user config:
enum action {	// what to do when booting
	ACTION_RUNBASIC,
	ACTION_BOOTMC,
	// ACTION_GO64LOAD,	// TODO - add action for [enter c64 mode and load":*",8,1:run]
	ACTIONLIMIT
};
enum forcecase {	// which charset to use
	FORCE_NONE,
	FORCE_LOWER,
	FORCE_UPPER,
	FORCELIMIT
};
struct {
	bool		remove_boot_msg;
	bool		lock_charset;
	enum forcecase	force_case;
	bool		use_local_charset;
	enum action	action;	// what to do: RUN file or BOOT file or GO64?
	uint8_t		alternative_device;	// from where to load file?
	uint8_t		chosen_bank;	// bank in which to run machine code
} conf;


// helper functions:

// call something from BASIC 7 ROMs
// FIXME - this only works as long as this code is located before $4000 ROM itself!
static void __fastcall__ call_basic_rom(int address)
{
	POKE(0x3fd, 0x4c);	// JMP opcode
	POKEW(0x3fe, address);
	asm(
"		lda $ff00	\n"
"		pha		\n"
"		lda #0		\n"
"		sta $ff00	\n"	// full ROMs
"		jsr $03fd	\n"	// jump to "JMP address" at $3fd, $3fe, $3ff
"		pla		\n"
"		sta $ff00	\n"
	);
}

// wait a number of vic frames
static void __fastcall__ vsync_wait(uint8_t frames)
{
	uint8_t	x;

	do {
		// wait for interrupt
		x = PEEK(0xa2);
		while (PEEK(0xa2) == x)
			;
	} while (--frames);
#if 0
	// this code relies on cc65's argument stack handling...
	(void) frames;	// inhibit compiler warning about unused parameter
	asm(
"		ldy #0				\n"
"		lda (sp), y			\n"	// read frame count
"		tax				\n"
"loop1:			lda $a2			\n"	// loop until time change
"loop2:				cmp $a2		\n"
"				beq loop2"	"\n"
"			dex			\n"
"			bne loop1"		"\n"
	);
#endif
}

// reset both screens to system's default colors
static void colors_system(void)
{
	POKE(0xd020, 13);	// vic: bright green border
	POKE(0xd021, 11);	// vic: dark gray background
	asm(
"		lda #$f0	\n"	// vdc: black background
"		ldx #26		\n"
"		jsr $cdcc	\n"	// store A in vdc reg X
	);
	// set text colors
	if (ON_VDC) {
		POKE(0xf1, 7);		// current (vdc) color: bright cyan (rGBI)
		POKE(0x0a51, 13);	// other (vic) color: bright green
	} else {
		POKE(0xf1, 13);		// current (vic) color: bright green
		POKE(0x0a51, 7);	// other (vdc) color: bright cyan (rGBI)
	}
	call_basic_rom(0x77c7);	// go SLOW and enable vic display
}

// set colors and machine speed for current screen
static void colors_own(void)
{
	if (ON_VDC) {
		fast();
		asm(
"			lda #$f0	\n"	// vdc: black background
"			ldx #26		\n"
"			jsr $cdcc	\n"	// store A in vdc reg X
		);
		POKE(0xf1, 14);		// current (vdc) text color: light gray (dark white)
	} else {
		POKE(0xd020, 6);	// vic: blue border
		POKE(0xd021, 0);	// vic: black background
		POKE(0xf1, 15);		// current (vic) text color: light gray
		call_basic_rom(0x77c7);	// go SLOW and enable vic display
	}
}

// clear keyboard buffer
static void keybuf_clear(void)
{
	POKE(208, 0);
	POKE(209, 0);
}

// enable local charset (on US machines, this would activate CAPS LOCK instead)
static void localcharset_on(void)
{
	// FIXME - check values for cassette problems!
	POKE(0, 0x6f);	// set bit 6 to output
	POKE(1, 0x33);	// and pull down
	vsync_wait(2);	// wait so interrupt can update charset
}

// undo the effect of the function above
static void localcharset_off(void)
{
	// FIXME - check values for cassette problems!
	POKE(1, 0x73);	// release bit 6
	POKE(0, 0x2f);	// and set back to input
	vsync_wait(2);	// wait so interrupt can update charset
}

// output zero-terminated string
static void __fastcall__ print(const char *msg)
{
	while (*msg)
		CHROUT(*msg++);
}

// replacement for INPUT
// bufsize must include space for terminator
// returns number of characters _before_ terminator
// TODO: replace this - it calls BASIN, which allows the user to destroy the screen layout
static uint8_t __fastcall__ input(uint8_t bufsize, char *buffer)
{
	static uint8_t	byte,
			written;

	written = 0;
	do {
		byte = cbm_k_basin();
		if (written < bufsize) {
			buffer[written++] = byte;
		}
	} while (byte != 13);
	buffer[--written] = '\0';	// terminate
	return written;
}

// set cursor position
static void __fastcall__ gotoxy(uint8_t xx, uint8_t yy)
{
	// this code relies on cc65's argument stack handling...
	(void) xx;	// inhibit compiler warnings
	(void) yy;	// about unused parameters
	asm(
"		ldy #0		\n"
"		lda (sp), y	\n"	// read yy (column)
"		tax		\n"
"		iny		\n"
"		lda (sp), y	\n"	// read xx (line)
"		tay		\n"
"		clc		\n"	// clear C to set position
"		jsr $fff0	\n"
	);
}

// wait for key with visible cursor
static uint8_t key_with_crsr(void)
{
	asm(
"		jsr $cd6f		\n"	// activates cursor
"loop:			lda $d0		\n"	// anything in key buffer?
"			ora $d1		\n"	//	or F-key buffer?
"			beq loop"	"\n"
"		jsr $cd9f		\n"	// will hide cursor, but A must be non-zero to really switch it off
	);
	return cbm_k_getin();
}

// ask for key press
static void key_ask(void)
{
	print(COLOR_EMPH "[any key to go on]" COLOR_STD);
	keybuf_clear();
	while (cbm_k_getin() == 0)
		;
}


// show program info
static void help_show(void)
{
	print(
		"About this program:\n"
		"\n"
		"  Name     MacBootMake\n"
		"  Purpose  Boot block creation utility\n"
		"  Author   (c) Marco Baye, 1994-" YEAR "\n"
		"  Licence  Free software (GPL'd)\n"
		"  Version  " VERSION " (" DATE " " YEAR ")\n"
		"\n"
//		"http://home.pages.de/%7Emac%5Fbacon/"
		"https://github.com/marcobaye/bootmake128\n"
		"\n"
	);
	key_ask();
}

// enter boot message
static const char	string_hhc[]	= {c_HOME, c_HOME, c_CLEAR, 0};
static void message_enter(void)
{
	static uint8_t	byte;

	colors_system();
	print(string_hhc);
	print(
		"enter the desired welcome message...\n"
		"caution - *all* key codes are recorded!\n"
	);
	if (conf.use_local_charset)
		localcharset_on();
	// FIXME - show system's boot message?
	gotoxy(0, 2);
	//POKE(820, 189);	// change quote-escape-vector so ESC becomes visible	(only makes sense when enforcing quote mode)
	switch (conf.force_case) {
	case FORCE_NONE:
		break;
	case FORCE_LOWER:
		CHROUT(c_LOWERCASE);
		break;
	case FORCE_UPPER:
		CHROUT(c_UPPERCASE);
		break;
	}
	print("text:");
	//print(message_buffer);	this is no longer useful...
	CHROUT(c_LOCK);
	message_len = 0;
	do {
		byte = key_with_crsr();
		CHROUT(byte);
		if (message_len < MSG_BUF_LEN) {
			message_buffer[message_len++] = byte;
		}
	} while (byte != 13);
	message_buffer[--message_len] = '\0';	// terminate
	//POKE(820, 185);	// restore quote-escape-vector
	if (conf.use_local_charset)
		localcharset_off();
	colors_own();	// user might have changed text color or switched screen
	redraw_screen = 1;
}

// replacement for basic's string handling: use a global buffer and functions
// to append various stuff
#define BUFFER_MAX	((uint8_t) 255)	// content length may be 0..255
static uint8_t	buf_used	= 0;
static char	buffer[BUFFER_MAX + 1];	// but buffer is a full page so terminator can be added

// add a single byte
static void __fastcall__ buf_add_byte(uint8_t cc)
{
	if (buf_used >= BUFFER_MAX)
		return;
	buffer[buf_used] = cc;
	++buf_used;
}

// add a sequence of bytes (may contain zeroes)
static void __fastcall__ buf_add_seq(uint8_t size, const char *buf)
{
	while (size--)
		buf_add_byte(*buf++);
}

// add a terminated string
static void __fastcall__ buf_add_string(const char *string)
{
	static unsigned int	length;

	length = 0;
	while (string[length] != '\0')
		++length;
	if (length > BUFFER_MAX)
		length = BUFFER_MAX;
	buf_add_seq(length, string);
}

// add decimal representation of unsigned byte
static void __fastcall__ buf_add_uint8dec99max(uint8_t byte)
{
	char	result[2];

	// this code relies on cc65's argument stack handling...
	(void) byte;	// inhibit compiler warning
	asm(
"		ldy #2		\n"
"		lda (sp), y	\n"	// read parameter
"		jsr $f9fb	\n"	// $f9fb converts uint8 in A to two ascii digits in XXAA (so only works in 0..99 range)
"		ldy #1		\n"
"		sta (sp), y	\n"	// write result[1]
"		txa		\n"
"		dey		\n"
"		sta (sp), y	\n"	// write result[0]
	);
	// inhibit leading zero
	if (result[0] == '0')
		buf_add_byte(result[1]);
	else
		buf_add_seq(2, result);
}

// add prefix codes (according to config) and actual message
static const char	string_epej[]	= { c_ESCAPE, 'p', c_ESCAPE, 'j', 0 };
static void buf_add_message(void)
{
	if (conf.remove_boot_msg)
		buf_add_string(string_epej);
	if (conf.lock_charset)
		buf_add_byte(c_LOCK);
	switch (conf.force_case) {
	case FORCE_NONE:
		break;
	case FORCE_LOWER:
		buf_add_byte(c_LOWERCASE);
		break;
	case FORCE_UPPER:
		buf_add_byte(c_UPPERCASE);
		break;
	}
	buf_add_seq(message_len, message_buffer);
}

// display boot message
static const char	string_uuhhc[]	= { c_UPPERCASE, c_UNLOCK, c_HOME, c_HOME, c_CLEAR, 0 };
static void message_display(void)
{
	static uint8_t	ii;

	colors_system();
	print(string_uuhhc);
	if (conf.use_local_charset)
		localcharset_on();
	call_basic_rom(0x419b);	//bank 15:sys 16795	show system's startup message
	print("\nbooting ");
	buf_used = 0;	// clear buffer
	buf_add_message();
	// print everything in buffer
	for (ii = 0; ii < buf_used; ++ii)
		CHROUT(buffer[ii]);
	print("...\n");
	key_ask();
	if (conf.use_local_charset)
		localcharset_off();
	colors_own();
	redraw_screen = 1;
}

// ask for decision
// returns true on CANCEL
static bool chance_to_cancel(void)
{
	print(COLOR_EMPH "[y/n]" COLOR_STD);
	keybuf_clear();
	for (;;) {
		switch (cbm_k_getin()) {
		case 'y':
		case 'Y':
			CHROUT('\n');
			return 0;
		case 'n':
		case 'N':
			CHROUT('\n');
			return 1;
		}
	}
}

// 
static void __fastcall__ error_decode(uint8_t code)
{
	print(COLOR_EMPH "\n");	//CHROUT('\n');
	switch (code) {
// these could be used from ROM {
	case 1:
		print("Too many files");
		break;
	case 2:
		print("File open");
		break;
	case 3:
		print("File not open");
		break;
	case 4:
		print("File not found");
		break;
	case 5:
		print("Device not present");	// this is the only one the user should encounter...
		break;
	case 6:
		print("Not input file");
		break;
	case 7:
		print("Not output file");
		break;
	case 8:
		print("Missing file name");
		break;
	case 9:
		print("Illegal device number");
		break;
// }
	case 10:
		print("Break");
		break;
	case 11:
		print("I/O");
		break;
	default:
		print("Unknown code");
	}
	print(" error.\n" COLOR_STD);
}

// test for existence of drive (by open/chkout/close on command channel)
// returns true if drive exists
static uint8_t	device_to_check;
static bool drive_check(void)
{
	cbm_open(LFN_CMD, device_to_check, SA_CMD, "");
	cbm_k_ckout(LFN_CMD);
	cbm_k_clrch();
	cbm_k_close(LFN_CMD);
	return (cbm_k_readst() & 0x80) == 0;	// bit 7 means device not present
}

// find next available drive
static void drive_next(void)
{
	device_to_check = chosen_device;
	do {
		++device_to_check;
		if (device_to_check > DEVICE_MAX)
			device_to_check = DEVICE_MIN;
		if (drive_check())
			break;
	} while (device_to_check != chosen_device);
	chosen_device = device_to_check;
}

// fetch and display drive status
// returns true on error (if message does not start with "0")
static bool drive_get_status(void)
{
	static int	ret;

	buffer[0] == '9';	// make sure to fail if no data arrives
	ret = cbm_read(LFN_CMD, buffer, BUFFER_MAX);
	if (ret == -1) {
		error_decode(_oserror);
		return 1;	// fail
	}
	// remove trailing CR
	if ((ret > 0) && (buffer[ret - 1] == 13))
		--ret;
	buffer[ret] = '\0';	// terminate
	print("\nStatus: \"\x1b\x1b");
	if (buffer[0] != '0')
		print(COLOR_EMPH);
	print(buffer);
	print(COLOR_STD "\"\n");
	return buffer[0] != '0';
}

// check disc/partition type
// result is in global var; returns true on error or unsupported drive
static bool drive_get_dpt(void)
{
	static uint8_t	err;
	static int	ret;
	static uint8_t	format;

	print("Checking drive/partition format.\n");
	err = cbm_open(LFN_RAWDIR, chosen_device, SA_RAWDIR, "$");
	if (err) {
		cbm_close(LFN_RAWDIR);	// if open fails, file must still be closed!
		error_decode(err);
		return 1;	// fail
	}
	ret = cbm_read(LFN_RAWDIR, &format, 1);
	cbm_close(LFN_RAWDIR);
	if (ret == -1) {
		error_decode(_oserror);
		return 1;	// fail
	}
	if (ret == 0) {
		// unexpected EOF - this happens with file system access under VICE
		// or with "21,read error"s or "drive not ready"
		print(COLOR_EMPH "Error: No data." COLOR_STD "\n");
		drive_get_status();	// ignore return value, we failed anyway
		return 1;	// fail
	}
	if (drive_get_status())
		return 1;	// fail

	switch (format) {
	case 'a':
		dpt = &dpt_1541;
		break;
	case 'c':
		dpt = &dpt_ieee;
		break;
	case 'd':
		dpt = &dpt_1581;
		break;
	case 'h':
		dpt = &dpt_cmdnative;
		break;
	case 'm':
		dpt = &dpt_cmdextnat;
		break;
	case '\0':
		dpt = &dpt_rawsd2iec;
		break;
	default:
		dpt = &dpt_unknown;
		break;
	}
	print("Drive/partition has " COLOR_EMPH);
	print(dpt->name);
	print(COLOR_STD " format.\n");
	if (dpt->valid)
		return 0;	// ok

	print("Sorry.\n\n");	// don't mess with unknown formats
	return 1;	// fail
}

// build the new boot block in memory
static const char	part1[]	= { 'c', 'b', 'm', 0, 0, 0, 0 };
static const char	part2[]	= { 0, 0, 0xa2 };	// text terminator, filename terminator, "ldx #"
static const char	part3[]	= { 0xa0, 0x0b, 0x4c, 0xa5, 0xaf };	// "ldy #$0b : jmp $afa5"
static void bootblock_build(void)
{
	// put version msg at end of buffer
	buf_used = 198;	// the string below takes 56 chars
	buf_add_string(" This boot block was created by MacBootMake Version " VERSION ".\n");
	// now create real data at start of buffer. version message may be overwritten, but that's ok.
	buf_used = 0;	// clear buffer
	buf_add_seq(7, part1);
	buf_add_message();
	buf_add_seq(3, part2);
	buf_add_byte(5 + buf_used);	// depends on position (length of previous data)
	buf_add_seq(5, part3);
	// x/y is set to point to latest byte.
	// calling $afa5 will increment pointer before using it,
	// therefore further contents will be interpreted as basic.
	if (conf.use_local_charset)
		buf_add_string("poK0,111:poK1,51:");	// FIXME - convert to machine code?
	buf_add_string("bA");	// bank
	buf_add_uint8dec99max(conf.chosen_bank);
	switch (conf.action) {
	case ACTION_RUNBASIC:
		buf_add_string(":rU\"");	// run
		break;
	case ACTION_BOOTMC:
		buf_add_string(":bO\"");	// boot
		break;
	//case ACTION_GO64LOAD:		// TODO - this algo would need some more changes in this function...
	//	break;
	}
	buf_add_string(filename_buf);
	buf_add_string("\",u");
	if (conf.alternative_device == ALTDEVICE_NONE) {
		buf_add_string("(peE(186))");
	} else {
		buf_add_uint8dec99max(conf.alternative_device);
	}
	buf_add_byte(0);	// end of basic line
	buf_used = MSG_BUF_LEN;	// make sure whole buffer is sent to drive
}

// send drive command (channel must be open)
static uint8_t send_buf_as_cmd(void)
{
	static int	ret;

	ret = cbm_write(LFN_CMD, buffer, buf_used);
	if (ret == -1) {
		error_decode(_oserror);
		return 1;
	}
	return 0;
}

// set drive buffer pointer (channels must be open, argument must be given as string)
static uint8_t __fastcall__ set_buffer_pointer(const char *byte_offset)
{
	buf_used = 0;	// clear buffer
	buf_add_string("b-p " XSTR(SA_BUF) " ");
	buf_add_string(byte_offset);
	return send_buf_as_cmd();
}

// send buffer contents to command channel and display drive status
// return true on error
static uint8_t send_and_check(void)
{
	if (send_buf_as_cmd())
		return 1;	// fail

	if (drive_get_status())
		return 1;	// fail

	return 0;	// ok
}

// send block command (read/write) to disk drive (channels must be open)
// track and sector must be given as string (space- or semicolon-separated)
static uint8_t __fastcall__ block_usercmd(uint8_t action, const char *ts)
{
	buf_used = 0;	// clear buffer
	buf_add_byte('u');
	buf_add_byte(action);
	buf_add_string(" " XSTR(SA_BUF) " 0 ");	// 0 is drive
	buf_add_string(ts);
	return send_and_check();
}

// tell disk drive to read a block into buffer
// track and sector must be given as string (space- or semicolon-separated)
#define block_read(ts)	block_usercmd('1', ts)

// tell disk drive to write buffer to block
// track and sector must be given as string (space- or semicolon-separated)
#define block_write(ts)	block_usercmd('2', ts)

// try to allocate t1s0
// return true on error
static bool bootblock_allocate(void)
{
	print("Allocating boot block.\n");
	buf_used = 0;
	buf_add_string("b-a 0 1 0");
	return send_and_check();
}

// try to free t1s0
// return true on error
static bool bootblock_free(void)
{
	print("Deallocating boot block.\n");
	buf_used = 0;
	buf_add_string("b-f 0 1 0");
	return send_and_check();
}

// check whether boot block is allocated and active:
// result is in global vars; returns true on error!
static bool bootblock_check(void)
{
	static int	ret;
	static uint8_t	allocation_byte;
	static char	signature[3];

	if (dpt->fiddle_with_bam) {
		print("Checking BAM.\n");
		if (block_read(dpt->track_and_sector))
			return 1;	// fail

		if (set_buffer_pointer(dpt->byte_offset))
			return 1;	// fail

		ret = cbm_read(LFN_BUF, &allocation_byte, 1);
		if (ret == -1) {
			error_decode(_oserror);
			return 1;	// fail
		}
		// boot block is sector 0, so check lsb:
		if (allocation_byte & 1) {
			allocation_state = AS_FREE;
			print("Boot block is not allocated.\n");
		} else {
			allocation_state = AS_ALLOCATED;
			print("Boot block is allocated.\n");
		}
	} else {
		allocation_state = AS_RESERVED;
		// FIXME - tell user why we don't care about allocation!
	}
	// check whether boot block is active:
	print("Reading boot block.\n");
	if (block_read("1 0"))
		return 1;	// fail

	// set buffer pointer to zero
	// FIXME - check whether old version works, because this was added!
	if (set_buffer_pointer("0"))
		return 1;	// fail

	ret = cbm_read(LFN_BUF, signature, 3);
	if (ret == -1) {
		error_decode(_oserror);
		return 1;	// fail
	}
	if (ret != 3) {
		print(COLOR_EMPH "Error: Unexpected EOF." COLOR_STD "\n");
		return 1;	// fail
	}
	bootblock_active = (signature[0] == 'c') && (signature[1] == 'b') && (signature[2] == 'm');
	return 0;	// ok
}

// create boot block ("inner" function)
static void bba_create(void)
{
	static int	ret;

	if (bootblock_check())
		goto prompt;

	if (bootblock_active) {
		CHROUT(c_BELL);
		print(
			"\nDisc already has a valid boot block!\n"
			"\nContinue?\n"
		);
		if (chance_to_cancel())
			return;

	} else {
		if ((dpt->fiddle_with_bam) && (allocation_state == AS_ALLOCATED)) {
			CHROUT(c_BELL);
			print(
				COLOR_EMPH
				"\nBoot block is allocated; CONTINUING WILL RESULT IN DATA LOSS!\n"
				"\nREALLY continue?\n"
				COLOR_STD
			);
			if (chance_to_cancel())
				return;

		}
	}
	if (set_buffer_pointer("0"))
		goto prompt;

	print("Writing boot block.\n");
	bootblock_build();
	ret = cbm_write(LFN_BUF, buffer, buf_used);
	if (ret == -1) {
		error_decode(_oserror);
		goto prompt;
	}
	if (ret != buf_used) {
		print(COLOR_EMPH "Error: Could not write all data." COLOR_STD);
		goto prompt;
	}
	if (block_write("1 0"))
		goto prompt;

	if ((dpt->fiddle_with_bam) && (allocation_state == AS_FREE)) {
		if (bootblock_allocate())
			goto prompt;
	}
	print("Done.\n");
prompt:	key_ask();
}

// check/destroy boot block ("inner" function)
static void bba_check(void)
{
	static int	ret;
	static uint8_t	zero	= 0;

	if (bootblock_check())
		goto prompt;

	if (bootblock_active == 0) {
		print("No active boot block.\n");
		// TODO - if allocated, ask user whether to free?
		goto prompt;
	}
	print(
		"Boot block found.\n"
		"Remove it?\n"
	);
	// FIXME - if block is free, ask user whether to allocate!
	if (chance_to_cancel())
		return;

	// ok, now destroy it
	if (set_buffer_pointer("0"))
		goto prompt;

	ret = cbm_write(LFN_BUF, &zero, 1);	// overwriting first byte should be enough
	if (ret == -1) {
		error_decode(_oserror);
		goto prompt;
	}
	if (ret == 0) {
		print(COLOR_EMPH "Error: Could not write all data." COLOR_STD);
		goto prompt;
	}
	//print#LFN_CMD, "b-p " XSTR(SA_BUF) " 0"	this is useless, as i now realize. a data becker book said to do this...
	if (block_write("1 0"))
		goto prompt;

	print("Boot block deactivated.\n");
	if ((dpt->fiddle_with_bam) && (allocation_state == AS_ALLOCATED)) {
// FIXME - ask user, maybe they want to keep it allocated for later use!
// but then, how do we recognize this the next time and do not scare user about data loss?
// maybe use special "disabled" boot sector contents?
		if (bootblock_free())
			goto prompt;
	}
	print("Done.\n");
prompt:	key_ask();
}

// wrapper function to create or destroy boot block
static void __fastcall__ bootblock_action(void (*bbaction)(void))
{
	static uint8_t	err;

	//OPEN
	err = cbm_open(LFN_CMD, chosen_device, SA_CMD, "i0");
	if (err) {
		error_decode(err);
		key_ask();
		goto fail;
	}
	if (drive_get_dpt()) {
		key_ask();
		goto fail;
	}
	//OPEN
	err = cbm_open(LFN_BUF, chosen_device, SA_BUF, "#");
	if (err) {
		error_decode(err);
		key_ask();
	} else {
		bbaction();
	}
	//CLOSE
	cbm_close(LFN_BUF);
fail:	//CLOSE
	cbm_close(LFN_CMD);
}

// create boot block
static void create_new_bb(void)
{
	bootblock_action(bba_create);
}

// check/destroy boot block
static void check_for_existing_bb(void)
{
	bootblock_action(bba_check);
}

// display directory (calls basic rom) and then drive status
#define LFN_DIR	0	// just like $a0a4 does it
static void show_directory(void)
{
	static uint8_t	err;

	err = cbm_open(LFN_DIR, chosen_device, 0, "$");	// just like $a0a4 does it
	if (err) {
		cbm_close(LFN_DIR);	// if open fails, file must still be closed!
		error_decode(err);
		key_ask();
		return;
	}

	call_basic_rom(0xa0bd);	// directory u(chosen_device)
	// "close" will have been done by rom routine!
	// re-open for status
	err = cbm_open(LFN_CMD, chosen_device, SA_CMD, "");	// CAUTION - do not use NULL if no filename!
	if (err)
		error_decode(err);
	else
		drive_get_status();	// ignore return value, as user is asked for key anyway
	cbm_close(LFN_CMD);	// if open fails, file must still be closed!
	key_ask();
}

// send command to drive and then display drive status
static void send_disc_command(void)
{
	static uint8_t	err;

	print("Command:" COLOR_EMPH);
	buf_used = input(BUFFER_MAX, buffer);
	print("\n" COLOR_STD);
	err = cbm_open(LFN_CMD, chosen_device, SA_CMD, buffer);
	if (err)
		error_decode(err);
	else
		drive_get_status();	// ignore return value, as user is asked for key anyway
	cbm_close(LFN_CMD);	// if open fails, file must still be closed!
	key_ask();
}

// set program name
static void program_setfilename(void)
{
	printat(10, 0, "----------------");
	printat(10, 1, filename_buf);
	printat(10, 2, "----------------");
	printat(0, 1, "File name:" COLOR_EMPH);
	input(FILENAME_BUF_LEN, filename_buf);
	print("\n" COLOR_STD);
}

// call function in sidescreen
static const char	string_et[]	= { c_ESCAPE, 't', 0 };
static const char	string_is[]	= { c_CLEAR, c_LOWERCASE, c_HOME, c_HOME, 0 };
static void __fastcall__ in_sidescreen(void (*fn)(void))
{
	// enter sidescreen
	if (ON_VDC)
		printat(40, 0, string_et);
	else
		redraw_screen = 1;
	CHROUT(c_CLEAR);
	// call function
	fn();
	// leave sidescreen
	if (ON_VDC)
		print(string_is);
}

// redraw functions for boot block config:

// using ternary operator on a literal and a "static const char[]" results in "Incompatible pointer types" error?!
static const char	leave_alone[]	= COLOR_EMPH "leave alone";
static const char	activate_it[]	= COLOR_EMPH "activate it";
static const char	remove_it[]	= COLOR_EMPH "remove it";
static const char	block_it[]	= COLOR_EMPH "block it";
static const char	line_tail[]	= COLOR_STD "\x1bq";	// { c_ESCAPE, 'q', 0 };
#define CONF_X	21	// x position of config values
#define CONF_Y	17	// y position of top config value

// redraw drive address
static void device_redraw(void)
{
	printat(7, 0, COLOR_EMPH);
	buf_used = 0;
	buf_add_uint8dec99max(chosen_device);
	buf_add_byte(' ');	// make sure to erase second digit from before
	buf_add_byte('\0');
	print(buffer);
	print(COLOR_STD);
}

// redraw "use local charset" option
static void uselocalcharset_redraw(void)
{
	gotoxy(CONF_X, CONF_Y + 0);
	print(conf.use_local_charset ? activate_it : leave_alone);
	print(line_tail);
}

// redraw "remove BOOTING" option
static void removebootmsg_redraw(void)
{
	gotoxy(CONF_X, CONF_Y + 1);
	print(conf.remove_boot_msg ? remove_it : leave_alone);
	print(line_tail);
}

// redraw "forbid cbm/shift" option
static void lockcharset_redraw(void)
{
	gotoxy(CONF_X, CONF_Y + 2);
	print(conf.lock_charset ? block_it : leave_alone);
	print(line_tail);
}

// redraw "force upper/lower case" option
static void forcecase_redraw(void)
{
	gotoxy(CONF_X, CONF_Y + 3);
	switch (conf.force_case) {
	case FORCE_NONE:
		print(leave_alone);
		break;
	case FORCE_LOWER:
		print(COLOR_EMPH "use lower case");
		break;
	case FORCE_UPPER:
		print(COLOR_EMPH "use upper case");
		break;
	}
	print(line_tail);
}

// redraw "basic/machine code" option
static void action_redraw(void)
{
	gotoxy(CONF_X, CONF_Y + 4);
	switch (conf.action) {
	case ACTION_RUNBASIC:
		print(COLOR_EMPH "run basic prg");
		break;
	case ACTION_BOOTMC:
		print(COLOR_EMPH "run machine prg");
		break;
	//case ACTION_GO64LOAD:
	//	print(COLOR_EMPH "run c64 prg");
	//	break;
	}
	print(line_tail);
}

// redraw filename
static void filename_redraw(void)
{
	printat(CONF_X, CONF_Y + 5, "\"\x1b\x1b" COLOR_EMPH);
	print(filename_buf);
	print(COLOR_STD "\"\x1b\x1b");
	print(line_tail);
}

// redraw alternative device
static void altdevice_redraw(void)
{
	gotoxy(CONF_X, CONF_Y + 6);
	if (conf.alternative_device == ALTDEVICE_NONE) {
		print(COLOR_EMPH "boot device");
	} else {
		buf_used = 0;
		buf_add_string(COLOR_EMPH "device ");
		buf_add_uint8dec99max(conf.alternative_device);
		buf_add_byte('\0');
		print(buffer);
	}
	print(line_tail);
}

// redraw bank option
static void chosenbank_redraw(void)
{
	printat(CONF_X, CONF_Y + 7, COLOR_EMPH);
	buf_used = 0;
	buf_add_uint8dec99max(conf.chosen_bank);
	buf_add_byte(' ');	// make sure to erase second digit from before
	buf_add_byte('\0');
	print(buffer);
	print(COLOR_STD);
}

// redraw whole screen
// CR at start ensures quote mode is off
static char	string_init[]	= { 13, 27, 'n', c_LOCK, c_LOWERCASE, c_HOME, c_HOME, c_CLEAR, 0 };
static void screen_redraw(void)
{
	print(string_init);
	print("Device:");
	device_redraw();
	printat(22, 0,		REVSON " MacBootMake V" VERSION " " REVSOFF "\n"
		"\n"
		" Key:      Action:\n"
		"\n"
//		" -/+   Change device address\n"
		"CTRL-d Cycle through available drives\n"
		"  e    Enter message text\n"
		"  d    Display message text\n"
		"  s    Store new boot block\n"
		"  r    Remove existing boot block\n"	// TODO - rename to "check"
		"  $    Show directory\n"	// TODO - also allow F1 and F3!
		"  @    Send disc command\n"
		"ESC-x  Toggle screen\n"
		"  i    Show program info\n"
		"  q    Quit\n"
		"\n"
		"           Boot block configuration:\n"
		"\n"
		"  1    Local charset\n"
		"  2    \"BOOTING\"\n"
		"  3    CBM/Shift\n"
		"  4    Case\n"
		"  5    Boot action\n"
		"  6    File name\n"
		" 7/8   From\n"
		" 9/0   Run in bank"
	);
	uselocalcharset_redraw();
	removebootmsg_redraw();
	lockcharset_redraw();
	forcecase_redraw();
	action_redraw();
	filename_redraw();
	altdevice_redraw();
	chosenbank_redraw();
}

// wait for valid key and act upon
static void menu_loop(void)
{
	static uint8_t	key,
			previous;	// for ESC-x

	do {
		if (key)
			previous = key;
		key = cbm_k_getin();
		switch (key) {
		case c_CONTROL_D:	// scan for next device
			drive_next();
			device_redraw();
			break;
		case '1':	// toggle local charset usage
			conf.use_local_charset = !conf.use_local_charset;
			uselocalcharset_redraw();
			break;
		case '2':	// hide 'booting'?
			conf.remove_boot_msg = !conf.remove_boot_msg;
			removebootmsg_redraw();
			break;
		case '3':	// cbm/shift?
			conf.lock_charset = !conf.lock_charset;
			lockcharset_redraw();
			break;
		case '4':	// force case?
			++conf.force_case;
			if (conf.force_case == FORCELIMIT)
				conf.force_case = 0;
			forcecase_redraw();
			break;
		case '5':	// set action
			++conf.action;
			if (conf.action == ACTIONLIMIT)
				conf.action = 0;
			action_redraw();
			break;
		case '6':
			in_sidescreen(program_setfilename);
			// there is no need to redraw component now if a full
			// redraw is scheduled anyway (depends on sidescreen):
			if (!redraw_screen)
				filename_redraw();
			break;
		case '7':	// decrement alternative device number
			--conf.alternative_device;
			if (conf.alternative_device < ALTDEVICE_MIN)
				conf.alternative_device = ALTDEVICE_MAX;
			altdevice_redraw();
			break;
		case '8':	// increment alternative device number
			++conf.alternative_device;
			if (conf.alternative_device > ALTDEVICE_MAX)
				conf.alternative_device = ALTDEVICE_MIN;
			altdevice_redraw();
			break;
		case '9':	// change bank
			--conf.chosen_bank;
			--conf.chosen_bank;
			/*FALLTHROUGH*/
		case '0':	// change bank
			++conf.chosen_bank;
			conf.chosen_bank &= 15;
			chosenbank_redraw();
			break;
		case 'i':
			in_sidescreen(help_show);
			break;
		case 'e':
			message_enter();
			break;
		case 'd':
			message_display();
			break;
		case 's':
			in_sidescreen(create_new_bb);
			break;
//FIXME - change 'r' (remove) to 'c' (check)? and then ask user what to do (remove/load-to-buffer/ignore)?
		case 'r':
			in_sidescreen(check_for_existing_bb);
			break;
		case '$':
			in_sidescreen(show_directory);
			break;
		case '@':
			in_sidescreen(send_disc_command);
			break;
		case 'q':	// quit
			quit_program = 1;
			break;
		case c_ESCAPE:
			break;
		case 'x':
			if (previous == c_ESCAPE) {
				CHROUT(c_CLEAR);
				asm(
"					jsr $c02a	\n"	// switch video
				);
				colors_own();	// init screen colors
				redraw_screen = 1;
			}
			break;
		case 0:
			break;
		default:
			previous = 0;
			key = 0;	// no valid key, so stay in loop
		}
	} while (key == 0);
}

// guess what
int main(void)
{
	colors_own();	// also goes fast/slow depending on screen
	chosen_device = PEEK(186);
	if (chosen_device < DEVICE_MIN)
		chosen_device = 8;
	conf.alternative_device = ALTDEVICE_NONE;
	conf.chosen_bank = 15;
	redraw_screen = 1;
	quit_program = 0;
	do {
		if (redraw_screen) {
			screen_redraw();
			//print("\nPlease press a key...");
			CHROUT(c_HOME);
			redraw_screen = 0;
		}
		menu_loop();
	} while (!quit_program);
	printat(0, 24, "\n\nCu...\n\n");
	return 0;
}
