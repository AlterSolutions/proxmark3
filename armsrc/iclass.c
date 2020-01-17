//-----------------------------------------------------------------------------
// Gerhard de Koning Gans - May 2008
// Hagen Fritsch - June 2010
// Gerhard de Koning Gans - May 2011
// Gerhard de Koning Gans - June 2012 - Added iClass card and reader emulation
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Routines to support iClass.
//-----------------------------------------------------------------------------
// Based on ISO14443a implementation. Still in experimental phase.
// Contribution made during a security research at Radboud University Nijmegen
//
// Please feel free to contribute and extend iClass support!!
//-----------------------------------------------------------------------------
//
// FIX:
// ====
// We still have sometimes a demodulation error when snooping iClass communication.
// The resulting trace of a read-block-03 command may look something like this:
//
//  +  22279:    :     0c  03  e8  01
//
//    ...with an incorrect answer...
//
//  +     85:   0: TAG ff! ff! ff! ff! ff! ff! ff! ff! bb  33  bb  00  01! 0e! 04! bb     !crc
//
// We still left the error signalling bytes in the traces like 0xbb
//
// A correct trace should look like this:
//
// +  21112:    :     0c  03  e8  01
// +     85:   0: TAG ff  ff  ff  ff  ff  ff  ff  ff  ea  f5
//
//-----------------------------------------------------------------------------

#include "iclass.h"

#include "proxmark3.h"
#include "apps.h"
#include "util.h"
#include "string.h"
#include "printf.h"
#include "common.h"
#include "cmd.h"
#include "iso14443a.h"
#include "iso15693.h"
// Needed for CRC in emulation mode;
// same construction as in ISO 14443;
// different initial value (CRC_ICLASS)
#include "iso14443crc.h"
#include "iso15693tools.h"
#include "protocols.h"
#include "optimized_cipher.h"
#include "usb_cdc.h" // for usb_poll_validate_length
#include "fpgaloader.h"

// iCLASS has a slightly different timing compared to ISO15693. According to the picopass data sheet the tag response is expected 330us after
// the reader command. This is measured from end of reader EOF to first modulation of the tag's SOF which starts with a 56,64us unmodulated period.
// 330us = 140 ssp_clk cycles @ 423,75kHz when simulating.
// 56,64us = 24 ssp_clk_cycles
#define DELAY_ICLASS_VCD_TO_VICC_SIM     (140 - 24)
// times in ssp_clk_cycles @ 3,3625MHz when acting as reader
#define DELAY_ICLASS_VICC_TO_VCD_READER  DELAY_ISO15693_VICC_TO_VCD_READER
// times in samples @ 212kHz when acting as reader
#define ICLASS_READER_TIMEOUT_ACTALL     330 // 1558us, nominal 330us + 7slots*160us = 1450us
#define ICLASS_READER_TIMEOUT_OTHERS      80 // 380us, nominal 330us


//-----------------------------------------------------------------------------
// The software UART that receives commands from the reader, and its state
// variables.
//-----------------------------------------------------------------------------
static struct {
	enum {
		STATE_UNSYNCD,
		STATE_START_OF_COMMUNICATION,
		STATE_RECEIVING
	}        state;
	uint16_t shiftReg;
	int      bitCnt;
	int      byteCnt;
	int      byteCntMax;
	int      posCnt;
	int      nOutOfCnt;
	int      OutOfCnt;
	int      syncBit;
	int      samples;
	int      highCnt;
	int      swapper;
	int      counter;
	int      bitBuffer;
	int      dropPosition;
	uint8_t  *output;
} Uart;

static RAMFUNC int OutOfNDecoding(int bit) {
	//int error = 0;
	int bitright;

	if (!Uart.bitBuffer) {
		Uart.bitBuffer = bit ^ 0xFF0;
		return false;
	} else {
		Uart.bitBuffer <<= 4;
		Uart.bitBuffer ^= bit;
	}

	/*if (Uart.swapper) {
		Uart.output[Uart.byteCnt] = Uart.bitBuffer & 0xFF;
		Uart.byteCnt++;
		Uart.swapper = 0;
		if (Uart.byteCnt > 15) { return true; }
	}
	else {
		Uart.swapper = 1;
	}*/

	if (Uart.state != STATE_UNSYNCD) {
		Uart.posCnt++;

		if ((Uart.bitBuffer & Uart.syncBit) ^ Uart.syncBit) {
			bit = 0x00;
		} else {
			bit = 0x01;
		}
		if (((Uart.bitBuffer << 1) & Uart.syncBit) ^ Uart.syncBit) {
			bitright = 0x00;
		} else {
			bitright = 0x01;
		}
		if (bit != bitright) {
			bit = bitright;
		}


		// So, now we only have to deal with *bit*, lets see...
		if (Uart.posCnt == 1) {
			// measurement first half bitperiod
			if (!bit) {
				// Drop in first half means that we are either seeing
				// an SOF or an EOF.

				if (Uart.nOutOfCnt == 1) {
					// End of Communication
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
					if (Uart.byteCnt == 0) {
						// Its not straightforward to show single EOFs
						// So just leave it and do not return true
						Uart.output[0] = 0xf0;
						Uart.byteCnt++;
					} else {
						return true;
					}
				} else if (Uart.state != STATE_START_OF_COMMUNICATION) {
					// When not part of SOF or EOF, it is an error
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
					//error = 4;
				}
			}
		} else {
			// measurement second half bitperiod
			// Count the bitslot we are in... (ISO 15693)
			Uart.nOutOfCnt++;

			if (!bit) {
				if (Uart.dropPosition) {
					if (Uart.state == STATE_START_OF_COMMUNICATION) {
						//error = 1;
					} else {
						//error = 7;
					}
					// It is an error if we already have seen a drop in current frame
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
				} else {
					Uart.dropPosition = Uart.nOutOfCnt;
				}
			}

			Uart.posCnt = 0;


			if (Uart.nOutOfCnt == Uart.OutOfCnt && Uart.OutOfCnt == 4) {
				Uart.nOutOfCnt = 0;

				if (Uart.state == STATE_START_OF_COMMUNICATION) {
					if (Uart.dropPosition == 4) {
						Uart.state = STATE_RECEIVING;
						Uart.OutOfCnt = 256;
					} else if (Uart.dropPosition == 3) {
						Uart.state = STATE_RECEIVING;
						Uart.OutOfCnt = 4;
						//Uart.output[Uart.byteCnt] = 0xdd;
						//Uart.byteCnt++;
					} else {
						Uart.state = STATE_UNSYNCD;
						Uart.highCnt = 0;
					}
					Uart.dropPosition = 0;
				} else {
					// RECEIVING DATA
					// 1 out of 4
					if (!Uart.dropPosition) {
						Uart.state = STATE_UNSYNCD;
						Uart.highCnt = 0;
						//error = 9;
					} else {
						Uart.shiftReg >>= 2;

						// Swap bit order
						Uart.dropPosition--;
						//if (Uart.dropPosition == 1) { Uart.dropPosition = 2; }
						//else if (Uart.dropPosition == 2) { Uart.dropPosition = 1; }

						Uart.shiftReg ^= ((Uart.dropPosition & 0x03) << 6);
						Uart.bitCnt += 2;
						Uart.dropPosition = 0;

						if (Uart.bitCnt == 8) {
							Uart.output[Uart.byteCnt] = (Uart.shiftReg & 0xff);
							Uart.byteCnt++;
							Uart.bitCnt = 0;
							Uart.shiftReg = 0;
						}
					}
				}
			} else if (Uart.nOutOfCnt == Uart.OutOfCnt) {
				// RECEIVING DATA
				// 1 out of 256
				if (!Uart.dropPosition) {
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
					//error = 3;
				} else {
					Uart.dropPosition--;
					Uart.output[Uart.byteCnt] = (Uart.dropPosition & 0xff);
					Uart.byteCnt++;
					Uart.bitCnt = 0;
					Uart.shiftReg = 0;
					Uart.nOutOfCnt = 0;
					Uart.dropPosition = 0;
				}
			}

			/*if (error) {
				Uart.output[Uart.byteCnt] = 0xAA;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = error & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = 0xAA;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = (Uart.bitBuffer >> 8) & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = Uart.bitBuffer & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = (Uart.syncBit >> 3) & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = 0xAA;
				Uart.byteCnt++;
				return true;
			}*/
		}

	} else {
		bit = Uart.bitBuffer & 0xf0;
		bit >>= 4;
		bit ^= 0x0F; // drops become 1s ;-)
		if (bit) {
			// should have been high or at least (4 * 128) / fc
			// according to ISO this should be at least (9 * 128 + 20) / fc
			if (Uart.highCnt == 8) {
				// we went low, so this could be start of communication
				// it turns out to be safer to choose a less significant
				// syncbit... so we check whether the neighbour also represents the drop
				Uart.posCnt = 1;   // apparently we are busy with our first half bit period
				Uart.syncBit = bit & 8;
				Uart.samples = 3;
				if (!Uart.syncBit)  { Uart.syncBit = bit & 4; Uart.samples = 2; }
				else if (bit & 4)   { Uart.syncBit = bit & 4; Uart.samples = 2; bit <<= 2; }
				if (!Uart.syncBit)  { Uart.syncBit = bit & 2; Uart.samples = 1; }
				else if (bit & 2)   { Uart.syncBit = bit & 2; Uart.samples = 1; bit <<= 1; }
				if (!Uart.syncBit)  { Uart.syncBit = bit & 1; Uart.samples = 0;
					if (Uart.syncBit && (Uart.bitBuffer & 8)) {
						Uart.syncBit = 8;

						// the first half bit period is expected in next sample
						Uart.posCnt = 0;
						Uart.samples = 3;
					}
				} else if (bit & 1) { Uart.syncBit = bit & 1; Uart.samples = 0; }

				Uart.syncBit <<= 4;
				Uart.state = STATE_START_OF_COMMUNICATION;
				Uart.bitCnt = 0;
				Uart.byteCnt = 0;
				Uart.nOutOfCnt = 0;
				Uart.OutOfCnt = 4; // Start at 1/4, could switch to 1/256
				Uart.dropPosition = 0;
				Uart.shiftReg = 0;
				//error = 0;
			} else {
				Uart.highCnt = 0;
			}
		} else if (Uart.highCnt < 8) {
			Uart.highCnt++;
		}
	}

	return false;
}


