/*
 * EV3 Tacho Motor device driver
 *
 * Copyright (C) 2013-2014 Ralph Hempel <rhempel@hempeldesigngroup.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Note: The comment block below is used to generate docs on the ev3dev website.
 * Use kramdown (markdown) syntax. Use a '.' as a placeholder when blank lines
 * or leading whitespace is important for the markdown syntax.
 */

/**
 * DOC: website
 *
 * EV3/NXT Tacho Motor Driver
 *
 * This driver provides a [tacho-motor] interface for EV3 and NXT motors or any
 * other compatible motor with an [incremental rotary encoder] for position
 * and direction feedback that is connected to an output port. We call them
 * "tacho" motors because that is what the LMS2012 source code calls them. You
 * can find the devices bound to this driver in the directory
 * `/sys/bus/lego/drivers/ev3-tacho-motor /`. There is not much of interest
 * there though - all of the useful stuff is in the [tacho-motor] class.
 * .
 * [tacho-motor]: ../tacho-motor-class
 * [incremental rotary encoder]: https://en.wikipedia.org/wiki/Rotary_encoder#Incremental_rotary_encoder
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/random.h>
#include <linux/platform_data/legoev3.h>

#include <mach/time.h>

#include <lego.h>
#include <lego_port_class.h>
#include <dc_motor_class.h>
#include <tacho_motor_class.h>

#define TACHO_MOTOR_POLL_NS	2000000	/* 2 msec */

#define TACHO_SAMPLES		128

#define MAX_PWM_CNT		10000
#define MAX_SPEED		100
#define MAX_POWER		100
#define MAX_SYNC_MOTORS		2

enum ev3_tacho_motor_type {
	MOTOR_TYPE_0,
	MOTOR_TYPE_1,
	MOTOR_TYPE_2,
	MOTOR_TYPE_3,
	MOTOR_TYPE_4,
	MOTOR_TYPE_5,
	MOTOR_TYPE_6,
	MOTOR_TYPE_TACHO,
	MOTOR_TYPE_MINITACHO,
	MOTOR_TYPE_NEWTACHO,
	MOTOR_TYPE_10,
	MOTOR_TYPE_11,
	MOTOR_TYPE_12,
	MOTOR_TYPE_13,
	MOTOR_TYPE_14,
	MOTOR_TYPE_15,
	NO_OF_MOTOR_TYPES,
};

enum ev3_tacho_motor_samples {
	SAMPLES_PER_SPEED_BELOW_40,
	SAMPLES_PER_SPEED_ABOVE_40,
	SAMPLES_PER_SPEED_ABOVE_60,
	SAMPLES_PER_SPEED_ABOVE_80,
	NO_OF_SAMPLE_STEPS
};

enum ev3_tacho_motor_command {
	UNKNOWN,
	FORWARD,
	REVERSE,
	BRAKE,
	COAST,
};

struct ev3_tacho_motor_data {
	struct tacho_motor_device tm;
	struct lego_device *motor;

	struct hrtimer timer;
	struct work_struct notify_state_change_work;

	unsigned tacho_samples[TACHO_SAMPLES];
	unsigned tacho_samples_head;

	bool got_new_sample;

	int samples_per_speed;
	int dir_chg_samples;

	int counts_per_pulse;
	int pulses_per_second;

	/*
	 * TODO - The class mutex interlock is not implemented - should be up
	 * at device level to allow busy indication
	 */

	bool class_mutex;
	bool irq_mutex;

	struct {
		struct {
			int start;
			int end;
			int full;
		} up;

		struct {
			int start;
			int end;
			int full;
		} down;

		int percent;
		int direction;
		int position_sp;
		int count;	/* This must be set to either tacho or time increment! */
	} ramp;

	struct {
		int P;
		int I;
		int D;
		int speed_regulation_P;
		int speed_regulation_I;
		int speed_regulation_D;
		int speed_regulation_K;
		int prev_pulses_per_second;
		int prev_position_error;
	} pid;

	int speed_reg_sp;
	int run_direction;

	int run;
	int estop;

	int motor_type;

	int tacho;
	int irq_tacho;	/* tacho and irq_tacho combine to make position - change name to pulse? */

	int speed;
	int power;
	int state;

	long duty_cycle_sp;
	long pulses_per_second_sp;
	long time_sp;
	long position_sp;
	long ramp_up_sp;
	long ramp_down_sp;

	long run_mode;
	long regulation_mode;
	long stop_mode;
	long position_mode;
	enum dc_motor_polarity polarity_mode;
	enum dc_motor_polarity encoder_mode;
};

static const int SamplesPerSpeed[NO_OF_MOTOR_TYPES][NO_OF_SAMPLE_STEPS] = {
	{  2,  2,  2,  2 } , /* Motor Type  0             */
	{  2,  2,  2,  2 } , /* Motor Type  1             */
	{  2,  2,  2,  2 } , /* Motor Type  2             */
	{  2,  2,  2,  2 } , /* Motor Type  3             */
	{  2,  2,  2,  2 } , /* Motor Type  4             */
	{  2,  2,  2,  2 } , /* Motor Type  5             */
	{  2,  2,  2,  2 } , /* Motor Type  6             */
	{  4, 16, 32, 64 } , /* Motor Type  7 - TACHO     */
	{  2,  4,  8, 16 } , /* Motor Type  8 - MINITACHO */
	{  2,  2,  2,  2 } , /* Motor Type  9 - NEWTACHO  */
	{  2,  2,  2,  2 } , /* Motor Type 10             */
	{  2,  2,  2,  2 } , /* Motor Type 11             */
	{  2,  2,  2,  2 } , /* Motor Type 12             */
	{  2,  2,  2,  2 } , /* Motor Type 13             */
	{  2,  2,  2,  2 } , /* Motor Type 14             */
	{  2,  2,  2,  2 } , /* Motor Type 15             */
};

static const int CountsPerPulse[NO_OF_MOTOR_TYPES] = {
	      1, /* Motor Type  0             */
	      1, /* Motor Type  1             */
	      1, /* Motor Type  2             */
	      1, /* Motor Type  3             */
	      1, /* Motor Type  4             */
	      1, /* Motor Type  5             */
	      1, /* Motor Type  6             */
	3300000, /* Motor Type  7 - TACHO     */
	2062500, /* Motor Type  8 - MINITACHO */
	      1, /* Motor Type  9 - NEWTACHO  */
	      1, /* Motor Type 10             */
	      1, /* Motor Type 11             */
	      1, /* Motor Type 12             */
	      1, /* Motor Type 13             */
	      1, /* Motor Type 14             */
	      1, /* Motor Type 15             */
};

static const int MaxPulsesPerSec[NO_OF_MOTOR_TYPES] = {
	   1, /* Motor Type  0             */
	   1, /* Motor Type  1             */
	   1, /* Motor Type  2             */
	   1, /* Motor Type  3             */
	   1, /* Motor Type  4             */
	   1, /* Motor Type  5             */
	   1, /* Motor Type  6             */
	 900, /* Motor Type  7 - TACHO     */
	1200, /* Motor Type  8 - MINITACHO */
	   1, /* Motor Type  9 - NEWTACHO  */
	   1, /* Motor Type 10             */
	   1, /* Motor Type 11             */
	   1, /* Motor Type 12             */
	   1, /* Motor Type 13             */
	   1, /* Motor Type 14             */
	   1, /* Motor Type 15             */
};

static void set_samples_per_speed(struct ev3_tacho_motor_data *ev3_tm, int speed) {
	if (speed > 80) {
		ev3_tm->samples_per_speed = SamplesPerSpeed[ev3_tm->motor_type][SAMPLES_PER_SPEED_ABOVE_80];
	} else if (speed > 60) {
		ev3_tm->samples_per_speed = SamplesPerSpeed[ev3_tm->motor_type][SAMPLES_PER_SPEED_ABOVE_60];
	} else if (speed > 40) {
		ev3_tm->samples_per_speed = SamplesPerSpeed[ev3_tm->motor_type][SAMPLES_PER_SPEED_ABOVE_40];
	} else {
		ev3_tm->samples_per_speed = SamplesPerSpeed[ev3_tm->motor_type][SAMPLES_PER_SPEED_BELOW_40];
	}
}

