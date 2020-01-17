//-----------------------------------------------------------------------------
// Jonathan Westhues, split Nov 2006
// Modified by Greg Jones, Jan 2009
// Modified by Adrian Dabrowski "atrox", Mar-Sept 2010,Oct 2011
// Modified by piwi, Oct 2018
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Routines to support ISO 15693. This includes both the reader software and
// the `fake tag' modes.
//-----------------------------------------------------------------------------

// The ISO 15693 describes two transmission modes from reader to tag, and four
// transmission modes from tag to reader. As of Oct 2018 this code supports
// both reader modes and the high speed variant with one subcarrier from card to reader.
// As long as the card fully support ISO 15693 this is no problem, since the
// reader chooses both data rates, but some non-standard tags do not.
// For card simulation, the code supports both high and low speed modes with one subcarrier.
//
// VCD (reader) -> VICC (tag)
// 1 out of 256:
//  data rate: 1,66 kbit/s (fc/8192)
//  used for long range
// 1 out of 4:
//  data rate: 26,48 kbit/s (fc/512)
//  used for short range, high speed
//
// VICC (tag) -> VCD (reader)
// Modulation:
//      ASK / one subcarrier (423,75 khz)
//      FSK / two subcarriers (423,75 khz && 484,28 khz)
// Data Rates / Modes:
//  low ASK: 6,62 kbit/s
//  low FSK: 6.67 kbit/s
//  high ASK: 26,48 kbit/s
//  high FSK: 26,69 kbit/s
//-----------------------------------------------------------------------------


// Random Remarks:
// *) UID is always used "transmission order" (LSB), which is reverse to display order

// TODO / BUGS / ISSUES:
// *) signal decoding is unable to detect collisions.
// *) add anti-collision support for inventory-commands
// *) read security status of a block
// *) sniffing and simulation do not support two subcarrier modes.
// *) remove or refactor code under "deprecated"
// *) document all the functions

#include "iso15693.h"

#include "proxmark3.h"
#include "util.h"
#include "apps.h"
#include "string.h"
#include "iso15693tools.h"
#include "protocols.h"
#include "cmd.h"
#include "BigBuf.h"
#include "fpgaloader.h"

#define arraylen(x) (sizeof(x)/sizeof((x)[0]))

// Delays in SSP_CLK ticks.
// SSP_CLK runs at 13,56MHz / 32 = 423.75kHz when simulating a tag
#define DELAY_READER_TO_ARM               8
#define DELAY_ARM_TO_READER               0
//SSP_CLK runs at 13.56MHz / 4 = 3,39MHz when acting as reader. All values should be multiples of 16
#define DELAY_TAG_TO_ARM                 32
#define DELAY_ARM_TO_TAG                 16

static int DEBUG = 0;


// specific LogTrace function for ISO15693: the duration needs to be scaled because otherwise it won't fit into a uint16_t
bool LogTrace_ISO15693(const uint8_t *btBytes, uint16_t iLen, uint32_t timestamp_start, uint32_t timestamp_end, uint8_t *parity, bool readerToTag) {
	uint32_t duration = timestamp_end - timestamp_start;
	duration /= 32;
	timestamp_end = timestamp_start + duration;
	return LogTrace(btBytes, iLen, timestamp_start, timestamp_end, parity, readerToTag);
}


///////////////////////////////////////////////////////////////////////
// ISO 15693 Part 2 - Air Interface
// This section basically contains transmission and receiving of bits
///////////////////////////////////////////////////////////////////////

// buffers
#define ISO15693_DMA_BUFFER_SIZE       2048 // must be a power of 2
#define ISO15693_MAX_RESPONSE_LENGTH   2052 // allows read multiple block with the maximum block size of 256bits and a maximum block number of 64.
#define ISO15693_MAX_COMMAND_LENGTH      45 // allows write single block with the maximum block size of 256bits. Write multiple blocks not supported yet

// ---------------------------
// Signal Processing
// ---------------------------

// prepare data using "1 out of 4" code for later transmission
// resulting data rate is 26.48 kbit/s (fc/512)
// cmd ... data
// n ... length of data
void CodeIso15693AsReader(uint8_t *cmd, int n) {

	ToSendReset();

	// SOF for 1of4
	ToSend[++ToSendMax] = 0x84; //10000100

	// data
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < 8; j += 2) {
			int these = (cmd[i] >> j) & 0x03;
			switch(these) {
				case 0:
					ToSend[++ToSendMax] = 0x40; //01000000
					break;
				case 1:
					ToSend[++ToSendMax] = 0x10; //00010000
					break;
				case 2:
					ToSend[++ToSendMax] = 0x04; //00000100
					break;
				case 3:
					ToSend[++ToSendMax] = 0x01; //00000001
					break;
			}
		}
	}

	// EOF
	ToSend[++ToSendMax] = 0x20; //0010 + 0000 padding

	ToSendMax++;
}

// encode data using "1 out of 256" scheme
// data rate is 1,66 kbit/s (fc/8192)
// is designed for more robust communication over longer distances
static void CodeIso15693AsReader256(uint8_t *cmd, int n)
{
	ToSendReset();

	// SOF for 1of256
	ToSend[++ToSendMax] = 0x81; //10000001

	// data
	for(int i = 0; i < n; i++) {
		for (int j = 0; j <= 255; j++) {
			if (cmd[i] == j) {
				ToSendStuffBit(0);
				ToSendStuffBit(1);
			} else {
				ToSendStuffBit(0);
				ToSendStuffBit(0);
			}
		}
	}

	// EOF
	ToSend[++ToSendMax] = 0x20; //0010 + 0000 padding

	ToSendMax++;
}


// static uint8_t encode4Bits(const uint8_t b) {
	// uint8_t c = b & 0xF;
	// // OTA, the least significant bits first
	// //         The columns are
	// //               1 - Bit value to send
	// //               2 - Reversed (big-endian)
	// //               3 - Manchester Encoded
	// //               4 - Hex values

	// switch(c){
	// //                          1       2         3         4
	  // case 15: return 0x55; // 1111 -> 1111 -> 01010101 -> 0x55
	  // case 14: return 0x95; // 1110 -> 0111 -> 10010101 -> 0x95
	  // case 13: return 0x65; // 1101 -> 1011 -> 01100101 -> 0x65
	  // case 12: return 0xa5; // 1100 -> 0011 -> 10100101 -> 0xa5
	  // case 11: return 0x59; // 1011 -> 1101 -> 01011001 -> 0x59
	  // case 10: return 0x99; // 1010 -> 0101 -> 10011001 -> 0x99
	  // case 9:  return 0x69; // 1001 -> 1001 -> 01101001 -> 0x69
	  // case 8:  return 0xa9; // 1000 -> 0001 -> 10101001 -> 0xa9
	  // case 7:  return 0x56; // 0111 -> 1110 -> 01010110 -> 0x56
	  // case 6:  return 0x96; // 0110 -> 0110 -> 10010110 -> 0x96
	  // case 5:  return 0x66; // 0101 -> 1010 -> 01100110 -> 0x66
	  // case 4:  return 0xa6; // 0100 -> 0010 -> 10100110 -> 0xa6
	  // case 3:  return 0x5a; // 0011 -> 1100 -> 01011010 -> 0x5a
	  // case 2:  return 0x9a; // 0010 -> 0100 -> 10011010 -> 0x9a
	  // case 1:  return 0x6a; // 0001 -> 1000 -> 01101010 -> 0x6a
	  // default: return 0xaa; // 0000 -> 0000 -> 10101010 -> 0xaa

	// }
// }

static const uint8_t encode_4bits[16] = { 0xaa, 0x6a, 0x9a, 0x5a, 0xa6, 0x66, 0x96, 0x56, 0xa9, 0x69, 0x99, 0x59, 0xa5, 0x65, 0x95, 0x55 };

void CodeIso15693AsTag(uint8_t *cmd, size_t len) {
	/*
	 * SOF comprises 3 parts;
	 * * An unmodulated time of 56.64 us
	 * * 24 pulses of 423.75 kHz (fc/32)
	 * * A logic 1, which starts with an unmodulated time of 18.88us
	 *   followed by 8 pulses of 423.75kHz (fc/32)
	 *
	 * EOF comprises 3 parts:
	 * - A logic 0 (which starts with 8 pulses of fc/32 followed by an unmodulated
	 *   time of 18.88us.
	 * - 24 pulses of fc/32
	 * - An unmodulated time of 56.64 us
	 *
	 * A logic 0 starts with 8 pulses of fc/32
	 * followed by an unmodulated time of 256/fc (~18,88us).
	 *
	 * A logic 0 starts with unmodulated time of 256/fc (~18,88us) followed by
	 * 8 pulses of fc/32 (also 18.88us)
	 *
	 * A bit here becomes 8 pulses of fc/32. Therefore:
	 * The SOF can be written as 00011101 = 0x1D
	 * The EOF can be written as 10111000 = 0xb8
	 * A logic 1 is 01
	 * A logic 0 is 10
	 *
	 * */

	ToSendReset();

	// SOF
	ToSend[++ToSendMax] = 0x1D;  // 00011101

	// data
	for (int i = 0; i < len; i++) {
		ToSend[++ToSendMax] = encode_4bits[cmd[i] & 0xF];
		ToSend[++ToSendMax] = encode_4bits[cmd[i] >> 4];
	}

	// EOF
	ToSend[++ToSendMax] = 0xB8; // 10111000

	ToSendMax++;
}


// Transmit the command (to the tag) that was placed in cmd[].
void TransmitTo15693Tag(const uint8_t *cmd, int len, uint32_t *start_time) {

	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_FULL_MOD);

	if (*start_time < DELAY_ARM_TO_TAG) {
		*start_time = DELAY_ARM_TO_TAG;
	}

	*start_time = (*start_time - DELAY_ARM_TO_TAG) & 0xfffffff0;

	while (GetCountSspClk() > *start_time) { // we may miss the intended time
		*start_time += 16; // next possible time
	}


	while (GetCountSspClk() < *start_time)
		/* wait */ ;

	LED_B_ON();
	for (int c = 0; c < len; c++) {
		uint8_t data = cmd[c];
		for (int i = 0; i < 8; i++) {
			uint16_t send_word = (data & 0x80) ? 0xffff : 0x0000;
			while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
			AT91C_BASE_SSC->SSC_THR = send_word;
			while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
			AT91C_BASE_SSC->SSC_THR = send_word;

			data <<= 1;
		}
		WDT_HIT();
	}
	LED_B_OFF();

	*start_time = *start_time + DELAY_ARM_TO_TAG;

}


//-----------------------------------------------------------------------------
// Transmit the tag response (to the reader) that was placed in cmd[].
//-----------------------------------------------------------------------------
void TransmitTo15693Reader(const uint8_t *cmd, size_t len, uint32_t *start_time, uint32_t slot_time, bool slow) {
	// don't use the FPGA_HF_SIMULATOR_MODULATE_424K_8BIT minor mode. It would spoil GetCountSspClk()
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_424K);

	uint32_t modulation_start_time = *start_time - DELAY_ARM_TO_READER + 3 * 8;  // no need to transfer the unmodulated start of SOF

	while (GetCountSspClk() > (modulation_start_time & 0xfffffff8) + 3) { // we will miss the intended time
		if (slot_time) {
			modulation_start_time += slot_time; // use next available slot
		} else {
			modulation_start_time = (modulation_start_time & 0xfffffff8) + 8; // next possible time
		}
	}

	while (GetCountSspClk() < (modulation_start_time & 0xfffffff8))
		/* wait */ ;

	uint8_t shift_delay = modulation_start_time & 0x00000007;

	*start_time = modulation_start_time + DELAY_ARM_TO_READER - 3 * 8;

	LED_C_ON();
	uint8_t bits_to_shift = 0x00;
	uint8_t bits_to_send = 0x00;
	for (size_t c = 0; c < len; c++) {
		for (int i = (c==0?4:7); i >= 0; i--) {
			uint8_t cmd_bits = ((cmd[c] >> i) & 0x01) ? 0xff : 0x00;
			for (int j = 0; j < (slow?4:1); ) {
				if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY) {
					bits_to_send = bits_to_shift << (8 - shift_delay) | cmd_bits >> shift_delay;
					AT91C_BASE_SSC->SSC_THR = bits_to_send;
					bits_to_shift = cmd_bits;
					j++;
				}
			}
		}
		WDT_HIT();
	}
	// send the remaining bits, padded with 0:
	bits_to_send = bits_to_shift << (8 - shift_delay);
	for ( ; ; ) {
		if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY) {
			AT91C_BASE_SSC->SSC_THR = bits_to_send;
			break;
		}
	}
	LED_C_OFF();
}