//=============================================================================
// Manchester
//=============================================================================

static struct {
	enum {
		DEMOD_UNSYNCD,
		DEMOD_START_OF_COMMUNICATION,
		DEMOD_START_OF_COMMUNICATION2,
		DEMOD_START_OF_COMMUNICATION3,
		DEMOD_SOF_COMPLETE,
		DEMOD_MANCHESTER_D,
		DEMOD_MANCHESTER_E,
		DEMOD_END_OF_COMMUNICATION,
		DEMOD_END_OF_COMMUNICATION2,
		DEMOD_MANCHESTER_F,
		DEMOD_ERROR_WAIT
	}        state;
	int      bitCount;
	int      posCount;
	int      syncBit;
	uint16_t shiftReg;
	int      buffer;
	int      buffer2;
	int      buffer3;
	int      buff;
	int      samples;
	int      len;
	enum {
		SUB_NONE,
		SUB_FIRST_HALF,
		SUB_SECOND_HALF,
		SUB_BOTH
	}        sub;
	uint8_t  *output;
} Demod;

static RAMFUNC int ManchesterDecoding(int v) {
	int bit;
	int modulation;
	int error = 0;

	bit = Demod.buffer;
	Demod.buffer = Demod.buffer2;
	Demod.buffer2 = Demod.buffer3;
	Demod.buffer3 = v;

	if (Demod.buff < 3) {
		Demod.buff++;
		return false;
	}

	if (Demod.state==DEMOD_UNSYNCD) {
		Demod.output[Demod.len] = 0xfa;
		Demod.syncBit = 0;
		//Demod.samples = 0;
		Demod.posCount = 1;     // This is the first half bit period, so after syncing handle the second part

		if (bit & 0x08) {
			Demod.syncBit = 0x08;
		}

		if (bit & 0x04) {
			if (Demod.syncBit) {
				bit <<= 4;
			}
			Demod.syncBit = 0x04;
		}

		if (bit & 0x02) {
			if (Demod.syncBit) {
				bit <<= 2;
			}
			Demod.syncBit = 0x02;
		}

		if (bit & 0x01 && Demod.syncBit) {
			Demod.syncBit = 0x01;
		}

		if (Demod.syncBit) {
			Demod.len = 0;
			Demod.state = DEMOD_START_OF_COMMUNICATION;
			Demod.sub = SUB_FIRST_HALF;
			Demod.bitCount = 0;
			Demod.shiftReg = 0;
			Demod.samples = 0;
			if (Demod.posCount) {
				switch (Demod.syncBit) {
					case 0x08: Demod.samples = 3; break;
					case 0x04: Demod.samples = 2; break;
					case 0x02: Demod.samples = 1; break;
					case 0x01: Demod.samples = 0; break;
				}
				// SOF must be long burst... otherwise stay unsynced!!!
				if (!(Demod.buffer & Demod.syncBit) || !(Demod.buffer2 & Demod.syncBit)) {
					Demod.state = DEMOD_UNSYNCD;
				}
			} else {
				// SOF must be long burst... otherwise stay unsynced!!!
				if (!(Demod.buffer2 & Demod.syncBit) || !(Demod.buffer3 & Demod.syncBit)) {
					Demod.state = DEMOD_UNSYNCD;
					error = 0x88;
				}

			}
			error = 0;

		}
	} else {
		// state is DEMOD is in SYNC from here on.
		modulation = bit & Demod.syncBit;
		modulation |= ((bit << 1) ^ ((Demod.buffer & 0x08) >> 3)) & Demod.syncBit;

		Demod.samples += 4;

		if (Demod.posCount == 0) {
			Demod.posCount = 1;
			if (modulation) {
				Demod.sub = SUB_FIRST_HALF;
			} else {
				Demod.sub = SUB_NONE;
			}
		} else {
			Demod.posCount = 0;
			if (modulation) {
				if (Demod.sub == SUB_FIRST_HALF) {
					Demod.sub = SUB_BOTH;
				} else {
					Demod.sub = SUB_SECOND_HALF;
				}
			} else if (Demod.sub == SUB_NONE) {
				if (Demod.state == DEMOD_SOF_COMPLETE) {
					Demod.output[Demod.len] = 0x0f;
					Demod.len++;
					Demod.state = DEMOD_UNSYNCD;
					return true;
				} else {
					Demod.state = DEMOD_ERROR_WAIT;
					error = 0x33;
				}
			}

			switch(Demod.state) {
				case DEMOD_START_OF_COMMUNICATION:
					if (Demod.sub == SUB_BOTH) {
						Demod.state = DEMOD_START_OF_COMMUNICATION2;
						Demod.posCount = 1;
						Demod.sub = SUB_NONE;
					} else {
						Demod.output[Demod.len] = 0xab;
						Demod.state = DEMOD_ERROR_WAIT;
						error = 0xd2;
					}
					break;
				case DEMOD_START_OF_COMMUNICATION2:
					if (Demod.sub == SUB_SECOND_HALF) {
						Demod.state = DEMOD_START_OF_COMMUNICATION3;
					} else {
						Demod.output[Demod.len] = 0xab;
						Demod.state = DEMOD_ERROR_WAIT;
						error = 0xd3;
					}
					break;
				case DEMOD_START_OF_COMMUNICATION3:
					if (Demod.sub == SUB_SECOND_HALF) {
						Demod.state = DEMOD_SOF_COMPLETE;
					} else {
						Demod.output[Demod.len] = 0xab;
						Demod.state = DEMOD_ERROR_WAIT;
						error = 0xd4;
					}
					break;
				case DEMOD_SOF_COMPLETE:
				case DEMOD_MANCHESTER_D:
				case DEMOD_MANCHESTER_E:
					// OPPOSITE FROM ISO14443 - 11110000 = 0 (1 in 14443)
					//                          00001111 = 1 (0 in 14443)
					if (Demod.sub == SUB_SECOND_HALF) { // SUB_FIRST_HALF
						Demod.bitCount++;
						Demod.shiftReg = (Demod.shiftReg >> 1) ^ 0x100;
						Demod.state = DEMOD_MANCHESTER_D;
					} else if (Demod.sub == SUB_FIRST_HALF) { // SUB_SECOND_HALF
						Demod.bitCount++;
						Demod.shiftReg >>= 1;
						Demod.state = DEMOD_MANCHESTER_E;
					} else if (Demod.sub == SUB_BOTH) {
						Demod.state = DEMOD_MANCHESTER_F;
					} else {
						Demod.state = DEMOD_ERROR_WAIT;
						error = 0x55;
					}
					break;

				case DEMOD_MANCHESTER_F:
					// Tag response does not need to be a complete byte!
					if (Demod.len > 0 || Demod.bitCount > 0) {
						if (Demod.bitCount > 1) {  // was > 0, do not interpret last closing bit, is part of EOF
							Demod.shiftReg >>= (9 - Demod.bitCount);    // right align data
							Demod.output[Demod.len] = Demod.shiftReg & 0xff;
							Demod.len++;
						}

						Demod.state = DEMOD_UNSYNCD;
						return true;
					} else {
						Demod.output[Demod.len] = 0xad;
						Demod.state = DEMOD_ERROR_WAIT;
						error = 0x03;
					}
					break;

				case DEMOD_ERROR_WAIT:
					Demod.state = DEMOD_UNSYNCD;
					break;

				default:
					Demod.output[Demod.len] = 0xdd;
					Demod.state = DEMOD_UNSYNCD;
					break;
			}

			if (Demod.bitCount >= 8) {
				Demod.shiftReg >>= 1;
				Demod.output[Demod.len] = (Demod.shiftReg & 0xff);
				Demod.len++;
				Demod.bitCount = 0;
				Demod.shiftReg = 0;
			}

			if (error) {
				Demod.output[Demod.len] = 0xBB;
				Demod.len++;
				Demod.output[Demod.len] = error & 0xFF;
				Demod.len++;
				Demod.output[Demod.len] = 0xBB;
				Demod.len++;
				Demod.output[Demod.len] = bit & 0xFF;
				Demod.len++;
				Demod.output[Demod.len] = Demod.buffer & 0xFF;
				Demod.len++;
				// Look harder ;-)
				Demod.output[Demod.len] = Demod.buffer2 & 0xFF;
				Demod.len++;
				Demod.output[Demod.len] = Demod.syncBit & 0xFF;
				Demod.len++;
				Demod.output[Demod.len] = 0xBB;
				Demod.len++;
				return true;
			}

		}

	} // end (state != UNSYNCED)

	return false;
}

