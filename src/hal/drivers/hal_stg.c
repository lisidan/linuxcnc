/** This is the driver for an Servo-To-Go Model I board.
    The board includes 8 channels of quadrature encoder input,
    8 channels of analog input and output, 32 bits digital I/O,
    and an interval timer with interrupt.
        
    Installation of the driver only realtime:
    
	insmod hal_stg num_chan=8 dio="IIOO"
	    - autodetects the address
	or
    
	insmod hal_stg base=0x200 num_chan=8 dio="IIOO"
    
    Check your Hardware manual for your base address.

    The digital inputs/outputs configuration is determined by a 
    config string passed to insmod when loading the module.  
    The format consists by a four character string that sets the
    direction of each group of pins. Each character of the direction
    string is either "I" or "O".  The first character sets the
    direction of port A (Port A - DIO.0-7), the next sets 
    port B (Port B - DIO.8-15), the next sets port C (Port C - DIO.16-23), 
    and the fourth sets port D (Port D - DIO.24-31).
    
    The following items are exported to the HAL.
   
    Encoders:
      Parameters:
/totest	float	stg.<channel>.enc-scale
   
      Pins:
/totest	s32	stg.<channel>.enc-counts
/totest	float	stg.<channel>.enc-position

/todo   bit	stg.<channel>.enc-index
/todo  	bit	stg.<channel>.enc-idx-latch
/todo  	bit	stg.<channel>.enc-latch-index
/todo  	bit	stg.<channel>.enc-reset-count
   
      Functions:
/totest void    stg.<channel>.capture_position
   
   
    DACs:
      Parameters:
/totest	float	stg.<channel>.dac-offset
/totest	float	stg.<channel>.dac-gain
   
      Pins:
/totest	float	stg.<channel>.dac-value
   
      Functions:
/totest	void    stg.<channel>.dac_write
   
   
    ADC:
      Parameters:
/totest	float	stg.<channel>.adc-offset
/totest	float	stg.<channel>.adc-gain
   
      Pins:
/totest	float	stg.<channel>.adc-value
   
      Functions:
/totest	void    stg.<channel>.adc_read
   
   
    Digital In:
      Pins:
/totest	bit	stg.in-<pinnum>
/totest	bit	stg.in-<pinnum>-not
   
      Functions:
/totest	void    stg.digital_in_read
   
   
    Digital Out:
      Parameters:
/totest	bit	stg.out-<pinnum>-invert
   
      Pins:
/totest	bit	stg.out-<pinnum>
   
      Functions:
/totest	void    stg.digital_out_write

*/

/** Copyright (C) 2004 Alex Joni
                       <alex DOT joni AT robcon DOT ro>
*/

/** Copyright (C) 2003 John Kasunich
                       <jmkasunich AT users DOT sourceforge DOT net>
*/

/* Based on STGMEMBS.CPP from the Servo To Go Windows drivers 
    - Copyright (c) 1996 Servo To Go Inc and released under GPL Version 2 */

/** This program is free software; you can redistribute it and/or
    modify it under the terms of version 2.1 of the GNU General
    Public License as published by the Free Software Foundation.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA

    THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
    ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
    TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
    harming persons must have provisions for completely removing power
    from all motors, etc, before persons enter any danger area.  All
    machinery must be designed to comply with local and national safety
    codes, and the authors of this software can not, and do not, take
    any responsibility for such compliance.

    This code was written as part of the EMC HAL project.  For more
    information, go to www.linuxcnc.org.
*/


#ifndef RTAPI
#error This is a realtime component only!
#endif

#include <asm/io.h>
#include "rtapi.h"		/* RTAPI realtime OS API */
#include "rtapi_app.h"		/* RTAPI realtime module decls */
#include "hal.h"		/* HAL public API decls */
#include "hal_stg.h"		/* STG related defines */

#ifndef MODULE
#define MODULE
#endif

