/*
 * controller.c - tinyg controller and top level parser
 * Part of TinyG project
 *
 * Copyright (c) 2010-2011 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify
 * it under the terms of the Creative Commons CC-BY-NC license 
 * (Creative Commons Attribution Non-Commercial Share-Alike license)
 * as published by Creative Commons. You should have received a copy 
 * of the Creative Commons CC-BY-NC license along with TinyG.
 * If not see http://creativecommons.org/licenses/
 *
 * TinyG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 *
 * ---- Controller Operation ----
 *
 *	The controller provides a simple process control scheme to manage 
 *	blocking of multiple "threads" in the application. The controller is 
 *	an event-driven hierarchical state machine (HSM) using inverted
 *	control to manage a set of cooperative run-to-completion kernel tasks.
 * 	(ref: http://www.state-machine.com/products)
 *
 *	More simply, it works as a set of aborting "superloops", one superloop
 *	per hierarchical state machine (or thread - sort of). Within each HSM
 *	the highest priority tasks are run first and progressively lower 
 *	priority tasks are run only if the higher priority tasks are not blocked. 
 *	No task ever actually blocks, but instead returns "busy" (eagain) when 
 *	it would ordinarily block. It must also provide a re-entry point to 
 *	resume the task once the blocking condition has been removed.
 *
 *	For this scheme to work tasks must be written to run-to-completion 
 *	(non-blocking), and must offer re-entry points (continuations) to 
 *	resume operations that would have blocked (see line generator for an 
 *	example). A task returns TG_EAGAIN to indicate a blocking point. If 
 *	TG_EAGAIN is received the controller quits the loop (HSM) and starts 
 *	the next one in the round-robin (all HSMs are round robined).  
 *	Any other return code allows the controller to proceed down the task 
 *	list. See end notes in this file for how to write a continuation.
 *
 *	Interrupts run at the highest priority levels; kernel tasks are 
 *	organized into priority groups below the interrupt levels. The 
 *	priority of operations is:
 *
 *	- High priority ISRs
 *		- issue steps to motors / count dwell timings
 *		- dequeue and load next stepper move
 *
 *	- Medium priority ISRs
 *		- receive serial input (RX)
 *		- execute signals received by serial input
 *		- detect and flag limit switch closures
 *
 *	- Low priority ISRs
 *		- send serial output (TX)
 *
 *	- Main loop tasks
 *		These are divided up into layers depending on priority and blocking
 *		hierarchy. See tg_controller() for details.
 *
 *	Notes:
 *	  - Gcode and other command line flow control is managed cooperatively
 *		with the application sending Gcode or other commands. The '*' 
 *		char in the prompt indicates that the controller is ready for the 
 *		next line. The sending app is supposed to honor this and not stuff
 *		lines down the pipe (which will choke the controller).
 *
 *	Futures: Using a super loop instead of an event system is a design 
 *	tradoff - or more to the point - a hack. If the flow of control gets 
 *	much more complicated it will make sense to replace this section 
 *	with an event driven dispatcher.
 *
 * ---- Mode Auto-Detection behaviors ----
 *
 *	The first letter of an IDLE mode line performs the following actions
 *
 *		G,M,N,F,%,(	enter GCODE_MODE (as will lower-case of the same)
 *		C,?			enter CONFIG_MODE
 *		D,A			enter DIRECT_DRIVE_MODE
 *		F			enter FILE_MODE (returns automatically after file ends)
 *		H			help screen (returns to IDLE mode)
 *		T			execute primary test (whatever you link into it)
 *		U			execute secondary test (whatever you link into it)
 *		I			<reserved>
 *		V			<reserved>
 *
 *	Once in the selected mode these characters are not active as mode 
 *	selects. All modes use Q (Quit) to exit and return to idle mode.
 */

#include <stdio.h>
#include <ctype.h>
//#include <avr/pgmspace.h>