/*
 * Handling the Tachometer Inputs
 *
 * The tacho motor driver uses two pins on each port to determine the direction
 * of rotation of the motor.
 *
 * `pdata->tacho_int_gpio` is the pin that is set up to trigger an interrupt
 * any edge change
 *
 * `pdata->tacho_dir_gpio` is the pin that helps to determine the direction
 * of rotation
 *
 * When int == dir then the encoder is turning in the forward direction
 * When int != dir then the encoder is turning in the reverse direction
 *
 * -----     --------           --------      -----
 *     |     |      |           |      |      |
 *     |     |      |           |      |      |
 *     -------      -------------       -------          DIRx signal
 *
 *  -------     --------     --------     --------       INTx signal
 *        |     |      |     |      |     |      |
 *        |     |      |     |      |     |      |
 *        -------      -------      -------      -----
 *        \     \      \     \      \     \      \
 *         ^     ^      ^     ^      ^     ^      ^      ISR handler
 *         +1    +1     +1    -1     -1    -1     -1     TachoCount
 *
 * All this works perfectly well when there are no missed interrupts, and when
 * the transitions on these pins are clean (no bounce or noise). It is possible
 * to get noisy operation when the transitions are very slow, and we have
 * observed signals similar to this:
 *
 * -------------                       -------------
 *             |                       |
 *             |                       |
 *             -------------------------                 DIRx signal
 *
 *    ---------------   ----                             INTx signal
 *    |             |   |  |
 *    |             |   |  |
 * ----             -----  -------------------------
 *    \              \   \  \
 *     ^              ^   ^  ^                           ISR Handler
 *     +1             +1  -1 +1                          TachoCount
 *                    A   B  C
 *
 * The example above has three transitions that we are interested in
 * labeled A, B, and C - they represent a noisy signal. As long as
 * all three transitions are caught by the ISR, then the count is
 * incremented by 2 as expected. But there are other outcomes possible.
 *
 * For example, if the A transition is handled, but the INT signal
 * is not measured until after B, then the final count value is 1.
 *
 * On the other hand, if the B transition is missed, and only A and
 * C are handled, then the final count value is 3.
 *
 * Either way, we need to figure out a way to clean things up, and as
 * long as at least two of the interrupts are caught, we can "undo"
 * a reading quite easily.
 *
 * The mini-tacho motor turns at a maximum of 1200 pulses per second, the
 * standard tacho motor has a maximum speed of 900 pulses per second. Taking
 * the highest value, this means that about 800 usec is the fastest time
 * between interrupts. If we see two interrupts with a delta of much less
 * than, say 400 usec, then we're probably looking at a noisy transition.
 *
 * In most cases that have been captured, the shortest delta is the A-B
 * transition, anywhere from 10 to 20 usec, which is faster than the ISR
 * response time. The B-C transition has been measured up to 150 usec.
 *
 * It is clear that the correct transition to use for changing the
 * value of `TachoCount` is C - so if the delta from A-C is less than
 * the threshold, we should "undo" whatever the A transition told us.
 */

static irqreturn_t tacho_motor_isr(int irq, void *id)
{
	struct ev3_tacho_motor_data *ev3_tm = id;
	struct ev3_motor_platform_data *pdata = ev3_tm->motor->dev.platform_data;

	bool int_state =  gpio_get_value(pdata->tacho_int_gpio);
	bool dir_state = !gpio_get_value(pdata->tacho_dir_gpio);

	unsigned long timer      = legoev3_hires_timer_read();
	unsigned long prev_timer = ev3_tm->tacho_samples[ev3_tm->tacho_samples_head];

	unsigned next_sample;

	int  next_direction = ev3_tm->run_direction;

	next_sample = (ev3_tm->tacho_samples_head + 1) % TACHO_SAMPLES;

	/* If the speed is high enough, just update the tacho counter based on direction */

	if ((35 < ev3_tm->speed) || (-35 > ev3_tm->speed)) {

		if (ev3_tm->dir_chg_samples < (TACHO_SAMPLES-1))
			ev3_tm->dir_chg_samples++;

	} else {

		/*
		 * Update the tacho count and motor direction for low speed, taking
		 * advantage of the fact that if state and dir match, then the motor
		 * is turning FORWARD!
		 *
		 * We also look after the polarity_mode and encoder_mode here as follows:
		 *
		 * polarity_mode | encoder_mode | next_direction
		 * --------------+--------------+---------------
		 * normal        | normal       | normal
		 * normal        | inverted     | inverted
		 * inverted      | normal       | inverted
		 * inverted      | inverted     | normal
		 *
		 * Yes, this could be compressed into a clever set of conditionals that
		 * results in only two assignments, or a lookup table, but it's clearer
		 * to write nested if statements in this case - it looks a lot more
		 * like the truth table
		 */

		if (ev3_tm->polarity_mode == DC_MOTOR_POLARITY_NORMAL) {
			if (ev3_tm->encoder_mode == DC_MOTOR_POLARITY_NORMAL) {
				next_direction = (int_state == dir_state) ? FORWARD : REVERSE;
			} else {
				next_direction = (int_state == dir_state) ? REVERSE : FORWARD;
			}
		} else {
			if (ev3_tm->encoder_mode == DC_MOTOR_POLARITY_NORMAL) {
				next_direction = (int_state == dir_state) ? REVERSE : FORWARD;
			} else {
				next_direction = (int_state == dir_state) ? FORWARD : REVERSE;
			}
		}

		/*
		 * If the difference in timestamps is too small, then undo the
		 * previous increment - it's OK for a count to waver once in
		 * a while - better than being wrong!
		 *
		 * Here's what we'll do when the transition is too small:
		 *
		 * 1) UNDO the increment to the next timer sample update
		 *    dir_chg_samples count!
		 * 2) UNDO the previous run_direction count update
		 *
		 * TODO: Make this work out to 400 usec based on the clock rate!
		 */

		if ((400 * 33) > (timer - prev_timer)) {
			ev3_tm->tacho_samples[ev3_tm->tacho_samples_head] = timer;

			if (FORWARD == ev3_tm->run_direction)
				ev3_tm->irq_tacho--;
			else
				ev3_tm->irq_tacho++;

			next_sample = ev3_tm->tacho_samples_head;
		} else {
			/*
			 * If the saved and next direction states
			 * match, then update the dir_chg_sample count
			 */

			if (ev3_tm->run_direction == next_direction) {
				if (ev3_tm->dir_chg_samples < (TACHO_SAMPLES-1))
					ev3_tm->dir_chg_samples++;
			} else {
				ev3_tm->dir_chg_samples = 0;
			}
		}
	}

	ev3_tm->run_direction = next_direction;

	/* Grab the next incremental sample timestamp */

	ev3_tm->tacho_samples[next_sample] = timer;
	ev3_tm->tacho_samples_head = next_sample;

	ev3_tm->irq_mutex = true;

	if (FORWARD == ev3_tm->run_direction)
		ev3_tm->irq_tacho++;
	else
		ev3_tm->irq_tacho--;

	ev3_tm->got_new_sample = true;

	ev3_tm->irq_mutex = false;

	return IRQ_HANDLED;
}

void ev3_tacho_motor_update_output(struct ev3_tacho_motor_data *ev3_tm)
{
	struct dc_motor_ops *motor_ops = ev3_tm->motor->port->motor_ops;
	void *context = ev3_tm->motor->port->context;
	int err;

	if (ev3_tm->power > 0) {
		motor_ops->set_direction(context, ev3_tm->polarity_mode);
		motor_ops->set_command(context, DC_MOTOR_COMMAND_RUN);
		if (ev3_tm->regulation_mode == TM_REGULATION_OFF && ev3_tm->power < 10)
			ev3_tm->power = 10;
	} else if (ev3_tm->power < 0) {
		motor_ops->set_direction(context, !ev3_tm->polarity_mode);
		motor_ops->set_command(context, DC_MOTOR_COMMAND_RUN);
		if (ev3_tm->regulation_mode == TM_REGULATION_OFF && ev3_tm->power > -10)
			ev3_tm->power = -10;
	} else {
		if (ev3_tm->stop_mode == TM_STOP_COAST)
			motor_ops->set_command(context, DC_MOTOR_COMMAND_COAST);
		else if (TM_STOP_BRAKE == ev3_tm->stop_mode)
			motor_ops->set_command(context, DC_MOTOR_COMMAND_BRAKE);
		else if (TM_STOP_HOLD == ev3_tm->stop_mode)
			motor_ops->set_command(context, DC_MOTOR_COMMAND_BRAKE);
	}

	/* The power sets the duty cycle - 100% power == 100% duty cycle */
	err = motor_ops->set_duty_cycle(context, abs(ev3_tm->power));
	WARN_ONCE(err, "Failed to set pwm duty cycle! (%d)\n", err);
}

static void ev3_tacho_motor_set_power(struct ev3_tacho_motor_data *ev3_tm, int power)
{
	if (ev3_tm->power == power)
		return;

	if (power > MAX_POWER)
		power = MAX_POWER;
	else if (power < -MAX_POWER)
		power = -MAX_POWER;

	ev3_tm->power = power;
	ev3_tacho_motor_update_output(ev3_tm);
}

