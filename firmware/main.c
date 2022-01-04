/*
 * AlienDie.c
 *
 * Created: 12/21/2021 10:36:39 PM
 * Author : AndreyKa
 */ 

#define F_CPU 1000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>

uint8_t refresh_stage;
uint8_t refresh_row;

// App->ISR
// Stores zeros in 3 LSB where LEDs should be ON for each row, ignores other bits
volatile uint8_t reds[3];
volatile uint8_t greens[3];

// ISR->App
// Stores zeroes in 3 LSB where button is pressed for each row, other bits hold garbage.
volatile uint8_t new_buttons[3];

// ISR->App
// Counts full frames (all 18 LEDs)
volatile uint8_t frames_count;

// Returns from power off
ISR(PCINT_vect) {
	__asm__ __volatile__ ("ldi	r28, 0xDF" ::: "memory");  // reset stack pointer
	__asm__ __volatile__ ("out	0x3d, r28" ::: "memory");
	__asm__ __volatile__ ("rjmp main" ::: "memory");	
}

// State machine that goes through
// 9 red LEDs: `refresh_stage`=1..9
// and 9 green LEDs: `refresh_stage`=10..19
ISR(TIMER1_COMPA_vect) {
	switch(++refresh_stage) {
		case 19:
			refresh_stage = 1;
			--frames_count;
			// fall through
		case 1:
			PORTB = 0xfb;
			refresh_row = reds[0];
			new_buttons[0] = PINB >> 3;
			PORTB = 0x04;
		red_wrap:
			PORTD = (refresh_row >> 1) | 0xfd;
			break;
		case 2:
		case 5:
		case 8:
			PORTD = 0xf;
			PORTA = (refresh_row >> 1) | 0xfe;
			break;
		case 3:
		case 6:
		case 9:
			PORTA = 3;
			PORTD = (refresh_row << 3) | 0xf7;
			break;
		case 4:
			PORTB = 0xfd;
			refresh_row = reds[1];
			new_buttons[1] = PINB >> 3;			
			PORTB = 0x02;
			goto red_wrap;
		case 7:
			PORTB = 0xbf;
			refresh_row = reds[2];
			new_buttons[2] = PINB >> 3;
			PORTB = 0x40;
			goto red_wrap;
		case 10:
			PORTB = 0x04;
			refresh_row = greens[0];
		green_wrap:
			PORTD = (refresh_row >> 2) | 0xfe;
			break;
		case 11:
		case 14:
		case 17:
			PORTD = 0xf;
			PORTA = refresh_row | 0xfd;
			break;
		case 12:
		case 15:
		case 18:
			PORTA = 3;
			PORTD = (refresh_row << 2) | 0xfb;
			break;
		case 13:
			PORTB = 0x02;
			refresh_row = greens[1];
			goto green_wrap;
		case 16:
			PORTB = 0x40;
			refresh_row = greens[2];
			goto green_wrap;			
	}
}

// Power saving sleep till given number of frames drawn.
void sleep(uint8_t frames) {
	frames_count = frames;
	do {
		__asm__ __volatile__ ("sleep" ::: "memory");
	} while (frames_count);
}

uint8_t read_btn_row(volatile uint8_t* cur, uint8_t* prev) {
	int8_t t = *cur & 7;
	uint8_t r = ~t & *prev;
	*prev = t;
	return r;
}

uint8_t prev_buttons[3];

// 3 LSB indicate the newly pressed buttons in each row.
// refreshed by `peek_buttons` or `read_buttons`
uint8_t buttons[3];
uint16_t random_seed;

// Returns 0 if no buttons pressed, updates `buttons` bitmap
uint8_t peek_buttons() {
	return
		((buttons[0] = read_btn_row(new_buttons, prev_buttons)) |
		(buttons[1] = read_btn_row(new_buttons + 1, prev_buttons + 1)) |
		(buttons[2] = read_btn_row(new_buttons + 2, prev_buttons + 2)));
}

// Blocks till button pressed. Updates `buttons` bitmap.
// If no buttons pressed for about 15 seconds, went power-off.
void read_buttons(void) {
	uint16_t counter = 15000; // 
	do {
		__asm__ __volatile__ ("sleep" ::: "memory");
		if (!--counter) {
			cli();
			TCCR1B = 0; // Stop the timer
			MCUCR = 0x30; // Power-down on sleep, sleep enabled, pullup not disabled
			PORTA = PORTD = 0;
			PORTB = 0x38; // pull-ups are on for b-3-4-5
			GIMSK = 0x20; // Pin Change Interrupt Enable
			PCMSK = 0x38; // Interrupt on pins b-3-4-5
			sei();
			__asm__ __volatile__ ("sleep" ::: "memory");
		}
	} while (0 == peek_buttons());
	random_seed += counter;
	sleep(2); // de-bounce
};