//=============================================================================
// An ISO 15693 decoder for tag responses (one subcarrier only).
// Uses cross correlation to identify each bit and EOF.
// This function is called 8 times per bit (every 2 subcarrier cycles).
// Subcarrier frequency fs is 424kHz, 1/fs = 2,36us,
// i.e. function is called every 4,72us
// LED handling:
//    LED C -> ON once we have received the SOF and are expecting the rest.
//    LED C -> OFF once we have received EOF or are unsynced
//
// Returns: true if we received a EOF
//          false if we are still waiting for some more
//=============================================================================

#define NOISE_THRESHOLD          160                   // don't try to correlate noise
#define MAX_PREVIOUS_AMPLITUDE   (-1 - NOISE_THRESHOLD)

typedef struct DecodeTag {
	enum {
		STATE_TAG_SOF_LOW,
		STATE_TAG_SOF_RISING_EDGE,
		STATE_TAG_SOF_HIGH,
		STATE_TAG_SOF_HIGH_END,
		STATE_TAG_RECEIVING_DATA,
		STATE_TAG_EOF,
		STATE_TAG_EOF_TAIL
	}         state;
	int       bitCount;
	int       posCount;
	enum {
		LOGIC0,
		LOGIC1,
		SOF_PART1,
		SOF_PART2
	}         lastBit;
	uint16_t  shiftReg;
	uint16_t  max_len;
	uint8_t   *output;
	int       len;
	int       sum1, sum2;
	int       threshold_sof;
	int       threshold_half;
	uint16_t  previous_amplitude;
} DecodeTag_t;


static int inline __attribute__((always_inline)) Handle15693SamplesFromTag(uint16_t amplitude, DecodeTag_t *DecodeTag, bool recv_speed)
{
	switch(DecodeTag->state) {
		case STATE_TAG_SOF_LOW:
			// waiting for a rising edge
			if (amplitude > NOISE_THRESHOLD + DecodeTag->previous_amplitude) {
				if (DecodeTag->posCount > 10) {
					DecodeTag->threshold_sof = amplitude - DecodeTag->previous_amplitude;
					DecodeTag->threshold_half = 0;
					DecodeTag->state = STATE_TAG_SOF_RISING_EDGE;
				} else {
					DecodeTag->posCount = 0;
				}
			} else {
				DecodeTag->posCount++;
				DecodeTag->previous_amplitude = amplitude;
			}
			break;

		case STATE_TAG_SOF_RISING_EDGE:
			if (amplitude - DecodeTag->previous_amplitude > DecodeTag->threshold_sof) { // edge still rising
				if (amplitude - DecodeTag->threshold_sof > DecodeTag->threshold_sof) { // steeper edge, take this as time reference
					DecodeTag->posCount = 1;
				} else {
					DecodeTag->posCount = 2;
				}
				DecodeTag->threshold_sof = (amplitude - DecodeTag->previous_amplitude) / 2;
			} else {
				DecodeTag->posCount = 2;
				DecodeTag->threshold_sof = DecodeTag->threshold_sof/2;
			}
			// DecodeTag->posCount = 2;
			DecodeTag->state = STATE_TAG_SOF_HIGH;
			break;

		case STATE_TAG_SOF_HIGH:
			// waiting for 10 times high. Take average over the last 8
			if (amplitude > DecodeTag->threshold_sof) {
				DecodeTag->posCount++;
				if (DecodeTag->posCount > 2) {
					DecodeTag->threshold_half += amplitude; // keep track of average high value
				}
				if (DecodeTag->posCount == (recv_speed?10:40)) {
					DecodeTag->threshold_half >>= 2; // (4 times 1/2 average)
					DecodeTag->state = STATE_TAG_SOF_HIGH_END;
				}
			} else { // high phase was too short
				DecodeTag->posCount = 1;
				DecodeTag->previous_amplitude = amplitude;
				DecodeTag->state = STATE_TAG_SOF_LOW;
			}
			break;

		case STATE_TAG_SOF_HIGH_END:
			// check for falling edge
			if (DecodeTag->posCount == (recv_speed?13:52) && amplitude < DecodeTag->threshold_sof) {
				DecodeTag->lastBit = SOF_PART1;  // detected 1st part of SOF (12 samples low and 12 samples high)
				DecodeTag->shiftReg = 0;
				DecodeTag->bitCount = 0;
				DecodeTag->len = 0;
				DecodeTag->sum1 = amplitude;
				DecodeTag->sum2 = 0;
				DecodeTag->posCount = 2;
				DecodeTag->state = STATE_TAG_RECEIVING_DATA;
				FpgaDisableTracing(); // DEBUGGING
				Dbprintf("amplitude = %d, threshold_sof = %d, threshold_half/4 = %d, previous_amplitude = %d",
					amplitude,
					DecodeTag->threshold_sof,
					DecodeTag->threshold_half/4,
					DecodeTag->previous_amplitude); // DEBUGGING
				LED_C_ON();
			} else {
				DecodeTag->posCount++;
				if (DecodeTag->posCount > (recv_speed?13:52)) { // high phase too long
					DecodeTag->posCount = 0;
					DecodeTag->previous_amplitude = amplitude;
					DecodeTag->state = STATE_TAG_SOF_LOW;
					LED_C_OFF();
				}
			}
			break;

		case STATE_TAG_RECEIVING_DATA:
			if (DecodeTag->posCount == 1) {
				DecodeTag->sum1 = 0;
				DecodeTag->sum2 = 0;
			}
			if (DecodeTag->posCount <= (recv_speed?4:16)) {
				DecodeTag->sum1 += amplitude;
			} else {
				DecodeTag->sum2 += amplitude;
			}
			if (DecodeTag->posCount == (recv_speed?8:32)) {
				if (DecodeTag->sum1 > DecodeTag->threshold_half && DecodeTag->sum2 > DecodeTag->threshold_half) { // modulation in both halves
					if (DecodeTag->lastBit == LOGIC0) {  // this was already part of EOF
						DecodeTag->state = STATE_TAG_EOF;
					} else {
						DecodeTag->posCount = 0;
						DecodeTag->previous_amplitude = amplitude;
						DecodeTag->state = STATE_TAG_SOF_LOW;
						LED_C_OFF();
					}
				} else if (DecodeTag->sum1 < DecodeTag->threshold_half && DecodeTag->sum2 > DecodeTag->threshold_half) { // modulation in second half
					// logic 1
					if (DecodeTag->lastBit == SOF_PART1) { // still part of SOF
						DecodeTag->lastBit = SOF_PART2;    // SOF completed
					} else {
						DecodeTag->lastBit = LOGIC1;
						DecodeTag->shiftReg >>= 1;
						DecodeTag->shiftReg |= 0x80;
						DecodeTag->bitCount++;
						if (DecodeTag->bitCount == 8) {
							DecodeTag->output[DecodeTag->len] = DecodeTag->shiftReg;
							DecodeTag->len++;
							// if (DecodeTag->shiftReg == 0x12 && DecodeTag->len == 1) FpgaDisableTracing(); // DEBUGGING
							if (DecodeTag->len > DecodeTag->max_len) {
								// buffer overflow, give up
								LED_C_OFF();
								return true;
							}
							DecodeTag->bitCount = 0;
							DecodeTag->shiftReg = 0;
						}
					}
				} else if (DecodeTag->sum1 > DecodeTag->threshold_half && DecodeTag->sum2 < DecodeTag->threshold_half) { // modulation in first half
					// logic 0
					if (DecodeTag->lastBit == SOF_PART1) { // incomplete SOF
						DecodeTag->posCount = 0;
						DecodeTag->previous_amplitude = amplitude;
						DecodeTag->state = STATE_TAG_SOF_LOW;
						LED_C_OFF();
					} else {
						DecodeTag->lastBit = LOGIC0;
						DecodeTag->shiftReg >>= 1;
						DecodeTag->bitCount++;
						if (DecodeTag->bitCount == 8) {
							DecodeTag->output[DecodeTag->len] = DecodeTag->shiftReg;
							DecodeTag->len++;
							// if (DecodeTag->shiftReg == 0x12 && DecodeTag->len == 1) FpgaDisableTracing(); // DEBUGGING
							if (DecodeTag->len > DecodeTag->max_len) {
								// buffer overflow, give up
								DecodeTag->posCount = 0;
								DecodeTag->previous_amplitude = amplitude;
								DecodeTag->state = STATE_TAG_SOF_LOW;
								LED_C_OFF();
							}
							DecodeTag->bitCount = 0;
							DecodeTag->shiftReg = 0;
						}
					}
				} else { // no modulation
					if (DecodeTag->lastBit == SOF_PART2) { // only SOF (this is OK for iClass)
						LED_C_OFF();
						return true;
					} else {
						DecodeTag->posCount = 0;
						DecodeTag->state = STATE_TAG_SOF_LOW;
						LED_C_OFF();
					}
				}
				DecodeTag->posCount = 0;
			}
			DecodeTag->posCount++;
			break;

		case STATE_TAG_EOF:
			if (DecodeTag->posCount == 1) {
				DecodeTag->sum1 = 0;
				DecodeTag->sum2 = 0;
			}
			if (DecodeTag->posCount <= (recv_speed?4:16)) {
				DecodeTag->sum1 += amplitude;
			} else {
				DecodeTag->sum2 += amplitude;
			}
			if (DecodeTag->posCount == (recv_speed?8:32)) {
				if (DecodeTag->sum1 > DecodeTag->threshold_half && DecodeTag->sum2 < DecodeTag->threshold_half) { // modulation in first half
					DecodeTag->posCount = 0;
					DecodeTag->state = STATE_TAG_EOF_TAIL;
				} else {
					DecodeTag->posCount = 0;
					DecodeTag->previous_amplitude = amplitude;
					DecodeTag->state = STATE_TAG_SOF_LOW;
					LED_C_OFF();
				}
			}
			DecodeTag->posCount++;
			break;

		case STATE_TAG_EOF_TAIL:
			if (DecodeTag->posCount == 1) {
				DecodeTag->sum1 = 0;
				DecodeTag->sum2 = 0;
			}
			if (DecodeTag->posCount <= (recv_speed?4:16)) {
				DecodeTag->sum1 += amplitude;
			} else {
				DecodeTag->sum2 += amplitude;
			}
			if (DecodeTag->posCount == (recv_speed?8:32)) {
				if (DecodeTag->sum1 < DecodeTag->threshold_half && DecodeTag->sum2 < DecodeTag->threshold_half) { // no modulation in both halves
					LED_C_OFF();
					return true;
				} else {
					DecodeTag->posCount = 0;
					DecodeTag->previous_amplitude = amplitude;
					DecodeTag->state = STATE_TAG_SOF_LOW;
					LED_C_OFF();
				}
			}
			DecodeTag->posCount++;
			break;
	}

	return false;
}


static void DecodeTagInit(DecodeTag_t *DecodeTag, uint8_t *data, uint16_t max_len) {
	DecodeTag->previous_amplitude = MAX_PREVIOUS_AMPLITUDE;
	DecodeTag->posCount = 0;
	DecodeTag->state = STATE_TAG_SOF_LOW;
	DecodeTag->output = data;
	DecodeTag->max_len = max_len;
}