#include "xio.h"
#include "tinyg.h"
#include "controller.h"
#include "signals.h"
#include "gcode.h"				// calls out to gcode parser, etc.
#include "config.h"				// calls out to config parser, etc.
#include "canonical_machine.h"	// uses homing cycle
#include "planner.h"
#include "direct_drive.h"
#include "stepper.h"			// needed for stepper kill and terminate
#include "limit_switches.h"
//#include "xmega_eeprom.h"
//#include "motor_queue.h"

/*
 * Canned gcode files for testing
 */

//#include "gcode_tests.h"		// system tests and other assorted test code
//#include "gcode_zoetrope.h"	// zoetrope moves. makes really cool sounds
#include "gcode_contraptor_circle.h"
//#include "gcode_mudflap.h"
#include "gcode_braid2d.h"
//#include "gcode_hacdc.h"

struct tgController tg;			// controller state structure

static void _tg_controller_HSM(void);
static int _tg_parser(char * buf);
static int _tg_run_prompt(void);
static int _tg_read_next_line(void);
static void _tg_prompt(void);
static void _tg_set_mode(uint8_t mode);
static void _tg_set_source(uint8_t d);
static int _tg_kill_handler(void);
static int _tg_term_handler(void);
static int _tg_pause_handler(void);
static int _tg_resume_handler(void);
static int _tg_reset(void);
static int _tg_print_idle_help_screen(void);
static int _tg_test_T(void);
static int _tg_test_U(void);
static void _tg_canned_startup(void);

/*
 * tg_init() - controller init
 * tg_alive() - announce that TinyG is alive
 *
 *	The controller init is split in two: the actual init, and tg_alive()
 *	which should be issued once the rest of he application is initialized.
 */

void tg_init() 
{
	// set input source
	tg.default_src = DEFAULT_SOURCE;// set in tinyg.h
	_tg_set_source(tg.default_src);	// set initial active source
	_tg_set_mode(TG_IDLE_MODE);		// set initial operating mode
}

void tg_alive() 
{
#ifdef __SIMULATION_MODE
	return;
#endif									// see tinyg.h for string
	printf_P(PSTR("==== TinyG %S ====\n"), (PSTR(TINYG_VERSION)));
	printf_P(PSTR("---- hit 'h' for help\n"));

	_tg_prompt();
}

/*
 * tg_trap() - trap and throw an exception
 */

void tg_trap(uint8_t code)
{
	tg.trap_code = code;
#ifndef __SIMULATION_MODE
	printf_P(PSTR("######## TRAP %d ########\n"), code);
#endif
	return;
}

/* 
 * tg_controller() - top-level controller
 *
 * The order of the dispatched tasks is very important. 
 * Tasks are ordered by increasing dependency (blocking hierarchy).
 * Tasks that are dependent on completion of lower-level tasks must be
 * later in the list than the task(s) they are dependent upon. 
 *
 * Tasks must be written as continuations as they will be called 
 * repeatedly, and often called even if they are not currently active. 
 * See end notes in this file for how to code continuations.
 *
 * The DISPATCH macro calls the function and returns to the controller 
 * parent if not finished (TG_EAGAIN), preventing later routines from 
 * running (they remain blocked). Any other condition - OK or ERR - 
 * drops through and runs the next routine in the list.
 *
 * A routine that had no action (i.e. is OFF or idle) should return TG_NOOP
 *
 *---> mp_move_dispatcher() is called FALSE becuase it's not a KILL (TRUE)
 *	It has no DISPATCH wrapper because regardless of the results the later
 *	routines should be run.
 */
#define	DISPATCH(func) if (func == TG_EAGAIN) return; 

void tg_controller()
{
	while (TRUE) {
		_tg_controller_HSM();
	}
}

static void _tg_controller_HSM()
{
//----- kernel level ISR handlers ----(flags are set in ISRs)-----------//
	DISPATCH(ls_handler());			// limit switch handler
	DISPATCH(_tg_kill_handler());	// complete processing of ENDs (M2)
	DISPATCH(_tg_term_handler());	// complete processing of ENDs (M2)
	DISPATCH(_tg_pause_handler());	// complete processing of STOPs
	DISPATCH(_tg_resume_handler());	// complete processing of STARTs

//----- low-level motor control ----------------------------------------//
	DISPATCH(st_execute_move());	// run next stepper queue command
	mp_move_dispatcher(FALSE);		// run current or next move in queue

//----- machine cycles -------------------------------------------------//
	DISPATCH(cm_run_homing_cycle());// homing cycle

//----- command readers and parsers ------------------------------------//
	DISPATCH(_tg_run_prompt());		// manage prompts and flow control
	DISPATCH(_tg_read_next_line());	// read and execute next command
}