static void ev3_tacho_motor_reset(struct ev3_tacho_motor_data *ev3_tm)
{
	struct ev3_motor_platform_data *pdata = ev3_tm->motor->dev.platform_data;

	/*
	 * This is the same as initializing a motor - we will set everything
	 * to default values, as if it had just been plugged in
	 */

	memset(ev3_tm->tacho_samples, 0, sizeof(unsigned) * TACHO_SAMPLES);

	ev3_tm->tacho_samples_head	= 0;
	ev3_tm->got_new_sample		= false;
	ev3_tm->samples_per_speed	= SamplesPerSpeed[MOTOR_TYPE_TACHO][SAMPLES_PER_SPEED_BELOW_40];
	ev3_tm->dir_chg_samples		= 0;
	ev3_tm->counts_per_pulse	= CountsPerPulse[MOTOR_TYPE_TACHO];
	ev3_tm->pulses_per_second	= 0;
	ev3_tm->class_mutex		= false;
	ev3_tm->irq_mutex		= false;
	ev3_tm->ramp.up.start		= 0;
	ev3_tm->ramp.up.end		= 0;
	ev3_tm->ramp.down.start		= 0;
	ev3_tm->ramp.down.end		= 0;
	ev3_tm->ramp.percent		= 0;
	ev3_tm->ramp.direction		= 0;
	ev3_tm->ramp.position_sp	= 0;
	ev3_tm->ramp.count		= 0;
	ev3_tm->pid.P			= 0;
	ev3_tm->pid.I			= 0;
	ev3_tm->pid.D			= 0;

	/* TODO: These need to get converted to an id lookup table like sensors */

	if (TACHO_MOTOR_EV3_MEDIUM == pdata->motor_type_id)
		ev3_tm->motor_type = MOTOR_TYPE_MINITACHO;
	else if (TACHO_MOTOR_EV3_LARGE == pdata->motor_type_id)
		ev3_tm->motor_type = MOTOR_TYPE_TACHO;
	else
		ev3_tm->motor_type = MOTOR_TYPE_TACHO;

	if (MOTOR_TYPE_MINITACHO == ev3_tm->motor_type) {
		ev3_tm->pid.speed_regulation_P  = 1000;
		ev3_tm->pid.speed_regulation_I  = 60;
		ev3_tm->pid.speed_regulation_D  = 0;
	} else if (TACHO_MOTOR_EV3_LARGE == pdata->motor_type_id) {
		ev3_tm->pid.speed_regulation_P  = 1000;
		ev3_tm->pid.speed_regulation_I  = 60;
		ev3_tm->pid.speed_regulation_D  = 0;
	} else {
		ev3_tm->pid.speed_regulation_P  = 1000;
		ev3_tm->pid.speed_regulation_I  = 60;
		ev3_tm->pid.speed_regulation_D  = 0;
	}

	ev3_tm->pid.speed_regulation_K	= 9000;

	ev3_tm->pid.prev_pulses_per_second = 0;

	ev3_tm->pid.prev_position_error	= 0;
	ev3_tm->speed_reg_sp		= 0;
	ev3_tm->run_direction		= UNKNOWN;
	ev3_tm->run			= 0;
	ev3_tm->estop			= 0;

	ev3_tm->tacho			= 0;
	ev3_tm->irq_tacho		= 0;
	ev3_tm->speed			= 0;
	ev3_tm->power			= 0;
	ev3_tm->state			= TM_STATE_IDLE;

	ev3_tm->duty_cycle_sp		= 0;
	ev3_tm->pulses_per_second_sp	= 0;
	ev3_tm->time_sp			= 0;
	ev3_tm->position_sp		= 0;
	ev3_tm->ramp_up_sp		= 0;
	ev3_tm->ramp_down_sp		= 0;

	ev3_tm->run_mode	= TM_RUN_FOREVER;
	ev3_tm->regulation_mode	= TM_REGULATION_OFF;
	ev3_tm->stop_mode	= TM_STOP_COAST;
	ev3_tm->position_mode	= TM_POSITION_ABSOLUTE;
	ev3_tm->polarity_mode	= DC_MOTOR_POLARITY_NORMAL;
	ev3_tm->encoder_mode	= DC_MOTOR_POLARITY_NORMAL;
};

/*
 * NOTE: The comments are from the original LEGO source code, but the code
 *       has been changed to reflect the per-motor data structures.
 *
 * NOTE: The original LEGO code used the 24 most significant bits of the
 *       free-running P3 timer by dividing the values captured at every
 *       interrupt edge by 256. Unfortunately, this results in having to
 *       mask off the 24 least significant bits in all subsequent calculations
 *       that involve the captured values.
 *
 *       The reason is probably to keep most of the calculations within the
 *       16 bit value size. This driver avoids that problem by simply scaling
 *       the results when needed. The code and comments are updated to
 *       reflect the change.
 *
 *  - Calculates the actual speed for a motor
 *
 *  - Returns true when a new speed has been calculated, false if otherwise
 *
 *  - Time is sampled every edge on the tacho
 *      - Timer used is 64bit timer plus (P3) module (dual 32bit un-chained mode)
 *      - 64bit timer is running 33Mhz (24Mhz (Osc) * 22 (Multiplier) / 2 (Post divider) / 2 (DIV2)) / 4 (T64 prescaler)
 *
 *  - Tacho counter is updated on every edge of the tacho INTx pin signal
 *  - Time capture is updated on every edge of the tacho INTx pin signal
 *
 *  - Speed is calculated from the following parameters
 *
 *      - Time is measured edge to edge of the tacho interrupt pin. Average of time is always minimum 2 pulses
 *        (1 high + 1 low period or 1 low + 1 high period) because the duty of the high and low period of the
 *        tacho pulses are not always 50%.
 *
 *        - Average of the large motor
 *          - Above speed 80 it is:          64 samples
 *          - Between speed 60 - 80 it is:   32 samples
 *          - Between speed 40 - 60 it is:   16 samples
 *          - below speed 40 it is:           4 samples
 *
 *        - Average of the medium motor
 *          - Above speed 80 it is:          16 samples
 *          - Between speed 60 - 80 it is:    8 samples
 *          - Between speed 40 - 60 it is:    4 samples
 *          - below speed 40 it is:           2 sample
 *
 *      - Number of samples is always determined based on 1 sample meaning 1 low period or 1 high period,
 *        this is to enable fast adoption to changes in speed. Medium motor has the critical timing because
 *        it can change speed and direction very fast.
 *
 *      - Large Motor
 *        - Maximum speed of the Large motor is approximately 2mS per tacho pulse (low + high period)
 *          resulting in minimum timer value of: 2mS / (1/(33MHz)) = 66000 T64 timer ticks.
 *          Because 1 sample is based on only half a period minimum speed is 66000/2 = 33000
 *        - Minimum speed of the large motor is a factor of 100 less than max. speed
 *          max. speed timer ticks * 100 => 66000 * 100 = 6,600,000 T64 timer ticks
 *          Because 1 sample is based on only half a period minimum speed is 6,600,000/2 = 3,300,000.
 *
 *      - Medium Motor
 *        - Maximum speed of the medium motor is approximately 1,25mS per tacho pulse (low + high period)
 *          resulting in minimum timer value og: 1,25mS / (1/(33MHz)) = 41250 (approximately)
 *          Because 1 sample is based on only half a period minimum speed is 41250/2 = 20625.
 *        - Minimum speed of the medium motor is a factor of 100 less than max. speed
 *          max. speed timer ticks * 100 => 41250 * 100 = 4,125,000 T64 timer ticks
 *          Because 1 sample is based on only half a period minimum speed is 4,125,000/2 = 2,062,500.
 *
 *      - Actual speed is then calculated as:
 *        - Large motor:
 *          3,300,000 * number of samples / actual time elapsed for number of samples
 *        - Medium motor:
 *          2,062,500 * number of samples / actual time elapsed for number of samples
 *
 *  - Parameters:
 *    - Input:
 *      - No        : Motor output number
 *      - *pSpeed   : Pointer to the speed value
 *
 *    - Output:
 *      - Status    : Indication of new speed available or not
 *  - Tacho pulse examples:
 *
 *
 *    - Normal
 *
 *      ----       ------       ------       ------
 *          |     |      |     |      |     |      |
 *          |     |      |     |      |     |      |
 *          -------      -------      -------      --- DIRx signal
 *
 *
 *         ----       ------       ------       ------ INTx signal
 *             |     |      |     |      |     |
 *             |     |      |     |      |     |
 *             -------      -------      -------
 *
 *             ^     ^      ^     ^      ^     ^
 *             |     |      |     |      |     |
 *             |   Timer    |   Timer    |   Timer
 *             |     +      |     +      |     +
 *             |  Counter   |  Counter   |  Counter
 *             |            |            |
 *           Timer        Timer        Timer
 *             +            +            +
 *          Counter      Counter      Counter
 *
 *
 *
 *    - Direction change
 *
 *      DirChgPtr variable is used to indicate how many timer samples have been sampled
 *      since direction has been changed. DirChgPtr is set to 0 when tacho interrupt detects
 *      direction change and then it is counted up for every timer sample. So when DirChgPtr
 *      has the value of 2 then there must be 2 timer samples in the the same direction
 *      available.
 *
 *      ----       ------       ------       ------       ---
 *          |     |      |     |      |     |      |     |
 *          |     |      |     |      |     |      |     |
 *          -------      -------      -------      -------   DIRx signal
 *
 *
 *       ------       -------------       ------       ------INTx signal
 *             |     |             |     |      |     |
 *             |     |             |     |      |     |
 *             -------             -------      -------
 *
 *             ^     ^             ^     ^      ^     ^
 *             |     |             |     |      |     |
 *           Timer   |           Timer   |    Timer   |
 *             +     |             +     |      +     |
 *          Counter  |          Counter  |   Counter  |
 *             +     |             +     |      +     |
 *       DirChgPtr++ |       DirChgPtr=0 |DirChgPtr++ |
 *                 Timer               Timer        Timer
 *                   +                   +            +
 *                Counter             Counter      Counter
 *                   +                   +            +
 *               DirChgPtr++         DirChgPtr++  DirChgPtr++
 *
 *
 *
 *
 *      ----       ------             ------        ----
 *          |     |      |           |      |      |
 *          |     |      |           |      |      |
 *          -------      -------------       -------          DIRx signal
 *
 *
 *       ------       ------       ------       ------        INTx signal
 *             |     |      |     |      |     |      |
 *             |     |      |     |      |     |      |
 *             -------      -------      -------       ----
 *
 *             ^     ^      ^     ^      ^     ^      ^
 *             |     |      |     |      |     |      |
 *           Timer   |    Timer   |    Timer   |    Timer
 *             +     |      +     |      +     |      +
 *          Counter  |   Counter  |   Counter  |   Counter
 *             +     |      +     |      +     |      +
 *        DirChgPtr++| DirChgPtr++| DirChgPtr++| DirChgPtr++
 *                 Timer        Timer        Timer
 *                   +            +            +
 *                Counter      Counter      Counter
 *                   +            +            +
 *               DirChgPtr++  DirChgPtr=0  DirChgPtr++
 *
 */