static void DecodeTagReset(DecodeTag_t *DecodeTag) {
	DecodeTag->posCount = 0;
	DecodeTag->state = STATE_TAG_SOF_LOW;
	DecodeTag->previous_amplitude = MAX_PREVIOUS_AMPLITUDE;
}


/*
 *  Receive and decode the tag response, also log to tracebuffer
 */
int GetIso15693AnswerFromTag(uint8_t* response, uint16_t max_len, uint16_t timeout, uint32_t *eof_time, bool recv_speed) {

	int samples = 0;
	int ret = 0;

	uint16_t dmaBuf[ISO15693_DMA_BUFFER_SIZE];

	// the Decoder data structure
	DecodeTag_t DecodeTag = { 0 };
	DecodeTagInit(&DecodeTag, response, max_len);

	// wait for last transfer to complete
	while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXEMPTY));

	// And put the FPGA in the appropriate mode
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_424_KHZ | FPGA_HF_READER_MODE_RECEIVE_AMPLITUDE);

	// Setup and start DMA.
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);
	FpgaSetupSscDma((uint8_t*) dmaBuf, ISO15693_DMA_BUFFER_SIZE);
	uint32_t dma_start_time = 0;
	uint16_t *upTo = dmaBuf;

	for(;;) {
		uint16_t behindBy = ((uint16_t*)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (ISO15693_DMA_BUFFER_SIZE-1);

		if (behindBy == 0) continue;

		samples++;
		if (samples == 1) {
			// DMA has transferred the very first data
			dma_start_time = GetCountSspClk() & 0xfffffff0;
		}

		uint16_t tagdata = *upTo++;

		if(upTo >= dmaBuf + ISO15693_DMA_BUFFER_SIZE) {                // we have read all of the DMA buffer content.
			upTo = dmaBuf;                                             // start reading the circular buffer from the beginning
			if(behindBy > (9*ISO15693_DMA_BUFFER_SIZE/10)) {
				Dbprintf("About to blow circular buffer - aborted! behindBy=%d", behindBy);
				ret = -1;
				break;
			}
		}
		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {              // DMA Counter Register had reached 0, already rotated.
			AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;          // refresh the DMA Next Buffer and
			AT91C_BASE_PDC_SSC->PDC_RNCR = ISO15693_DMA_BUFFER_SIZE;   // DMA Next Counter registers
		}

		if (Handle15693SamplesFromTag(tagdata, &DecodeTag, recv_speed)) {
			*eof_time = dma_start_time + samples*16 - DELAY_TAG_TO_ARM; // end of EOF
			if (DecodeTag.lastBit == SOF_PART2) {
				*eof_time -= 8*16; // needed 8 additional samples to confirm single SOF (iCLASS)
			}
			if (DecodeTag.len > DecodeTag.max_len) {
				ret = -2; // buffer overflow
			}
			break;
		}

		if (samples > timeout && DecodeTag.state < STATE_TAG_RECEIVING_DATA) {
			ret = -1;   // timeout
			break;
		}

	}

	FpgaDisableSscDma();

	if (DEBUG) Dbprintf("samples = %d, ret = %d, Decoder: state = %d, lastBit = %d, len = %d, bitCount = %d, posCount = %d",
						samples, ret, DecodeTag.state, DecodeTag.lastBit, DecodeTag.len, DecodeTag.bitCount, DecodeTag.posCount);

	if (ret < 0) {
		return ret;
	}

	uint32_t sof_time = *eof_time
						- DecodeTag.len * 8 * 8 * 16 // time for byte transfers
						- 32 * 16  // time for SOF transfer
						- (DecodeTag.lastBit != SOF_PART2?32*16:0); // time for EOF transfer

	if (DEBUG) Dbprintf("timing: sof_time = %d, eof_time = %d", sof_time, *eof_time);

	LogTrace_ISO15693(DecodeTag.output, DecodeTag.len, sof_time*4, *eof_time*4, NULL, false);

	return DecodeTag.len;
}

//=============================================================================
// An ISO 15693 decoder for tag responses in FSK (two subcarriers) mode.
// Subcarriers frequencies are 424kHz and 484kHz (fc/32 and fc/28),
// LED handling:
//    LED C -> ON once we have received the SOF and are expecting the rest.
//    LED C -> OFF once we have received EOF or are unsynced
//
// Returns: true if we received a EOF
//          false if we are still waiting for some more
//=============================================================================

#define FREQ_IS_484(f)    (f >= 26 && f <= 30)
#define FREQ_IS_424(f)    (f >= 30 && f <= 34)
#define SEOF_COUNT(c, s)  ((s) ? (c >= 11 && c <= 13) : (c >= 44 && c <= 52))
#define LOGIC_COUNT(c, s) ((s) ? (c >= 3 && c <= 6) : (c >= 13 && c <= 21))
#define MAX_COUNT(c, s)   ((s) ? (c >= 13) : (c >= 52))
#define MIN_COUNT(c, s)   ((s) ? (c <= 2) : (c <= 4))

typedef struct DecodeTagFSK {
	enum {
		STATE_FSK_BEFORE_SOF,
		STATE_FSK_SOF_484,
		STATE_FSK_SOF_424,
		STATE_FSK_SOF_END,
		STATE_FSK_RECEIVING_DATA_484,
		STATE_FSK_RECEIVING_DATA_424,
		STATE_FSK_EOF,
		STATE_FSK_ERROR
	}        state;
	enum {
		LOGIC0_PART1,
		LOGIC1_PART1,
		LOGIC0_PART2,
		LOGIC1_PART2,
		SOF
	}        lastBit;
	uint8_t  count;
	uint8_t  bitCount;
	uint8_t  shiftReg;
	uint16_t len;
	uint16_t max_len;
	uint8_t  *output;
} DecodeTagFSK_t;

static void DecodeTagFSKReset(DecodeTagFSK_t *DecodeTag) {
	DecodeTag->state = STATE_FSK_BEFORE_SOF;
	DecodeTag->bitCount = 0;
	DecodeTag->len = 0;
	DecodeTag->shiftReg = 0;
}

static void DecodeTagFSKInit(DecodeTagFSK_t *DecodeTag, uint8_t *data, uint16_t max_len) {
	DecodeTag->output = data;
	DecodeTag->max_len = max_len;
	DecodeTagFSKReset(DecodeTag);
}

// Performances of this function are crutial for stability
// as it is called in real time for every samples
static int inline __attribute__((always_inline)) Handle15693FSKSamplesFromTag(uint8_t freq, DecodeTagFSK_t *DecodeTag, bool recv_speed)
{
	switch(DecodeTag->state) {
		case STATE_FSK_BEFORE_SOF:
			if (FREQ_IS_484(freq))
			{ // possible SOF starting
				DecodeTag->state = STATE_FSK_SOF_484;
				DecodeTag->lastBit = LOGIC0_PART1;
				DecodeTag->count = 1;
			}
			break;

		case STATE_FSK_SOF_484:
			if (FREQ_IS_484(freq) && !MAX_COUNT(DecodeTag->count, recv_speed)) // still in SOF at 484
				DecodeTag->count++;
			else if (FREQ_IS_424(freq) && SEOF_COUNT(DecodeTag->count, recv_speed))
			{ // SOF part1 continue at 424
				DecodeTag->state = STATE_FSK_SOF_424;
				DecodeTag->count = 1;
			}
			else // SOF failed, roll back
				DecodeTag->state = STATE_FSK_BEFORE_SOF;
			break;

		case STATE_FSK_SOF_424:
			if (FREQ_IS_424(freq) && !MAX_COUNT(DecodeTag->count, recv_speed)) // still in SOF at 424
				DecodeTag->count++;
			else if (FREQ_IS_484(freq) && SEOF_COUNT(DecodeTag->count, recv_speed))
			{ // SOF part 1 finished
				DecodeTag->state = STATE_FSK_SOF_END;
				DecodeTag->count = 1;
			}
			else // SOF failed, roll back
				DecodeTag->state = STATE_FSK_BEFORE_SOF;
			break;

		case STATE_FSK_SOF_END:
			if (FREQ_IS_484(freq) && !MAX_COUNT(DecodeTag->count, recv_speed))  // still in SOF_END (484)
				DecodeTag->count++;
			else if (FREQ_IS_424(freq) && LOGIC_COUNT(DecodeTag->count, recv_speed))
			{ // SOF END finished or SOF END 1st part finished
				DecodeTag->count = 0;
				if (DecodeTag->lastBit == SOF)
				{ // SOF finished at 424
					DecodeTag->state = STATE_FSK_RECEIVING_DATA_424;
					LED_C_ON();
				}
				DecodeTag->lastBit = SOF;
			}
			else if (FREQ_IS_424(freq) && !MAX_COUNT(DecodeTag->count, recv_speed)) // still in SOF_END (424)
				DecodeTag->count++;
			else if (DecodeTag->lastBit == SOF && FREQ_IS_484(freq) &&
					 LOGIC_COUNT(DecodeTag->count, recv_speed))
			{ // SOF finished at 484
				DecodeTag->state = STATE_FSK_RECEIVING_DATA_484;
				DecodeTag->count = 1;
				LED_C_ON();
			}
			else // SOF failed, roll back
				DecodeTag->state = STATE_FSK_BEFORE_SOF;
			break;


		case STATE_FSK_RECEIVING_DATA_424:
			if (DecodeTag->lastBit == LOGIC1_PART1 &&
				LOGIC_COUNT(DecodeTag->count, recv_speed))
			{ // logic 1 finished
				DecodeTag->lastBit = LOGIC1_PART2;
				DecodeTag->count = 0;

				DecodeTag->shiftReg >>= 1;
				DecodeTag->shiftReg |= 0x80;
				DecodeTag->bitCount++;
				if (DecodeTag->bitCount == 8) {
					DecodeTag->output[DecodeTag->len++] = DecodeTag->shiftReg;
					if (DecodeTag->len > DecodeTag->max_len) {
						// buffer overflow, give up
						LED_C_OFF();
						return true;
					}
					DecodeTag->bitCount = 0;
					DecodeTag->shiftReg = 0;
				}
			}
			else if (FREQ_IS_424(freq) && !MAX_COUNT(DecodeTag->count, recv_speed)) // still at 424
				DecodeTag->count++;
			else if (FREQ_IS_484(freq) && LOGIC_COUNT(DecodeTag->count, recv_speed) &&
					 DecodeTag->lastBit >= LOGIC0_PART2)
			{ // end of LOGIC0_PART1
				DecodeTag->count = 1;
				DecodeTag->state = STATE_FSK_RECEIVING_DATA_484;
				DecodeTag->lastBit = LOGIC0_PART1;
			}
			else if (FREQ_IS_484(freq) && MIN_COUNT(DecodeTag->count, recv_speed))
			{ // it was just the end of the previous block
				DecodeTag->count = 1;
				DecodeTag->state = STATE_FSK_RECEIVING_DATA_484;
			}
			else if (FREQ_IS_484(freq) && DecodeTag->lastBit == LOGIC0_PART2 &&
					 SEOF_COUNT(DecodeTag->count, recv_speed))
			{ // EOF has started
				DecodeTag->count = 1;
				DecodeTag->state = STATE_FSK_EOF;
				LED_C_OFF();
			}
			else // error
			{
				DecodeTag->state = STATE_FSK_ERROR;
				LED_C_OFF();
				return true;
			}
			break;

		case STATE_FSK_RECEIVING_DATA_484:
			if (DecodeTag->lastBit == LOGIC0_PART1 &&
				LOGIC_COUNT(DecodeTag->count, recv_speed))
			{ // logic 0 finished
				DecodeTag->lastBit = LOGIC0_PART2;
				DecodeTag->count = 0;

				DecodeTag->shiftReg >>= 1;
				DecodeTag->bitCount++;
				if (DecodeTag->bitCount == 8) {
					DecodeTag->output[DecodeTag->len++] = DecodeTag->shiftReg;
					if (DecodeTag->len > DecodeTag->max_len) {
						// buffer overflow, give up
						LED_C_OFF();
						return true;
					}
					DecodeTag->bitCount = 0;
					DecodeTag->shiftReg = 0;
				}
			}
			else if (FREQ_IS_484(freq) && !MAX_COUNT(DecodeTag->count, recv_speed)) // still at 484
				DecodeTag->count++;
			else if (FREQ_IS_424(freq) && LOGIC_COUNT(DecodeTag->count, recv_speed) &&
					 DecodeTag->lastBit >= LOGIC0_PART2)
			{ // end of LOGIC1_PART1
				DecodeTag->count = 1;
				DecodeTag->state = STATE_FSK_RECEIVING_DATA_424;
				DecodeTag->lastBit = LOGIC1_PART1;
			}
			else if (FREQ_IS_424(freq) && MIN_COUNT(DecodeTag->count, recv_speed))
			{ // it was just the end of the previous block
				DecodeTag->count = 1;
				DecodeTag->state = STATE_FSK_RECEIVING_DATA_424;
			}
			else // error
			{
				LED_C_OFF();
				DecodeTag->state = STATE_FSK_ERROR;
				return true;
			}
			break;

		case STATE_FSK_EOF:
			if (FREQ_IS_484(freq) && !MAX_COUNT(DecodeTag->count, recv_speed)) // still at 484
			{
				DecodeTag->count++;
				if (SEOF_COUNT(DecodeTag->count, recv_speed))
					return true; // end of the transmission
			}
			else // error
			{
				DecodeTag->state = STATE_FSK_ERROR;
				return true;
			}
			break;
		case STATE_FSK_ERROR:
			LED_C_OFF();
			return true; // error
			break;
	}
	return false;
}

