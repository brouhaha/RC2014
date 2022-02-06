/*
 *	Platform features
 *
 *	Z180 at 18.432Hz
 *	1MB RAM / 512KB ROM
 *	CSIO to SPI with multiple device mux/demux for obnoxious devices
 *	78-7F: 82C55
 *	70-77: FDC
 *
 *	Extmem low disables onboard high RAM
 *	ROMen high enables low 512K ROM else RAM
 *
 *	Outputs from the 82C55 start high
 *
 *	The 82C55 drives
 *
 *	Port A (out)
 *	7: Keyboard Data
 *	6: Keyboard Clock
 *	5: Mouse Data
 *	4: Mouse Clock
 *	3: ROM Enable
 *	2: External Memory Enable (active low)
 *	1: SCL
 *	0: SDA
 *
 *	Port B (in)
 *	7: SDA
 *	6: SCL
 *	5: Fire2
 *	4: Fire
 *	3: Right
 *	2: Left
 *	1: Down
 *	0: Up
 *
 *	Port C(I/O)
 *
 *	7: Keyboard Data In
 *	6: Keyboard Clock In
 *	5: Mouse Data In
 *	4: Mouse Clock In
 *	3: DC		- routed to some SPI ports
 *	2: CSEL2	}
 *	1: CSEL1	}	SPI device select
 *	0: CSEL0	}
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "system.h"
#include "libz180/z180.h"
#include "lib765/include/765.h"
#include "z180_io.h"

#include "rtc_bitbang.h"
#include "sdcard.h"
#include "z80dis.h"
#include "zxkey.h"

#include "ide.h"

static uint8_t ram[1024 * 1024];	/* 1MB RAM */
static uint8_t rom[512 * 1024];		/* 512K ROM */
static uint8_t port_a = 0xFF;
static uint8_t port_b = 0xFF;
static uint8_t port_c = 0xFF;
static uint8_t port_ctl = 0x9B;

static uint8_t fast = 0;
static uint8_t int_recalc = 0;
static uint8_t leds = 0;

static struct sdcard *sdcard;
static FDC_PTR fdc;
static FDRV_PTR drive_a, drive_b;
static struct z180_io *io;

static uint8_t ide = 0;
static struct ide_controller *ide0;

static uint16_t tstate_steps = 737;	/* 18.432MHz */

/* IRQ source that is live in IM2 */
static uint8_t live_irq;

static Z180Context cpu_z180;

volatile int emulator_done;

#define TRACE_MEM	0x000001
#define TRACE_IO	0x000002
#define TRACE_UNK	0x000004
#define TRACE_CPU	0x000008
#define TRACE_CPU_IO	0x000010
#define TRACE_IRQ	0x000020
#define TRACE_SD	0x008040
#define TRACE_FDC	0x000080
#define TRACE_SPI	0x000100
#define TRACE_IDE	0x000200

static int trace = 0;

static void reti_event(void);

/*
 *	Model the physical bus interface including wrapping and
 *	the like. This is used directly by the DMA engines
 */
uint8_t z180_phys_read(int unused, uint32_t addr)
{
	if (addr & 0x80000) {
		if (port_a & 0x04)
			return ram[addr & 0xFFFFF];
		else
			return 0xFF;
	}
	if (port_a & 0x08)
		return rom[addr & 0x3FFFF];
	else
		return ram[addr & 0xFFFFF];
}

void z180_phys_write(int unused, uint32_t addr, uint8_t val)
{
	addr &= 0xFFFFF;
	if (addr & 0x80000) {
		if (port_a & 0x04)
			ram[addr] = val;
		return;
	}
	if (port_a & 0x08)
		fprintf(stderr, "[%06X: write to ROM.]\n", addr);
	else
		ram[addr] = val;
}

/*
 *	Model CPU accesses starting with a virtual address
 */
static uint8_t do_mem_read0(uint16_t addr, int quiet)
{
	uint32_t pa = z180_mmu_translate(io, addr);
	uint8_t r;
	r = z180_phys_read(0, pa);
	if (!quiet && (trace & TRACE_MEM))
		fprintf(stderr, "R %04X[%06X] -> %02X\n", addr, pa, r);
	return r;
}

static void mem_write0(uint16_t addr, uint8_t val)
{
	uint32_t pa = z180_mmu_translate(io, addr);
	if (trace & TRACE_MEM)
		fprintf(stderr, "W: %04X[%06X] <- %02X\n", addr, pa, val);
	z180_phys_write(0, pa, val);
}