static bool calculate_speed(struct ev3_tacho_motor_data *ev3_tm)
{
	unsigned DiffIdx;
	unsigned Diff;

	bool speed_updated = false;

	/* TODO - Don't run this if we're updating the ev3_tm in the isr! */

	/*
	 * Determine the approximate speed of the motor using the difference
	 * in time between this tacho pulse and the previous pulse.
	 *
	 * The old code had a conditional that forced the difference to be at
	 * least 1 by checking for the special case of a zero difference, which
	 * would be almost impossible to achieve in practice.
	 *
	 * This version simply ORs a 1 into the LSB of the difference - now
	 * that we're using the full 32 bit free running counter, the impact
	 * on an actual speed calculation is insignificant, and it avoids the
	 * issue with simply adding 1 in the obscure case that the difference
	 * is 0xFFFFFFFF!
	 *
	 * Only do this estimated speed calculation if we've accumulated at
	 * least two tacho pulses where the motor is turning in the same
	 * direction!
	 */

	DiffIdx = ev3_tm->tacho_samples_head;

	/* TODO - This should really be a boolean value that gets set at the ISR level */
	/* TODO - Can/Should we change this to not set_samples_per_speed every time we're called? */

	if (ev3_tm->dir_chg_samples >= 1) {

		Diff = ev3_tm->tacho_samples[DiffIdx]
				- ev3_tm->tacho_samples[(DiffIdx + TACHO_SAMPLES - 1) % TACHO_SAMPLES];

		Diff |= 1;

		set_samples_per_speed(ev3_tm, ev3_tm->counts_per_pulse / Diff);
	}

	/*
	 * Now get a better estimate of the motor speed by using the total
	 * time used to accumulate the last n samples, where n is determined
	 * by the first approximation to the speed.
	 *
	 * The new speed can only be updated if we have accumulated at least
	 * as many samples as are required depending on the estimated speed
	 * of the motor.
	 *
	 * If the speed cannot be updated, then we need to check if the speed
	 * is 0!
	 */

	if (ev3_tm->got_new_sample && (ev3_tm->dir_chg_samples >= ev3_tm->samples_per_speed)) {

		Diff = ev3_tm->tacho_samples[DiffIdx]
				- ev3_tm->tacho_samples[(DiffIdx + TACHO_SAMPLES - ev3_tm->samples_per_speed) % TACHO_SAMPLES];

		Diff |= 1;

		/* TODO - This should be based on the low level clock rate */

		ev3_tm->pulses_per_second = (33000000 * ev3_tm->samples_per_speed) / Diff;

		if (ev3_tm->run_direction == REVERSE)
			ev3_tm->pulses_per_second  = -ev3_tm->pulses_per_second ;

		speed_updated = true;

		ev3_tm->got_new_sample = false;

	} else if (ev3_tm->counts_per_pulse < (legoev3_hires_timer_read() - ev3_tm->tacho_samples[DiffIdx])) {

		ev3_tm->dir_chg_samples = 0;

		ev3_tm->pulses_per_second = 0;

		/* TODO - This is where we can put in a calculation for a stalled motor! */

		speed_updated = true;
	}

	return(speed_updated);
}

static void regulate_speed(struct ev3_tacho_motor_data *ev3_tm)
{
	int power;
	int speed_error;

	/* Make sure speed_reg_setpoint is within a reasonable range */

	if (ev3_tm->speed_reg_sp > MaxPulsesPerSec[ev3_tm->motor_type]) {
		ev3_tm->speed_reg_sp = MaxPulsesPerSec[ev3_tm->motor_type];
	} else if (ev3_tm->speed_reg_sp < -MaxPulsesPerSec[ev3_tm->motor_type]) {
		ev3_tm->speed_reg_sp = -MaxPulsesPerSec[ev3_tm->motor_type];
	}

	speed_error = ev3_tm->speed_reg_sp - ev3_tm->pulses_per_second;

	/* TODO - Implement an attribute set for PID constants that adjusts based on speed */

	ev3_tm->pid.P = speed_error;

	/*
	 * The integral term can get quite large if the speed setpoint is higher than the
	 * maximum speed that the motor can get to. This can happen if the motor is heavily
	 * loaded or if the setpoint is high and the battery voltage is low.
	 *
	 * To avoid the problem of "integral windup", we stop adding to the integral
	 * term if its contribution alone would set the power level to 100%
	 *
	 * Earlier versions of this algorithm did not allow the pid.I component to change
	 * once it hit the 100% limit. This algorithm allows the change if the absolute
	 * value of the result is less than 100.
	 */

	ev3_tm->pid.I = ev3_tm->pid.I + speed_error;

	ev3_tm->pid.D = ev3_tm->pulses_per_second - ev3_tm->pid.prev_pulses_per_second;

	ev3_tm->pid.prev_pulses_per_second = ev3_tm->pulses_per_second;

	power = ( (ev3_tm->pid.P * ev3_tm->pid.speed_regulation_P)
		+ (ev3_tm->pid.I * ev3_tm->pid.speed_regulation_I)
		+ (ev3_tm->pid.D * ev3_tm->pid.speed_regulation_D) ) / ev3_tm->pid.speed_regulation_K;

	/*
	 * Subtract the speed error to avoid integral windup if the resulting power is
	 * more than 100%
	 */

	if (100 < abs(power))
		ev3_tm->pid.I = ev3_tm->pid.I - speed_error;

	/*
	 * When regulation_mode is on, and the user sets the
	 * pulses_per_second_sp to 0, the motor may have been
	 * running at a non-zero speed - which will make the
	 * motor oscillate to achieve the 0 speed. A check
	 * for the special condition of pulses_per_second_sp
	 * equal to 0 will turn off the motor to prevent the
	 * oscillation.
	 */

	if (0 == ev3_tm->speed_reg_sp) {
		ev3_tacho_motor_set_power(ev3_tm, 0    );
	} else {
		ev3_tacho_motor_set_power(ev3_tm, power);
	}
}

/*
 * This function changes either the actual power setting for the motor, or the speed
 * regulation setpoint, depending on whether the regulation_mode is on or off.
 *
 * Note that it is assumed by this function and all of its callers that
 * ev3_tacho_motor_set_power() checks whether there's an actual change
 * as well as limiting the range of input values.
 *
 * Similarly, the regulation function must verify the range of ev3_tm->speed_reg_setpoint
 * to avoid unreasonable values.
 *
 * By pushing the checks further down the line, we simplify the higher levels
 * of code!
 */

static void update_motor_speed_or_power(struct ev3_tacho_motor_data *ev3_tm, int percent)
{
	if (TM_REGULATION_OFF == ev3_tm->regulation_mode) {

		if ((TM_RUN_POSITION == ev3_tm->run_mode))
			ev3_tacho_motor_set_power(ev3_tm, ev3_tm->ramp.direction * abs((ev3_tm->duty_cycle_sp * percent)/100));
		else
			ev3_tacho_motor_set_power(ev3_tm,                             ((ev3_tm->duty_cycle_sp * percent)/100));

	} else if (TM_REGULATION_ON == ev3_tm->regulation_mode) {

		if ((TM_RUN_POSITION == ev3_tm->run_mode))
			ev3_tm->speed_reg_sp = ev3_tm->ramp.direction * abs((ev3_tm->pulses_per_second_sp * percent)/100);
		else
			ev3_tm->speed_reg_sp =                             ((ev3_tm->pulses_per_second_sp * percent)/100);
	}
}

static void regulate_position(struct ev3_tacho_motor_data *ev3_tm)
{
	int power;
	int position_error;


	/*
	 * Make sure that the irq_tacho value has been set to a value that represents the
	 * current error from the desired position so we can drive the motor towards
	 * the desired position hold point.
	 */

	position_error = 0 - ev3_tm->irq_tacho;

	if (MOTOR_TYPE_MINITACHO == ev3_tm->motor_type) {
		ev3_tm->pid.P = position_error * 400;
		ev3_tm->pid.I = ((ev3_tm->pid.I * 99)/100) + (position_error / 1);
		ev3_tm->pid.D = (((position_error - ev3_tm->pid.prev_position_error) * 4)/2) *  2;

	} else if (MOTOR_TYPE_TACHO == ev3_tm->motor_type) {
		ev3_tm->pid.P = position_error * 400;
		ev3_tm->pid.I = ((ev3_tm->pid.I * 99)/100) + (position_error / 1);
		ev3_tm->pid.D = (((position_error - ev3_tm->pid.prev_position_error) * 4)/2) *  2;

	} else {
		/* This space intentionally left blank! */
	}

	ev3_tm->pid.prev_position_error = position_error;

	power = ((ev3_tm->pid.P + ev3_tm->pid.I + ev3_tm->pid.D) / 100);

	ev3_tacho_motor_set_power(ev3_tm, power);
}