// Returns a random number (pseudo random with feed from key press milliseconds).
uint8_t rand(void) {
	random_seed = (random_seed << 2) + random_seed + 12345;
	return random_seed >> 5;
}

void set_array(volatile uint8_t* array, uint8_t val) {
	array[0] = array[1] = array[2] = val;
}

// Fills a single bitmap from a packed 9-bit form
void fill_frame(volatile uint8_t* dst, uint8_t v, uint8_t tail) {
	dst[0] = v;
	dst[1] = v >> 3;
	dst[2] = ((v >> 5) & 6) | (tail & 1);	
}

// Draws 4 animation frames from 9 bytes of program memory.
// All bits are inverted: 0-LED on, 1-LED off.
// Bits layout is as follows (A..I - reds, a..i - greens):
// Screen bits:
//    ABC  abc
//    DEF  def
//    GHI  ghi
// Memory layout:
//   Byte 0 encodes i- and I-bits for all 4 frames:
//     : iI iI iI iI
//       ^3 ^2 ^1 ^0 
//   Bytes 1..9 encode bits A..H and a..h for Red and Green bit planes
//     GHDEFABC; ghdefabc;  << frame 0
//     GHDEFABC; ghdefabc;  << frame 1
//     GHDEFABC; ghdefabc;  << frame 2
//     GHDEFABC; ghdefabc;  << frame 3
void animate4(const uint8_t* data) {
	uint8_t tails = pgm_read_byte_near(data++);
	for (uint8_t i = 5; --i;) {
		fill_frame(reds, pgm_read_byte_near(data++), tails);
		tails >>= 1;
		fill_frame(greens, pgm_read_byte_near(data++), tails);
		tails >>= 1;
		sleep(5);
	}
}

const PROGMEM uint8_t start_animation[] = {
	0x7f, // 01 11 11 11
	0xff, 0xff, // 11i 111 111  11i 111 111
	0xfd, 0xfa, // 11i 111 101  11i 111 010
	0xe8, 0xd5, // 11i 101 000  11i 010 101
	0x85, 0x6a, // 10i 000 101  01o 101 010
};

// 1-bit image for six dies. Pixel (x=1, y=0) is always off, thus excluded from encoding.
// All bits are inverted: 0-LED on, 1-LED off.
const PROGMEM uint8_t dies[] = { // v|0x40, v>>6
	0xef, // 1x1 101 111
	0xd7, // 1x1 010 111
	0xab, // 1x0 101 011
	0x3a, // 0x0 111 010
	0x2a, // 0x0 101 010
	0x12, // 0x0 010 010
};

uint8_t expand(uint8_t v) {
	return v | (v << 1) | (v >> 1);
}

#define MESSAGE_MAX 20
uint8_t message_size = 0;
uint8_t message_reds[MESSAGE_MAX];
uint8_t message_greens[MESSAGE_MAX];
uint8_t message_tails[(MESSAGE_MAX + 3) / 4];

void load_frame(uint8_t i) {
	uint8_t tail = message_tails[i >> 2] >> ((i & 3) << 1);
	fill_frame(reds, message_reds[i], tail);
	fill_frame(greens, message_greens[i], tail >> 1);
}

uint8_t pack_frame(volatile uint8_t* data) {
	return (data[0] & 7) | ((data[1] & 7) << 3) | ((data[2] & 6) << 5);
}

void scroll (volatile uint8_t* data) {
	uint8_t i = 3;
	do {
		*data = (*data << 1) | (rand() & 1);
		++data;
	} while (--i);
}