uint8_t mem_read(int unused, uint16_t addr)
{
	static uint8_t rstate = 0;
	uint8_t r = do_mem_read0(addr, 0);

	if (cpu_z180.M1) {
		/* DD FD CB see the Z80 interrupt manual */
		if (r == 0xDD || r == 0xFD || r == 0xCB) {
			rstate = 2;
			return r;
		}
		/* Look for ED with M1, followed directly by 4D and if so trigger
		   the interrupt chain */
		if (r == 0xED && rstate == 0) {
			rstate = 1;
			return r;
		}
	}
	if (r == 0x4D && rstate == 1)
		reti_event();
	rstate = 0;
	return r;
}

void mem_write(int unused, uint16_t addr, uint8_t val)
{
	mem_write0(addr, val);
}

static unsigned int nbytes;

uint8_t z80dis_byte(uint16_t addr)
{
	uint8_t r = do_mem_read0(addr, 1);
	fprintf(stderr, "%02X ", r);
	nbytes++;
	return r;
}

uint8_t z80dis_byte_quiet(uint16_t addr)
{
	return do_mem_read0(addr, 1);
}

static void rc2014_trace(unsigned unused)
{
	static uint32_t lastpc = -1;
	char buf[256];

	if ((trace & TRACE_CPU) == 0)
		return;
	nbytes = 0;
	/* Spot XXXR repeating instructions and squash the trace */
	if (cpu_z180.M1PC == lastpc && z80dis_byte_quiet(lastpc) == 0xED &&
		(z80dis_byte_quiet(lastpc + 1) & 0xF4) == 0xB0) {
		return;
	}
	lastpc = cpu_z180.M1PC;
	fprintf(stderr, "%04X: ", lastpc);
	z80_disasm(buf, lastpc);
	while(nbytes++ < 6)
		fprintf(stderr, "   ");
	fprintf(stderr, "%-16s ", buf);
	fprintf(stderr, "[ %02X:%02X %04X %04X %04X %04X %04X %04X ]\n",
		cpu_z180.R1.br.A, cpu_z180.R1.br.F,
		cpu_z180.R1.wr.BC, cpu_z180.R1.wr.DE, cpu_z180.R1.wr.HL,
		cpu_z180.R1.wr.IX, cpu_z180.R1.wr.IY, cpu_z180.R1.wr.SP);
}

unsigned int check_chario(void)
{
	fd_set i, o;
	struct timeval tv;
	unsigned int r = 0;

	FD_ZERO(&i);
	FD_SET(0, &i);
	FD_ZERO(&o);
	FD_SET(1, &o);
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (select(2, &i, NULL, NULL, &tv) == -1) {
		if (errno == EINTR)
			return 0;
		perror("select");
		exit(1);
	}
	if (FD_ISSET(0, &i))
		r |= 1;
	if (FD_ISSET(1, &o))
		r |= 2;
	return r;
}

unsigned int next_char(void)
{
	char c;
	if (read(0, &c, 1) != 1) {
		printf("(tty read without ready byte)\n");
		return 0xFF;
	}
	if (c == 0x0A)
		c = '\r';
	return c;
}

void recalc_interrupts(void)
{
	int_recalc = 1;
}

/* Software SPI test: one device for now */

#include "bitrev.h"

uint8_t z180_csio_write(struct z180_io *io, uint8_t bits)
{
	uint8_t r;

	if (sdcard == NULL)
		return 0xFF;

	r = bitrev[sd_spi_in(sdcard, bitrev[bits])];
	if (trace & TRACE_SPI)
		fprintf(stderr,	"[SPI %02X:%02X]\n", bitrev[bits], bitrev[r]);
	return r;
}

static void fdc_log(int debuglevel, char *fmt, va_list ap)
{
	if ((trace & TRACE_FDC) || debuglevel == 0)
		vfprintf(stderr, "fdc: ", ap);
}