static void adjust_ramp_for_position(struct ev3_tacho_motor_data *ev3_tm)
{
	long ramp_down_time = 0;
	long ramp_down_distance;

	/*
	 * The ramp down time is based on the current power level when regulation is off, and
	 * on the current speed when regulation is on - don't forget, we're not always at the
	 * end of the up ramp by the time we need to ramp down!
	 */

	if (TM_REGULATION_OFF == ev3_tm->regulation_mode) {
		ramp_down_time  = abs((ev3_tm->ramp_down_sp * ev3_tm->power) / 100);
	} else if (TM_REGULATION_ON == ev3_tm->regulation_mode) {
		ramp_down_time  = abs((ev3_tm->ramp_down_sp * ev3_tm->pulses_per_second) / MaxPulsesPerSec[ev3_tm->motor_type]);
	}

	/*
	 * The adjustment for ramp distance is to take into account that we'll have trouble hitting
	 * the position setpoint at low speeds...shorten the distance!
	 */

	ramp_down_distance = abs((ev3_tm->pulses_per_second * ramp_down_time * 7) / (2000 * 10));

	/*
	 * Depending on the direction we are turning, figure out if we're going to overshoot
	 * the target position based on current speed. Note the calculation of ramp.down.end
	 * is relative to the current ramp.count, and that the ramp.down.start is recalculated
	 * backwards from the end so that the setpoint percentages work out properly!
	 *
	 * Remember, the timer callback function increments ramp.count by 2, so ramp.count
	 * always represents milliseconds!
	 */

	if (ev3_tm->ramp.direction > 0) {

		if ((ev3_tm->ramp.position_sp - ramp_down_distance) <= (ev3_tm->tacho + ev3_tm->irq_tacho)) {
			ev3_tm->ramp.up.end     = ev3_tm->ramp.count;
			ev3_tm->ramp.down.end   = ev3_tm->ramp.count + ramp_down_time;
			ev3_tm->ramp.down.start = ev3_tm->ramp.down.end - ev3_tm->ramp_down_sp;
		}

	} else {

		if ((ev3_tm->ramp.position_sp + ramp_down_distance) >= (ev3_tm->tacho + ev3_tm->irq_tacho)) {
			ev3_tm->ramp.up.end     = ev3_tm->ramp.count;
			ev3_tm->ramp.down.end   = ev3_tm->ramp.count + ramp_down_time;
			ev3_tm->ramp.down.start = ev3_tm->ramp.down.end - ev3_tm->ramp_down_sp;
		}
	}
}

/*
 * This function plays a key part in simplifying the calculation of ramp
 * progress in the code, and handles a number of special cases that can
 * cause odd behaviour.
 *
 * The strangest behaviour is when the numerator is two less than the
 * denominator - for cases where the denominator is small, this results in
 * very weird results for the speed, often many percent below the target
 * speed. ie 2/3 = 66 % , and the next iteration of the timer callback
 * adds 2 to the numerator so the ramp never gets re-evaluated!
 *
 * 1) If the denominator is less than or equal to the numerator, return 100
 * 2) If the denominator is 0
 * 3) If the denominator is two greater than the numerator, return 100
 */

static int calculate_ramp_progress(int numerator, int denominator)
{
	if (denominator <= (numerator + 2))
		return 100;
	else if (0 == denominator)
		return 100;
	else
		return((numerator * 100) / denominator);
}