//=============================================================================
// Finally, a `sniffer' for iClass communication
// Both sides of communication!
//=============================================================================

//-----------------------------------------------------------------------------
// Record the sequence of commands sent by the reader to the tag, with
// triggering so that we start recording at the point that the tag is moved
// near the reader.
//-----------------------------------------------------------------------------
void RAMFUNC SnoopIClass(void) {

	// We won't start recording the frames that we acquire until we trigger;
	// a good trigger condition to get started is probably when we see a
	// response from the tag.
	//int triggered = false; // false to wait first for card

	// The command (reader -> tag) that we're receiving.
	// The length of a received command will in most cases be no more than 18 bytes.
	// So 32 should be enough!
	#define ICLASS_BUFFER_SIZE 32
	uint8_t readerToTagCmd[ICLASS_BUFFER_SIZE];
	// The response (tag -> reader) that we're receiving.
	uint8_t tagToReaderResponse[ICLASS_BUFFER_SIZE];

	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

	// free all BigBuf memory
	BigBuf_free();
	// The DMA buffer, used to stream samples from the FPGA
	uint8_t *dmaBuf = BigBuf_malloc(DMA_BUFFER_SIZE);

	set_tracing(true);
	clear_trace();
	iso14a_set_trigger(false);

	int lastRxCounter;
	uint8_t *upTo;
	int smpl;
	int maxBehindBy = 0;

	// Count of samples received so far, so that we can include timing
	// information in the trace buffer.
	int samples = 0;
	rsamples = 0;

	// Set up the demodulator for tag -> reader responses.
	Demod.output = tagToReaderResponse;
	Demod.len = 0;
	Demod.state = DEMOD_UNSYNCD;

	// Setup for the DMA.
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_ISO14443A);
	upTo = dmaBuf;
	lastRxCounter = DMA_BUFFER_SIZE;
	FpgaSetupSscDma((uint8_t *)dmaBuf, DMA_BUFFER_SIZE);

	// And the reader -> tag commands
	memset(&Uart, 0, sizeof(Uart));
	Uart.output = readerToTagCmd;
	Uart.byteCntMax = 32; // was 100 (greg)////////////////////////////////////////////////////////////////////////
	Uart.state = STATE_UNSYNCD;

	// And put the FPGA in the appropriate mode
	// Signal field is off with the appropriate LED
	LED_D_OFF();
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_SNIFFER);
	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

	uint32_t time_0 = GetCountSspClk();
	uint32_t time_start = 0;
	uint32_t time_stop  = 0;

	int div = 0;
	//int div2 = 0;
	int decbyte = 0;
	int decbyter = 0;

	// And now we loop, receiving samples.
	for (;;) {
		LED_A_ON();
		WDT_HIT();
		int behindBy = (lastRxCounter - AT91C_BASE_PDC_SSC->PDC_RCR) & (DMA_BUFFER_SIZE-1);
		if (behindBy > maxBehindBy) {
			maxBehindBy = behindBy;
			if (behindBy > (9 * DMA_BUFFER_SIZE / 10)) {
				Dbprintf("blew circular buffer! behindBy=0x%x", behindBy);
				goto done;
			}
		}
		if (behindBy < 1) continue;

		LED_A_OFF();
		smpl = upTo[0];
		upTo++;
		lastRxCounter -= 1;
		if (upTo - dmaBuf > DMA_BUFFER_SIZE) {
			upTo -= DMA_BUFFER_SIZE;
			lastRxCounter += DMA_BUFFER_SIZE;
			AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) upTo;
			AT91C_BASE_PDC_SSC->PDC_RNCR = DMA_BUFFER_SIZE;
		}

		//samples += 4;
		samples += 1;

		if (smpl & 0xF) {
			decbyte ^= (1 << (3 - div));
		}

		// FOR READER SIDE COMMUMICATION...

		decbyter <<= 2;
		decbyter ^= (smpl & 0x30);

		div++;

		if ((div + 1) % 2 == 0) {
			smpl = decbyter;
			if (OutOfNDecoding((smpl & 0xF0) >> 4)) {
				rsamples = samples - Uart.samples;
				time_stop = (GetCountSspClk()-time_0) << 4;

				//if (!LogTrace(Uart.output, Uart.byteCnt, rsamples, Uart.parityBits,true)) break;
				//if (!LogTrace(NULL, 0, Uart.endTime*16 - DELAY_READER_AIR2ARM_AS_SNIFFER, 0, true)) break;
				uint8_t parity[MAX_PARITY_SIZE];
				GetParity(Uart.output, Uart.byteCnt, parity);
				LogTrace_ISO15693(Uart.output, Uart.byteCnt, time_start*32, time_stop*32, parity, true);

				/* And ready to receive another command. */
				Uart.state = STATE_UNSYNCD;
				/* And also reset the demod code, which might have been */
				/* false-triggered by the commands from the reader. */
				Demod.state = DEMOD_UNSYNCD;
				Uart.byteCnt = 0;
			} else {
				time_start = (GetCountSspClk()-time_0) << 4;
			}
			decbyter = 0;
		}

		if (div > 3) {
			smpl = decbyte;
			if (ManchesterDecoding(smpl & 0x0F)) {
				time_stop = (GetCountSspClk()-time_0) << 4;

				rsamples = samples - Demod.samples;

				uint8_t parity[MAX_PARITY_SIZE];
				GetParity(Demod.output, Demod.len, parity);
				LogTrace_ISO15693(Demod.output, Demod.len, time_start*32, time_stop*32, parity, false);

				// And ready to receive another response.
				memset(&Demod, 0, sizeof(Demod));
				Demod.output = tagToReaderResponse;
				Demod.state = DEMOD_UNSYNCD;
			} else {
				time_start = (GetCountSspClk()-time_0) << 4;
			}

			div = 0;
			decbyte = 0x00;
		}

		if (BUTTON_PRESS()) {
			DbpString("cancelled_a");
			goto done;
		}
	}

	DbpString("COMMAND FINISHED");

	Dbprintf("%x %x %x", maxBehindBy, Uart.state, Uart.byteCnt);
	Dbprintf("%x %x %x", Uart.byteCntMax, BigBuf_get_traceLen(), (int)Uart.output[0]);