static void fdc_write(uint8_t addr, uint8_t val)
{
	switch(addr) {
	case 1:	/* Data */
		fprintf(stderr, "FDC Data: %02X\n", val);
		fdc_write_data(fdc, val);
		break;
	case 2:	/* DOR */
		fprintf(stderr, "FDC DOR %02X [", val);
		if (val & 0x80)
			fprintf(stderr, "SPECIAL ");
		else
			fprintf(stderr, "AT/EISA ");
		if (val & 0x20)
			fprintf(stderr, "MOEN2 ");
		if (val & 0x10)
			fprintf(stderr, "MOEN1 ");
		if (val & 0x08)
			fprintf(stderr, "DMA ");
		if (!(val & 0x04))
			fprintf(stderr, "SRST ");
		if (!(val & 0x02))
			fprintf(stderr, "DSEN ");
		if (val & 0x01)
			fprintf(stderr, "DSEL1");
		else
			fprintf(stderr, "DSEL0");
		fprintf(stderr, "]\n");
		fdc_write_dor(fdc, val);
#if 0		
		if ((val & 0x21) == 0x21)
			fdc_set_motor(fdc, 2);
		else if ((val & 0x11) == 0x10)
			fdc_set_motor(fdc, 1);
		else
			fdc_set_motor(fdc, 0);
#endif			
		break;
	case 3:	/* DCR */
		fprintf(stderr, "FDC DCR %02X [", val);
		if (!(val & 4))
			fprintf(stderr, "WCOMP");
		switch(val & 3) {
		case 0:
			fprintf(stderr, "500K MFM RPM");
			break;
		case 1:
			fprintf(stderr, "250K MFM");
			break;
		case 2:
			fprintf(stderr, "250K MFM RPM");
			break;
		case 3:
			fprintf(stderr, "INVALID");
		}
		fprintf(stderr, "]\n");
		fdc_write_drr(fdc, val & 3);	/* TODO: review */
		break;
	case 4:	/* TC */
		fdc_set_terminal_count(fdc, 0);
		fdc_set_terminal_count(fdc, 1);
		fprintf(stderr, "FDC TC\n");
		break;
	case 5:	/* RESET */
		fprintf(stderr, "FDC RESET\n");
		break;
	default:
		fprintf(stderr, "FDC bogus %02X->%02X\n", addr, val);
	}
}

static uint8_t fdc_read(uint8_t addr)
{
	uint8_t val = 0x78;
	switch(addr) {
	case 0:	/* Status*/
		fprintf(stderr, "FDC Read Status: ");
		val = fdc_read_ctrl(fdc);
		break;
	case 1:	/* Data */
		fprintf(stderr, "FDC Read Data: ");
		val = fdc_read_data(fdc);
		break;
	case 4:	/* TC */
		fprintf(stderr, "FDC TC: ");
		break;
	case 5:	/* RESET */
		fprintf(stderr, "FDC RESET: ");
		break;
	default:
		fprintf(stderr, "FDC bogus read %02X: ", addr);
	}
	fprintf(stderr, "%02X\n", val);
	return val;
}

static void diag_write(uint8_t val)
{
	uint8_t x[12];
	unsigned int i;
	if (leds == 0)
		return;

	memcpy(x, "\n[--------]\n", 12);
	for (i = 0; i < 8; i++)
		if (val & (1 << i))
			x[i + 2] = '@';
	write(1, x, 12);
}

/* The 82C55 has 3 modes but we only really model mode 0 */

static void ppi_recalc(void)
{
	/* Model the SD card on CSIO and chip select line 0 */
	static unsigned int old_cs = 7;
	unsigned int new_cs = port_c & 7;
	if (sdcard && new_cs != old_cs) {
		if (old_cs == 0)
			sd_spi_raise_cs(sdcard);
		else if (new_cs == 0)
			sd_spi_lower_cs(sdcard);
		old_cs = new_cs;
	}
}

static uint8_t ppi_read(uint8_t addr)
{
	switch(addr) {
		case 0:
			if (port_ctl & 0x10)		/* Port A is input */
				return 0xFF;		/* For now */
			else
				return port_a;
		case 1:
			if (port_ctl & 0x02)		/* Port B is input */
				return 0xFF;		/* TODO: model SDA/SCL */
			else
				return port_b;
		case 2: {
			/* This one has nybble size control */
			uint8_t r;
			if (port_ctl & 0x01)		/* Port C lower */
				r = 0x0F;
			else
				r = port_c & 0x0F;
			if (port_ctl & 0x08)		/* Port C upper */
				r |= 0xF0;		/* No keyboard model yet */
			else
				r |= port_c & 0xF0;
			return r;
			}
		case 3:
			return port_ctl;
		default:
			fprintf(stderr, "Invalid PPI offset.\n");
			return 0xFF;
	}
}

static void ppi_write(uint8_t addr, uint8_t val)
{
	switch(addr) {
		case 0:
			port_a = val;
			break;
		case 1:
			port_b = val;
			break;
		case 2:
			port_c = val;
			break;
		case 3:
			/* Bit 7 controls mode set or bit level control */
			if (val & 0x80)
				port_ctl = val;
			else {
				unsigned int bit = val & 1;
				val >>= 1;
				val &= 0x07;
				port_c &= ~(1 << val);
				if (bit)
					port_c |= 1 << val;
			}
			break;
		default:
			fprintf(stderr, "Invalid PPI offset.\n");
	}
	ppi_recalc();
}

