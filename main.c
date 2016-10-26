#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>


#define SET(a,b)				a |=  _BV(b)
#define CLEAR(a,b)				a &= ~_BV(b)
#define HIGH(a,b)				SET(a,b)
#define LOW(a,b)				CLEAR(a,b)
#define GET(a,b)				(a & _BV(b))
#define TOGGLE(a,b)				a ^= _BV(b)

#define MAGIC 0x616e6469
#define VERSION 1

#define FLAGS_FRAME 1
#define DEFAULT_FLAGS 0

#define PIN_LED PB0
#define PIN_SENDER PB1
#define PIN_RECEIVER PB2
#define PIN_BUTTON PB3
#define PIN_INPUT PB4

#define MAX_MICROS 4294967295

#define MIN_PULSES 20
#define MAX_PULSES 256 // must be even
#define MIN_TIME 70
#define MIN_GAP 3000
#define MAX_SEQUENCE MAX_PULSES / 2 // since one sequence is 4 bit
#define MAX_TIMES 16
#define MAX_TIMEDIF 100
#define TIMEOUT 10000000
#define BLINK_INTERVAL 300


typedef struct {
	uint16_t times[MAX_TIMES];
	uint8_t squence[MAX_SEQUENCE];
	uint8_t times_count;
	uint16_t length;
} frame_t;

typedef enum {
	IDLE, SEND, RECEIVE
} state_t;


void setup();
int main();

bool config_load_meta();
bool config_load_frame();
void config_save_meta();
void config_save_frame();
void frame_clear(frame_t* frame);
bool frame_add(frame_t* frame, uint16_t time);
uint16_t frame_get(const frame_t* frame, uint16_t i);
void state_set(state_t new_state);
void send(const frame_t* frame, uint8_t repeat);
void receive();
void receive_callback();
void receive_timeout();

uint16_t abs_diff(uint16_t a, uint16_t b);
bool time_cmp(uint16_t a, uint16_t b);
uint32_t time_diff(uint32_t a, uint32_t b);
void timer_start();
void timer_stop();
void delay_ms(uint16_t count);
void delay_us(uint16_t count);
void led_blink(uint16_t interval, uint8_t repeat);


uint32_t EEMEM eeprom_magic = MAGIC;
uint8_t EEMEM eeprom_version = VERSION;
uint8_t EEMEM eeprom_flags = DEFAULT_FLAGS;
frame_t EEMEM eeprom_frame;

volatile state_t state = IDLE;
volatile uint32_t micros = 0;
uint8_t flags;
frame_t frame;
uint16_t last_frame_length = 0;
uint16_t last_frame_gap = 0;


void setup() {
	CLEAR(SREG, SREG_I); // disable interrupts
	
	frame_clear(&frame);
	
	// setup pcint
	SET(GIMSK, PCIE); // endable pin change interrupt
	SET(PCMSK, PIN_BUTTON); // enable pcint for button
	SET(PCMSK, PIN_INPUT); // enable pcint for input
	
	// setup pins
	DDRB = 0xff; // set all pins as outputs
	CLEAR(DDRB, PIN_RECEIVER); // set receiver as input
	CLEAR(DDRB, PIN_BUTTON); // set button as input
	CLEAR(DDRB, PIN_INPUT); // set input as input
	SET(PORTB, PIN_BUTTON); // set pull-up for button
	SET(PORTB, PIN_INPUT); // set pull-up for input
	
	// load config
	if (config_load_meta()) {
		config_load_frame();
	} else {
		led_blink(BLINK_INTERVAL, 2);
		flags = DEFAULT_FLAGS;
		config_save_meta();
	}
	
	timer_start();
	
	SET(SREG, SREG_I); // enable interrupts
}

int main() {
	setup();
	
	while (true) {
		switch (state) {
		case IDLE:
			break;
		case RECEIVE:
			receive();
			state = IDLE;
			break;
		case SEND:
			CLEAR(SREG, SREG_I); // disable interrupts
			send(&frame, 10);
			SET(SREG, SREG_I); // enable interrupts
			state = IDLE;
			break;
		}
	}
	
	return 0;
}

bool config_load_meta() {
	if (eeprom_read_dword(&eeprom_magic) != MAGIC) return false;
	if (eeprom_read_byte(&eeprom_version) != VERSION) return false;
	
	flags = eeprom_read_byte(&eeprom_flags);
	
	return true;
}

bool config_load_frame() {
	if ((flags & FLAGS_FRAME) == 0) return false;
	
	eeprom_read_block((void*) &frame, (const void*) &eeprom_frame, sizeof(frame_t));
	
	return true;
}

void config_save_meta() {
	eeprom_update_dword(&eeprom_magic, MAGIC);
	eeprom_update_byte(&eeprom_version, VERSION);
	eeprom_update_byte(&eeprom_flags, flags);
}

void config_save_frame() {
	eeprom_update_block((void*) &frame, (void*) &eeprom_frame, sizeof(frame_t));
}

void frame_clear(frame_t* frame) {
	frame->times_count = 0;
	frame->length = 0;
}