done:
	AT91C_BASE_PDC_SSC->PDC_PTCR = AT91C_PDC_RXTDIS;
	Dbprintf("%x %x %x", maxBehindBy, Uart.state, Uart.byteCnt);
	Dbprintf("%x %x %x", Uart.byteCntMax, BigBuf_get_traceLen(), (int)Uart.output[0]);
	LEDsoff();
}

void rotateCSN(uint8_t* originalCSN, uint8_t* rotatedCSN) {
	int i;
	for (i = 0; i < 8; i++) {
		rotatedCSN[i] = (originalCSN[i] >> 3) | (originalCSN[(i+1)%8] << 5);
	}
}

// Encode SOF only
static void CodeIClassTagSOF() {
	ToSendReset();
	ToSend[++ToSendMax] = 0x1D;
	ToSendMax++;
}

static void AppendCrc(uint8_t *data, int len) {
	ComputeCrc14443(CRC_ICLASS, data, len, data+len, data+len+1);
}


/**
 * @brief Does the actual simulation
 */
int doIClassSimulation(int simulationMode, uint8_t *reader_mac_buf) {

	// free eventually allocated BigBuf memory
	BigBuf_free_keep_EM();

	uint16_t page_size = 32 * 8;
	uint8_t current_page = 0;

	// maintain cipher states for both credit and debit key for each page
	State cipher_state_KC[8];
	State cipher_state_KD[8];
	State *cipher_state = &cipher_state_KD[0];

	uint8_t *emulator = BigBuf_get_EM_addr();
	uint8_t *csn = emulator;

	// CSN followed by two CRC bytes
	uint8_t anticoll_data[10];
	uint8_t csn_data[10];
	memcpy(csn_data, csn, sizeof(csn_data));
	Dbprintf("Simulating CSN %02x%02x%02x%02x%02x%02x%02x%02x", csn[0], csn[1], csn[2], csn[3], csn[4], csn[5], csn[6], csn[7]);

	// Construct anticollision-CSN
	rotateCSN(csn_data, anticoll_data);

	// Compute CRC on both CSNs
	AppendCrc(anticoll_data, 8);
	AppendCrc(csn_data, 8);

	uint8_t diversified_key_d[8] = { 0x00 };
	uint8_t diversified_key_c[8] = { 0x00 };
	uint8_t *diversified_key = diversified_key_d;

	// configuration block
	uint8_t conf_block[10] = {0x12, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0xFF, 0x3C, 0x00, 0x00};

	// e-Purse
	uint8_t card_challenge_data[8] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if (simulationMode == ICLASS_SIM_MODE_FULL) {
		// initialize from page 0
		memcpy(conf_block, emulator + 8 * 1, 8);
		memcpy(card_challenge_data, emulator + 8 * 2, 8); // e-purse
		memcpy(diversified_key_d, emulator + 8 * 3, 8);   // Kd
		memcpy(diversified_key_c, emulator + 8 * 4, 8);   // Kc
	}

	AppendCrc(conf_block, 8);

	// save card challenge for sim2,4 attack
	if (reader_mac_buf != NULL) {
		memcpy(reader_mac_buf, card_challenge_data, 8);
	}

	if (conf_block[5] & 0x80) {
		page_size = 256 * 8;
	}

	// From PicoPass DS:
	// When the page is in personalization mode this bit is equal to 1.
	// Once the application issuer has personalized and coded its dedicated areas, this bit must be set to 0:
	// the page is then "in application mode".
	bool personalization_mode = conf_block[7] & 0x80;

	// chip memory may be divided in 8 pages
	uint8_t max_page = conf_block[4] & 0x10 ? 0 : 7;

	// Precalculate the cipher states, feeding it the CC
	cipher_state_KD[0] = opt_doTagMAC_1(card_challenge_data, diversified_key_d);
	cipher_state_KC[0] = opt_doTagMAC_1(card_challenge_data, diversified_key_c);
	if (simulationMode == ICLASS_SIM_MODE_FULL) {
		for (int i = 1; i < max_page; i++) {
			uint8_t *epurse = emulator + i*page_size + 8*2;
			uint8_t *Kd = emulator + i*page_size + 8*3;
			uint8_t *Kc = emulator + i*page_size + 8*4;
			cipher_state_KD[i] = opt_doTagMAC_1(epurse, Kd);
			cipher_state_KC[i] = opt_doTagMAC_1(epurse, Kc);
		}
	}

	int exitLoop = 0;
	// Reader 0a
	// Tag    0f
	// Reader 0c
	// Tag    anticoll. CSN
	// Reader 81 anticoll. CSN
	// Tag    CSN

	uint8_t *modulated_response;
	int modulated_response_size = 0;
	uint8_t *trace_data = NULL;
	int trace_data_size = 0;

	// Respond SOF -- takes 1 bytes
	uint8_t *resp_sof = BigBuf_malloc(1);
	int resp_sof_Len;

	// Anticollision CSN (rotated CSN)
	// 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
	uint8_t *resp_anticoll = BigBuf_malloc(22);
	int resp_anticoll_len;

	// CSN (block 0)
	// 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
	uint8_t *resp_csn = BigBuf_malloc(22);
	int resp_csn_len;

	// configuration (block 1) picopass 2ks
	uint8_t *resp_conf = BigBuf_malloc(22);
	int resp_conf_len;

	// e-Purse (block 2)
	// 18: Takes 2 bytes for SOF/EOF and 8 * 2 = 16 bytes (2 bytes/bit)
	uint8_t *resp_cc = BigBuf_malloc(18);
	int resp_cc_len;

	// Kd, Kc (blocks 3 and 4). Cannot be read. Always respond with 0xff bytes only
	uint8_t *resp_ff = BigBuf_malloc(22);
	int resp_ff_len;
	uint8_t ff_data[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
	AppendCrc(ff_data, 8);

	// Application Issuer Area (block 5)
	uint8_t *resp_aia = BigBuf_malloc(22);
	int resp_aia_len;
	uint8_t aia_data[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
	AppendCrc(aia_data, 8);

	uint8_t *receivedCmd = BigBuf_malloc(MAX_FRAME_SIZE);
	int len;

	// Prepare card messages

	// First card answer: SOF only
	CodeIClassTagSOF();
	memcpy(resp_sof, ToSend, ToSendMax);
	resp_sof_Len = ToSendMax;

	// Anticollision CSN
	CodeIso15693AsTag(anticoll_data, sizeof(anticoll_data));
	memcpy(resp_anticoll, ToSend, ToSendMax);
	resp_anticoll_len = ToSendMax;

	// CSN (block 0)
	CodeIso15693AsTag(csn_data, sizeof(csn_data));
	memcpy(resp_csn, ToSend, ToSendMax);
	resp_csn_len = ToSendMax;

	// Configuration (block 1)
	CodeIso15693AsTag(conf_block, sizeof(conf_block));
	memcpy(resp_conf, ToSend, ToSendMax);
	resp_conf_len = ToSendMax;

	// e-Purse (block 2)
	CodeIso15693AsTag(card_challenge_data, sizeof(card_challenge_data));
	memcpy(resp_cc, ToSend, ToSendMax);
	resp_cc_len = ToSendMax;

	// Kd, Kc (blocks 3 and 4)
	CodeIso15693AsTag(ff_data, sizeof(ff_data));
	memcpy(resp_ff, ToSend, ToSendMax);
	resp_ff_len = ToSendMax;

	// Application Issuer Area (block 5)
	CodeIso15693AsTag(aia_data, sizeof(aia_data));
	memcpy(resp_aia, ToSend, ToSendMax);
	resp_aia_len = ToSendMax;

	//This is used for responding to READ-block commands or other data which is dynamically generated
	uint8_t *data_generic_trace = BigBuf_malloc(32 + 2); // 32 bytes data + 2byte CRC is max tag answer
	uint8_t *data_response = BigBuf_malloc( (32 + 2) * 2 + 2);

	bool buttonPressed = false;
	enum { IDLE, ACTIVATED, SELECTED, HALTED } chip_state = IDLE;

	while (!exitLoop) {
		WDT_HIT();

		uint32_t reader_eof_time = 0;
		len = GetIso15693CommandFromReader(receivedCmd, MAX_FRAME_SIZE, &reader_eof_time);
		if (len < 0) {
			buttonPressed = true;
			break;
		}

		// Now look at the reader command and provide appropriate responses
		// default is no response:
		modulated_response = NULL;
		modulated_response_size = 0;
		trace_data = NULL;
		trace_data_size = 0;

		if (receivedCmd[0] == ICLASS_CMD_ACTALL && len == 1) {
			// Reader in anticollision phase
			if (chip_state != HALTED) {
				modulated_response = resp_sof;
				modulated_response_size = resp_sof_Len;
				chip_state = ACTIVATED;
			}

		} else if (receivedCmd[0] == ICLASS_CMD_READ_OR_IDENTIFY && len == 1) { // identify
			// Reader asks for anticollision CSN
			if (chip_state == SELECTED || chip_state == ACTIVATED) {
				modulated_response = resp_anticoll;
				modulated_response_size = resp_anticoll_len;
				trace_data = anticoll_data;
				trace_data_size = sizeof(anticoll_data);
			}

		} else if (receivedCmd[0] == ICLASS_CMD_SELECT && len == 9) {
			// Reader selects anticollision CSN.
			// Tag sends the corresponding real CSN
			if (chip_state == ACTIVATED || chip_state == SELECTED) {
				if (!memcmp(receivedCmd+1, anticoll_data, 8)) {
					modulated_response = resp_csn;
					modulated_response_size = resp_csn_len;
					trace_data = csn_data;
					trace_data_size = sizeof(csn_data);
					chip_state = SELECTED;
				} else {
					chip_state = IDLE;
				}
			} else if (chip_state == HALTED) {
				// RESELECT with CSN
				if (!memcmp(receivedCmd+1, csn_data, 8)) {
					modulated_response = resp_csn;
					modulated_response_size = resp_csn_len;
					trace_data = csn_data;
					trace_data_size = sizeof(csn_data);
					chip_state = SELECTED;
				}
			}

		} else if (receivedCmd[0] == ICLASS_CMD_READ_OR_IDENTIFY && len == 4) { // read block
			uint16_t blockNo = receivedCmd[1];
			if (chip_state == SELECTED) {
				if (simulationMode == ICLASS_SIM_MODE_EXIT_AFTER_MAC) {
					// provide defaults for blocks 0 ... 5
					switch (blockNo) {
						case 0: // csn (block 00)
							modulated_response = resp_csn;
							modulated_response_size = resp_csn_len;
							trace_data = csn_data;
							trace_data_size = sizeof(csn_data);
							break;
						case 1: // configuration (block 01)
							modulated_response = resp_conf;
							modulated_response_size = resp_conf_len;
							trace_data = conf_block;
							trace_data_size = sizeof(conf_block);
							break;
						case 2: // e-purse (block 02)
							modulated_response = resp_cc;
							modulated_response_size = resp_cc_len;
							trace_data = card_challenge_data;
							trace_data_size = sizeof(card_challenge_data);
							// set epurse of sim2,4 attack
							if (reader_mac_buf != NULL) {
								memcpy(reader_mac_buf, card_challenge_data, 8);
							}
							break;
						case 3:
						case 4: // Kd, Kc, always respond with 0xff bytes
							modulated_response = resp_ff;
							modulated_response_size = resp_ff_len;
							trace_data = ff_data;
							trace_data_size = sizeof(ff_data);
							break;
						case 5: // Application Issuer Area (block 05)
							modulated_response = resp_aia;
							modulated_response_size = resp_aia_len;
							trace_data = aia_data;
							trace_data_size = sizeof(aia_data);
							break;
						// default: don't respond
					}
				} else if (simulationMode == ICLASS_SIM_MODE_FULL) {
					if (blockNo == 3 || blockNo == 4) { // Kd, Kc, always respond with 0xff bytes
						modulated_response = resp_ff;
						modulated_response_size = resp_ff_len;
						trace_data = ff_data;
						trace_data_size = sizeof(ff_data);
					} else { // use data from emulator memory
						memcpy(data_generic_trace, emulator + current_page*page_size + 8*blockNo, 8);
						AppendCrc(data_generic_trace, 8);
						trace_data = data_generic_trace;
						trace_data_size = 10;
						CodeIso15693AsTag(trace_data, trace_data_size);
						memcpy(data_response, ToSend, ToSendMax);
						modulated_response = data_response;
						modulated_response_size = ToSendMax;
					}
				}
			}

		} else if ((receivedCmd[0] == ICLASS_CMD_READCHECK_KD
					|| receivedCmd[0] == ICLASS_CMD_READCHECK_KC) && receivedCmd[1] == 0x02 && len == 2) {
			// Read e-purse (88 02 || 18 02)
			if (chip_state == SELECTED) {
				if(receivedCmd[0] == ICLASS_CMD_READCHECK_KD){
					cipher_state = &cipher_state_KD[current_page];
					diversified_key = diversified_key_d;
				} else {
					cipher_state = &cipher_state_KC[current_page];
					diversified_key = diversified_key_c;
				}
				modulated_response = resp_cc;
				modulated_response_size = resp_cc_len;
				trace_data = card_challenge_data;
				trace_data_size = sizeof(card_challenge_data);
			}

		} else if ((receivedCmd[0] == ICLASS_CMD_CHECK_KC
					|| receivedCmd[0] == ICLASS_CMD_CHECK_KD) && len == 9) {
			// Reader random and reader MAC!!!
			if (chip_state == SELECTED) {
				if (simulationMode == ICLASS_SIM_MODE_FULL) {
					//NR, from reader, is in receivedCmd+1
					opt_doTagMAC_2(*cipher_state, receivedCmd+1, data_generic_trace, diversified_key);
					trace_data = data_generic_trace;
					trace_data_size = 4;
					CodeIso15693AsTag(trace_data, trace_data_size);
					memcpy(data_response, ToSend, ToSendMax);
					modulated_response = data_response;
					modulated_response_size = ToSendMax;
					//exitLoop = true;
				} else { // Not fullsim, we don't respond
					// We do not know what to answer, so lets keep quiet
					if (simulationMode == ICLASS_SIM_MODE_EXIT_AFTER_MAC) {
						if (reader_mac_buf != NULL) {
							// save NR and MAC for sim 2,4
							memcpy(reader_mac_buf + 8, receivedCmd + 1, 8);
						}
						exitLoop = true;
					}
				}
			}

		} else if (receivedCmd[0] == ICLASS_CMD_HALT && len == 1) {
			if (chip_state == SELECTED) {
				// Reader ends the session
				modulated_response = resp_sof;
				modulated_response_size = resp_sof_Len;
				chip_state = HALTED;
			}

		} else if (simulationMode == ICLASS_SIM_MODE_FULL && receivedCmd[0] == ICLASS_CMD_READ4 && len == 4) {  // 0x06
			//Read 4 blocks
			if (chip_state == SELECTED) {
				uint8_t blockNo = receivedCmd[1];
				memcpy(data_generic_trace, emulator + current_page*page_size + blockNo*8, 8 * 4);
				AppendCrc(data_generic_trace, 8 * 4);
				trace_data = data_generic_trace;
				trace_data_size = 8 * 4 + 2;
				CodeIso15693AsTag(trace_data, trace_data_size);
				memcpy(data_response, ToSend, ToSendMax);
				modulated_response = data_response;
				modulated_response_size = ToSendMax;
			}

		} else if (receivedCmd[0] == ICLASS_CMD_UPDATE && (len == 12 || len == 14)) {
			// We're expected to respond with the data+crc, exactly what's already in the receivedCmd
			// receivedCmd is now UPDATE 1b | ADDRESS 1b | DATA 8b | Signature 4b or CRC 2b
			if (chip_state == SELECTED) {
				uint8_t blockNo = receivedCmd[1];
				if (blockNo == 2) { // update e-purse
					memcpy(card_challenge_data, receivedCmd+2, 8);
					CodeIso15693AsTag(card_challenge_data, sizeof(card_challenge_data));
					memcpy(resp_cc, ToSend, ToSendMax);
					resp_cc_len = ToSendMax;
					cipher_state_KD[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_key_d);
					cipher_state_KC[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_key_c);
					if (simulationMode == ICLASS_SIM_MODE_FULL) {
						memcpy(emulator + current_page*page_size + 8*2, card_challenge_data, 8);
					}
				} else if (blockNo == 3) { // update Kd
					for (int i = 0; i < 8; i++) {
						if (personalization_mode) {
							diversified_key_d[i] = receivedCmd[2 + i];
						} else {
							diversified_key_d[i] ^= receivedCmd[2 + i];
						}
					}
					cipher_state_KD[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_key_d);
					if (simulationMode == ICLASS_SIM_MODE_FULL) {
						memcpy(emulator + current_page*page_size + 8*3, diversified_key_d, 8);
					}
				} else if (blockNo == 4) { // update Kc
					for (int i = 0; i < 8; i++) {
						if (personalization_mode) {
							diversified_key_c[i] = receivedCmd[2 + i];
						} else {
							diversified_key_c[i] ^= receivedCmd[2 + i];
						}
					}
					cipher_state_KC[current_page] = opt_doTagMAC_1(card_challenge_data, diversified_key_c);
					if (simulationMode == ICLASS_SIM_MODE_FULL) {
						memcpy(emulator + current_page*page_size + 8*4, diversified_key_c, 8);
					}
				} else if (simulationMode == ICLASS_SIM_MODE_FULL) { // update any other data block
						memcpy(emulator + current_page*page_size + 8*blockNo, receivedCmd+2, 8);
				}
				memcpy(data_generic_trace, receivedCmd + 2, 8);
				AppendCrc(data_generic_trace, 8);
				trace_data = data_generic_trace;
				trace_data_size = 10;
				CodeIso15693AsTag(trace_data, trace_data_size);
				memcpy(data_response, ToSend, ToSendMax);
				modulated_response = data_response;
				modulated_response_size = ToSendMax;
			}

		} else if (receivedCmd[0] == ICLASS_CMD_PAGESEL && len == 4) {
			// Pagesel
			// Chips with a single page will not answer to this command
			// Otherwise, we should answer 8bytes (conf block 1) + 2bytes CRC
			if (chip_state == SELECTED) {
				if (simulationMode == ICLASS_SIM_MODE_FULL && max_page > 0) {
					current_page = receivedCmd[1];
					memcpy(data_generic_trace, emulator + current_page*page_size + 8*1, 8);
					memcpy(diversified_key_d, emulator + current_page*page_size + 8*3, 8);
					memcpy(diversified_key_c, emulator + current_page*page_size + 8*4, 8);
					cipher_state = &cipher_state_KD[current_page];
					personalization_mode = data_generic_trace[7] & 0x80;
					AppendCrc(data_generic_trace, 8);
					trace_data = data_generic_trace;
					trace_data_size = 10;
					CodeIso15693AsTag(trace_data, trace_data_size);
					memcpy(data_response, ToSend, ToSendMax);
					modulated_response = data_response;
					modulated_response_size = ToSendMax;
				}
			}

		} else if (receivedCmd[0] == 0x26 && len == 5) {
			// standard ISO15693 INVENTORY command. Ignore.

		} else {
			// don't know how to handle this command
			char debug_message[250]; // should be enough
			sprintf(debug_message, "Unhandled command (len = %d) received from reader:", len);
			for (int i = 0; i < len && strlen(debug_message) < sizeof(debug_message) - 3 - 1; i++) {
				sprintf(debug_message + strlen(debug_message), " %02x", receivedCmd[i]);
			}
			Dbprintf("%s", debug_message);
			// Do not respond
		}

		/**
		A legit tag has about 273,4us delay between reader EOT and tag SOF.
		**/
		if (modulated_response_size > 0) {
			uint32_t response_time = reader_eof_time + DELAY_ICLASS_VCD_TO_VICC_SIM;
			TransmitTo15693Reader(modulated_response, modulated_response_size, &response_time, 0, false);
			LogTrace_ISO15693(trace_data, trace_data_size, response_time*32, response_time*32 + modulated_response_size/2, NULL, false);
		}

	}

	if (buttonPressed)
	{
		DbpString("Button pressed");
	}
	return buttonPressed;
}

/**
 * @brief SimulateIClass simulates an iClass card.
 * @param arg0 type of simulation
 *          - 0 uses the first 8 bytes in usb data as CSN
 *          - 2 "dismantling iclass"-attack. This mode iterates through all CSN's specified
 *          in the usb data. This mode collects MAC from the reader, in order to do an offline
 *          attack on the keys. For more info, see "dismantling iclass" and proxclone.com.
 *          - Other : Uses the default CSN (031fec8af7ff12e0)
 * @param arg1 - number of CSN's contained in datain (applicable for mode 2 only)
 * @param arg2
 * @param datain
 */
void SimulateIClass(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint8_t *datain) {

	LED_A_ON();

	uint32_t simType = arg0;
	uint32_t numberOfCSNS = arg1;

	// setup hardware for simulation:
	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_NO_MODULATION);
	LED_D_OFF();
	FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);
	StartCountSspClk();

	// Enable and clear the trace
	set_tracing(true);
	clear_trace();
	//Use the emulator memory for SIM
	uint8_t *emulator = BigBuf_get_EM_addr();

	if (simType == ICLASS_SIM_MODE_CSN) {
		// Use the CSN from commandline
		memcpy(emulator, datain, 8);
		doIClassSimulation(ICLASS_SIM_MODE_CSN, NULL);
	} else if (simType == ICLASS_SIM_MODE_CSN_DEFAULT) {
		//Default CSN
		uint8_t csn_crc[] = { 0x03, 0x1f, 0xec, 0x8a, 0xf7, 0xff, 0x12, 0xe0, 0x00, 0x00 };
		// Use the CSN from commandline
		memcpy(emulator, csn_crc, 8);
		doIClassSimulation(ICLASS_SIM_MODE_CSN, NULL);
	} else if (simType == ICLASS_SIM_MODE_READER_ATTACK) {
		uint8_t mac_responses[USB_CMD_DATA_SIZE] = { 0 };
		Dbprintf("Going into attack mode, %d CSNS sent", numberOfCSNS);
		// In this mode, a number of csns are within datain. We'll simulate each one, one at a time
		// in order to collect MAC's from the reader. This can later be used in an offline-attack
		// in order to obtain the keys, as in the "dismantling iclass"-paper.
		int i;
		for (i = 0; i < numberOfCSNS && i*16+16 <= USB_CMD_DATA_SIZE; i++) {
			// The usb data is 512 bytes, fitting 32 responses (8 byte CC + 4 Byte NR + 4 Byte MAC = 16 Byte response).
			memcpy(emulator, datain+(i*8), 8);
			if (doIClassSimulation(ICLASS_SIM_MODE_EXIT_AFTER_MAC, mac_responses+i*16)) {
				 // Button pressed
				 break;
			}
			Dbprintf("CSN: %02x %02x %02x %02x %02x %02x %02x %02x",
					datain[i*8+0], datain[i*8+1], datain[i*8+2], datain[i*8+3],
					datain[i*8+4], datain[i*8+5], datain[i*8+6], datain[i*8+7]);
			Dbprintf("NR,MAC: %02x %02x %02x %02x %02x %02x %02x %02x",
					mac_responses[i*16+ 8], mac_responses[i*16+ 9], mac_responses[i*16+10], mac_responses[i*16+11],
					mac_responses[i*16+12], mac_responses[i*16+13], mac_responses[i*16+14], mac_responses[i*16+15]);
			SpinDelay(100); // give the reader some time to prepare for next CSN
		}
		cmd_send(CMD_ACK, CMD_SIMULATE_TAG_ICLASS, i, 0, mac_responses, i*16);
	} else if (simType == ICLASS_SIM_MODE_FULL) {
		//This is 'full sim' mode, where we use the emulator storage for data.
		doIClassSimulation(ICLASS_SIM_MODE_FULL, NULL);
	} else {
		// We may want a mode here where we hardcode the csns to use (from proxclone).
		// That will speed things up a little, but not required just yet.
		Dbprintf("The mode is not implemented, reserved for future use");
	}

	Dbprintf("Done...");

	LED_A_OFF();
}