static enum hrtimer_restart ev3_tacho_motor_timer_callback(struct hrtimer *timer)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(timer, struct ev3_tacho_motor_data, timer);
	struct dc_motor_ops *motor_ops = ev3_tm->motor->port->motor_ops;
	void *context = ev3_tm->motor->port->context;
	int speed;
	bool reprocess = true;

	hrtimer_forward_now(timer, ktime_set(0, TACHO_MOTOR_POLL_NS));

	/* Continue with the actual calculations */

	speed = calculate_speed(ev3_tm);

	if (0 == ev3_tm->run)
		goto no_run;

	/*
	 * Update the ramp counter if we're in any of the ramp modes - the
	 * ramp counter always reflects milliseconds! Much cleaner this way.
	 *
	 * This has to be done outside of the main state processing
	 * loop, otherwise we can end up updating the counter multiple
	 * times.
	 */

	switch (ev3_tm->state) {
	case TM_STATE_RAMP_UP:
	case TM_STATE_RAMP_CONST:
	case TM_STATE_POSITION_RAMP_DOWN:
	case TM_STATE_RAMP_DOWN:
		ev3_tm->ramp.count = ev3_tm->ramp.count + 2;
		break;
	default:
		break;
	}

	while (reprocess) {

		/*
		 * Some cases (such as RAMP_XXX) may change the state of the
		 * handler and require reprocessing. If so, they must set the
		 * reprocess flag to force an extra evaluation
		 */

		reprocess = 0;

		switch (ev3_tm->state) {

		case TM_STATE_RUN_FOREVER:

			/*
			 * Just fall through to set the ramp time. If ramp times are zero
			 * then start/stop is immediate!
			 */

		case TM_STATE_SETUP_RAMP_TIME:

			ev3_tm->ramp.up.start = 0;
			ev3_tm->ramp.down.end = ev3_tm->time_sp;

			/* In RUN_FOREVER mode, set the endpoint a long way out - an hour of milliseconds! */

			if ((TM_RUN_FOREVER == ev3_tm->run_mode))
				ev3_tm->ramp.down.end = 60*60*1000;

			/*
			 * First, we calculate ramp.up.full and ramp.down.full which are the point at which
			 * the ramp hits 100% of the setpoint - not the maximum theoretical speed or
			 * duty cycle.
			 *
			 * Why do we need this helper variable? It's because we also need to calculate the
			 * percentage completion of the ramp later on - and we must always maintain the
			 * rule that passing 100% to the update_motor_speed_or_power() function sets the
			 * speed or power to 100% of the setpoint, not the theoretical max.
			 */

			if (TM_REGULATION_OFF == ev3_tm->regulation_mode) {
				ev3_tm->ramp.up.full    = ((abs(ev3_tm->duty_cycle_sp) * ev3_tm->ramp_up_sp  ) / 100);
				ev3_tm->ramp.down.full  = ((abs(ev3_tm->duty_cycle_sp) * ev3_tm->ramp_down_sp) / 100);
				ev3_tm->ramp.direction  = (ev3_tm->duty_cycle_sp >= 0 ? 1 : -1);

			} else if (TM_REGULATION_ON == ev3_tm->regulation_mode) {
				ev3_tm->ramp.up.full    = ((abs(ev3_tm->pulses_per_second_sp) * ev3_tm->ramp_up_sp  ) / MaxPulsesPerSec[ev3_tm->motor_type]);
				ev3_tm->ramp.down.full  = ((abs(ev3_tm->pulses_per_second_sp) * ev3_tm->ramp_down_sp) / MaxPulsesPerSec[ev3_tm->motor_type]);
				ev3_tm->ramp.direction  = (ev3_tm->pulses_per_second_sp >= 0 ? 1 : -1);
			}

			/*
			 * Now set the ramp.up and ramp.down start and end fields based on the values
			 * we just calculated for full in the previous step. We'll check for overlaps later
			 */

			ev3_tm->ramp.up.end     = ev3_tm->ramp.up.start + ev3_tm->ramp.up.full;
			ev3_tm->ramp.down.start = ev3_tm->ramp.down.end - ev3_tm->ramp.down.full;

			/*
			 * Now figure out if ramp.up.end is past ramp.down.start
			 * and adjust if needed using the intersection of the
			 * ramp up line and ramp down line.
			 *
			 * Basic high-school algebra and knowing ramp.up.end must
			 * equal ramp.down.start, and that the ramp.setpoint is
			 * reduced in proportion to how far the intersection is
			 * from the original end point gives us:
			 */

			if (ev3_tm->ramp.up.end > ev3_tm->ramp.down.start) {
				ev3_tm->ramp.up.end     = ((ev3_tm->time_sp * ev3_tm->ramp_up_sp)/(ev3_tm->ramp_up_sp + ev3_tm->ramp_down_sp));
				ev3_tm->ramp.down.start = ev3_tm->ramp.up.end;
			}

			ev3_tm->state = TM_STATE_SETUP_RAMP_REGULATION;
			reprocess = true;
			break;

		case TM_STATE_SETUP_RAMP_POSITION:

			/*
			 * The position setups are a bit "interesting". We'll want
			 * use the same time based ramping mechanism, but we also
			 * need to take into account position.
			 *
			 * Since the ramp is a linear increase in velocity up to
			 * a setpoint, the position is the "area under the curve"
			 * which happens to be a triangle. The distance covered in
			 * the initial ramp up is 1/2(V*T) where V is measured in
			 * ticks per second and T is measured in milliseconds)
			 *
			 * It's easiest if we simply allow the speed to ramp up
			 * normally up to the speed setpoint and continuously
			 * estimate the ramp down start and end points based on
			 * the current speed. We have a nice attribute called
			 * pulses_per_second and that value is calculated every time
			 * the speed is actually updated, about 500 times a
			 * second.
			 *
			 * Given the current speed and the ramp_down attribute, and
			 * assuming a linear ramp down from the current speed, we
			 * can estimate the time it will take to ramp down as:
			 *
			 * ramp_time = ((pulses_per_sec * ramp_down) / MaxPulsesPerSec[ev3_tm->motor_type] ) msec
			 *
			 * The actual speed in pulses_per_sec can then be used
			 * to estimate how far the motor will travel in that
			 * time as:
			 *
			 * ramp_distance = (( pulses_per_sec * ramp_time ) / (1000)) pulses
			 *
			 * Now it's a simple matter to figure out if we're within
			 * distance pulses of the desired endpoint, and then
			 * we can fill in the ramp_down values. The trick is that we
			 * must constantly update the estimate of the ramp_down
			 * start and endpoints, so it's best to do that before the
			 * state handlers!
			 */

			if (TM_POSITION_ABSOLUTE == ev3_tm->position_mode)
				ev3_tm->ramp.position_sp = ev3_tm->position_sp;
			else
				ev3_tm->ramp.position_sp = ev3_tm->ramp.position_sp + ev3_tm->position_sp;

			/* TODO - These get recalculated in SETUP_RAMP_REGULATION - but it's OK */

			ev3_tm->ramp.direction = ((ev3_tm->ramp.position_sp >= (ev3_tm->tacho + ev3_tm->irq_tacho)) ? 1 : -1);

			ev3_tm->ramp.up.start = 0;

			/*
			 * The ramp transition point calculations depend on whether
			 * regulation is on or not
			 */
			if (TM_REGULATION_OFF == ev3_tm->regulation_mode) {
				ev3_tm->ramp.up.full    = ((abs(ev3_tm->duty_cycle_sp) * ev3_tm->ramp_up_sp  ) / 100);
				ev3_tm->ramp.down.full  = ((abs(ev3_tm->duty_cycle_sp) * ev3_tm->ramp_down_sp) / 100);
			} else if (TM_REGULATION_ON == ev3_tm->regulation_mode) {
				ev3_tm->ramp.up.full    = ((abs(ev3_tm->pulses_per_second_sp) * ev3_tm->ramp_up_sp  ) / MaxPulsesPerSec[ev3_tm->motor_type]);
				ev3_tm->ramp.down.full  = ((abs(ev3_tm->pulses_per_second_sp) * ev3_tm->ramp_down_sp) / MaxPulsesPerSec[ev3_tm->motor_type]);
			}

			/*
			 * Now set the ramp.up and ramp.down start and end fields based on the values
			 * we just calculated for full in the previous step. We'll check for overlaps later
			 */

			ev3_tm->ramp.up.end     = ev3_tm->ramp.up.start + ev3_tm->ramp.up.full;

//			if (TM_REGULATION_OFF == ev3_tm->regulation_mode) {
//				ev3_tm->ramp.up.end = ev3_tm->ramp.up.start + ((abs(ev3_tm->duty_cycle_sp) * ev3_tm->ramp_up_sp) / 100);
//			} else if (TM_REGULATION_ON == ev3_tm->regulation_mode) {
//				ev3_tm->ramp.up.end = ev3_tm->ramp.up.start + ((abs(ev3_tm->pulses_per_second_sp) * ev3_tm->ramp_up_sp) / MaxPulsesPerSec[ev3_tm->motor_type]);
//			}

			/* TODO - Can this get handled in RAMP_CONST? */

			ev3_tm->ramp.down.end   =  60*60*1000;
			ev3_tm->ramp.down.start =  60*60*1000;

			ev3_tm->state = TM_STATE_SETUP_RAMP_REGULATION;
			reprocess = true;
			break;

		case TM_STATE_SETUP_RAMP_REGULATION:
			ev3_tm->ramp.count    = 0;

			ev3_tm->state = TM_STATE_RAMP_UP;
			reprocess = true;
			break;

		/*
		 * The LIMITED_XXX functions have to handle the three phases (any of
		 * which are optional) of a motor move operation. It is assumed that
		 * when the run mode was set, the ramp factors were calculated.
		 *
		 * The LIMITED_XXX functions need to handle the following combinations:
		 *
		 * REGULATED_TIME    - Speed is regulated, ramping is time based
		 * REGULATED_TACHO   - Speed is regulated, ramping is tacho based
		 * UNREGULATED_TIME  - Speed is not regulated, ramping is time based
		 * UNREGULATED_TACHO - Speed is not regulated, ramping is tacho based
		 *
		 * When ramping, the code needs to figure out which combination is in
		 * use, and that's handled by a couple of booleans in the motor struct.
		 *
		 * Regardless of the direction of the ramp (up or down), the first part
		 * of the sequence is ramping up, the tail end of the sequence is
		 * ramping down.
		 */

		case TM_STATE_RAMP_UP:
			/*
			 * Figure out if we're done ramping up - if yes set state to RAMP_CONST
			 * and allow states to get reprocessed
			 */
			if (ev3_tm->run_mode == TM_RUN_POSITION) {
				adjust_ramp_for_position(ev3_tm);
			}

			if (ev3_tm->ramp.count >= ev3_tm->ramp.up.end) {
				ev3_tm->state = TM_STATE_RAMP_CONST;
				reprocess = true;
			}

			/* Figure out how far along we are in the ramp operation */

			ev3_tm->ramp.percent = calculate_ramp_progress( ev3_tm->ramp.count, ev3_tm->ramp.up.full);

			update_motor_speed_or_power(ev3_tm, ev3_tm->ramp.percent);
			break;

		case TM_STATE_RAMP_CONST:
			/*
			 * Figure out if we're done with the const section - if yes set state to RAMP_DOWN
			 * and allow states to get reprocessed.
			 *
			 *  Just push out the end point if we're in TM_RUN_FOREVER mode
			 */

			if (ev3_tm->run_mode == TM_RUN_FOREVER) {
				ev3_tm->ramp.down.start = ev3_tm->ramp.count;
				ev3_tm->ramp.down.end   = ev3_tm->ramp.count + ((abs(ev3_tm->duty_cycle_sp) * ev3_tm->ramp_down_sp) / 100);
			}

			/*
			 * Just push out the end point if we're in TM_RUN_TIME mode, and
			 * then check to see if we should start ramping down
			 */

			else if (ev3_tm->run_mode == TM_RUN_TIME) {
				if (ev3_tm->ramp.count >= ev3_tm->ramp.down.start) {
					ev3_tm->state = TM_STATE_RAMP_DOWN;
					reprocess = true;
				}
			}

			/*
			 * In TM_RUN_POSITION mode, estimate where the end point would
			 * be, and ramp down if we're past it.
			 */

			else if (ev3_tm->run_mode == TM_RUN_POSITION) {
				adjust_ramp_for_position(ev3_tm);

				if (ev3_tm->ramp.count >= ev3_tm->ramp.down.start) {
					ev3_tm->state = TM_STATE_POSITION_RAMP_DOWN;
					reprocess = true;
				}
			}

			/*
			 * This has to be here or else changing the pulses_per_second_sp
			 * or the duty_cycle_sp when the motor is running won't work...
			 */

			update_motor_speed_or_power(ev3_tm, ev3_tm->ramp.percent);

			break;

		case TM_STATE_POSITION_RAMP_DOWN:
			/* TODO - Maybe incorporate this into the adjust_ramp_for_position() function */

			if (ev3_tm->ramp.direction > 0) {

				if (ev3_tm->ramp.position_sp <= (ev3_tm->tacho + ev3_tm->irq_tacho /*+ (ev3_tm->power/4)*/)) {
					ev3_tm->ramp.down.end = ev3_tm->ramp.count;
				} else if (ev3_tm->ramp.down.end <= ev3_tm->ramp.count) {
					/* TODO - Increase ramp endpoint to nudge the ramp setpoint higher */
					ev3_tm->ramp.down.end = ev3_tm->ramp.count + 100;
				}

			} else {

				if (ev3_tm->ramp.position_sp >= (ev3_tm->tacho + ev3_tm->irq_tacho /*+ (ev3_tm->power/4)*/)) {
					ev3_tm->ramp.down.end = ev3_tm->ramp.count;
				} else if (ev3_tm->ramp.down.end <= ev3_tm->ramp.count) {
					/* TODO - Increase ramp endpoint to nudge the ramp setpoint higher */
					ev3_tm->ramp.down.end = ev3_tm->ramp.count + 100;
				}
			}

			ev3_tm->ramp.down.start = ev3_tm->ramp.down.end - ev3_tm->ramp_down_sp;

			/*
			 * NOTE: Intentional fallthrough to the TM_STATE_RAMP_DOWN case
			 *
			 * The TM_STATE_POSTION_RAMP_DOWN is busy recalculating the
			 * end point based on the current motor speed, so we can use
			 * the code in TM_STATE_RAMP_DOWN to stop for us!
			 */

		case TM_STATE_RAMP_DOWN:
			/*
			 * Figure out if we're done ramping down - if yes then
			 * decide whether to brake, coast, or leave the motor
			 * unchanged, and allow states to get reprocessed
			 */

			if (ev3_tm->ramp.count >= ev3_tm->ramp.down.end) {
				ev3_tm->state = TM_STATE_STOP;
				reprocess = true;

			}
			/* Figure out how far along we are in the ramp operation */

			ev3_tm->ramp.percent = calculate_ramp_progress( (ev3_tm->ramp.down.end - ev3_tm->ramp.count), ev3_tm->ramp.down.full );

			update_motor_speed_or_power(ev3_tm, ev3_tm->ramp.percent);
			break;

		case TM_STATE_STOP:
			/*
			 * Add in the irq_tacho for the current move so that we can use
			 * the value of irq_tacho in the HOLD mode - the current, real
			 * tacho reading is ALWAYS tacho + irq_tacho!
			 */

			if (ev3_tm->run_mode == TM_RUN_POSITION) {
				ev3_tm->irq_tacho  = (ev3_tm->tacho + ev3_tm->irq_tacho) - ev3_tm->ramp.position_sp;
				ev3_tm->tacho      = ev3_tm->ramp.position_sp;
			} else {
				ev3_tm->tacho      = ev3_tm->tacho + ev3_tm->irq_tacho;
				ev3_tm->irq_tacho  = 0;
			}

			ev3_tm->speed_reg_sp = 0;
			ev3_tacho_motor_set_power(ev3_tm, 0);

			/*
			 * Reset the PID terms here to avoid having these terms influence the motor
			 * operation at the beginning of the next sequence. The most common issue is
			 * having some residual integral value briefly turn the motor on hard if
			 * we're ramping up slowly
			 */

			ev3_tm->pid.P = 0;
			ev3_tm->pid.I = 0;
			ev3_tm->pid.D = 0;

			reprocess     = true;
			ev3_tm->state = TM_STATE_IDLE;
			break;

		case TM_STATE_IDLE:
			ev3_tm->run = 0;
			schedule_work(&ev3_tm->notify_state_change_work);
			break;
		default:
			/* Intentionally left empty */
			break;
		}
	}

	if (ev3_tm->run && (TM_REGULATION_ON == ev3_tm->regulation_mode))
		regulate_speed(ev3_tm);