int main(void)
{
	DDRB = 0x46;
	DDRA = 3;
	DDRD = 0xf;
	PORTA = 3;
	PORTD = 0xf;

	// Timer
	TCCR1A = 0x00; // disconnected from pins, normal mode, clear on match to OCR1A
	TCCR1B = 0x0B; // 1/64 of clock
	TCNT1H = TCNT1L = 0; // the counter value
	OCR1BH = OCR1BL = 0; // b-capture register is not used
	OCR1AH = 0;
	OCR1AL = 16; // 16 x 64 = 1024 clock ticks = 1 millisecond for 1 MHz
	TIMSK = 1 << OCIE1A; // interrupts from timer1 comparer
	// Sleep
	GIMSK = 0; // Pin Change Interrupt Disable
	PCMSK = 0; // No interrupts on pins
	MCUCR = 0x20; // sleep goes idle
	sei();
	
	for (;;) {
		animate4(start_animation);
		read_buttons();
		set_array(greens, 7);
		set_array(reds, 7);
		if (buttons[1] & 1) { // quad flip-flop game
			greens[0] = rand();
			greens[1] = rand();
			greens[2] = rand();
			do {
				read_buttons();
				uint8_t a = expand(buttons[0]);
				uint8_t b = expand(buttons[1]);
				uint8_t c = expand(buttons[2]);
				greens[0] ^= a | b;
				greens[1] ^= a | b | c;
				greens[2] ^= b | c;
			} while ((greens[0] | greens[1] | greens[2]) & 7);
		} else if (buttons[2] & 4) { // tic-tac-toe game
			volatile uint8_t* cur_plane = reds;
			for (;;) {
				read_buttons();
				for (uint8_t i = 0; i < 3; ++i) {
					uint8_t b = buttons[i];
					if ((reds[i] & greens[i] & b) == 0)
						continue;
					cur_plane[i] &= ~b;
					if (!(((greens[0] & reds[0]) | (greens[1] & reds[1]) | (greens[2] & reds[2])) & 7))
						goto main_menu;  // par
					if ((cur_plane[i] == 0) ||
					    (((cur_plane[0] | cur_plane[1] | cur_plane[2]) & b) == 0) ||
						(((cur_plane[0] << 1 | cur_plane[1] | cur_plane[2] >> 1) & 2) == 0) ||
						(((cur_plane[0] >> 1 | cur_plane[1] | cur_plane[2] << 1) & 2) == 0)) {
							goto main_menu; // win
					}
					cur_plane = cur_plane == reds ? greens : reds;
					break;
				}
			}
		} else if (buttons[0] & 4) { // slow player-with black frames
			for (uint8_t i = 0; i < message_size; ++i) {
				load_frame(i);
				sleep(50);
				set_array(greens, 7);
				set_array(reds, 7);				
				sleep(10);
				if (peek_buttons())
					goto main_menu;
			}				
		} else if (buttons[2] & 1) { // timer
			for (uint8_t g = 0; g < 3; ++g) {
				for (;;) {
					set_array(reds, 0xff);
					for (uint8_t r = 0; r < 3; ++r) {
						for (;;) {
							uint8_t v = reds[r] <<= 1;
							sleep(20);
							if (!(v & 7))
								break;
							if (peek_buttons()) {
								read_buttons();
								goto main_menu;
							}
						}
					}
					uint8_t v = greens[g] <<= 1;
					sleep(20);
					if (!(v & 7))
						break;
				}
			}			
		} else if (buttons[1] & 2) { // die x 6
			uint8_t v = pgm_read_byte_near(dies + rand() % 6);
			fill_frame(reds, v | 0x40, v >> 6);
			read_buttons();
		} else if (buttons[0] & 2) { // editor
			uint8_t frame = 0;
			for (;;) {
				load_frame(frame);
				for (;;) {
					read_buttons();
					uint8_t chord = new_buttons[0] & 7;
					if (chord == 1) {
						if ((message_size = ++frame) == MESSAGE_MAX)
							goto main_menu;
						break;
					} else if (chord == 4) {
						if (frame > 0)
							--frame;
						break;						
					} else if (chord == 2) {
						message_size = frame;
						goto main_menu;
					} else {
						message_reds[frame] = pack_frame(reds);
						message_greens[frame] = pack_frame(greens);
						uint8_t shift = (frame & 3) << 1;
						message_tails[frame >> 2] = (message_tails[frame >> 2] & ~(3 << shift)) | ((reds[2] & 1) | (greens[2] & 1) << 1) << shift;
						for (int8_t i = 3; --i >= 0;) {
							reds[i] ^= buttons[i];
							if (reds[i] & buttons[i])
								greens[i] ^= buttons[i];
						}
					}
				}
			}
		} else if (buttons[2] & 2) {  // 9 coins
			fill_frame(greens, rand(), rand());
			reds[0] = greens[0];
			reds[1] = greens[1];
			reds[2] = greens[2];
			read_buttons();
		} else if (buttons[0] & 1) {  // fast player
			for (uint8_t i = 0; i < message_size; ++i) {
				load_frame(i);
				sleep(30);
				if (peek_buttons())
					goto main_menu;
			}
		} else { // screen saver
			for (uint8_t i = 0; --i && !peek_buttons(); ) {
				scroll(reds);
				scroll(greens);
				sleep(10);
			}
		}
	main_menu:
		sleep(10);
	}
}

// Todo: Morse text input
//  x  xx  xxx xx  xxx xxx xx  x x xxx xxx x x x   xxx xx  xxx xxx xxx xxx  xx xxx x x x x x x x x x x xx   O xx  Z xx  x x S x   xxx xxx xxx
// xxx xxx x   x x xx  xx  x x xxx  x   x  xx  x   xxx x x x x xxx x x xx   x   x  x x x x xxx  x   x   x      x     xx xxx   xxx   x xxx xxx
// x x xxx xxx xx  xxx x   xxx x x xxx xx  x x xxx x x x x xxx x   xx  x x xx   x  xxx  x  xxx x x  x   xx    xxx   xxx   x   xxx  x  xxx   x