/// THE READER CODE

static void ReaderTransmitIClass(uint8_t *frame, int len, uint32_t *start_time) {

	CodeIso15693AsReader(frame, len);

	TransmitTo15693Tag(ToSend, ToSendMax, start_time);

	uint32_t end_time = *start_time + 32*(8*ToSendMax-4); // substract the 4 padding bits after EOF
	LogTrace_ISO15693(frame, len, *start_time*4, end_time*4, NULL, true);
}


static bool sendCmdGetResponseWithRetries(uint8_t* command, size_t cmdsize, uint8_t* resp, size_t max_resp_size,
										  uint8_t expected_size, uint8_t retries, uint32_t start_time, uint32_t *eof_time) {
	while (retries-- > 0) {
		ReaderTransmitIClass(command, cmdsize, &start_time);
		if (expected_size == GetIso15693AnswerFromTag(resp, max_resp_size, ICLASS_READER_TIMEOUT_OTHERS, eof_time, true)) {
			return true;
		}
	}
	return false;//Error
}

/**
 * @brief Selects an iclass tag
 * @param card_data where the CSN is stored for return
 * @return false = fail
 *         true = success
 */
static bool selectIclassTag(uint8_t *card_data, uint32_t *eof_time) {
	uint8_t act_all[]      = { 0x0a };
	uint8_t identify[]     = { 0x0c };
	uint8_t select[]       = { 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	uint8_t resp[ICLASS_BUFFER_SIZE];

	uint32_t start_time = GetCountSspClk();

	// Send act_all
	ReaderTransmitIClass(act_all, 1, &start_time);
	// Card present?
	if (GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_ACTALL, eof_time, true) < 0) return false;//Fail

	//Send Identify
	start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
	ReaderTransmitIClass(identify, 1, &start_time);
	//We expect a 10-byte response here, 8 byte anticollision-CSN and 2 byte CRC
	uint8_t len = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, true);
	if (len != 10) return false;//Fail

	//Copy the Anti-collision CSN to our select-packet
	memcpy(&select[1], resp, 8);
	//Select the card
	start_time = *eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
	ReaderTransmitIClass(select, sizeof(select), &start_time);
	//We expect a 10-byte response here, 8 byte CSN and 2 byte CRC
	len = GetIso15693AnswerFromTag(resp, sizeof(resp), ICLASS_READER_TIMEOUT_OTHERS, eof_time, true);
	if (len != 10) return false;//Fail

	//Success - we got CSN
	//Save CSN in response data
	memcpy(card_data, resp, 8);

	return true;
}