#ifdef MODULE
/* module information */
MODULE_AUTHOR("Alex Joni");
MODULE_DESCRIPTION("Driver for Servo-to-Go Model I for EMC HAL");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif /* MODULE_LICENSE */
static int base = 0x000;		/* board base address, 0 means autodetect */
MODULE_PARM(base, "i");
MODULE_PARM_DESC(base, "board base address, don't use for autodetect");
static int num_chan = MAX_CHANS;	/* number of channels - default = 8 */
MODULE_PARM(num_chan, "i");
MODULE_PARM_DESC(num_chan, "number of channels");
static long period = 0;			/* thread period - default = no thread */
MODULE_PARM(period, "l");
MODULE_PARM_DESC(period, "thread period (nsecs)");
static char *dio = "IIOO";		/* thread period - default = port A&B inputs, port C&D outputs */
MODULE_PARM(dio, "s");
MODULE_PARM_DESC(dio, "dio config string - expects something like IIOO");
#endif /* MODULE */

/***********************************************************************
*                STRUCTURES AND GLOBAL VARIABLES                       *
************************************************************************/

typedef struct {
    hal_bit_t *data;		/* basic pin for input or output */
    union {
	hal_bit_t *not;		/* pin for inverted data (input only) */
	hal_bit_t invert;	/* param for inversion (output only) */
	} io;
} io_pin;

typedef struct {
/* counter data */
    hal_s32_t *count[MAX_CHANS];		/* captured binary count value */
    hal_float_t *pos[MAX_CHANS];		/* scaled position (floating point) */
    hal_float_t pos_scale[MAX_CHANS];		/* parameter: scaling factor for pos */

/* dac data */
    hal_float_t *dac_value[MAX_CHANS];		/* value to be written to dac */
    hal_float_t dac_offset[MAX_CHANS];		/* offset value for DAC */
    hal_float_t dac_gain[MAX_CHANS];		/* gain to be applied */

/* adc data */
    hal_float_t *adc_value[MAX_CHANS];		/* value to be read from adc */
    hal_float_t adc_offset[MAX_CHANS];		/* offset value for ADC */
    hal_float_t adc_gain[MAX_CHANS];		/* gain to be applied */
    int adc_current_chan;			/* holds the currently converting channel */

/* dio data */
    io_pin port[4][8];				/* holds 4 ports each 8 pins, either input or output */
    unsigned char dir_bits;			/* remembers config (which port is input which is output) */

} stg_struct;

static stg_struct *stg_driver;

/* other globals */
static int comp_id;		/* component ID */
static int outpinnum=0, inputpinnum=0;

#define DATA(x) (base + (2 * x) - (x % 2))	/* Address of Data register 0 
						 */
#define CTRL(x) (base + (2 * (x+1)) - (x % 2))	/* Address of Control
						   register 0 */

/***********************************************************************
*                  LOCAL FUNCTION DECLARATIONS                         *
************************************************************************/
/* helper functions, to export HAL pins & co. */
static int export_counter(int num, stg_struct * addr);
static int export_dac(int num, stg_struct * addr);
static int export_adc(int num, stg_struct * addr);
static int export_pins(int num, int dir, stg_struct * addr);
static int export_input_pin(int pinnum, io_pin * pin);
static int export_output_pin(int pinnum, io_pin * pin);

/* Board specific functions */

/* initializes the STG, takes care of autodetection, all initialisations */
static int stg_init_card(void);
/* scans possible addresses for STG cards */
static unsigned short stg_autodetect(void);

/* counter related functions */
static int stg_counter_init(int ch);
static long stg_counter_read(int i);

/* dac related functions */
static int stg_dac_init(int ch);
static int stg_dac_write(int ch, short value);

/* adc related functions */
static int stg_adc_init(int ch);
static int stg_adc_start(unsigned short wAxis);
static short stg_adc_read(int ch);

/* counter related functions */
static int stg_dio_init(stg_struct * addr);