no_run:
	/*
	 * Note, we get here even if we're running - so we need to check
	 * explicitly. These are some special cases to handle changes in the
	 * brake_mode when the motor is not running!
	 */

	if (!ev3_tm->run) {
		if (TM_STOP_COAST == ev3_tm->stop_mode)
			motor_ops->set_command(context, DC_MOTOR_COMMAND_COAST);

		else if (TM_STOP_BRAKE == ev3_tm->stop_mode)
			motor_ops->set_command(context, DC_MOTOR_COMMAND_BRAKE);

		else if (TM_STOP_HOLD == ev3_tm->stop_mode)
			regulate_position(ev3_tm);
	}

	return HRTIMER_RESTART;
}

static void ev3_tacho_motor_notify_state_change_work(struct work_struct *work)
{
	struct ev3_tacho_motor_data *ev3_tm =
		container_of(work, struct ev3_tacho_motor_data, notify_state_change_work);

	tacho_motor_notify_state_change(&ev3_tm->tm);
}

static int ev3_tacho_motor_get_type(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	if (ev3_tm->motor_type == MOTOR_TYPE_MINITACHO)
		return TM_TYPE_MINITACHO;
	else if (ev3_tm->motor_type == MOTOR_TYPE_TACHO)
		return TM_TYPE_TACHO;
	else
		return TM_TYPE_TACHO;
}

static void ev3_tacho_motor_set_type(struct tacho_motor_device *tm, long type)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	if (type == TM_TYPE_MINITACHO)
		ev3_tm->motor_type = MOTOR_TYPE_MINITACHO;
	else if (type == TM_TYPE_TACHO)
		ev3_tm->motor_type = MOTOR_TYPE_TACHO;
	else
		ev3_tm->motor_type = MOTOR_TYPE_TACHO;
}

static int ev3_tacho_motor_get_position(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->tacho + ev3_tm->irq_tacho;
}

static void ev3_tacho_motor_set_position(struct tacho_motor_device *tm, long position)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->irq_tacho	 = 0;
	ev3_tm->tacho		 = position;
	ev3_tm->ramp.position_sp = position;
}

static int ev3_tacho_motor_get_duty_cycle(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->power;
}

static int ev3_tacho_motor_get_state(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->state;
}

static int ev3_tacho_motor_get_pulses_per_second(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->pulses_per_second;
}

static int ev3_tacho_motor_get_duty_cycle_sp(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->duty_cycle_sp;
}

static void ev3_tacho_motor_set_duty_cycle_sp(struct tacho_motor_device *tm, long duty_cycle_sp)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->duty_cycle_sp = duty_cycle_sp;
}

static int ev3_tacho_motor_get_pulses_per_second_sp(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->pulses_per_second_sp;
}

static void ev3_tacho_motor_set_pulses_per_second_sp(struct tacho_motor_device *tm, long pulses_per_second_sp)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->pulses_per_second_sp = pulses_per_second_sp;
}

static int ev3_tacho_motor_get_time_sp(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->time_sp;
}

static void ev3_tacho_motor_set_time_sp(struct tacho_motor_device *tm, long time_sp)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->time_sp = time_sp;
}

static int ev3_tacho_motor_get_position_sp(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->position_sp;
}

static void ev3_tacho_motor_set_position_sp(struct tacho_motor_device *tm, long position_sp)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->position_sp = position_sp;
}

static int ev3_tacho_motor_get_regulation_mode(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->regulation_mode;
}

static void ev3_tacho_motor_set_regulation_mode(struct tacho_motor_device *tm, long regulation_mode)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->regulation_mode = regulation_mode;
}

static int ev3_tacho_motor_get_position_mode(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->position_mode;
}

static void ev3_tacho_motor_set_position_mode(struct tacho_motor_device *tm, long position_mode)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->position_mode = position_mode;
}

static int ev3_tacho_motor_get_stop_mode(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->stop_mode;
}

static void ev3_tacho_motor_set_stop_mode(struct tacho_motor_device *tm, long stop_mode)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->stop_mode = stop_mode;
}

static int ev3_tacho_motor_get_polarity_mode(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->polarity_mode;
}

static void ev3_tacho_motor_set_polarity_mode(struct tacho_motor_device *tm,
						long polarity_mode)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->polarity_mode = polarity_mode;
	ev3_tacho_motor_update_output(ev3_tm);
}

static int ev3_tacho_motor_get_encoder_mode(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->encoder_mode;
}

static void ev3_tacho_motor_set_encoder_mode(struct tacho_motor_device *tm,
						long encoder_mode)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->encoder_mode = encoder_mode;
}

static int ev3_tacho_motor_get_ramp_up_sp(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->ramp_up_sp;
}

static void ev3_tacho_motor_set_ramp_up_sp(struct tacho_motor_device *tm, long ramp_up_sp)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->ramp_up_sp = ramp_up_sp;
}

static int ev3_tacho_motor_get_ramp_down_sp(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->ramp_down_sp;
}

static void ev3_tacho_motor_set_ramp_down_sp(struct tacho_motor_device *tm, long ramp_down_sp)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->ramp_down_sp = ramp_down_sp;
}

static int ev3_tacho_motor_get_speed_regulation_P(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->pid.speed_regulation_P;
}

static void ev3_tacho_motor_set_speed_regulation_P(struct tacho_motor_device *tm, long speed_regulation_P)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->pid.speed_regulation_P = speed_regulation_P;
}

static int ev3_tacho_motor_get_speed_regulation_I(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->pid.speed_regulation_I;
}

static void ev3_tacho_motor_set_speed_regulation_I(struct tacho_motor_device *tm, long speed_regulation_I)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->pid.speed_regulation_I = speed_regulation_I;
}

static int ev3_tacho_motor_get_speed_regulation_D(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->pid.speed_regulation_D;
}

static void ev3_tacho_motor_set_speed_regulation_D(struct tacho_motor_device *tm, long speed_regulation_D)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->pid.speed_regulation_D = speed_regulation_D;
}

static int ev3_tacho_motor_get_speed_regulation_K(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->pid.speed_regulation_K;
}

static void ev3_tacho_motor_set_speed_regulation_K(struct tacho_motor_device *tm, long speed_regulation_K)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->pid.speed_regulation_K = speed_regulation_K;
}

static int ev3_tacho_motor_get_run_mode(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->run_mode;
}

static void ev3_tacho_motor_set_run_mode(struct tacho_motor_device *tm, long run_mode)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tm->run_mode = run_mode;
}

static int ev3_tacho_motor_get_run(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->run;
}

static void ev3_tacho_motor_set_run(struct tacho_motor_device *tm, long run)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	/* Safety First! If the estop is set, then unconditionally STOP */

	if (0 != ev3_tm->estop) {
		ev3_tm->state = TM_STATE_STOP;
	}

	/*
	 * If the motor is currently running and we're asked to stop
	 * it, then figure out how we're going to stop it - maybe we
	 * need to ramp it down first!
	 */

	else if ((0 == run) && (ev3_tm->state != TM_STATE_IDLE)) {
		ev3_tm->ramp.down.start = ev3_tm->ramp.count;
		ev3_tm->ramp.down.end   = ev3_tm->ramp_down_sp;

		if (TM_RUN_FOREVER == ev3_tm->run_mode)
			ev3_tm->state = TM_STATE_RAMP_DOWN;

		else if (TM_RUN_TIME == ev3_tm->run_mode)
			ev3_tm->state = TM_STATE_RAMP_DOWN;

		else if (TM_RUN_POSITION == ev3_tm->run_mode)
			ev3_tm->state = TM_STATE_STOP;
	}

	/*
	 * If the motor is currently idle and we're asked to run
	 * it, then figure out how we're going to get things started
	 */

	else if ((0 != run) && (ev3_tm->state == TM_STATE_IDLE)) {
		if (TM_RUN_FOREVER == ev3_tm->run_mode)
			ev3_tm->state = TM_STATE_RUN_FOREVER;

		else if (TM_RUN_TIME == ev3_tm->run_mode)
			ev3_tm->state = TM_STATE_SETUP_RAMP_TIME;

		else if (TM_RUN_POSITION == ev3_tm->run_mode)
			ev3_tm->state = TM_STATE_SETUP_RAMP_POSITION;
	}

	/* Otherwise, put the motor in STOP state - it will eventually stop */

	else if (0 == run) {
		ev3_tm->state = TM_STATE_STOP;
	}

	/*
	 * what's going on here - why is run always set to 1?
	 *
	 * the answer is that we check for run == 0 as the first condition
	 * at the top of this function. if it's set, then the next motor state
	 * is stop_motor, but it won't be evaluated if run == 0
	 *
	 * so we always force the state machine to run once, and count on the
	 * state machine to dtrt (do the right thing). This avoids setting motor
	 * power in wierd places
	 */

	ev3_tm->run = 1;
}