static uint8_t my_ide_read(uint16_t addr)
{
	uint8_t r =  ide_read8(ide0, addr);
	if (trace & TRACE_IDE)
		fprintf(stderr, "ide read %d = %02X\n", addr, r);
	return r;
}

static void my_ide_write(uint16_t addr, uint8_t val)
{
	if (trace & TRACE_IDE)
		fprintf(stderr, "ide write %d = %02X\n", addr, val);
	ide_write8(ide0, addr, val);
}


uint8_t io_read(int unused, uint16_t addr)
{
	if (trace & TRACE_IO)
		fprintf(stderr, "read %02x\n", addr);
	if (z180_iospace(io, addr))
		return z180_read(io, addr);
	addr &= 0xFF;
	if ((addr >= 0x10 && addr <= 0x17) && ide == 1)
		return my_ide_read(addr & 7);
	if (addr >= 0x70 && addr < 0x77) 
		return fdc_read(addr & 7);
	if (addr >= 0x78 && addr <= 0x7F)
		return ppi_read(addr & 3);
	if (trace & TRACE_UNK)
		fprintf(stderr, "Unknown read from port %04X\n", addr);
	return 0xFF;
}

void io_write(int unused, uint16_t addr, uint8_t val)
{
	if (trace & TRACE_IO)
		fprintf(stderr, "write %02x <- %02x\n", addr, val);

	if (z180_iospace(io, addr)) {
		z180_write(io, addr, val);
		return;
	}

	addr &= 0xFF;

	if ((addr >= 0x10 && addr <= 0x17) && ide == 1)
		my_ide_write(addr & 7, val);
	else if (addr >= 0x70 && addr < 0x78)
		fdc_write(addr & 7, val);
	else if (addr >= 0x78 && addr <= 0x7F)
		ppi_write(addr & 3, val);
	else if (addr == 0x0D)
		diag_write(val);
	else if (addr == 0xFD) {
		trace &= 0xFF00;
		trace |= val;
		printf("trace set to %04X\n", trace);
	} else if (addr == 0xFE) {
		trace &= 0xFF;
		trace |= val << 8;
		printf("trace set to %d\n", trace);
	} else if (trace & TRACE_UNK)
		fprintf(stderr, "Unknown write to port %04X of %02X\n", addr, val);
}

static void poll_irq_event(void)
{
	z180_interrupt(io, 0, 0, 0);
}

static void reti_event(void)
{
	if (live_irq && (trace & TRACE_IRQ))
		fprintf(stderr, "RETI\n");
	live_irq = 0;
	poll_irq_event();
}

static struct termios saved_term, term;

static void cleanup(int sig)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
	emulator_done = 1;
}

static void exit_cleanup(void)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
}