/* 
 * _tg_run_prompt()
 *
 * This routine is specific to Gcode parser (because of the buffers)
 * May need to be revised if other parsers are added
 */

static int _tg_run_prompt()
{
	if ((tg.prompt_disabled) || (tg.prompted)) { 
		return (TG_NOOP);			// exit w/continue if already prompted
	}
	_tg_prompt();
	return (TG_OK);
}

/* 
 * _tg_read_next_line() - non-blocking line read from active input device
 *
 *	Reads next command line and dispatches to currently active parser
 *	Manages various device and mode change conditions
 *	Also responsible for prompts and for flow control. 
 *	Accepts commands if the move queue has room - halts if it doesn't
 */

static int _tg_read_next_line()
{
	// test if it's OK to read the next line
	if (!mp_test_write_buffer(MP_BUFFERS_NEEDED)) {
		return (TG_EAGAIN);				// exit w/busy if not enough buffers
	}
	// read input line or return if not a completed line
	// xio_gets() is a non-blocking workalike of fgets()
	if ((tg.status = xio_gets(tg.src, tg.buf, sizeof(tg.buf))) == TG_OK) {
//		printf_P(PSTR("Read next line %s\n"), tg.buf);
		tg.status = _tg_parser(tg.buf);	// dispatch to active parser
		tg.prompted = FALSE;			// signals ready-for-next-line
	}

	// handle case where the parser detected a QUIT
	if (tg.status == TG_QUIT) {
		_tg_set_mode(TG_IDLE_MODE);
	}

	// handle end-of-file case (EOF can come from file devices only)
	if (tg.status == TG_EOF) {
		printf_P(PSTR("End of command file\n"));
		tg_reset_source();				// reset to default src
	}

	// Note that TG_OK, TG_EAGAIN, TG_NOOP etc. will just flow through
	return (tg.status);
}

/* 
 * tg_application_startup() - application start and restart
 */

uint8_t tg_application_startup(void)
{
	tg.status = TG_OK;
	if (cfg.homing_mode == TRUE) { 	// conditionally run startup homing
		tg.status = cm_homing_cycle();
	}
#ifdef __SIMULATION_MODE
	_tg_canned_startup();			// pre-load input buffers (for test)
#endif
	return (tg.status);
}

/* 
 * _tg_parser() - process top-level serial input 
 *
 *	tg_parser is the top-level of the input parser tree; dispatches other 
 *	parsers. Calls lower level parser based on mode
 *
 *	Keeps the system MODE, one of:
 *		- idle mode (no lines are interpreted, just control characters)
 *		- config mode
 *		- gcode mode
 *		- direct drive mode
 *
 *	In idle mode it auto-detects mode by first character of input buffer
 *	Quits from a parser are handled by the controller (not individual parsers)
 *	Preserves and passes through return codes (status codes) from lower levels
 */

int _tg_parser(char * buf)
{
	// auto-detect mode if not already set 
	if (tg.mode == TG_IDLE_MODE) {
		switch (toupper(buf[0])) {
			case 'G': case 'M': case 'N': case 'F': case '(': case '%': case '\\':
				_tg_set_mode(TG_GCODE_MODE); break;
			case 'C': case '?': _tg_set_mode(TG_CONFIG_MODE); break;
			case 'D': 			_tg_set_mode(TG_DIRECT_DRIVE_MODE); break;
			case 'R': return (_tg_reset());
			case 'T': return (_tg_test_T());	// run whatever test u want
			case 'U': return (_tg_test_U());	// run 2nd test you want
			case 'H': return (_tg_print_idle_help_screen());
//			case 'I': return (_tg_reserved());	// reserved
//			case 'V': return (_tg_reserved());	// reserved
			case 'Q': return (TG_OK);			// no operation on a Q
			default:  _tg_set_mode(TG_IDLE_MODE); break;
		}
	}
	// dispatch based on mode
	tg.status = TG_OK;
	switch (tg.mode) {
		case TG_CONFIG_MODE: tg.status = cfg_parse(buf, TRUE, TRUE); break;
		case TG_GCODE_MODE: tg.status = gc_gcode_parser(buf); break;
		case TG_DIRECT_DRIVE_MODE: tg.status = dd_parser(buf); break;
	}
	return (tg.status);
}