static int ev3_tacho_motor_get_estop(struct tacho_motor_device *tm)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	return ev3_tm->estop;
}

static void ev3_tacho_motor_set_estop(struct tacho_motor_device *tm, long estop)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	/*
	 * If the estop is unarmed, then writing ANY value will arm it!
	 *
	 * Note that stop_mode gets set to TM_STOP_COAST to make it easier to
	 * move the motor by hand if needed...
	 */

	if (0 == ev3_tm->estop) {
		ev3_tm->stop_mode = TM_STOP_COAST;
		ev3_tm->state     = TM_STATE_STOP;

		/*
		 * This handles the obscure case of accidentally getting
		 * a random value of 0 for the estop key.
		 */

		while(0 == ev3_tm->estop)
			get_random_bytes(&ev3_tm->estop, sizeof(ev3_tm->estop));

	/*
	 * If the estop is armed and we're writing the exact value back
	 * disarm the estop
	 */

	} else if (estop == ev3_tm->estop) {
		ev3_tm->estop = 0;

	/*
	 * Otherwise the estop is armed and we wrote the wrong value back
	 * so do nothing
	 */

	} else {
		/* This space intentionally left blank */
	}
}

static void ev3_tacho_motor_set_reset(struct tacho_motor_device *tm, long reset)
{
	struct ev3_tacho_motor_data *ev3_tm =
			container_of(tm, struct ev3_tacho_motor_data, tm);

	ev3_tacho_motor_reset(ev3_tm);
}

static const struct function_pointers fp = {
	.get_type		  = ev3_tacho_motor_get_type,
	.set_type		  = ev3_tacho_motor_set_type,

	.get_position		  = ev3_tacho_motor_get_position,
	.set_position		  = ev3_tacho_motor_set_position,

	.get_state		  = ev3_tacho_motor_get_state,
	.get_duty_cycle		  = ev3_tacho_motor_get_duty_cycle,
	.get_pulses_per_second	  = ev3_tacho_motor_get_pulses_per_second,

	.get_duty_cycle_sp	  = ev3_tacho_motor_get_duty_cycle_sp,
	.set_duty_cycle_sp	  = ev3_tacho_motor_set_duty_cycle_sp,

	.get_pulses_per_second_sp = ev3_tacho_motor_get_pulses_per_second_sp,
	.set_pulses_per_second_sp = ev3_tacho_motor_set_pulses_per_second_sp,

	.get_time_sp		  = ev3_tacho_motor_get_time_sp,
	.set_time_sp		  = ev3_tacho_motor_set_time_sp,

	.get_position_sp	  = ev3_tacho_motor_get_position_sp,
	.set_position_sp	  = ev3_tacho_motor_set_position_sp,

	.get_run_mode		  = ev3_tacho_motor_get_run_mode,
	.set_run_mode		  = ev3_tacho_motor_set_run_mode,

 	.get_regulation_mode	  = ev3_tacho_motor_get_regulation_mode,
 	.set_regulation_mode	  = ev3_tacho_motor_set_regulation_mode,

 	.get_stop_mode		  = ev3_tacho_motor_get_stop_mode,
 	.set_stop_mode		  = ev3_tacho_motor_set_stop_mode,

 	.get_position_mode	  = ev3_tacho_motor_get_position_mode,
 	.set_position_mode	  = ev3_tacho_motor_set_position_mode,

 	.get_polarity_mode	  = ev3_tacho_motor_get_polarity_mode,
 	.set_polarity_mode	  = ev3_tacho_motor_set_polarity_mode,

 	.get_encoder_mode	  = ev3_tacho_motor_get_encoder_mode,
 	.set_encoder_mode	  = ev3_tacho_motor_set_encoder_mode,

 	.get_ramp_up_sp		  = ev3_tacho_motor_get_ramp_up_sp,
 	.set_ramp_up_sp		  = ev3_tacho_motor_set_ramp_up_sp,

 	.get_ramp_down_sp	  = ev3_tacho_motor_get_ramp_down_sp,
 	.set_ramp_down_sp	  = ev3_tacho_motor_set_ramp_down_sp,

 	.get_speed_regulation_P	  = ev3_tacho_motor_get_speed_regulation_P,
 	.set_speed_regulation_P	  = ev3_tacho_motor_set_speed_regulation_P,

 	.get_speed_regulation_I	  = ev3_tacho_motor_get_speed_regulation_I,
 	.set_speed_regulation_I	  = ev3_tacho_motor_set_speed_regulation_I,

 	.get_speed_regulation_D	  = ev3_tacho_motor_get_speed_regulation_D,
 	.set_speed_regulation_D	  = ev3_tacho_motor_set_speed_regulation_D,

 	.get_speed_regulation_K	  = ev3_tacho_motor_get_speed_regulation_K,
 	.set_speed_regulation_K	  = ev3_tacho_motor_set_speed_regulation_K,

	.get_run		  = ev3_tacho_motor_get_run,
	.set_run		  = ev3_tacho_motor_set_run,

	.get_estop		  = ev3_tacho_motor_get_estop,
	.set_estop		  = ev3_tacho_motor_set_estop,

	.set_reset		  = ev3_tacho_motor_set_reset,
};


static int ev3_tacho_motor_probe(struct lego_device *motor)
{
	struct ev3_tacho_motor_data *ev3_tm;
	struct ev3_motor_platform_data *pdata = motor->dev.platform_data;
	int err;

	if (WARN_ON(!pdata))
		return -EINVAL;
	if (WARN_ON(!motor->port->motor_ops))
		return -EINVAL;

	ev3_tm = kzalloc(sizeof(struct ev3_tacho_motor_data), GFP_KERNEL);

	if (!ev3_tm)
		return -ENOMEM;

	ev3_tm->motor = motor;

	ev3_tm->tm.port_name = motor->port->port_name;
	ev3_tm->tm.fp = &fp;

	err = register_tacho_motor(&ev3_tm->tm, &motor->dev);
	if (err)
		goto err_register_tacho_motor;

	dev_set_drvdata(&motor->dev, ev3_tm);

	/* Here's where we set up the port pins on a per-port basis */
	if(request_irq(gpio_to_irq(pdata->tacho_int_gpio), tacho_motor_isr, 0,
				dev_name(&motor->port->dev), ev3_tm))
		goto err_dev_request_irq;

	irq_set_irq_type(gpio_to_irq(pdata->tacho_int_gpio),
			 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING);

	hrtimer_init(&ev3_tm->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ev3_tm->timer.function = ev3_tacho_motor_timer_callback;

	INIT_WORK(&ev3_tm->notify_state_change_work,
		  ev3_tacho_motor_notify_state_change_work);

	hrtimer_start(&ev3_tm->timer, ktime_set(0, TACHO_MOTOR_POLL_NS),
		      HRTIMER_MODE_REL);

	ev3_tacho_motor_reset(ev3_tm);

	return 0;

err_dev_request_irq:
	dev_set_drvdata(&motor->dev, NULL);
	unregister_tacho_motor(&ev3_tm->tm);
err_register_tacho_motor:
	kfree(ev3_tm);

	return err;
}

static int ev3_tacho_motor_remove(struct lego_device *motor)
{
	struct ev3_motor_platform_data *pdata = motor->dev.platform_data;
	struct ev3_tacho_motor_data *ev3_tm = dev_get_drvdata(&motor->dev);

	hrtimer_cancel(&ev3_tm->timer);
	cancel_work_sync(&ev3_tm->notify_state_change_work);
	free_irq(gpio_to_irq(pdata->tacho_int_gpio), ev3_tm);
	dev_set_drvdata(&motor->dev, NULL);
	unregister_tacho_motor(&ev3_tm->tm);
	kfree(ev3_tm);
	return 0;
}

struct lego_device_driver ev3_tacho_motor_driver = {
	.probe	= ev3_tacho_motor_probe,
	.remove	= ev3_tacho_motor_remove,
	.driver = {
		.name	= "ev3-tacho-motor",
		.owner	= THIS_MODULE,
	},
};
lego_device_driver(ev3_tacho_motor_driver);

MODULE_DESCRIPTION("EV3 tacho motor driver");
MODULE_AUTHOR("Ralph Hempel <rhempel@hempeldesigngroup.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("lego:ev3-tacho-motor");