int GetIso15693AnswerFromTagFSK(uint8_t* response, uint16_t max_len, uint16_t timeout, uint32_t *eof_time, bool recv_speed) {

	int samples = 0;
	int ret = 0;

	uint8_t dmaBuf[ISO15693_DMA_BUFFER_SIZE];

	// the Decoder data structure
	DecodeTagFSK_t DecodeTag = { 0 };
	DecodeTagFSKInit(&DecodeTag, response, max_len);

	// wait for last transfer to complete
	while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXEMPTY));

	// And put the FPGA in the appropriate mode
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_FSK_READER | FPGA_HF_FSK_READER_OUTPUT_212_KHZ);

	// Setup and start DMA.
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_FSK_READER);
	FpgaSetupSscDma((uint8_t*) dmaBuf, ISO15693_DMA_BUFFER_SIZE);
	uint32_t dma_start_time = 0;
	uint8_t *upTo = dmaBuf;

	for(;;) {
		uint8_t behindBy = ((uint8_t*)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (ISO15693_DMA_BUFFER_SIZE-1);

		if (behindBy == 0) continue;

		samples++;
		if (samples == 1) {
			// DMA has transferred the very first data
			dma_start_time = GetCountSspClk() & 0xfffffff0;
		}

		uint8_t tagdata = *upTo++;

		if(upTo >= dmaBuf + ISO15693_DMA_BUFFER_SIZE) {                // we have read all of the DMA buffer content.
			upTo = dmaBuf;                                             // start reading the circular buffer from the beginning
			if(behindBy > (9*ISO15693_DMA_BUFFER_SIZE/10)) {
				Dbprintf("About to blow circular buffer - aborted! behindBy=%d", behindBy);
				ret = -1;
				break;
			}
		}
		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {              // DMA Counter Register had reached 0, already rotated.
			AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;          // refresh the DMA Next Buffer and
			AT91C_BASE_PDC_SSC->PDC_RNCR = ISO15693_DMA_BUFFER_SIZE;   // DMA Next Counter registers
		}

		if (Handle15693FSKSamplesFromTag(tagdata, &DecodeTag, recv_speed)) {
			*eof_time = dma_start_time + samples*16 - DELAY_TAG_TO_ARM; // end of EOF
			if (DecodeTag.lastBit == SOF) {
				*eof_time -= 8*16; // needed 8 additional samples to confirm single SOF (iCLASS)
			}
			if (DecodeTag.len > DecodeTag.max_len) {
				ret = -2; // buffer overflow
			}
			break;
		}

		if (samples > timeout && DecodeTag.state < STATE_FSK_RECEIVING_DATA_484) {
			ret = -1;   // timeout
			break;
		}
	}

	FpgaDisableSscDma();

	if (DEBUG) Dbprintf("samples = %d, ret = %d, Decoder: state = %d, lastBit = %d, len = %d, bitCount = %d, count = %d",
						samples, ret, DecodeTag.state, DecodeTag.lastBit, DecodeTag.len, DecodeTag.bitCount, DecodeTag.count);

	if (ret < 0) {
		return ret;
	}

	uint32_t sof_time = *eof_time
						- DecodeTag.len * 8 * 8 * 16 // time for byte transfers
						- 32 * 16  // time for SOF transfer
						- (DecodeTag.lastBit != SOF?32*16:0); // time for EOF transfer

	if (DEBUG) Dbprintf("timing: sof_time = %d, eof_time = %d", sof_time, *eof_time);

	LogTrace_ISO15693(DecodeTag.output, DecodeTag.len, sof_time*4, *eof_time*4, NULL, false);

	return DecodeTag.len;

	return 1;
}


//=============================================================================
// An ISO15693 decoder for reader commands.
//
// This function is called 4 times per bit (every 2 subcarrier cycles).
// Subcarrier frequency fs is 848kHz, 1/fs = 1,18us, i.e. function is called every 2,36us
// LED handling:
//    LED B -> ON once we have received the SOF and are expecting the rest.
//    LED B -> OFF once we have received EOF or are in error state or unsynced
//
// Returns: true  if we received a EOF
//          false if we are still waiting for some more
//=============================================================================

typedef struct DecodeReader {
	enum {
		STATE_READER_UNSYNCD,
		STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF,
		STATE_READER_AWAIT_1ST_RISING_EDGE_OF_SOF,
		STATE_READER_AWAIT_2ND_FALLING_EDGE_OF_SOF,
		STATE_READER_AWAIT_2ND_RISING_EDGE_OF_SOF,
		STATE_READER_AWAIT_END_OF_SOF_1_OUT_OF_4,
		STATE_READER_RECEIVE_DATA_1_OUT_OF_4,
		STATE_READER_RECEIVE_DATA_1_OUT_OF_256
	}           state;
	enum {
		CODING_1_OUT_OF_4,
		CODING_1_OUT_OF_256
	}           Coding;
	uint8_t     shiftReg;
	uint8_t     bitCount;
	int         byteCount;
	int         byteCountMax;
	int         posCount;
	int         sum1, sum2;
	uint8_t     *output;
} DecodeReader_t;


static void DecodeReaderInit(DecodeReader_t* DecodeReader, uint8_t *data, uint16_t max_len)
{
	DecodeReader->output = data;
	DecodeReader->byteCountMax = max_len;
	DecodeReader->state = STATE_READER_UNSYNCD;
	DecodeReader->byteCount = 0;
	DecodeReader->bitCount = 0;
	DecodeReader->posCount = 1;
	DecodeReader->shiftReg = 0;
}


static void DecodeReaderReset(DecodeReader_t* DecodeReader)
{
	DecodeReader->state = STATE_READER_UNSYNCD;
}


static int inline __attribute__((always_inline)) Handle15693SampleFromReader(uint8_t bit, DecodeReader_t *restrict DecodeReader)
{
	switch (DecodeReader->state) {
		case STATE_READER_UNSYNCD:
			// wait for unmodulated carrier
			if (bit) {
				DecodeReader->state = STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF;
			}
			break;

		case STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF:
			if (!bit) {
				// we went low, so this could be the beginning of a SOF
				DecodeReader->posCount = 1;
				DecodeReader->state = STATE_READER_AWAIT_1ST_RISING_EDGE_OF_SOF;
			}
			break;

		case STATE_READER_AWAIT_1ST_RISING_EDGE_OF_SOF:
			DecodeReader->posCount++;
			if (bit) { // detected rising edge
				if (DecodeReader->posCount < 4) { // rising edge too early (nominally expected at 5)
					DecodeReader->state = STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF;
				} else { // SOF
					DecodeReader->state = STATE_READER_AWAIT_2ND_FALLING_EDGE_OF_SOF;
				}
			} else {
				if (DecodeReader->posCount > 5) { // stayed low for too long
					DecodeReaderReset(DecodeReader);
				} else {
					// do nothing, keep waiting
				}
			}
			break;

		case STATE_READER_AWAIT_2ND_FALLING_EDGE_OF_SOF:
			DecodeReader->posCount++;
			if (!bit) { // detected a falling edge
				if (DecodeReader->posCount < 20) {         // falling edge too early (nominally expected at 21 earliest)
					DecodeReaderReset(DecodeReader);
				} else if (DecodeReader->posCount < 23) {  // SOF for 1 out of 4 coding
					DecodeReader->Coding = CODING_1_OUT_OF_4;
					DecodeReader->state = STATE_READER_AWAIT_2ND_RISING_EDGE_OF_SOF;
				} else if (DecodeReader->posCount < 28) {  // falling edge too early (nominally expected at 29 latest)
					DecodeReaderReset(DecodeReader);
				} else {                                   // SOF for 1 out of 256 coding
					DecodeReader->Coding = CODING_1_OUT_OF_256;
					DecodeReader->state = STATE_READER_AWAIT_2ND_RISING_EDGE_OF_SOF;
				}
			} else {
				if (DecodeReader->posCount > 29) { // stayed high for too long
					DecodeReader->state = STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF;
				} else {
					// do nothing, keep waiting
				}
			}
			break;

		case STATE_READER_AWAIT_2ND_RISING_EDGE_OF_SOF:
			DecodeReader->posCount++;
			if (bit) { // detected rising edge
				if (DecodeReader->Coding == CODING_1_OUT_OF_256) {
					if (DecodeReader->posCount < 32) { // rising edge too early (nominally expected at 33)
						DecodeReader->state = STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF;
					} else {
						DecodeReader->posCount = 1;
						DecodeReader->bitCount = 0;
						DecodeReader->byteCount = 0;
						DecodeReader->sum1 = 1;
						DecodeReader->state = STATE_READER_RECEIVE_DATA_1_OUT_OF_256;
						LED_B_ON();
					}
				} else { // CODING_1_OUT_OF_4
					if (DecodeReader->posCount < 24) { // rising edge too early (nominally expected at 25)
						DecodeReader->state = STATE_READER_AWAIT_1ST_FALLING_EDGE_OF_SOF;
					} else {
						DecodeReader->posCount = 1;
						DecodeReader->state = STATE_READER_AWAIT_END_OF_SOF_1_OUT_OF_4;
					}
				}
			} else {
				if (DecodeReader->Coding == CODING_1_OUT_OF_256) {
					if (DecodeReader->posCount > 34) { // signal stayed low for too long
						DecodeReaderReset(DecodeReader);
					} else {
						// do nothing, keep waiting
					}
				} else { // CODING_1_OUT_OF_4
					if (DecodeReader->posCount > 26) { // signal stayed low for too long
						DecodeReaderReset(DecodeReader);
					} else {
						// do nothing, keep waiting
					}
				}
			}
			break;

		case STATE_READER_AWAIT_END_OF_SOF_1_OUT_OF_4:
			DecodeReader->posCount++;
			if (bit) {
				if (DecodeReader->posCount == 9) {
					DecodeReader->posCount = 1;
					DecodeReader->bitCount = 0;
					DecodeReader->byteCount = 0;
					DecodeReader->sum1 = 1;
					DecodeReader->state = STATE_READER_RECEIVE_DATA_1_OUT_OF_4;
					LED_B_ON();
				} else {
					// do nothing, keep waiting
				}
			} else { // unexpected falling edge
					DecodeReaderReset(DecodeReader);
			}
			break;

		case STATE_READER_RECEIVE_DATA_1_OUT_OF_4:
			bit = !!bit;
			DecodeReader->posCount++;
			if (DecodeReader->posCount == 1) {
				DecodeReader->sum1 = bit;
			} else if (DecodeReader->posCount <= 4) {
				DecodeReader->sum1 += bit;
			} else if (DecodeReader->posCount == 5) {
				DecodeReader->sum2 = bit;
			} else {
				DecodeReader->sum2 += bit;
			}
			if (DecodeReader->posCount == 8) {
				DecodeReader->posCount = 0;
				if (DecodeReader->sum1 <= 1 && DecodeReader->sum2 >= 3) { // EOF
					LED_B_OFF(); // Finished receiving
					DecodeReaderReset(DecodeReader);
					if (DecodeReader->byteCount != 0) {
						return true;
					}
				}
				if (DecodeReader->sum1 >= 3 && DecodeReader->sum2 <= 1) { // detected a 2bit position
					DecodeReader->shiftReg >>= 2;
					DecodeReader->shiftReg |= (DecodeReader->bitCount << 6);
				}
				if (DecodeReader->bitCount == 15) { // we have a full byte
					DecodeReader->output[DecodeReader->byteCount++] = DecodeReader->shiftReg;
					if (DecodeReader->byteCount > DecodeReader->byteCountMax) {
						// buffer overflow, give up
						LED_B_OFF();
						DecodeReaderReset(DecodeReader);
					}
					DecodeReader->bitCount = 0;
					DecodeReader->shiftReg = 0;
				} else {
					DecodeReader->bitCount++;
				}
			}
			break;

		case STATE_READER_RECEIVE_DATA_1_OUT_OF_256:
			bit = !!bit;
			DecodeReader->posCount++;
			if (DecodeReader->posCount == 1) {
				DecodeReader->sum1 = bit;
			} else if (DecodeReader->posCount <= 4) {
				DecodeReader->sum1 += bit;
			} else if (DecodeReader->posCount == 5) {
				DecodeReader->sum2 = bit;
			} else {
				DecodeReader->sum2 += bit;
			}
			if (DecodeReader->posCount == 8) {
				DecodeReader->posCount = 0;
				if (DecodeReader->sum1 <= 1 && DecodeReader->sum2 >= 3) { // EOF
					LED_B_OFF(); // Finished receiving
					DecodeReaderReset(DecodeReader);
					if (DecodeReader->byteCount != 0) {
						return true;
					}
				}
				if (DecodeReader->sum1 >= 3 && DecodeReader->sum2 <= 1) { // detected the bit position
					DecodeReader->shiftReg = DecodeReader->bitCount;
				}
				if (DecodeReader->bitCount == 255) { // we have a full byte
					DecodeReader->output[DecodeReader->byteCount++] = DecodeReader->shiftReg;
					if (DecodeReader->byteCount > DecodeReader->byteCountMax) {
						// buffer overflow, give up
						LED_B_OFF();
						DecodeReaderReset(DecodeReader);
					}
				}
				DecodeReader->bitCount++;
			}
			break;

		default:
			LED_B_OFF();
			DecodeReaderReset(DecodeReader);
			break;
	}

	return false;
}