/*
 * _tg_set_mode() - Set current operating mode
 */

void _tg_set_mode(uint8_t mode)
{
	tg.mode = mode;
}

/*
 * _tg_reset() - run power-up resets, including homing (table zero)
 */

int _tg_reset(void)
{
	tg.status = tg_application_startup();	// application startup sequence
	return(tg.status);
}

/*
 * _tg_kill_handler()
 * _tg_term_handler()
 * _tg_pause_handler()
 * _tg_resume_handler()
 */

int _tg_kill_handler(void)
{
	if (!sig_kill_flag) { return (TG_NOOP); }
	sig_kill_flag = 0;
	tg_reset_source();
	cm_async_end();			// stop computing and generating motions
	return (TG_EAGAIN);		// best to restart the control loop;
}

int _tg_term_handler(void)
{
	if (!sig_term_flag) { return (TG_NOOP); }
	sig_term_flag = 0;
	tg_reset_source();
	cm_async_end();			// stop computing and generating motions
	return (TG_EAGAIN);
}

int _tg_pause_handler(void)
{
	if (!sig_pause_flag) { return (TG_NOOP); }
	sig_pause_flag = 0;
	cm_async_stop();
	return (TG_EAGAIN);
}

int _tg_resume_handler(void)
{
	if (!sig_resume_flag) { return (TG_NOOP); }
	sig_resume_flag = 0;
	cm_async_start();
	return (TG_EAGAIN);
}

/*
 * tg_reset_source()  Reset source to default input device
 * _tg_set_source()  Set current input source
 *
 * Note: Once multiple serial devices are supported this function should 
 *	be expanded to also set the stdout/stderr console device so the prompt
 *	and other messages are sent to the active device.
 */

void tg_reset_source()
{
	_tg_set_source(tg.default_src);
}

void _tg_set_source(uint8_t d)
{
	tg.src = d;							// d = XIO device #. See xio.h
	if (tg.src == XIO_DEV_PGM) {
		tg.prompt_disabled = TRUE;
	} else {
		tg.prompt_disabled = FALSE;
	}
}

/* 
 * _tg_prompt() - conditionally display command line prompt
 *
 * We only want a prompt if the following conditions apply:
 *	- system is ready for the next line of input
 *	- no prompt has been issued (issue only one)
 *
 * Further, we only want an asterisk in the prompt if it's not a file device.
 *
 * ---- Mode Strings - for ASCII output ----
 *	This is an example of how to put a string table into program memory
 *	The order of strings in the table must match order of prModeTypes enum
 *	Access is by: (PGM_P)pgm_read_word(&(tgModeStrings[i]));
 *	  where i is the tgModeTypes enum, e.g. modeGCode
 *
 *	ref: http://www.cs.mun.ca/~paul/cs4723/material/atmel/avr-libc-user-manual-1.6.5/pgmspace.html
 *	ref: http://johnsantic.com/comp/state.html, "Writing Efficient State Machines in C"
 */

char tgModeStringIdle[] PROGMEM = "IDLE"; // put strings in program memory
char tgModeStringConfig[] PROGMEM = "CONFIG";
char tgModeStringGCode[] PROGMEM = "GCODE";
char tgModeStringDirect[] PROGMEM = "DIRECT";

PGM_P tgModeStrings[] PROGMEM = {	// put string pointer array in program memory
	tgModeStringIdle,
	tgModeStringConfig,
	tgModeStringGCode,
	tgModeStringDirect
};