// Select an iClass tag and read all blocks which are always readable without authentication
void ReaderIClass(uint8_t arg0) {

	LED_A_ON();

	uint8_t card_data[6 * 8] = {0};
	memset(card_data, 0xFF, sizeof(card_data));
	uint8_t resp[ICLASS_BUFFER_SIZE];
	//Read conf block CRC(0x01) => 0xfa 0x22
	uint8_t readConf[] = {ICLASS_CMD_READ_OR_IDENTIFY, 0x01, 0xfa, 0x22};
	//Read e-purse block CRC(0x02) => 0x61 0x10
	uint8_t readEpurse[] = {ICLASS_CMD_READ_OR_IDENTIFY, 0x02, 0x61, 0x10};
	//Read App Issuer Area block CRC(0x05) => 0xde  0x64
	uint8_t readAA[] = {ICLASS_CMD_READ_OR_IDENTIFY, 0x05, 0xde, 0x64};

	uint8_t result_status = 0;

	// test flags for what blocks to be sure to read
	uint8_t flagReadConfig = arg0 & FLAG_ICLASS_READER_CONF;
	uint8_t flagReadCC = arg0 & FLAG_ICLASS_READER_CC;
	uint8_t flagReadAA = arg0 & FLAG_ICLASS_READER_AA;

	set_tracing(true);
	clear_trace();
	Iso15693InitReader();

	StartCountSspClk();
	uint32_t start_time = 0;
	uint32_t eof_time = 0;

	if (selectIclassTag(resp, &eof_time)) {
		result_status = FLAG_ICLASS_READER_CSN;
		memcpy(card_data, resp, 8);
	}
	
	start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
	
	//Read block 1, config
	if (flagReadConfig) {
		if (sendCmdGetResponseWithRetries(readConf, sizeof(readConf), resp, sizeof(resp), 10, 10, start_time, &eof_time)) {
			result_status |= FLAG_ICLASS_READER_CONF;
			memcpy(card_data+8, resp, 8);
		} else {
			Dbprintf("Failed to read config block");
		}
		start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
	}

	//Read block 2, e-purse
	if (flagReadCC) {
		if (sendCmdGetResponseWithRetries(readEpurse, sizeof(readEpurse), resp, sizeof(resp), 10, 10, start_time, &eof_time)) {
			result_status |= FLAG_ICLASS_READER_CC;
			memcpy(card_data + (8*2), resp, 8);
		} else {
			Dbprintf("Failed to read e-purse");
		}
		start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
	}

	//Read block 5, AA
	if (flagReadAA) {
		if (sendCmdGetResponseWithRetries(readAA, sizeof(readAA), resp, sizeof(resp), 10, 10, start_time, &eof_time)) {
			result_status |= FLAG_ICLASS_READER_AA;
			memcpy(card_data + (8*5), resp, 8);
		} else {
			Dbprintf("Failed to read AA block");
		}
	}

	cmd_send(CMD_ACK, result_status, 0, 0, card_data, sizeof(card_data));

	LED_A_OFF();
}