bool frame_add(frame_t* frame, uint16_t time) {
	int8_t index = -1;
	for (uint8_t i = 0; i < frame->times_count; i++) {
		if (time_cmp(frame->times[i], time)) {
			index = i;
			break;
		}
	}
	if (index == -1) {
		if (frame->times_count >= MAX_TIMES) return false;
		frame->times[frame->times_count] = time;
		index = frame->times_count;
		frame->times_count++;
	}
	
	if (frame->length >= MAX_PULSES) return false;
	uint8_t i = frame->length >> 1;
	if (frame->length & 1) {
		frame->squence[i] &= 0b00001111;
		frame->squence[i] |= index << 4;
	} else {
		frame->squence[i] &= 0b11110000;
		frame->squence[i] |= index;
	}
	frame->length++;
	return true;
}

uint16_t frame_get(const frame_t* frame, uint16_t i) {
	uint8_t shift = (i & 1) ? 4 : 0;
	uint8_t index = (frame->squence[i >> 1] >> shift) & 0x0f;
	return frame->times[index];
}

void state_set(state_t new_state) {
	if (state != IDLE) return;
	state = new_state;
}

void send(const frame_t* frame, uint8_t repeat) {
	if ((flags & FLAGS_FRAME) == 0) {
		led_blink(BLINK_INTERVAL, 3);
		return;
	}
	
	CLEAR(PORTB, PIN_LED);
	CLEAR(PORTB, PIN_SENDER);
	
	for (uint8_t i = 0; i < repeat; i++) {
		for (uint16_t j = 0; j < frame->length; j++) {
			TOGGLE(PORTB, PIN_LED);
			TOGGLE(PORTB, PIN_SENDER);
			uint16_t time = frame_get(frame, j);
			delay_us(time);
		}
		
		CLEAR(PORTB, PIN_LED);
		CLEAR(PORTB, PIN_SENDER);
	}
}

void receive() {
	uint32_t start = micros;
	uint8_t last_in = GET(PINB, PIN_RECEIVER);
	uint32_t last_time = start;
	
	frame_clear(&frame);
	last_frame_length = 0;
	
	while (true) {
		uint32_t now = micros;
		uint32_t duration = time_diff(now, start);
		uint8_t in = GET(PINB, PIN_RECEIVER);
		
		if (duration > TIMEOUT) {
			CLEAR(PORTB, PIN_LED); // led off
			receive_timeout();
			break;
		}
		
		if (last_in != in) {
			TOGGLE(PORTB, PIN_LED); // led toggle
			
			uint16_t time = (uint16_t) time_diff(now, last_time);
			if (time >= MIN_TIME) {
				bool success = frame_add(&frame, time);
				if (!success) {
					frame_clear(&frame);
					frame_add(&frame, time);
				}
			}
			last_in = in;
			last_time = now;
			
			if ((frame.length >= MIN_PULSES) & (time > MIN_GAP)) {
				if ((last_frame_length == frame.length) &
					time_cmp(last_frame_gap, time)) {
					CLEAR(PORTB, PIN_LED); // led off
					receive_callback();
					break;
				}
				
				last_frame_length = frame.length;
				last_frame_gap = time;
				frame_clear(&frame);
			}
		}
	}
}

void receive_callback() {
	led_blink(BLINK_INTERVAL, 4);
	
	flags |= FLAGS_FRAME;
	config_save_meta();
	config_save_frame();
}

void receive_timeout() {
	led_blink(BLINK_INTERVAL, 5);
	
	config_load_frame();
}

ISR(PCINT0_vect) {
	static uint8_t last = 0;
	uint8_t change = PINB ^ last;
	
	if (GET(change, PIN_INPUT)) {
		if (!GET(PINB, PIN_INPUT)) {
			state_set(SEND);
		}
	}
	
	if (GET(change, PIN_BUTTON)) {
		if (!GET(PINB, PIN_BUTTON)) {
			state_set(RECEIVE);
		}
	}
	
	last = PINB;
}

uint16_t abs_diff(uint16_t a, uint16_t b) {
	if (a > b) return a - b;
	else return b - a;
}

void delay_ms(uint16_t count) {
	while (true) {
		_delay_ms(10);
		if (count <= 10) break;
		count -= 10;
	}
}

void delay_us(uint16_t count) {
	while (true) {
		_delay_us(10);
		if (count <= 10) break;
		count -= 10;
	}
}

void led_blink(uint16_t interval, uint8_t repeat) {
	bool intenabled = GET(SREG, SREG_I);
	if (intenabled) CLEAR(SREG, SREG_I); // disable interrupts
	
	interval >>= 1;
	repeat = (repeat << 1) - 1;
	SET(PORTB, PIN_LED); // led on
	for (uint8_t i = 0; i < repeat; i++) {
		delay_ms(interval);
		TOGGLE(PORTB, PIN_LED); // led toggle
	}
	
	if (intenabled) SET(SREG, SREG_I); // enable interrupts
}

bool time_cmp(uint16_t a, uint16_t b) {
	return abs_diff(a, b) <= MAX_TIMEDIF;
}

uint32_t time_diff(uint32_t a, uint32_t b) {
	if (a >= b) return a - b;
	return MAX_MICROS - (b - a);
}

void timer_start() {
	micros = 0;
	
	// setup timer
	SET(TIMSK, OCIE1A); // enable compare match interrupt
	SET(TCCR1, CTC1); // enable clear on compare match
	SET(TCCR1, CS12); // enable timer, set prescale CK/8
	OCR1C = 19; // set max timer value
}

void timer_stop() {
	TCCR1 &= 0b11110000; // disable timer
}

ISR(TIMER1_COMPA_vect) {
	// each 10µs called
	if (micros > MAX_MICROS - 10) {
		micros = 0;
	} else {
		micros += 10;
	}
}