void _tg_prompt()
{
	printf_P(PSTR("tinyg[%S]*> "),(PGM_P)pgm_read_word(&tgModeStrings[tg.mode]));
	tg.prompted = TRUE;				// set prompt state
}

/*
 * tg_print_status()
 *
 *	Send status message to stderr. "Case out" common messages 
 */

// put strings in program memory
char tgs00[] PROGMEM = "{00} OK";
char tgs01[] PROGMEM = "{01} ERROR";
char tgs02[] PROGMEM = "{02} EAGAIN";
char tgs03[] PROGMEM = "{03} NOOP";
char tgs04[] PROGMEM = "{04} COMPLETE";
char tgs05[] PROGMEM = "{05} End of line";
char tgs06[] PROGMEM = "{06} End of file";
char tgs07[] PROGMEM = "{07} File not open";
char tgs08[] PROGMEM = "{08} Max file size exceeded";
char tgs09[] PROGMEM = "{09} No such device";
char tgs10[] PROGMEM = "{10} Buffer empty";
char tgs11[] PROGMEM = "{11} Buffer full - fatal";
char tgs12[] PROGMEM = "{12} Buffer full - non-fatal";
char tgs13[] PROGMEM = "{13} QUIT";
char tgs14[] PROGMEM = "{14} Unrecognized command";
char tgs15[] PROGMEM = "{15} Expected command letter";
char tgs16[] PROGMEM = "{16} Unsupported statement";
char tgs17[] PROGMEM = "{17} Parameter under range";
char tgs18[] PROGMEM = "{18} Parameter over range";
char tgs19[] PROGMEM = "{19} Bad number format";
char tgs20[] PROGMEM = "{20} Floating point error";
char tgs21[] PROGMEM = "{21} Motion control error";
char tgs22[] PROGMEM = "{22} Arc specification error";
char tgs23[] PROGMEM = "{23} Zero length line";
char tgs24[] PROGMEM = "{24} Maximum feed rate exceeded";
char tgs25[] PROGMEM = "{25} Maximum seek rate exceeded";
char tgs26[] PROGMEM = "{26} Maximum table travel exceeded";
char tgs27[] PROGMEM = "{27} Maximum spindle speed exceeded";
char tgs28[] PROGMEM = "{28} Failed to converge";
char tgs29[] PROGMEM = "{29} Unused error string";

// put string pointer array in program memory. MUST BE SAME COUNT AS ABOVE
PGM_P tgStatus[] PROGMEM = {	
	tgs00, tgs01, tgs02, tgs03, tgs04, tgs05, tgs06, tgs07, tgs08, tgs09,
	tgs10, tgs11, tgs12, tgs13, tgs14, tgs15, tgs16, tgs17, tgs18, tgs19,
	tgs20, tgs21, tgs22, tgs23, tgs24, tgs25, tgs26, tgs27, tgs28, tgs29
};

void tg_print_status(const uint8_t status_code, const char *textbuf)
{
	switch (status_code) {		// don't send messages for these status codes
		case TG_OK: return;
		case TG_EAGAIN: return;
		case TG_NOOP: return;
		case TG_QUIT: return;
		case TG_ZERO_LENGTH_MOVE: return;
	}
	printf_P(PSTR("%S: %s\n"),(PGM_P)pgm_read_word(&tgStatus[status_code]), textbuf);
//	printf_P(PSTR("%S\n"),(PGM_P)pgm_read_word(&tgStatus[status_code])); // w/no text
}



/*
 * _tg_print_idle_help_screen() - Send help screen to stderr
 */

int _tg_print_idle_help_screen(void)
{
	printf_P(PSTR("*** TinyG Help ***\n\
You are  in IDLE mode as you can see by the prompt\n\
- To enter CONFIG mode hit '?'\n\
- To enter GCODE mode hit 'g' or start loading gcode\n\
- To show the state of any mode hit '?'\n\
- To run a test pattern hit 't' (allow 6 inches in positive X and Y)\n\
- To exit any mode hit 'q'\n\
- To show this help file hit 'h'\n\
Please log any issues at http://synthetos.com/forums\n\
Have fun\n"));

	return (TG_OK);
}