/* periodic functions registered to HAL */
static void stg_adcs_read(void *arg, long period); //reads adc data from the board, check long description at the beginning of the function
static void stg_dacs_write(void *arg, long period); //writes dac's to the STG
static void stg_counter_capture(void *arg, long period); //captures encoder counters
static void stg_di_read(void *arg, long period); //reads digital inputs from the STG
static void stg_do_write(void *arg, long period); //writes digital outputs to the STG

/***********************************************************************
*                       INIT AND EXIT CODE                             *
************************************************************************/

#define MAX_CHAN 8

int rtapi_app_main(void)
{
    int n, retval, mask, m;
    unsigned char dir_bits;

    /* test for number of channels */
    if ((num_chan <= 0) || (num_chan > MAX_CHAN)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: invalid num_chan: %d\n", num_chan);
	return -1;
    }

    /* test for config string */
    if ((dio == 0) || (dio[0] == '\0')) {
	rtapi_print_msg(RTAPI_MSG_ERR, "STG: ERROR: no dio config string\n");
	return -1;
    }

    /* have good config info, connect to the HAL */
    comp_id = hal_init("hal_stg");
    if (comp_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "STG: ERROR: hal_init() failed\n");
	return -1;
    }


    /* allocate shared memory for stg data */
    stg_driver = hal_malloc(num_chan * sizeof(stg_struct));
    if (stg_driver == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "STG: ERROR: hal_malloc() failed\n");
	hal_exit(comp_id);
	return -1;
    }

    /* takes care of all initialisations, also autodetection if necessary */
    if (stg_init_card() != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: stg_init_card() failed\n");
	hal_exit(comp_id);
	return -1;
    }

    /* dio should be a string of 4 'I" or "O" characters */
    dir_bits = 0;
    mask = 0x01;
    for ( m = 0 ; m < 4 ; m++ ) {
	/* test character and set/clear bit */
	if ((dio[m] == 'i') || (dio[m] == 'I')) {
	    /* input, set mask bit to zero */
	    dir_bits &= ~mask;
	} else if ((dio[m] == 'o') || (dio[m] == 'O')) {
	    /* output, set mask bit to one */
	    dir_bits |= mask;
	} else {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"STG: ERROR: bad config info for port %d\n", m);
	    return -1;
	}
	/* shift mask for next bit */
	mask <<= 1;
    }

    /* we now should have directions figured out, next is exporting the pins based on that */
    mask = 0x01;
    for ( m = 0 ; m < 4 ; m++ ) {
    
	/*          port, direction, driver */
	export_pins(m, (dir_bits & mask), stg_driver);

	/* shift mask for next bit */
	mask <<= 1;
    }
    stg_driver->dir_bits = dir_bits; /* remember direction of each port, will be used in the write / read functions */

    stg_dio_init(stg_driver);
    
    /* export all the variables for each counter, dac */
    for (n = 0; n < num_chan; n++) {
	/* export all vars */
	retval = export_counter(n, stg_driver);
	if (retval != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"STG: ERROR: counter %d var export failed\n", n + 1);
	    hal_exit(comp_id);
	    return -1;
	}
	/* init counter */
	*(stg_driver->count[n]) = 0;
	*(stg_driver->pos[n]) = 0.0;
	stg_driver->pos_scale[n] = 1.0;

	/* init counter chip */
	stg_counter_init(n);
	
	retval = export_dac(n, stg_driver);
	if (retval != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"STG: ERROR: dac %d var export failed\n", n + 1);
	    hal_exit(comp_id);
	    return -1;
	}
	/* init counter */
	*(stg_driver->dac_value[n]) = 0;
	stg_driver->dac_offset[n] = 0.0;
	stg_driver->dac_gain[n] = 1.0;

	/* init dac chip */
	stg_dac_init(n);

	retval = export_adc(n, stg_driver);
	if (retval != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"STG: ERROR: adc %d var export failed\n", n + 1);
	    hal_exit(comp_id);
	    return -1;
	}
	/* init counter */
	*(stg_driver->adc_value[n]) = 0;
	stg_driver->adc_offset[n] = 0.0;
	stg_driver->adc_gain[n] = 1.0;
	
	stg_driver->adc_current_chan = -1; /* notify that no conversion has been started yet */

	/* init adc chip */
	stg_adc_init(n);
    }
    
    /* export functions */
    retval = hal_export_funct("stg.capture_position", stg_counter_capture,
	stg_driver, 1, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: stg_counter_capture funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    rtapi_print_msg(RTAPI_MSG_INFO,
	"STG: installed %d encoder counters\n", num_chan);

    retval = hal_export_funct("stg.write_dacs", stg_dacs_write,
	stg_driver, 1, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: stg_write_dacs funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    rtapi_print_msg(RTAPI_MSG_INFO,
	"STG: installed %d dacs\n", num_chan);

    retval = hal_export_funct("stg.read_adcs", stg_adcs_read,
	stg_driver, 1, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: stg_read_adcs funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    rtapi_print_msg(RTAPI_MSG_INFO,
	"STG: installed %d adcs\n", num_chan);

    retval = hal_export_funct("stg.di_read", stg_di_read,
	stg_driver, 0, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: stg_di_read funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }

    rtapi_print_msg(RTAPI_MSG_INFO,
	"STG: installed %d digital inputs\n", inputpinnum);

    retval = hal_export_funct("stg.do_write", stg_do_write,
	stg_driver, 0, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "STG: ERROR: stg_do_write funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    rtapi_print_msg(RTAPI_MSG_INFO,
	"STG: installed %d digital outputs\n", outpinnum);

    /* was 'period' specified in the insmod command? */
    if (period > 0) {
	/* create a thread */
	retval = hal_create_thread("stg.thread", period, 0);
	if (retval < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"STG: ERROR: could not create thread\n");
	    hal_exit(comp_id);
	    return -1;
	} else {
	    rtapi_print_msg(RTAPI_MSG_INFO, "STG: created %d uS thread\n",
		period / 1000);
	}
    }
    return 0;
}

void rtapi_app_exit(void)
{
    hal_exit(comp_id);
}

/***********************************************************************
*            REALTIME ENCODER COUNTING AND UPDATE FUNCTIONS            *
************************************************************************/

static void stg_counter_capture(void *arg, long period)
{
    stg_struct *stg;
    int n;

    stg = arg;
    for (n = 0; n < num_chan; n++) {
	/* capture raw counts to latches */
	*(stg->count[n]) = stg_counter_read(n);
	/* scale count to make floating point position */
	*(stg->pos[n]) = *(stg->count[n]) * stg->pos_scale[n];
    }
    /* done */
}

/* stg_dacs_write() - writes all dac's to the board
    - calls stg_dac_write() */

static void stg_dacs_write(void *arg, long period)
{    
    stg_struct *stg;
    float volts;
    short ncounts, i;

    stg=arg;
    for (i=0;i < num_chan; i++) {
	/* scale the voltage to be written based on offset and gain */
	volts = (*(stg->dac_value[i]) - stg->dac_offset[i]) * stg->dac_gain[i];
	/* compute the value for the DAC */
	ncounts = (short) ((10.0 - volts) / 20.0 * 0x1FFF);
	/* write it to the card */	
	stg_dac_write(i, ncounts);	
    }
}

/* stg_adcs_read() - reads one adc at a time from the board to hal
    - calls stg_adc_read() */

/* long description :

    Because the conversion takes a while (first mux to the right channel ~5usecs,
    then start the conversion, and wait for it to finish ~16-20 usecs) for all the
    8 channels, it would be too much to start the conversion, wait for the result
    for all the 8 axes.
    Instead a different approach is chosen:
    - on the beginning of the function the conversion should already be done
    - it gets read and sent to HAL
    - the new channel gets mux'ed
    - and at the end of the function the new conversion is started, so that the data
      will be available at the next run.
    This way 8 periods are needed to read 8 ADC's. It is possible to set the board
    to do faster conversions (AZ bit on INTC off), but that would make it less 
    reliable (autozero takes care of temp. errors).*/
/*! \todo STG_ADC_Improvement (if any user requests it).
    Another improvement might be to let the user chose what channels he would like
    for ADC (having only 2 channels might speed things up considerably).
*/
static void stg_adcs_read(void *arg, long period)
{    
    stg_struct *stg;
    float volts;
    short ncounts;

    stg=arg;
    if (stg->adc_current_chan > 0) { 
	/* we should have the conversion done for adc_num_chan */
	ncounts = stg_adc_read(stg->adc_current_chan);
	volts = 10.0 - (ncounts * 20.0 / 0x1FFF);
	*(stg->adc_value[stg->adc_current_chan]) = volts * stg->adc_gain[stg->adc_current_chan] \
			    - stg->adc_offset[stg->adc_current_chan];
    }
    /* if adc_num_chan < 0, it's the first time this routine runs
       thus we don't have any ready data, we simply start the next conversion */
    if (stg->adc_current_chan++ > num_chan) 
	stg->adc_current_chan=0; //increase the channel, and roll back to 0 after all chans are done

    /* select the current channel with the mux, and start the conversion */
    stg_adc_start(stg->adc_current_chan);
    /* the next time this function runs, the result should be available */
}


// helper function to extract the data out of a char and place it into HAL data
// written by JMK
static void split_input(unsigned char data, io_pin *dest, int num)
{
    int b;
    unsigned char mask;

    /* splits a byte into 'num' HAL pins (and their NOTs) */
    mask = 0x01;
    for (b = 0 ; b < num ; b++ ) {
	if ( data & mask ) {
	    /* input high, which means FALSE (active low) */
	    *(dest->data) = 0;
	    *(dest->io.not) = 1;
	} else {
	    /* input low, which means TRUE */
	    *(dest->data) = 1;
	    *(dest->io.not) = 0;
	}
	mask <<= 1;
	dest++;
    }
}    

// helper function to extract the data out of HAL and place it into a char
// written by JMK
unsigned char build_output(io_pin *src, int num)
{
    int b;
    unsigned char data, mask;

    data = 0x00;
    mask = 0x01;
    /* assemble output byte for data port from 'num' source variables */
    for (b = 0; b < num; b++) {
	/* get the data, add to output byte */
	if ( *(src->data) ) {
	    if ( !(src->io.invert) ) {
		data |= mask;
	    }
	} else {
	    if ( (src->io.invert) ) {
		data |= mask;
	    }
	}
	mask <<= 1;
	src++;
    }
    return data;
}


static void stg_di_read(void *arg, long period) //reads digital inputs from the STG
{
    stg_struct *stg;
    unsigned char val;
    stg=arg;
    
    if ( (stg->dir_bits & 0x01) == 0) { // if port A is set as input, read the bits
	val = inb(base + DIO_A);
	split_input(val, &(stg->port[0][0]), 8);
    }
    if ( (stg->dir_bits & 0x02) == 0) { // if port B is set as input, read the bits
	val = inb(base + DIO_B);
	split_input(val, &(stg->port[1][0]), 8);
    }
    if ( (stg->dir_bits & 0x04) == 0) { // if port C is set as input, read the bits
	val = inb(base + DIO_C);
	split_input(val, &(stg->port[2][0]), 8);
    }
    if ( (stg->dir_bits & 0x08) == 0) { // if port D is set as input, read the bits
	val = inb(base + DIO_D);
	split_input(val, &(stg->port[3][0]), 8);
    }
}

static void stg_do_write(void *arg, long period) //writes digital outputs to the STG
{
    stg_struct *stg;
    unsigned char val;
    stg=arg;

    if ( (stg->dir_bits & 0x01) != 0) { // if port A is set as output, write the bits
	val = build_output(&(stg->port[0][0]), 8);
	outb(base + DIO_A, val);
    }
    if ( (stg->dir_bits & 0x02) != 0) { // if port B is set as output, write the bits
	val = build_output(&(stg->port[1][0]), 8);
	outb(base + DIO_B, val);
    }
    if ( (stg->dir_bits & 0x04) != 0) { // if port C is set as output, write the bits
	val = build_output(&(stg->port[2][0]), 8);
	outb(base + DIO_C, val);
    }
    if ( (stg->dir_bits & 0x08) != 0) { // if port D is set as output, write the bits
	val = build_output(&(stg->port[3][0]), 8);
	outb(base + DIO_D, val);
    }

}

/***********************************************************************
*                      BOARD SPECIFIC FUNCTIONS                        *
************************************************************************/
/*
  stg_counter_init() - Initializes the channel
*/

static int stg_counter_init(int ch)
{
    /* Set Counter Command Register - Master Control, Master Reset (MRST), */
    /* and Reset address pointer (RADR). */
    outb(CTRL(ch), 0x23);

    /* Set Counter Command Register - Input Control, OL Load (P3), */
    /* and Enable Inputs A and B (INA/B). */
    outb(CTRL(ch), 0x68);

    /* Set Counter Command Register - Output Control */
    outb(CTRL(ch), 0x80);

    /* Set Counter Command Register - Quadrature */
    outb(CTRL(ch), 0xC3);
    return 0;
}


/*
  stg_dac_init() - Initializes the dac channel
*/

static int stg_dac_init(int ch)
{
    int i;
    
    /* set all DAC's to 0 on startup */
    for (i=0; i < num_chan; i++) {
	stg_dac_write(i, 0);
    }
    return 0;
}


/*
  stg_dac_write() - writes a dac channel
*/

static int stg_dac_write(int ch, short value)
{        

    /* write the DAC */
    outb (base + DAC_0 + (ch << 1), value);

    return 0;
}

/*
  stg_counter_read() - reads one channel
*/
static long stg_counter_read(int i)
{
    union pos_tag {
	long l;
	struct byte_tag {
	    char b0;
	    char b1;
	    char b2;
	    char b3;
	} byte;
    } pos;

    outb(CTRL(i), 0x03);
    pos.byte.b0 = inb(DATA(i));
    pos.byte.b1 = inb(DATA(i));
    pos.byte.b2 = inb(DATA(i));
    if (pos.byte.b2 < 0) {
	pos.byte.b3 = -1;
    } else {
	pos.byte.b3 = 0;
    }
    return pos.l;
}

/*
  stg_adc_init() - Initializes the dac channel
*/

static int stg_adc_init(int ch)
{
    /* not much to setup for the ADC's */
    /* only select the mode of operation we will work with AutoZero */
    outb(base + MIO_2, 0x01);	// the second 82C55 is already configured (by running stg_dio_init)
				// we only set bit 0 (AZ) to 1 to enable it
    return 0;
}

int stg_adc_start(unsigned short wAxis)
{
    /* do a dummy read from the ADC, just to set the input multiplexer to
     the right channel */
    inw(base + ADC_0 + (wAxis << 1));

    /* wait 4 uS for settling time on the multiplexer and ADC. You probably
     shouldn't really have a delay in a driver */
    outb(0x80, 0);
    outb(0x80, 0);
    outb(0x80, 0);
    outb(0x80, 0);

    /* now start conversion */
    outw(base + ADC_0 + (wAxis << 1), 0);

    return 0;
};

static short stg_adc_read(int axis)
{
    short j;

    /*
    there must have been a delay between stg_adc_start() and 
    stg_adc_read(), of 19 usec if autozeroing (we are), 4 usecs 
    otherwise. In code that calls this, make sure you split these 
    calls up with some intervening code
    */

    /* make sure conversion is done, assume polling delay is done.
    EOC (End Of Conversion) is bit 0x08 in IIR (Interrupt Request
    Register) of Interrupt Controller.  Don't wait forever though
    bail out eventually. */

    for (j = 0; !(inb(base + IRR) & 0x08) && (j < 1000); j++);
    
    j = inw(base + ADC_0 + (axis << 1));

    if (j & 0x1000)       /* is sign bit negative? */
	j |= 0xf000;      /* sign extend */
    else
	j &= 0xfff;       /* make sure high order bits are zero. */

    return j;
};


/*
  stg_dio_init() - Initializes the dio's
*/

static int stg_dio_init(stg_struct *addr)
{
    /* we will select the directions of each port */
    unsigned char control;

    control = 0x80; //set up |1|0|0|A|CH|0|B|CL|
    if ( (addr->dir_bits & 0x01) == 0) // if port A is set as input, set bit accordingly
	control |= 0x10;
    if ( (addr->dir_bits & 0x02) == 0) // if port B is set as input, set bit accordingly
	control |= 0x02;
    if ( (addr->dir_bits & 0x04) == 0) // if port C is set as input, set bits accordingly
	control |= 0x09;
    
    // write the computed control to MIO_1
    outb(base+MIO_1, control);
    
    // next compute the directions for port D, located on the second 82C55
    control = 0x82;
    
    if ( (addr->dir_bits & 0x08) == 0)// if port C is set as input, set bits accordingly
	control |= 0x10;
	
    // write the computed control to MIO_2
    outb(base+MIO_2, control);
    
    return 0;
}


/***********************************************************************
*                   LOCAL FUNCTION DEFINITIONS                         *
************************************************************************/

static int export_counter(int num, stg_struct *addr)
{
    int retval, msg;
    char buf[HAL_NAME_LEN + 2];

    /* This function exports a lot of stuff, which results in a lot of
       logging if msg_level is at INFO or ALL. So we save the current value
       of msg_level and restore it later.  If you actually need to log this
       function's actions, change the second line below */
    msg = rtapi_get_msg_level();
    rtapi_set_msg_level(RTAPI_MSG_WARN);

    /* export pin for counts captured by update() */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.counts", num);
    retval = hal_pin_s32_new(buf, HAL_WR, &addr->count[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export pin for scaled position captured by update() */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.position", num);
    retval = hal_pin_float_new(buf, HAL_WR, &addr->pos[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameter for scaling */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.position-scale", num);
    retval = hal_param_float_new(buf, HAL_WR, &addr->pos_scale[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* restore saved message level */
    rtapi_set_msg_level(msg);
    return 0;
}

static int export_dac(int num, stg_struct *addr)
{
    int retval, msg;
    char buf[HAL_NAME_LEN + 2];

    /* This function exports a lot of stuff, which results in a lot of
       logging if msg_level is at INFO or ALL. So we save the current value
       of msg_level and restore it later.  If you actually need to log this
       function's actions, change the second line below */
    msg = rtapi_get_msg_level();
    rtapi_set_msg_level(RTAPI_MSG_WARN);

    /* export pin for voltage received by the board() */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.dac-value", num);
    retval = hal_pin_float_new(buf, HAL_WR, &addr->dac_value[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameter for offset */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.dac-offset", num);
    retval = hal_param_float_new(buf, HAL_WR, &addr->dac_offset[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameter for gain */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.dac-gain", num);
    retval = hal_param_float_new(buf, HAL_WR, &addr->dac_gain[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* restore saved message level */
    rtapi_set_msg_level(msg);
    return 0;
}

static int export_adc(int num, stg_struct *addr)
{
    int retval, msg;
    char buf[HAL_NAME_LEN + 2];

    /* This function exports a lot of stuff, which results in a lot of
       logging if msg_level is at INFO or ALL. So we save the current value
       of msg_level and restore it later.  If you actually need to log this
       function's actions, change the second line below */
    msg = rtapi_get_msg_level();
    rtapi_set_msg_level(RTAPI_MSG_WARN);

    /* export pin for voltage received by the board() */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.adc-value", num);
    retval = hal_pin_float_new(buf, HAL_WR, &addr->adc_value[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameter for offset */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.adc-offset", num);
    retval = hal_param_float_new(buf, HAL_WR, &addr->adc_offset[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameter for gain */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.%d.adc-gain", num);
    retval = hal_param_float_new(buf, HAL_WR, &addr->adc_gain[num], comp_id);
    if (retval != 0) {
	return retval;
    }
    /* restore saved message level */
    rtapi_set_msg_level(msg);
    return 0;
}

static int export_pins(int num, int dir, stg_struct *addr)
{
    int retval, msg, i;

    /* This function exports a lot of stuff, which results in a lot of
       logging if msg_level is at INFO or ALL. So we save the current value
       of msg_level and restore it later.  If you actually need to log this
       function's actions, change the second line below */
    msg = rtapi_get_msg_level();
    rtapi_set_msg_level(RTAPI_MSG_WARN);

    for (i=0; i<8; i++) {

	if (dir != 0)
	    retval=export_output_pin(outpinnum++, &(addr->port[num][i]) );
	else
	    retval=export_input_pin(inputpinnum++, &(addr->port[num][i]) );

	if (retval != 0) {
	    return retval;
	}
    }
    /* restore saved message level */
    rtapi_set_msg_level(msg);
    return 0;
}

static int export_input_pin(int pinnum, io_pin * pin)
{
    char buf[HAL_NAME_LEN + 2];
    int retval;

    /* export read only HAL pin for input data */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.in-%02d", pinnum);
    retval = hal_pin_bit_new(buf, HAL_WR, &(pin->data), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export additional pin for inverted input data */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.in-%02d-not", pinnum);
    retval = hal_pin_bit_new(buf, HAL_WR, &(pin->io.not), comp_id);
    /* initialize HAL pins */
    *(pin->data) = 0;
    *(pin->io.not) = 1;
    return retval;
}

static int export_output_pin(int pinnum, io_pin * pin)
{
    char buf[HAL_NAME_LEN + 2];
    int retval;

    /* export read only HAL pin for output data */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.out-%02d", pinnum);
    retval = hal_pin_bit_new(buf, HAL_RD, &(pin->data), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameter for polarity */
    rtapi_snprintf(buf, HAL_NAME_LEN, "stg.out-%02d-invert", pinnum);
    retval = hal_param_bit_new(buf, HAL_WR, &(pin->io.invert), comp_id);
    /* initialize HAL pin and param */
    *(pin->data) = 0;
    pin->io.invert = 0;
    return retval;
}


static int stg_init_card()
{
    if (base == 0x00) {
	base = stg_autodetect();
    }
    if (base == 0x00) {
	rtapi_print_msg(RTAPI_MSG_ERR, "STG: ERROR: no STG card found\n");
	return -1;
    }
    // FIXME - further init will happen here

    // all ok
    return 0;
}

/* scans possible addresses for STG cards */
unsigned short stg_autodetect()
{

    short i, j, k, ofs;
    unsigned short address;

    /* search all possible addresses */
    for (i = 15; i >= 0; i--) {
	address = i * 0x20 + 0x200;

	/* does jumper = i? */
	if ((inb(address + BRDTST) & 0x0f) == i) {
	    k = 0;		// var for getting the serial
	    for (j = 0; j < 8; j++) {
		ofs = (inb(address + BRDTST) >> 4);

		if (ofs & 8) {	/* is SER set? */
		    ofs = ofs & 7;	/* mask for Q2,Q1,Q0 */
		    k += (1 << ofs);	/* shift bit into position specified
					   by Q2, Q1, Q0 */
		}
	    }
	    if (k == 0x75)
		return address;	/* SER sequence is 01110101 */
	    if (k == 0x74) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		    "STG: ERROR: found version 2 card, not suported by this driver\n");
		hal_exit(comp_id);
		return -1;
	    }
	}
    }
    return (0);
}