//-----------------------------------------------------------------------------
// Receive a command (from the reader to us, where we are the simulated tag),
// and store it in the given buffer, up to the given maximum length. Keeps
// spinning, waiting for a well-framed command, until either we get one
// (returns len) or someone presses the pushbutton on the board (returns -1).
//
// Assume that we're called with the SSC (to the FPGA) and ADC path set
// correctly.
//-----------------------------------------------------------------------------

int GetIso15693CommandFromReader(uint8_t *received, size_t max_len, uint32_t *eof_time) {
	int samples = 0;
	bool gotFrame = false;
	uint8_t b;

	uint8_t dmaBuf[ISO15693_DMA_BUFFER_SIZE];

	// the decoder data structure
	DecodeReader_t DecodeReader = {0};
	DecodeReaderInit(&DecodeReader, received, max_len);

	// wait for last transfer to complete
	while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXEMPTY));

	LED_D_OFF();
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_NO_MODULATION);

	// clear receive register and wait for next transfer
	uint32_t temp = AT91C_BASE_SSC->SSC_RHR;
	(void) temp;
	while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY)) ;

	uint32_t dma_start_time = GetCountSspClk() & 0xfffffff8;

	// Setup and start DMA.
	FpgaSetupSscDma(dmaBuf, ISO15693_DMA_BUFFER_SIZE);
	uint8_t *upTo = dmaBuf;

	for (;;) {
		uint16_t behindBy = ((uint8_t*)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (ISO15693_DMA_BUFFER_SIZE-1);

		if (behindBy == 0) continue;

		b = *upTo++;
		if (upTo >= dmaBuf + ISO15693_DMA_BUFFER_SIZE) {               // we have read all of the DMA buffer content.
			upTo = dmaBuf;                                             // start reading the circular buffer from the beginning
			if (behindBy > (9*ISO15693_DMA_BUFFER_SIZE/10)) {
				Dbprintf("About to blow circular buffer - aborted! behindBy=%d", behindBy);
				break;
			}
		}
		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {              // DMA Counter Register had reached 0, already rotated.
			AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;          // refresh the DMA Next Buffer and
			AT91C_BASE_PDC_SSC->PDC_RNCR = ISO15693_DMA_BUFFER_SIZE;   // DMA Next Counter registers
		}

		for (int i = 7; i >= 0; i--) {
			if (Handle15693SampleFromReader((b >> i) & 0x01, &DecodeReader)) {
				*eof_time = dma_start_time + samples - DELAY_READER_TO_ARM; // end of EOF
				gotFrame = true;
				break;
			}
			samples++;
		}

		if (gotFrame) {
			break;
		}

		if (BUTTON_PRESS()) {
			DecodeReader.byteCount = -1;
			break;
		}

		WDT_HIT();
	}

	FpgaDisableSscDma();

	if (DEBUG) Dbprintf("samples = %d, gotFrame = %d, Decoder: state = %d, len = %d, bitCount = %d, posCount = %d",
						samples, gotFrame, DecodeReader.state, DecodeReader.byteCount, DecodeReader.bitCount, DecodeReader.posCount);

	if (DecodeReader.byteCount > 0) {
		uint32_t sof_time = *eof_time
						- DecodeReader.byteCount * (DecodeReader.Coding==CODING_1_OUT_OF_4?128:2048) // time for byte transfers
						- 32  // time for SOF transfer
						- 16; // time for EOF transfer
		LogTrace_ISO15693(DecodeReader.output, DecodeReader.byteCount, sof_time*32, *eof_time*32, NULL, true);
	}

	return DecodeReader.byteCount;
}


// Encode (into the ToSend buffers) an identify request, which is the first
// thing that you must send to a tag to get a response.
static void BuildIdentifyRequest(void)
{
	uint8_t cmd[5];

	uint16_t crc;
	// one sub-carrier, inventory, 1 slot, fast rate
	// AFI is at bit 5 (1<<4) when doing an INVENTORY
	cmd[0] = (1 << 2) | (1 << 5) | (1 << 1);
	// inventory command code
	cmd[1] = 0x01;
	// no mask
	cmd[2] = 0x00;
	//Now the CRC
	crc = Iso15693Crc(cmd, 3);
	cmd[3] = crc & 0xff;
	cmd[4] = crc >> 8;

	CodeIso15693AsReader(cmd, sizeof(cmd));
}


//-----------------------------------------------------------------------------
// Start to read an ISO 15693 tag. We send an identify request, then wait
// for the response. The response is not demodulated, just left in the buffer
// so that it can be downloaded to a PC and processed there.
//-----------------------------------------------------------------------------
void AcquireRawAdcSamplesIso15693(void)
{
	LED_A_ON();

	uint8_t *dest = BigBuf_get_addr();

	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER);
	LED_D_ON();
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);
	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

	BuildIdentifyRequest();

	// Give the tags time to energize
	SpinDelay(100);

	// Now send the command
	uint32_t start_time = 0;
	TransmitTo15693Tag(ToSend, ToSendMax, &start_time);

	// wait for last transfer to complete
	while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXEMPTY)) ;

	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_424_KHZ | FPGA_HF_READER_MODE_RECEIVE_AMPLITUDE);

	for(int c = 0; c < 4000; ) {
		if(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
			uint16_t r = AT91C_BASE_SSC->SSC_RHR;
			dest[c++] = r >> 5;
		}
	}

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LEDsoff();
}


void SnoopIso15693(void)
{
	LED_A_ON();
	
	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
	BigBuf_free();

	clear_trace();
	set_tracing(true);

	// The DMA buffer, used to stream samples from the FPGA
	uint16_t* dmaBuf = (uint16_t*)BigBuf_malloc(ISO15693_DMA_BUFFER_SIZE*sizeof(uint16_t));
	uint16_t *upTo;

	// Count of samples received so far, so that we can include timing
	// information in the trace buffer.
	int samples = 0;

	DecodeTag_t DecodeTag = {0};
	uint8_t response[ISO15693_MAX_RESPONSE_LENGTH];
	DecodeTagInit(&DecodeTag, response, sizeof(response));

	DecodeReader_t DecodeReader = {0};;
	uint8_t cmd[ISO15693_MAX_COMMAND_LENGTH];
	DecodeReaderInit(&DecodeReader, cmd, sizeof(cmd));

	// Print some debug information about the buffer sizes
	if (DEBUG) {
		Dbprintf("Snooping buffers initialized:");
		Dbprintf("  Trace:         %i bytes", BigBuf_max_traceLen());
		Dbprintf("  Reader -> tag: %i bytes", ISO15693_MAX_COMMAND_LENGTH);
		Dbprintf("  tag -> Reader: %i bytes", ISO15693_MAX_RESPONSE_LENGTH);
		Dbprintf("  DMA:           %i bytes", ISO15693_DMA_BUFFER_SIZE * sizeof(uint16_t));
	}
	Dbprintf("Snoop started. Press PM3 Button to stop.");

	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SNOOP_AMPLITUDE);
	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

	// Setup for the DMA.
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);
	upTo = dmaBuf;
	FpgaSetupSscDma((uint8_t*) dmaBuf, ISO15693_DMA_BUFFER_SIZE);

	bool TagIsActive = false;
	bool ReaderIsActive = false;
	bool ExpectTagAnswer = false;

	// And now we loop, receiving samples.
	for(;;) {
		uint16_t behindBy = ((uint16_t*)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (ISO15693_DMA_BUFFER_SIZE-1);

		if (behindBy == 0) continue;

		uint16_t snoopdata = *upTo++;

		if(upTo >= dmaBuf + ISO15693_DMA_BUFFER_SIZE) {                    // we have read all of the DMA buffer content.
			upTo = dmaBuf;                                                 // start reading the circular buffer from the beginning
			if(behindBy > (9*ISO15693_DMA_BUFFER_SIZE/10)) {
				Dbprintf("About to blow circular buffer - aborted! behindBy=%d, samples=%d", behindBy, samples);
				break;
			}
			if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {              // DMA Counter Register had reached 0, already rotated.
				AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;          // refresh the DMA Next Buffer and
				AT91C_BASE_PDC_SSC->PDC_RNCR = ISO15693_DMA_BUFFER_SIZE;   // DMA Next Counter registers
				WDT_HIT();
				if(BUTTON_PRESS()) {
					DbpString("Snoop stopped.");
					break;
				}
			}
		}
		samples++;

		if (!TagIsActive) {                                            // no need to try decoding reader data if the tag is sending
			if (Handle15693SampleFromReader(snoopdata & 0x02, &DecodeReader)) {
				FpgaDisableSscDma();
				ExpectTagAnswer = true;
				LogTrace_ISO15693(DecodeReader.output, DecodeReader.byteCount, samples*64, samples*64, NULL, true);
				/* And ready to receive another command. */
				DecodeReaderReset(&DecodeReader);
				/* And also reset the demod code, which might have been */
				/* false-triggered by the commands from the reader. */
				DecodeTagReset(&DecodeTag);
				upTo = dmaBuf;
				FpgaSetupSscDma((uint8_t*) dmaBuf, ISO15693_DMA_BUFFER_SIZE);
			}
			if (Handle15693SampleFromReader(snoopdata & 0x01, &DecodeReader)) {
				FpgaDisableSscDma();
				ExpectTagAnswer = true;
				LogTrace_ISO15693(DecodeReader.output, DecodeReader.byteCount, samples*64, samples*64, NULL, true);
				/* And ready to receive another command. */
				DecodeReaderReset(&DecodeReader);
				/* And also reset the demod code, which might have been */
				/* false-triggered by the commands from the reader. */
				DecodeTagReset(&DecodeTag);
				upTo = dmaBuf;
				FpgaSetupSscDma((uint8_t*) dmaBuf, ISO15693_DMA_BUFFER_SIZE);
			}
			ReaderIsActive = (DecodeReader.state >= STATE_READER_AWAIT_2ND_RISING_EDGE_OF_SOF);
		}

		if (!ReaderIsActive && ExpectTagAnswer) {                       // no need to try decoding tag data if the reader is currently sending or no answer expected yet
			if (Handle15693SamplesFromTag(snoopdata >> 2, &DecodeTag, true)) {
				FpgaDisableSscDma();
				//Use samples as a time measurement
				LogTrace_ISO15693(DecodeTag.output, DecodeTag.len, samples*64, samples*64, NULL, false);
				// And ready to receive another response.
				DecodeTagReset(&DecodeTag);
				DecodeReaderReset(&DecodeReader);
				ExpectTagAnswer = false;
				upTo = dmaBuf;
				FpgaSetupSscDma((uint8_t*) dmaBuf, ISO15693_DMA_BUFFER_SIZE);
			}
			TagIsActive = (DecodeTag.state >= STATE_TAG_RECEIVING_DATA);
		}

	}

	FpgaDisableSscDma();
	BigBuf_free();

	LEDsoff();

	DbpString("Snoop statistics:");
	Dbprintf("  ExpectTagAnswer: %d", ExpectTagAnswer);
	Dbprintf("  DecodeTag State: %d", DecodeTag.state);
	Dbprintf("  DecodeTag byteCnt: %d", DecodeTag.len);
	Dbprintf("  DecodeReader State: %d", DecodeReader.state);
	Dbprintf("  DecodeReader byteCnt: %d", DecodeReader.byteCount);
	Dbprintf("  Trace length: %d", BigBuf_get_traceLen());
}