/*********************************************************************************
 * Various test routines
 * _tg_test_T() - 'T' runs a test file from program memory
 * _tg_test_U() - 'U' runs a different test file from program memory
 * _tg_canned_startup() - loads input buffer at reset
 */

int _tg_test_T(void)
{
//	xio_open_pgm(PGMFILE(&trajectory_cases_01));
//	xio_open_pgm(PGMFILE(&system_test01)); 		// collected system tests
//	xio_open_pgm(PGMFILE(&system_test01a)); 	// short version of 01
//	xio_open_pgm(PGMFILE(&system_test02)); 		// arcs only
//	xio_open_pgm(PGMFILE(&system_test03)); 		// lines only
//	xio_open_pgm(PGMFILE(&system_test04)); 		// decreasing 3d boxes
//	xio_open_pgm(PGMFILE(&straight_feed_test));
//	xio_open_pgm(PGMFILE(&arc_feed_test));
//	xio_open_pgm(PGMFILE(&contraptor_circle)); 	// contraptor circle test
	xio_open_pgm(PGMFILE(&braid2d)); 			// braid test, part 1
//	xio_open_pgm(PGMFILE(hacdc));	 			// HacDC logo
	_tg_set_source(XIO_DEV_PGM);
	_tg_set_mode(TG_GCODE_MODE);
	return (TG_OK);
}

int _tg_test_U(void)
{
	xio_open_pgm(PGMFILE(&braid2d_part2)); 		// braid test, part 2
//	xio_open_pgm(PGMFILE(&contraptor_circle)); 	// contraptor circle test
	_tg_set_source(XIO_DEV_PGM);
	_tg_set_mode(TG_GCODE_MODE);
	return (TG_OK);
}

// TESTS AND CANNED STARTUP ROUTINES
// Pre-load the USB RX (input) buffer with some test strings
// Be mindful of the char limit on the RX_BUFFER_SIZE (circular buffer)