static void usage(void)
{
	fprintf(stderr, "z180-mini-itx: [-f] [-R] [-r rompath] [-w] [-i idepath] [-S sdpath] [-d debug]\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	static struct timespec tc;
	int opt;
	int fd;
	char *rompath = "z180-mini-itx.rom";
	char *sdpath = NULL, *idepath = NULL;
	char *patha = NULL, *pathb = NULL;

	uint8_t *p = ram;
	while (p < ram + sizeof(ram))
		*p++= rand();

	while ((opt = getopt(argc, argv, "d:fF:lRS:i:")) != -1) {
		switch (opt) {
		case 'r':
			rompath = optarg;
			break;
		case 'S':
			sdpath = optarg;
			break;
		case 'd':
			trace = atoi(optarg);
			break;
		case 'l':
			leds = 1;
			break;
		case 'f':
			fast = 1;
			break;
		case 'F':
			if (pathb) {
				fprintf(stderr, "z180-mini-itx: too many floppy disks specified.\n");
				exit(1);
			}
			if (patha)
				pathb = optarg;
			else
				patha = optarg;
			break;
		case 'i':	/* Model a plugged in CF adapter for debugging convenience on the ROM */
			idepath = optarg;
			ide = 1;
			break;
		default:
			usage();
		}
	}
	if (optind < argc)
		usage();

	fd = open(rompath, O_RDONLY);
	if (fd == -1) {
		perror(rompath);
		exit(EXIT_FAILURE);
	}
	if (read(fd, rom, 512 * 1024) != 512 * 1024) {
		fprintf(stderr, "z180-mini-itx: ROM image should be 512K.\n");
		exit(EXIT_FAILURE);
	}
	close(fd);

	if (sdpath) {
		sdcard = sd_create("sd0");
		fd = open(sdpath, O_RDWR);
		if (fd == -1) {
			perror(sdpath);
			exit(1);
		}
		sd_attach(sdcard, fd);
		if (trace & TRACE_SD)
			sd_trace(sdcard, 1);
	}

	if (idepath) {
		ide0 = ide_allocate("cf");
		if (ide0) {
			int ide_fd = open(idepath, O_RDWR);
			if (ide_fd == -1) {
				perror(idepath);
				ide = 0;
			}
			if (ide_attach(ide0, 0, ide_fd) == 0) {
				ide = 1;
				ide_reset_begin(ide0);
			}
		} else
			ide = 0;
	}

	io = z180_create(&cpu_z180);
	z180_trace(io, trace & TRACE_CPU_IO);
	z180_set_input(io, 0, 1);

	fdc = fdc_new();

	lib765_register_error_function(fdc_log);

	if (patha) {
		drive_a = fd_newdsk();
		fd_settype(drive_a, FD_35);
		fd_setheads(drive_a, 2);
		fd_setcyls(drive_a, 80);
		fdd_setfilename(drive_a, patha);
	} else
		drive_a = fd_new();

	if (pathb) {
		drive_b = fd_newdsk();
		fd_settype(drive_a, FD_35);
		fd_setheads(drive_a, 2);
		fd_setcyls(drive_a, 80);
		fdd_setfilename(drive_a, pathb);
	} else
		drive_b = fd_new();

	fdc_reset(fdc);
	fdc_setisr(fdc, NULL);

	fdc_setdrive(fdc, 0, drive_a);
	fdc_setdrive(fdc, 1, drive_b);

	/* 20ms - it's a balance between nice behaviour and simulation
	   smoothness */
	tc.tv_sec = 0;
	tc.tv_nsec = 20000000L;

	if (tcgetattr(0, &term) == 0) {
		saved_term = term;
		atexit(exit_cleanup);
		signal(SIGINT, cleanup);
		signal(SIGQUIT, cleanup);
		signal(SIGPIPE, cleanup);
		term.c_lflag &= ~(ICANON | ECHO);
		term.c_cc[VMIN] = 0;
		term.c_cc[VTIME] = 1;
		term.c_cc[VINTR] = 0;
		term.c_cc[VSUSP] = 0;
		term.c_cc[VSTOP] = 0;
		tcsetattr(0, TCSADRAIN, &term);
	}

	Z180RESET(&cpu_z180);
	cpu_z180.ioRead = io_read;
	cpu_z180.ioWrite = io_write;
	cpu_z180.memRead = mem_read;
	cpu_z180.memWrite = mem_write;
	cpu_z180.trace = rc2014_trace;

	/* This is the wrong way to do it but it's easier for the moment. We
	   should track how much real time has occurred and try to keep cycle
	   matched with that. The scheme here works fine except when the host
	   is loaded though */

	while (!emulator_done) {
		int states = 0;
		unsigned int i, j;
		/* We have to run the DMA engine and Z180 in step per
		   instruction otherwise we will mess up on stalling DMA */

		/* Do an emulated 20ms of work (368640 clocks) */
		for (i = 0; i < 50; i++) {
			for (j = 0; j < 10; j++) {
				while (states < tstate_steps) {
					unsigned int used;
					used = z180_dma(io);
					if (used == 0)
						used = Z180Execute(&cpu_z180);
					states += used;
				}
				z180_event(io, states);
				states -= tstate_steps;
			}
			fdc_tick(fdc);
		}

		/* Do 20ms of I/O and delays */
		if (!fast)
			nanosleep(&tc, NULL);
		if (int_recalc) {
			/* If there is no pending Z180 vector IRQ but we think
			   there now might be one we use the same logic as for
			   reti */
			if (!live_irq)
				poll_irq_event();
			/* Clear this after because reti_event may set the
			   flags to indicate there is more happening. We will
			   pick up the next state changes on the reti if so */
			if (!(cpu_z180.IFF1|cpu_z180.IFF2))
				int_recalc = 0;
		}
	}
	fd_eject(drive_a);
	fd_eject(drive_b);
	fdc_destroy(&fdc);
	fd_destroy(&drive_a);
	fd_destroy(&drive_b);
	exit(0);
}