void ReaderIClass_Replay(uint8_t arg0, uint8_t *MAC) {

	LED_A_ON();

	bool use_credit_key = false;
	uint8_t card_data[USB_CMD_DATA_SIZE]={0};
	uint16_t block_crc_LUT[255] = {0};

	//Generate a lookup table for block crc
	for (int block = 0; block < 255; block++){
		char bl = block;
		block_crc_LUT[block] = iclass_crc16(&bl ,1);
	}
	//Dbprintf("Lookup table: %02x %02x %02x" ,block_crc_LUT[0],block_crc_LUT[1],block_crc_LUT[2]);

	uint8_t readcheck_cc[] = { ICLASS_CMD_READCHECK_KD, 0x02 };
	if (use_credit_key)
		readcheck_cc[0] = ICLASS_CMD_READCHECK_KC;
	uint8_t check[]       = { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t read[]        = { 0x0c, 0x00, 0x00, 0x00 };

	uint16_t crc = 0;
	uint8_t cardsize = 0;
	uint8_t mem = 0;

	static struct memory_t {
		int k16;
		int book;
		int k2;
		int lockauth;
		int keyaccess;
	} memory;

	uint8_t resp[ICLASS_BUFFER_SIZE];

	set_tracing(true);
	clear_trace();
	Iso15693InitReader();

	StartCountSspClk();
	uint32_t start_time = 0;
	uint32_t eof_time = 0;

	while (!BUTTON_PRESS()) {

		WDT_HIT();

		if (!get_tracing()) {
			DbpString("Trace full");
			break;
		}

		if (!selectIclassTag(card_data, &eof_time)) continue;

		start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
		if (!sendCmdGetResponseWithRetries(readcheck_cc, sizeof(readcheck_cc), resp, sizeof(resp), 8, 3, start_time, &eof_time)) continue;

		// replay captured auth (cc must not have been updated)
		memcpy(check+5, MAC, 4);

		start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
		if (!sendCmdGetResponseWithRetries(check, sizeof(check), resp, sizeof(resp), 4, 5, start_time, &eof_time)) {
			Dbprintf("Error: Authentication Fail!");
			continue;
		}

		//first get configuration block (block 1)
		crc = block_crc_LUT[1];
		read[1] = 1;
		read[2] = crc >> 8;
		read[3] = crc & 0xff;

		start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
		if (!sendCmdGetResponseWithRetries(read, sizeof(read), resp, sizeof(resp), 10, 10, start_time, &eof_time)) {
			start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
			Dbprintf("Dump config (block 1) failed");
			continue;
		}

		mem = resp[5];
		memory.k16 = (mem & 0x80);
		memory.book = (mem & 0x20);
		memory.k2 = (mem & 0x8);
		memory.lockauth = (mem & 0x2);
		memory.keyaccess = (mem & 0x1);

		cardsize = memory.k16 ? 255 : 32;
		WDT_HIT();
		//Set card_data to all zeroes, we'll fill it with data
		memset(card_data, 0x0, USB_CMD_DATA_SIZE);
		uint8_t failedRead = 0;
		uint32_t stored_data_length = 0;
		//then loop around remaining blocks
		for (int block = 0; block < cardsize; block++) {
			read[1] = block;
			crc = block_crc_LUT[block];
			read[2] = crc >> 8;
			read[3] = crc & 0xff;

			start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
			if (sendCmdGetResponseWithRetries(read, sizeof(read), resp, sizeof(resp), 10, 10, start_time, &eof_time)) {
				Dbprintf("     %02x: %02x %02x %02x %02x %02x %02x %02x %02x",
						block, resp[0], resp[1], resp[2],
						resp[3], resp[4], resp[5],
						resp[6], resp[7]);

				//Fill up the buffer
				memcpy(card_data+stored_data_length, resp, 8);
				stored_data_length += 8;
				if (stored_data_length +8 > USB_CMD_DATA_SIZE) {
					//Time to send this off and start afresh
					cmd_send(CMD_ACK,
							 stored_data_length,//data length
							 failedRead,//Failed blocks?
							 0,//Not used ATM
							 card_data, stored_data_length);
					//reset
					stored_data_length = 0;
					failedRead = 0;
				}

			} else {
				failedRead = 1;
				stored_data_length += 8;//Otherwise, data becomes misaligned
				Dbprintf("Failed to dump block %d", block);
			}
		}

		//Send off any remaining data
		if (stored_data_length > 0) {
			cmd_send(CMD_ACK,
					 stored_data_length,//data length
					 failedRead,//Failed blocks?
					 0,//Not used ATM
					 card_data,
					 stored_data_length);
		}
		//If we got here, let's break
		break;
	}
	//Signal end of transmission
	cmd_send(CMD_ACK,
			 0,//data length
			 0,//Failed blocks?
			 0,//Not used ATM
			 card_data,
			 0);

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();
	LED_A_OFF();
}


void iClass_Check(uint8_t *MAC) {
	uint8_t check[9] = {ICLASS_CMD_CHECK_KD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8_t resp[4];
	memcpy(check+5, MAC, 4);
	uint32_t eof_time;
	bool isOK = sendCmdGetResponseWithRetries(check, sizeof(check), resp, sizeof(resp), 4, 6, 0, &eof_time);
	cmd_send(CMD_ACK, isOK, 0, 0, resp, sizeof(resp));
}


void iClass_Readcheck(uint8_t block, bool use_credit_key) {
	uint8_t readcheck[2] = {ICLASS_CMD_READCHECK_KD, block};
	if (use_credit_key) {
		readcheck[0] = ICLASS_CMD_READCHECK_KC;
	}
	uint8_t resp[8];
	uint32_t eof_time;
	bool isOK = sendCmdGetResponseWithRetries(readcheck, sizeof(readcheck), resp, sizeof(resp), 8, 6, 0, &eof_time);
	cmd_send(CMD_ACK, isOK, 0, 0, resp, sizeof(resp));
}


static bool iClass_ReadBlock(uint8_t blockNo, uint8_t *readdata) {
	uint8_t readcmd[] = {ICLASS_CMD_READ_OR_IDENTIFY, blockNo, 0x00, 0x00}; //0x88, 0x00 // can i use 0C?
	char bl = blockNo;
	uint16_t rdCrc = iclass_crc16(&bl, 1);
	readcmd[2] = rdCrc >> 8;
	readcmd[3] = rdCrc & 0xff;
	uint8_t resp[10];
	bool isOK = false;
	uint32_t eof_time;

	isOK = sendCmdGetResponseWithRetries(readcmd, sizeof(readcmd), resp, sizeof(resp), 10, 10, 0, &eof_time);
	memcpy(readdata, resp, sizeof(resp));

	return isOK;
}


void iClass_ReadBlk(uint8_t blockno) {

	LED_A_ON();

	uint8_t readblockdata[] = {0,0,0,0,0,0,0,0,0,0};
	bool isOK = false;
	isOK = iClass_ReadBlock(blockno, readblockdata);
	cmd_send(CMD_ACK, isOK, 0, 0, readblockdata, 8);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();

	LED_A_OFF();
}

void iClass_Dump(uint8_t blockno, uint8_t numblks) {

	LED_A_ON();

	uint8_t readblockdata[] = {0,0,0,0,0,0,0,0,0,0};
	bool isOK = false;
	uint8_t blkCnt = 0;

	BigBuf_free();
	uint8_t *dataout = BigBuf_malloc(255*8);
	if (dataout == NULL) {
		Dbprintf("out of memory");
		FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
		LED_D_OFF();
		cmd_send(CMD_ACK, 0, 1, 0, 0, 0);
		LED_A_OFF();
		return;
	}
	memset(dataout, 0xFF, 255*8);

	for ( ; blkCnt < numblks; blkCnt++) {
		isOK = iClass_ReadBlock(blockno+blkCnt, readblockdata);
		if (!isOK || (readblockdata[0] == 0xBB || readblockdata[7] == 0xBB || readblockdata[2] == 0xBB)) { //try again
			isOK = iClass_ReadBlock(blockno+blkCnt, readblockdata);
			if (!isOK) {
				Dbprintf("Block %02X failed to read", blkCnt+blockno);
				break;
			}
		}
		memcpy(dataout + (blkCnt*8), readblockdata, 8);
	}
	//return pointer to dump memory in arg3
	cmd_send(CMD_ACK, isOK, blkCnt, BigBuf_max_traceLen(), 0, 0);

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();
	BigBuf_free();

	LED_A_OFF();
}


static bool iClass_WriteBlock_ext(uint8_t blockNo, uint8_t *data) {

	LED_A_ON();

	uint8_t write[] = { ICLASS_CMD_UPDATE, blockNo, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	//uint8_t readblockdata[10];
	//write[1] = blockNo;
	memcpy(write+2, data, 12); // data + mac
	char *wrCmd = (char *)(write+1);
	uint16_t wrCrc = iclass_crc16(wrCmd, 13);
	write[14] = wrCrc >> 8;
	write[15] = wrCrc & 0xff;
	uint8_t resp[10];
	bool isOK = false;
	uint32_t eof_time = 0;

	isOK = sendCmdGetResponseWithRetries(write, sizeof(write), resp, sizeof(resp), 10, 10, 0, &eof_time);
	uint32_t start_time = eof_time + DELAY_ICLASS_VICC_TO_VCD_READER;
	if (isOK) { //if reader responded correctly
		//Dbprintf("WriteResp: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",resp[0],resp[1],resp[2],resp[3],resp[4],resp[5],resp[6],resp[7],resp[8],resp[9]);
		if (memcmp(write+2, resp, 8)) {  //if response is not equal to write values
			if (blockNo != 3 && blockNo != 4) { //if not programming key areas (note key blocks don't get programmed with actual key data it is xor data)
				//error try again
				isOK = sendCmdGetResponseWithRetries(write, sizeof(write), resp, sizeof(resp), 10, 10, start_time, &eof_time);
			}
		}
	}

	LED_A_OFF();

	return isOK;
}


void iClass_WriteBlock(uint8_t blockNo, uint8_t *data) {

	LED_A_ON();

	bool isOK = iClass_WriteBlock_ext(blockNo, data);
	if (isOK){
		Dbprintf("Write block [%02x] successful", blockNo);
	} else {
		Dbprintf("Write block [%02x] failed", blockNo);
	}
	cmd_send(CMD_ACK, isOK, 0, 0, 0, 0);

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();

	LED_A_OFF();
}

void iClass_Clone(uint8_t startblock, uint8_t endblock, uint8_t *data) {
	int i;
	int written = 0;
	int total_block = (endblock - startblock) + 1;
	for (i = 0; i < total_block; i++) {
		// block number
		if (iClass_WriteBlock_ext(i+startblock, data + (i*12))){
			Dbprintf("Write block [%02x] successful", i + startblock);
			written++;
		} else {
			if (iClass_WriteBlock_ext(i+startblock, data + (i*12))){
				Dbprintf("Write block [%02x] successful", i + startblock);
				written++;
			} else {
				Dbprintf("Write block [%02x] failed", i + startblock);
			}
		}
	}
	if (written == total_block)
		Dbprintf("Clone complete");
	else
		Dbprintf("Clone incomplete");

	cmd_send(CMD_ACK, 1, 0, 0, 0, 0);
	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	LED_D_OFF();
	LED_A_OFF();
}