void _tg_canned_startup()
{
//	xio_queue_RX_string_usb("H\n");		// run help file

	xio_queue_RX_string_usb("T\n");		// run test file
	xio_queue_RX_string_usb("Q\n");		// back to idle mode
	xio_queue_RX_string_usb("U\n");		// run second test file
//
//	xio_queue_RX_string_usb("g64\n");	// test setting path control modes
//	xio_queue_RX_string_usb("g61\n");
//	xio_queue_RX_string_usb("g61.1\n");

//	xio_queue_RX_string_usb("?\n");		// enter config mode and dump config
//	xio_queue_RX_string_usb("Q\n");		// go to idle mode
//	xio_queue_RX_string_usb("R\n");		// run a homing cycle

//	xio_queue_RX_string_usb("!\n");		// kill
//	xio_queue_RX_string_usb("@\n");		// pause
//	xio_queue_RX_string_usb("$\n");		// resume

//	xio_queue_RX_string_usb("(MSGtest message in comment)\n");
//	xio_queue_RX_string_usb("g1 f450 x10 y13\n");
//	xio_queue_RX_string_usb("g0x0y0z0\n");
//	xio_queue_RX_string_usb("g0x10y10\n");
//	xio_queue_RX_string_usb("g0x1000\n");
//	xio_queue_RX_string_usb("g0x10000\n");
//	xio_queue_RX_string_usb("g0x10\ng4p1\ng0x0\n");
//	xio_queue_RX_string_usb("g0 x-10 (MSGtest)\n");

//	xio_queue_RX_string_usb("g1 F500 x10\n");
//	xio_queue_RX_string_usb("g1 F100 y8\n");
//	xio_queue_RX_string_usb("g1 F100 x10.1\n");
//	xio_queue_RX_string_usb("g0 x2.1 y1.1 z2.2\n");
//	xio_queue_RX_string_usb("g4 p2\n");

//	xio_queue_RX_string_usb("g1 f400 x0\n");
//	xio_queue_RX_string_usb("y0\n");
//	xio_queue_RX_string_usb("z0\n");

//	xio_queue_RX_string_usb("g2x100y100z25i50j50f249\n");
//	xio_queue_RX_string_usb("g2 f300 x10 y10 i8 j8\n");
//	xio_queue_RX_string_usb("g2 f300 x10 y10 i5 j5\n");
//	xio_queue_RX_string_usb("g2 f300 x3 y3 i1.5 j1.5\n");


//	xio_queue_RX_string_usb("g0 x100 y110 z120\n");
//	xio_queue_RX_string_usb("g0 x0 y0 z0\n");

//	xio_queue_RX_string_usb("g1 f400 x0\n");
//	xio_queue_RX_string_usb("y0\n");
//	xio_queue_RX_string_usb("z0\n");
//	xio_queue_RX_string_usb("x100\n");
//	xio_queue_RX_string_usb("y100\n");
//	xio_queue_RX_string_usb("z100\n");

//	xio_queue_RX_string_usb("g0 x10 y11 z12\n");
//	xio_queue_RX_string_usb("g1 f200 x20 y22.5 z28\n");
//	xio_queue_RX_string_usb("g0 x20 y22.5 z28\n");
//	xio_queue_RX_string_usb("g1 f340 x0 y10 z-13\n");

//	xio_queue_RX_string_usb("g1 f300 x3 y4 z5\n");
//	xio_queue_RX_string_usb("g1 f450 x-10 y-11 z-21.7\n");
//	xio_queue_RX_string_usb("g0 x0 y10 z-10\n");

//	xio_queue_RX_string_usb("g0 x1 y1.1 z1.2\n");
//	xio_queue_RX_string_usb("x-1\n");
//	xio_queue_RX_string_usb("y-1.3\n");
//	xio_queue_RX_string_usb("z-2.01\n");

//	xio_queue_RX_string_usb("g92 x0 y0 z0\n");
//	xio_queue_RX_string_usb("g0x10y10z0\n");
//	xio_queue_RX_string_usb("g91g0x5y5\n");
//	xio_queue_RX_string_usb("g1 f200 x10\n");
//	xio_queue_RX_string_usb("y10\n");
//	xio_queue_RX_string_usb("g0 x100 y100 z100 a100\n");
//	xio_queue_RX_string_usb("?\n");

// mudflap simulation
/*
	xio_queue_RX_string_usb("(SuperCam Ver 2.2a SPINDLE)\n");
	xio_queue_RX_string_usb("N1 G20	( set inches mode - ash )\n");
	xio_queue_RX_string_usb("N1 G20\n");
	xio_queue_RX_string_usb("N5 G40 G17\n");
	xio_queue_RX_string_usb("N10 T1 M06\n");
	xio_queue_RX_string_usb("(N15 G90 G0 X0 Y0 Z0)\n");
	xio_queue_RX_string_usb("N20 S5000 M03\n");
	xio_queue_RX_string_usb("N25 G00 F30.0\n");
	xio_queue_RX_string_usb("N30 X0.076 Y0.341\n");
	xio_queue_RX_string_usb("N35 G00 Z-1.000 F90.0\n");
	xio_queue_RX_string_usb("N40 G01 Z-1.125 F30.0\n");
	xio_queue_RX_string_usb("N45 G01 F60.0\n");
	xio_queue_RX_string_usb("N50 X0.064 Y0.326\n");
	xio_queue_RX_string_usb("N55 X0.060 Y0.293\n");
	xio_queue_RX_string_usb("N60 X0.077 Y0.267\n");
	xio_queue_RX_string_usb("N65 X0.111 Y0.257\n");
	xio_queue_RX_string_usb("N70 X0.149 Y0.252\n");
	xio_queue_RX_string_usb("N75 X0.188 Y0.255\n");
*/
}