// Initialize the proxmark as iso15k reader
void Iso15693InitReader() {
	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

	// Start from off (no field generated)
	LED_D_OFF();
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	SpinDelay(10);

	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

	// Give the tags time to energize
	LED_D_ON();
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER);
	SpinDelay(250);
}

///////////////////////////////////////////////////////////////////////
// ISO 15693 Part 3 - Air Interface
// This section basically contains transmission and receiving of bits
///////////////////////////////////////////////////////////////////////


// uid is in transmission order (which is reverse of display order)
static void BuildReadBlockRequest(uint8_t *uid, uint8_t blockNumber )
{
	uint8_t cmd[13];

	uint16_t crc;
	// If we set the Option_Flag in this request, the VICC will respond with the security status of the block
	// followed by the block data
	cmd[0] = ISO15693_REQ_OPTION | ISO15693_REQ_ADDRESS | ISO15693_REQ_DATARATE_HIGH;
	// READ BLOCK command code
	cmd[1] = ISO15693_READBLOCK;
	// UID may be optionally specified here
	// 64-bit UID
	cmd[2] = uid[0];
	cmd[3] = uid[1];
	cmd[4] = uid[2];
	cmd[5] = uid[3];
	cmd[6] = uid[4];
	cmd[7] = uid[5];
	cmd[8] = uid[6];
	cmd[9] = uid[7]; // 0xe0; // always e0 (not exactly unique)
	// Block number to read
	cmd[10] = blockNumber;
	//Now the CRC
	crc = Iso15693Crc(cmd, 11); // the crc needs to be calculated over 11 bytes
	cmd[11] = crc & 0xff;
	cmd[12] = crc >> 8;

	CodeIso15693AsReader(cmd, sizeof(cmd));
}

/*
// Now the VICC>VCD responses when we are simulating a tag
static void BuildInventoryResponse(uint8_t *uid)
{
	uint8_t cmd[12];

	uint16_t crc;

	cmd[0] = 0; // No error, no protocol format extension
	cmd[1] = 0; // DSFID (data storage format identifier).  0x00 = not supported
	// 64-bit UID
	cmd[2] = uid[7]; //0x32;
	cmd[3] = uid[6]; //0x4b;
	cmd[4] = uid[5]; //0x03;
	cmd[5] = uid[4]; //0x01;
	cmd[6] = uid[3]; //0x00;
	cmd[7] = uid[2]; //0x10;
	cmd[8] = uid[1]; //0x05;
	cmd[9] = uid[0]; //0xe0;
	//Now the CRC
	crc = Iso15693Crc(cmd, 10);
	cmd[10] = crc & 0xff;
	cmd[11] = crc >> 8;

	CodeIso15693AsTag(cmd, sizeof(cmd));
}
*/
// Universal Method for sending to and recv bytes from a tag
//  init ... should we initialize the reader?
//  speed ... 0 low speed, 1 hi speed
//  *recv will contain the tag's answer
//  return: length of received data, or -1 for timeout
int SendDataTag(uint8_t *send, int sendlen, bool init, int speed, uint8_t *recv, uint16_t max_recv_len, uint32_t start_time, uint32_t *eof_time) {

	if (init) {
		Iso15693InitReader();
		StartCountSspClk();
	}

	int answerLen = 0;

	bool fsk = send[0]&ISO15693_REQ_SUBCARRIER_TWO;
	bool recv_speed = send[0]&ISO15693_REQ_DATARATE_HIGH;

	if (!speed) {
		// low speed (1 out of 256)
		CodeIso15693AsReader256(send, sendlen);
	} else {
		// high speed (1 out of 4)
		CodeIso15693AsReader(send, sendlen);
	}

	TransmitTo15693Tag(ToSend, ToSendMax, &start_time);

	// Now wait for a response
	if (recv != NULL) {
		if (fsk)
			answerLen = GetIso15693AnswerFromTagFSK(recv, max_recv_len, ISO15693_READER_TIMEOUT*60, eof_time, recv_speed);
		else
			answerLen = GetIso15693AnswerFromTag(recv, max_recv_len, ISO15693_READER_TIMEOUT*60, eof_time, recv_speed);
	}

	return answerLen;
}


// --------------------------------------------------------------------
// Debug Functions
// --------------------------------------------------------------------

// Decodes a message from a tag and displays its metadata and content
#define DBD15STATLEN 48
void DbdecodeIso15693Answer(int len, uint8_t *d) {
	char status[DBD15STATLEN+1]={0};
	uint16_t crc;

	if (len > 3) {
		if (d[0] & ISO15693_RES_EXT)
			strncat(status,"ProtExt ", DBD15STATLEN);
		if (d[0] & ISO15693_RES_ERROR) {
			// error
			strncat(status,"Error ", DBD15STATLEN);
			switch (d[1]) {
				case 0x01:
					strncat(status,"01:notSupp", DBD15STATLEN);
					break;
				case 0x02:
					strncat(status,"02:notRecog", DBD15STATLEN);
					break;
				case 0x03:
					strncat(status,"03:optNotSupp", DBD15STATLEN);
					break;
				case 0x0f:
					strncat(status,"0f:noInfo", DBD15STATLEN);
					break;
				case 0x10:
					strncat(status,"10:doesn'tExist", DBD15STATLEN);
					break;
				case 0x11:
					strncat(status,"11:lockAgain", DBD15STATLEN);
					break;
				case 0x12:
					strncat(status,"12:locked", DBD15STATLEN);
					break;
				case 0x13:
					strncat(status,"13:progErr", DBD15STATLEN);
					break;
				case 0x14:
					strncat(status,"14:lockErr", DBD15STATLEN);
					break;
				default:
					strncat(status,"unknownErr", DBD15STATLEN);
			}
			strncat(status," ", DBD15STATLEN);
		} else {
			strncat(status,"NoErr ", DBD15STATLEN);
		}

		crc=Iso15693Crc(d,len-2);
		if ( (( crc & 0xff ) == d[len-2]) && (( crc >> 8 ) == d[len-1]) )
			strncat(status,"CrcOK",DBD15STATLEN);
		else
			strncat(status,"CrcFail!",DBD15STATLEN);

		Dbprintf("%s",status);
	}
}



///////////////////////////////////////////////////////////////////////
// Functions called via USB/Client
///////////////////////////////////////////////////////////////////////

void SetDebugIso15693(uint32_t debug) {
	DEBUG=debug;
	Dbprintf("Iso15693 Debug is now %s",DEBUG?"on":"off");
	return;
}


//---------------------------------------------------------------------------------------
// Simulate an ISO15693 reader, perform anti-collision and then attempt to read a sector.
// all demodulation performed in arm rather than host. - greg
//---------------------------------------------------------------------------------------
void ReaderIso15693(uint32_t parameter) {

	LED_A_ON();

	set_tracing(true);

	int answerLen = 0;
	uint8_t TagUID[8] = {0x00};

	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

	uint8_t answer[ISO15693_MAX_RESPONSE_LENGTH];

	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
	// Setup SSC
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

	// Start from off (no field generated)
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	SpinDelay(200);

	// Give the tags time to energize
	LED_D_ON();
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER);
	SpinDelay(200);
	StartCountSspClk();


	// FIRST WE RUN AN INVENTORY TO GET THE TAG UID
	// THIS MEANS WE CAN PRE-BUILD REQUESTS TO SAVE CPU TIME

	// Now send the IDENTIFY command
	BuildIdentifyRequest();
	uint32_t start_time = 0;
	TransmitTo15693Tag(ToSend, ToSendMax, &start_time);

	// Now wait for a response
	uint32_t eof_time;
	answerLen = GetIso15693AnswerFromTag(answer, sizeof(answer), DELAY_ISO15693_VCD_TO_VICC_READER * 2, &eof_time, true) ;
	start_time = eof_time + DELAY_ISO15693_VICC_TO_VCD_READER;

	if (answerLen >=12) // we should do a better check than this
	{
		TagUID[0] = answer[2];
		TagUID[1] = answer[3];
		TagUID[2] = answer[4];
		TagUID[3] = answer[5];
		TagUID[4] = answer[6];
		TagUID[5] = answer[7];
		TagUID[6] = answer[8]; // IC Manufacturer code
		TagUID[7] = answer[9]; // always E0

	}

	Dbprintf("%d octets read from IDENTIFY request:", answerLen);
	DbdecodeIso15693Answer(answerLen, answer);
	Dbhexdump(answerLen, answer, false);

	// UID is reverse
	if (answerLen >= 12)
		Dbprintf("UID = %02hX%02hX%02hX%02hX%02hX%02hX%02hX%02hX",
			TagUID[7],TagUID[6],TagUID[5],TagUID[4],
			TagUID[3],TagUID[2],TagUID[1],TagUID[0]);


	// Dbprintf("%d octets read from SELECT request:", answerLen2);
	// DbdecodeIso15693Answer(answerLen2,answer2);
	// Dbhexdump(answerLen2,answer2,true);

	// Dbprintf("%d octets read from XXX request:", answerLen3);
	// DbdecodeIso15693Answer(answerLen3,answer3);
	// Dbhexdump(answerLen3,answer3,true);

	// read all pages
	if (answerLen >= 12 && DEBUG) {
		for (int i = 0; i < 32; i++) {  // sanity check, assume max 32 pages
			BuildReadBlockRequest(TagUID, i);
			TransmitTo15693Tag(ToSend, ToSendMax, &start_time);
			int answerLen = GetIso15693AnswerFromTag(answer, sizeof(answer), DELAY_ISO15693_VCD_TO_VICC_READER * 2, &eof_time, true);
			start_time = eof_time + DELAY_ISO15693_VICC_TO_VCD_READER;
			if (answerLen > 0) {
				Dbprintf("READ SINGLE BLOCK %d returned %d octets:", i, answerLen);
				DbdecodeIso15693Answer(answerLen, answer);
				Dbhexdump(answerLen, answer, false);
				if ( *((uint32_t*) answer) == 0x07160101 ) break; // exit on NoPageErr
			}
		}
	}

	// for the time being, switch field off to protect rdv4.0
	// note: this prevents using hf 15 cmd with s option - which isn't implemented yet anyway
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();

	LED_A_OFF();
}


// Simulate an ISO15693 TAG.
// Tag Data and infos are taken from emulator memory
// Support all basic ISO15693 commands currently (11/2019) defined in common/protocols.h
// TODO: Add support for INVENTORY variants and colision avoidances
// TODO: Add support for SUBCARRIER_TWO and PROTOCOL_EXT
void SimTagIso15693(uint32_t parameter, uint8_t *uid) {
	bool highRate = false;
	bool selected = false;
	bool quiet = false;
	int cmd_len = 0;
	uint8_t error = 0;
	uint16_t crc = 0;
	uint32_t recvLen = 0;
	uint32_t cpt = 0, pageNum = 0, nbPages = 0;
	uint32_t eof_time = 0, start_time = 0;

	LED_A_ON();

	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_NO_MODULATION);
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);

	uint8_t cmd[ISO15693_MAX_COMMAND_LENGTH];
	uint8_t recv[ISO15693_MAX_RESPONSE_LENGTH] = {0};
	uint8_t *tag = BigBuf_get_EM_addr();
	uint8_t *tagUid = tag;
	uint8_t *tagDSFID = tagUid+8;
	uint8_t *tagDSFIDLock = tagDSFID+1;
	uint8_t *tagAFI = tagDSFIDLock+1;
	uint8_t *tagAFILock = tagAFI+1;
	uint8_t *tagBpP = tagAFILock+1; // Byte/Page
	uint8_t *tagPages = tagBpP+1;
	uint8_t *tagIC = tagPages+1;
	uint8_t *tagLocks = tagIC+1;
	uint8_t *tagData = tagLocks+1 + *tagPages;

	StartCountSspClk();

	// Listen to reader
	while (!BUTTON_PRESS())
	{
		error = 0;
		eof_time = 0;
		// Listen to reader
		cmd_len = GetIso15693CommandFromReader(cmd, sizeof(cmd), &eof_time);
		start_time = eof_time + DELAY_ISO15693_VCD_TO_VICC_SIM;

		cmd[cmd_len] = 0;

		if (DEBUG)
		{
			Dbprintf("%d bytes read from reader:", cmd_len);
			Dbhexdump(cmd_len, cmd, false);
		}

		if (cmd_len <= 3)
			continue;

		crc=Iso15693Crc(cmd,cmd_len-2);
		if ((( crc & 0xff ) != cmd[cmd_len-2]) || (( crc >> 8 ) != cmd[cmd_len-1]))
		{
			if (DEBUG) Dbprintf("CrcFail!");
			continue;
		}
		else if (DEBUG)
			Dbprintf("CrcOK");

		recvLen = 0;

		if (cmd[0]&ISO15693_REQ_SUBCARRIER_TWO)
			Dbprintf("ISO15693_REQ_SUBCARRIER_TWO not supported!");
		if (cmd[0]&ISO15693_REQ_PROTOCOL_EXT)
			Dbprintf("ISO15693_REQ_PROTOCOL_EXT not supported!");

		if (cmd[0]&ISO15693_REQ_DATARATE_HIGH)
			highRate = true;
		else
			highRate = false;

		if (cmd[0]&ISO15693_REQ_INVENTORY && !quiet)
		{
			// TODO : support REQINV_SLOT1 flags
			// TODO : support colision avoidances
			if (DEBUG) Dbprintf("Inventory req");
            if (cmd[0]&ISO15693_REQINV_AFI && cmd[2] != *tagAFI && cmd[2] != 0)
                continue; // bad AFI : drop request
			recv[0] = ISO15693_NOERROR;
			recv[1] = *tagDSFID;
			memcpy(&recv[2], tagUid, 8);
			recvLen = 10;
		}
		else
		{
			if (cmd[0]&ISO15693_REQ_SELECT)
			{
				if (DEBUG) Dbprintf("Selected Request");
				if (!selected)
					continue; // drop selected request if not selected
				selected = false; // Select flag set if already selected : unselect
			}

			cpt = 2;
			if (cmd[0]&ISO15693_REQ_ADDRESS)
			{
				if (DEBUG) Dbprintf("Addressed Request");
				if (memcmp(&cmd[2], tagUid, 8) != 0)
				{
					if (DEBUG) Dbprintf("Address don't match tag uid");
					if (cmd[1] == ISO15693_SELECT)
						selected = false; // we are not anymore the selected TAG
					continue; // drop addressed request with other uid
				}
				if (DEBUG) Dbprintf("Address match tag uid");
				cpt+=8;
			}
			else if (quiet)
			{
				if (DEBUG) Dbprintf("Unaddressed request in quit state : drop");
				continue; // drop unadressed request in quiet state
			}

			// we have to answer this
			switch(cmd[1])
			{
			case ISO15693_INVENTORY:
				if (DEBUG) Dbprintf("Inventory cmd");
				recv[0] = ISO15693_NOERROR;
				recv[1] = *tagDSFID;
				memcpy(&recv[2], tagUid, 8);
				recvLen = 10;
				break;
			case ISO15693_STAYQUIET:
				if (DEBUG) Dbprintf("StayQuiet cmd");
				quiet = true;
				break;
			case ISO15693_READBLOCK:
				if (DEBUG) Dbprintf("ReadBlock cmd");
				pageNum = cmd[cpt++];
				if (pageNum >= *tagPages)
					error = ISO15693_ERROR_BLOCK_UNAVAILABLE;
				else
				{
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
					if ((cmd[0]&ISO15693_REQ_OPTION)) // ask for lock status
					{
						recv[1] = tagLocks[pageNum];
						recvLen++;
					}
					for (unsigned i = 0 ; i < *tagBpP ; i++)
						recv[recvLen+i] = tagData[(pageNum*(*tagBpP)) + i];
					recvLen += *tagBpP;
				}
				break;
			case ISO15693_WRITEBLOCK:
				if (DEBUG) Dbprintf("WriteBlock cmd");
				pageNum = cmd[cpt++];
				if (pageNum >= *tagPages)
					error = ISO15693_ERROR_BLOCK_UNAVAILABLE;
				else
				{
					for (unsigned i = 0 ; i < *tagBpP ; i++)
						tagData[(pageNum*(*tagBpP)) + i] = cmd[i+cpt];
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
				}
				break;
			case ISO15693_LOCKBLOCK:
				if (DEBUG) Dbprintf("LockBlock cmd");
				pageNum = cmd[cpt++];
				if (pageNum >= *tagPages)
					error = ISO15693_ERROR_BLOCK_UNAVAILABLE;
				else if (tagLocks[pageNum])
					error = ISO15693_ERROR_BLOCK_LOCKED_ALREADY;
				else
				{
					tagLocks[pageNum] = 1;
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
				}
				break;
			case ISO15693_READ_MULTI_BLOCK:
				if (DEBUG) Dbprintf("ReadMultiBlock cmd");
				pageNum = cmd[cpt++];
				nbPages = cmd[cpt++];
				if (pageNum+nbPages >= *tagPages)
					error = ISO15693_ERROR_BLOCK_UNAVAILABLE;
				else
				{
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
					for (unsigned i = 0 ; i < (nbPages+1) * *tagBpP && \
							 i+4 < ISO15693_MAX_RESPONSE_LENGTH ; i++)
						recv[recvLen+i] = tagData[(pageNum*(*tagBpP)) + i];
					recvLen += (nbPages+1) * *tagBpP;
					if (recvLen+3 > ISO15693_MAX_RESPONSE_LENGTH) // limit response size
						recvLen = ISO15693_MAX_RESPONSE_LENGTH-3; // to avoid overflow
				}
				break;
			case ISO15693_WRITE_AFI:
				if (DEBUG) Dbprintf("WriteAFI cmd");
				if (*tagAFILock)
					error = ISO15693_ERROR_BLOCK_LOCKED;
				else
				{
					*tagAFI = cmd[cpt++];
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
				}
				break;
			case ISO15693_LOCK_AFI:
				if (DEBUG) Dbprintf("LockAFI cmd");
				if (*tagAFILock)
					error = ISO15693_ERROR_BLOCK_LOCKED_ALREADY;
				else
				{
					*tagAFILock = 1;
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
				}
				break;
			case ISO15693_WRITE_DSFID:
				if (DEBUG) Dbprintf("WriteDSFID cmd");
				if (*tagDSFIDLock)
					error = ISO15693_ERROR_BLOCK_LOCKED;
				else
				{
					*tagDSFID = cmd[cpt++];
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
				}
				break;
			case ISO15693_LOCK_DSFID:
				if (DEBUG) Dbprintf("LockDSFID cmd");
				if (*tagDSFIDLock)
					error = ISO15693_ERROR_BLOCK_LOCKED_ALREADY;
				else
				{
					*tagDSFIDLock = 1;
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
				}
				break;
			case ISO15693_SELECT:
				if (DEBUG) Dbprintf("Select cmd");
				selected = true;
				quiet = false;
				recv[0] = ISO15693_NOERROR;
				recvLen = 1;
				break;
			case ISO15693_RESET_TO_READY:
				if (DEBUG) Dbprintf("ResetToReady cmd");
				quiet = false;
				selected = false;
				recv[0] = ISO15693_NOERROR;
				recvLen = 1;
				break;
			case ISO15693_GET_SYSTEM_INFO:
				if (DEBUG) Dbprintf("GetSystemInfo cmd");
				recv[0] = ISO15693_NOERROR;
				recv[1] = 0x0f; // ?
				memcpy(&recv[2], tagUid, 8);
				recv[10] = *tagDSFID;
				recv[11] = *tagAFI;
				recv[12] = *tagPages-1;
				recv[13] = *tagBpP-1;
				recv[14] = *tagIC;
				recvLen = 15;
				break;
			case ISO15693_READ_MULTI_SECSTATUS:
				if (DEBUG) Dbprintf("ReadMultiSecStatus cmd");
				pageNum = cmd[cpt++];
				nbPages = cmd[cpt++];
				if (pageNum+nbPages >= *tagPages)
					error = ISO15693_ERROR_BLOCK_UNAVAILABLE;
				else
				{
					recv[0] = ISO15693_NOERROR;
					recvLen = 1;
					for (unsigned i = 0 ; i < nbPages+1 ; i++)
						recv[recvLen+i] = tagLocks[pageNum + i];
					recvLen += nbPages + 1;
				}
				break;

			default:
				Dbprintf("ISO15693 CMD 0x%2X not supported", cmd[1]);

				error = ISO15693_ERROR_CMD_NOT_SUP;
				break;
			}
		}

		if (error != 0)
		{
			recv[0] = ISO15693_RES_ERROR;
			recv[1] = error;
			recvLen = 2;
			if (DEBUG) Dbprintf("ERROR 0x%2X in received request", error);
		}

		if (recvLen > 0)
		{
			recvLen = Iso15693AddCrc(recv, recvLen);
			if (DEBUG)
			{
				Dbprintf("%d bytes to write to reader:", recvLen);
				Dbhexdump(recvLen, recv, false);
			}
			CodeIso15693AsTag(recv, recvLen);
			TransmitTo15693Reader(ToSend,ToSendMax, &start_time, 0, !highRate);
		}
	}

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();
	LED_A_OFF();
}