/* FURTHER NOTES

---- Generalized Serial Handler / Parser ----

  Want to do the following things:

	- Be able to interpret (and mix) various types of inputs, including:
		- Control commands from stdio - e.g. ^c, ^q/^p, ^n/^o...
		- Configuration commands for various sub-systems
		- Gcode blocks
		- Motion control commands (that bypass the Gcode layer)
		- Multi-DOF protocols TBD 
	- Accept and mix inputs from multiple sources:
		- USB
		- RS-485
		- Arduino serial port (Aux)
		- strings in program memory
		- EEPROM data
		- SD card data
	- Accept multiple types of line terminators including:
		- CR
		- LF
		- semicolon
		- NUL

---- Design notes ----

  	- XIO line readers are the lowest level (above single character read)
		From serial inputs: read single characters to assemble a string
		From in-memory strings: read characters from a string in program memory
		Either mode: read string to next terminator and return NULL terminated string 
		Do not otherwise process or normalize the string

	- tg_parser is the top-level parser / dispatcher
		Examine the head of the string to determine how to dispatch
		Supported dispatches:
		- Gcode block
		- Gcode configuration line
		- Direct drive (motion control) command
		- Network command / config (not implemented)

	- Individual parsers/interpreters are called from tg_parser
		These can assume:
		- They will only receive a single line (multi-line inputs have been split)
		- Tyey perform line normalization required for that dispatch type
		- Can run the current command to completion before receiving another command

	- Flow control
		Flow control is provided by the called routine running to completion 
		without blocking. If blocking could occur (e.g. move buffer is full)
		the routine should return and provide a continuation in the main 
		controller loop. This necessitates some careful state handling.

---- How To Code Continuations ----

	Continuations are used to manage points where the application would 
	ordinarily block. Call it application managed threading by way of an 
	inverted control loop. By coding using continuations the application 
	does not need an RTOS and is extremely responsive (there are no "ticks")

	Rules for writing a continuation task:
	  - A continuation is a pair of routines. The first is the main routine,
		the second the continuation. See mc_line() and mc_line_continue().

	  - The main routine is called first and should never block. It may 
	    have function arguments. It performs all initial actions and sets 
		up a static structure to hold data that is needed by the 
		continuation routine. The main routine should end by returning a 
		uint8_t TG_OK or an error code.

	  - The continuation task is a callback that is permanemtly registered 
	  	at the right level of the blocking heirarchy in the tg_controller 
		loop; where it will be called repeatedly by the controller. The 
		continuation cannot have input args - all necessary data must be 
		available in the static struct (or by some other means).

	  - Continuations should be coded as state machines. See the homing 
	  	cycle as an example. Common states used by most machines include: 
		OFF, NEW, or RUNNING. OFF means take no action (return NOOP). 
		The state on initial entry after the main routine should be NEW.
		RUNNING is a catch-all for simple routines. More complex state
		machines may have numerous other states.

	  - The continuation must return the following codes and may return 
	  	additional codes to indicate various exception conditions:

	 	TG_NOOP: No operation ocurred. This is the usual return from an 
			OFF state. All continuations must be callable with no effect 
			when they are OFF (as they are called repeatedly by the 
			controller whether or not they are active).

		TG_EAGAIN: The continuation is blocked or still processing. This one 
			is really important. As long as the continuation still has work 
			to do it must return TG_EAGAIN. Returning eagain causes the 
			tg_controller dispatcher to restart the controller loop from 
			the beginning, skipping all later routines. This enables 
			heirarchical blocking to be performed. The later routines will 
			not be run until the blocking conditions at the lower-level are
			removed.

		TG_OK; The continuation task  has just is completed - i.e. it has 
			just transitioned to OFF. TG_OK should only be returned only once. 
			The next state will be OFF, which will return NOOP.

		TG_COMPLETE: This additional state is used for nesting state 
			machines such as the homing cycle or other cycles (see the 
			homing cycle as an example of a nested state machine). 
			The lower-level routines called by a parent will return 
			TG_EAGAIN until they are done, then they return TG_OK. 
			The return codes from the continuation should be trapped by 
			a wrapper routine that manages the parent and child returns 
			When the parent REALLY wants to return it sends its wrapper 
			TG_COMPLETE, which is translated to an OK for the parent routine.
*/