// Since there is no standardized way of reading the AFI out of a tag, we will brute force it
// (some manufactures offer a way to read the AFI, though)
void BruteforceIso15693Afi(uint32_t speed)
{
	LED_A_ON();

	uint8_t data[6];
	uint8_t recv[ISO15693_MAX_RESPONSE_LENGTH];
	int datalen = 0, recvlen = 0;
	uint32_t eof_time;

	// first without AFI
	// Tags should respond without AFI and with AFI=0 even when AFI is active

	data[0] = ISO15693_REQ_DATARATE_HIGH | ISO15693_REQ_INVENTORY | ISO15693_REQINV_SLOT1;
	data[1] = ISO15693_INVENTORY;
	data[2] = 0; // mask length
	datalen = Iso15693AddCrc(data,3);
	uint32_t start_time = GetCountSspClk();
	recvlen = SendDataTag(data, datalen, true, speed, recv, sizeof(recv), 0, &eof_time);
	start_time = eof_time + DELAY_ISO15693_VICC_TO_VCD_READER;
	WDT_HIT();
	if (recvlen>=12) {
		Dbprintf("NoAFI UID=%s", Iso15693sprintUID(NULL, &recv[2]));
	}

	// now with AFI

	data[0] = ISO15693_REQ_DATARATE_HIGH | ISO15693_REQ_INVENTORY | ISO15693_REQINV_AFI | ISO15693_REQINV_SLOT1;
	data[1] = ISO15693_INVENTORY;
	data[2] = 0; // AFI
	data[3] = 0; // mask length

	for (int i = 0; i < 256; i++) {
		data[2] = i & 0xFF;
		datalen = Iso15693AddCrc(data,4);
		recvlen = SendDataTag(data, datalen, false, speed, recv, sizeof(recv), start_time, &eof_time);
		start_time = eof_time + DELAY_ISO15693_VICC_TO_VCD_READER;
		WDT_HIT();
		if (recvlen >= 12) {
			Dbprintf("AFI=%i UID=%s", i, Iso15693sprintUID(NULL, &recv[2]));
		}
	}
	Dbprintf("AFI Bruteforcing done.");

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();
	LED_A_OFF();

}

// Allows to directly send commands to the tag via the client
void DirectTag15693Command(uint32_t datalen, uint32_t speed, uint32_t recv, uint8_t data[]) {

	LED_A_ON();

	int recvlen = 0;
	uint8_t recvbuf[ISO15693_MAX_RESPONSE_LENGTH];
	uint32_t eof_time;

	if (DEBUG) {
		Dbprintf("SEND:");
		Dbhexdump(datalen, data, false);
	}

	recvlen = SendDataTag(data, datalen, true, speed, (recv?recvbuf:NULL), sizeof(recvbuf), 0, &eof_time);

	// for the time being, switch field off to protect rdv4.0
	// note: this prevents using hf 15 cmd with s option - which isn't implemented yet anyway
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();

	if (recv) {
		if (DEBUG) {
			Dbprintf("RECV:");
			if (recvlen > 0) {
				Dbhexdump(recvlen, recvbuf, false);
				DbdecodeIso15693Answer(recvlen, recvbuf);
			}
		}
		if (recvlen > ISO15693_MAX_RESPONSE_LENGTH) {
			recvlen = ISO15693_MAX_RESPONSE_LENGTH;
		}
		cmd_send(CMD_ACK, recvlen, 0, 0, recvbuf, ISO15693_MAX_RESPONSE_LENGTH);
	}

	LED_A_OFF();
}

//-----------------------------------------------------------------------------
// Work with "magic Chinese" card.
//
//-----------------------------------------------------------------------------

// Set the UID to the tag (based on Iceman work).
void SetTag15693Uid(uint8_t *uid) {

	LED_A_ON();

	uint8_t cmd[4][9] = {0x00};
	uint16_t crc;

	int recvlen = 0;
	uint8_t recvbuf[ISO15693_MAX_RESPONSE_LENGTH];
	uint32_t eof_time;

	// Command 1 : 02213E00000000
	cmd[0][0] = 0x02;
	cmd[0][1] = 0x21;
	cmd[0][2] = 0x3e;
	cmd[0][3] = 0x00;
	cmd[0][4] = 0x00;
	cmd[0][5] = 0x00;
	cmd[0][6] = 0x00;

	// Command 2 : 02213F69960000
	cmd[1][0] = 0x02;
	cmd[1][1] = 0x21;
	cmd[1][2] = 0x3f;
	cmd[1][3] = 0x69;
	cmd[1][4] = 0x96;
	cmd[1][5] = 0x00;
	cmd[1][6] = 0x00;

	// Command 3 : 022138u8u7u6u5 (where uX = uid byte X)
	cmd[2][0] = 0x02;
	cmd[2][1] = 0x21;
	cmd[2][2] = 0x38;
	cmd[2][3] = uid[7];
	cmd[2][4] = uid[6];
	cmd[2][5] = uid[5];
	cmd[2][6] = uid[4];

	// Command 4 : 022139u4u3u2u1 (where uX = uid byte X)
	cmd[3][0] = 0x02;
	cmd[3][1] = 0x21;
	cmd[3][2] = 0x39;
	cmd[3][3] = uid[3];
	cmd[3][4] = uid[2];
	cmd[3][5] = uid[1];
	cmd[3][6] = uid[0];

	for (int i = 0; i < 4; i++) {
		// Add the CRC
		crc = Iso15693Crc(cmd[i], 7);
		cmd[i][7] = crc & 0xff;
		cmd[i][8] = crc >> 8;

		if (DEBUG) {
			Dbprintf("SEND:");
			Dbhexdump(sizeof(cmd[i]), cmd[i], false);
		}

		recvlen = SendDataTag(cmd[i], sizeof(cmd[i]), true, 1, recvbuf, sizeof(recvbuf), 0, &eof_time);

		if (DEBUG) {
			Dbprintf("RECV:");
			if (recvlen > 0) {
				Dbhexdump(recvlen, recvbuf, false);
				DbdecodeIso15693Answer(recvlen, recvbuf);
			}
		}

		cmd_send(CMD_ACK, recvlen>ISO15693_MAX_RESPONSE_LENGTH?ISO15693_MAX_RESPONSE_LENGTH:recvlen, 0, 0, recvbuf, ISO15693_MAX_RESPONSE_LENGTH);
	}

	LED_A_OFF();
}



// --------------------------------------------------------------------
// -- Misc & deprecated functions
// --------------------------------------------------------------------

/*

// do not use; has a fix UID
static void __attribute__((unused)) BuildSysInfoRequest(uint8_t *uid)
{
	uint8_t cmd[12];

	uint16_t crc;
	// If we set the Option_Flag in this request, the VICC will respond with the security status of the block
	// followed by the block data
	// one sub-carrier, inventory, 1 slot, fast rate
	cmd[0] =  (1 << 5) | (1 << 1); // no SELECT bit
	// System Information command code
	cmd[1] = 0x2B;
	// UID may be optionally specified here
	// 64-bit UID
	cmd[2] = 0x32;
	cmd[3]= 0x4b;
	cmd[4] = 0x03;
	cmd[5] = 0x01;
	cmd[6] = 0x00;
	cmd[7] = 0x10;
	cmd[8] = 0x05;
	cmd[9]= 0xe0; // always e0 (not exactly unique)
	//Now the CRC
	crc = Iso15693Crc(cmd, 10); // the crc needs to be calculated over 2 bytes
	cmd[10] = crc & 0xff;
	cmd[11] = crc >> 8;

	CodeIso15693AsReader(cmd, sizeof(cmd));
}


// do not use; has a fix UID
static void __attribute__((unused)) BuildReadMultiBlockRequest(uint8_t *uid)
{
	uint8_t cmd[14];

	uint16_t crc;
	// If we set the Option_Flag in this request, the VICC will respond with the security status of the block
	// followed by the block data
	// one sub-carrier, inventory, 1 slot, fast rate
	cmd[0] =  (1 << 5) | (1 << 1); // no SELECT bit
	// READ Multi BLOCK command code
	cmd[1] = 0x23;
	// UID may be optionally specified here
	// 64-bit UID
	cmd[2] = 0x32;
	cmd[3]= 0x4b;
	cmd[4] = 0x03;
	cmd[5] = 0x01;
	cmd[6] = 0x00;
	cmd[7] = 0x10;
	cmd[8] = 0x05;
	cmd[9]= 0xe0; // always e0 (not exactly unique)
	// First Block number to read
	cmd[10] = 0x00;
	// Number of Blocks to read
	cmd[11] = 0x2f; // read quite a few
	//Now the CRC
	crc = Iso15693Crc(cmd, 12); // the crc needs to be calculated over 2 bytes
	cmd[12] = crc & 0xff;
	cmd[13] = crc >> 8;

	CodeIso15693AsReader(cmd, sizeof(cmd));
}

// do not use; has a fix UID
static void __attribute__((unused)) BuildArbitraryRequest(uint8_t *uid,uint8_t CmdCode)
{
	uint8_t cmd[14];

	uint16_t crc;
	// If we set the Option_Flag in this request, the VICC will respond with the security status of the block
	// followed by the block data
	// one sub-carrier, inventory, 1 slot, fast rate
	cmd[0] =   (1 << 5) | (1 << 1); // no SELECT bit
	// READ BLOCK command code
	cmd[1] = CmdCode;
	// UID may be optionally specified here
	// 64-bit UID
	cmd[2] = 0x32;
	cmd[3]= 0x4b;
	cmd[4] = 0x03;
	cmd[5] = 0x01;
	cmd[6] = 0x00;
	cmd[7] = 0x10;
	cmd[8] = 0x05;
	cmd[9]= 0xe0; // always e0 (not exactly unique)
	// Parameter
	cmd[10] = 0x00;
	cmd[11] = 0x0a;

//  cmd[12] = 0x00;
//  cmd[13] = 0x00; //Now the CRC
	crc = Iso15693Crc(cmd, 12); // the crc needs to be calculated over 2 bytes
	cmd[12] = crc & 0xff;
	cmd[13] = crc >> 8;

	CodeIso15693AsReader(cmd, sizeof(cmd));
}

// do not use; has a fix UID
static void __attribute__((unused)) BuildArbitraryCustomRequest(uint8_t uid[], uint8_t CmdCode)
{
	uint8_t cmd[14];

	uint16_t crc;
	// If we set the Option_Flag in this request, the VICC will respond with the security status of the block
	// followed by the block data
	// one sub-carrier, inventory, 1 slot, fast rate
	cmd[0] =   (1 << 5) | (1 << 1); // no SELECT bit
	// READ BLOCK command code
	cmd[1] = CmdCode;
	// UID may be optionally specified here
	// 64-bit UID
	cmd[2] = 0x32;
	cmd[3]= 0x4b;
	cmd[4] = 0x03;
	cmd[5] = 0x01;
	cmd[6] = 0x00;
	cmd[7] = 0x10;
	cmd[8] = 0x05;
	cmd[9]= 0xe0; // always e0 (not exactly unique)
	// Parameter
	cmd[10] = 0x05; // for custom codes this must be manufacturer code
	cmd[11] = 0x00;

//  cmd[12] = 0x00;
//  cmd[13] = 0x00; //Now the CRC
	crc = Iso15693Crc(cmd, 12); // the crc needs to be calculated over 2 bytes
	cmd[12] = crc & 0xff;
	cmd[13] = crc >> 8;

	CodeIso15693AsReader(cmd, sizeof(cmd));
}




*/


