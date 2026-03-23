/* UniDOS emulator */
/* By Nguyen Anh Quynh, 2015 */

#include <unicorn/unicorn.h>
#include <string.h>

#include "ints/ints.h"
#include "global.h"
#include "psp.h"
#include "intvec.h"

// Real DOS loads .COM files into a segment chosen from free memory,
// typically around 0x1000 (physical 0x10000).  The PSP occupies the
// first 0x100 bytes of that segment; the .COM image starts at offset
// 0x100 within it.  Using segment 0 breaks programs that compute
// addresses relative to CS (e.g. self-relocation stubs that do CS-N).
#define COM_SEG  0x01A4          // ~26 KB in, well above IVT/BDA
#define DOS_ADDR (COM_SEG * 16 + 0x100)  // physical start of .COM code


static void usage(char* prog)
{
	printf("UniDOS for DOS emulation. Based on Unicorn engine (www.unicorn-engine.org)\n");
	printf("Syntax: %s [-v] [--dump-mem[=FILE]] <COM> [args...]\n", prog);
	printf("  --dump-mem        Dump emulated memory to 'mem.bin' after exit\n");
	printf("  --dump-mem=FILE   Dump emulated memory to FILE after exit\n");
}

// callback for handling interrupt
void hook_intr(uc_engine* uc, uint32_t intno, void* user_data)
{
	uint32_t r_ip;
	uint8_t r_ah;

	uc_reg_read(uc, UC_X86_REG_IP, &r_ip);
	uc_reg_read(uc, UC_X86_REG_AH, &r_ah);

	// only handle DOS interrupt

	struct interrupt* custom_handler = intvec_find(intno);

	if (custom_handler)
	{
		printf(">>> 0x%x: using custom handler at %04x:%04x for interrupt %02x\n", r_ip, custom_handler->seg,
			   custom_handler->off, intno);
		r_ip = MK_FP(custom_handler->seg, custom_handler->off);
		uc_reg_write(uc, UC_X86_REG_IP, &r_ip);
		return;
	}

	switch (intno)
	{
		default:
			printf(">>> 0x%x: unknown interrupt: %02x, function %02x\n", r_ip, intno, r_ah);
			break;
		case 0x05:
			break;
		case 0x10:
			int10();
			break;
		case 0x15:
			int15();
			break;
		case 0x21:
			int21();
			break;
		case 0x20:
			int20();
			break;
	}
}

// Unicorn's real-mode RETF handling is unreliable when CS changes to a
// segment it hasn't seen fetched from before -- it raises UC_ERR_FETCH_UNMAPPED
// even though the target memory is mapped and valid.  Work around this by
// intercepting every RETF, doing the pop IP / pop CS ourselves, and writing
// the results back so Unicorn resumes at the correct CS:IP.
void hook_retf(uc_engine* uc, void* user_data)
{
	uint32_t r_ss, r_sp;
	uc_reg_read(uc, UC_X86_REG_SS, &r_ss);
	uc_reg_read(uc, UC_X86_REG_SP, &r_sp);

	uint32_t stack_phys = MK_FP(r_ss, r_sp);

	uint16_t new_ip, new_cs;
	uc_mem_read(uc, stack_phys,     &new_ip, 2);
	uc_mem_read(uc, stack_phys + 2, &new_cs, 2);

	r_sp += 4;   // consume the two words
	uc_reg_write(uc, UC_X86_REG_SP, &r_sp);

	uint32_t cs = new_cs, ip = new_ip;
	uc_reg_write(uc, UC_X86_REG_CS, &cs);
	uc_reg_write(uc, UC_X86_REG_IP, &ip);

	dbgprintf(">>> retf -> %04x:%04x (phys %05x)\n",
			  new_cs, new_ip, MK_FP(new_cs, new_ip));
}


uc_engine* uc;

int main(int argc, char** argv)
{
	uc_hook trace;
	uc_err err;
	char* fname;
	FILE* f;
	// Flat image buffer: [0x000..0x0FF] = PSP, [0x100..] = .COM code.
	// This is written wholesale to COM_SEG*16 in emulated memory.
	// Must be 64 KB so the .COM image (up to ~64 KB) fits after the PSP.
	uint8_t fcontent[64 * 1024];
	long fsize;

	setbuf(stdout, NULL);

	char* pname = argv[0];
	char* dump_path = NULL;   // NULL = no dump

	if (argc == 1)
	{
		usage(pname);
		return -1;
	}

	bool verbose = false;

	if (strcmp(argv[1], "-v") == 0)
	{
		verbose = true;
		dbgprintf = printf;
		argv++;
		argc--;

		if (argc == 1)
		{
			usage(pname);
			return -1;
		}
	}

	// --dump-mem or --dump-mem=FILE
	if (argc > 1 && strncmp(argv[1], "--dump-mem", 10) == 0)
	{
		if (argv[1][10] == '=')
			dump_path = argv[1] + 11;   // custom filename
		else if (argv[1][10] == '\0')
			dump_path = "mem.bin";      // default filename
		else
			{ usage(pname); return -1; }
		argv++;
		argc--;
	}

	fname = argv[1];
	f = fopen(fname, "r");
	if (f == NULL)
	{
		printf("ERROR: failed to open file '%s'\n", fname);
		return -2;
	}

	// find the file size
	fseek(f, 0, SEEK_END); // seek to end of file
	fsize = ftell(f); // get current file pointer
	fseek(f, 0, SEEK_SET); // seek back to beginning of file

	// PSP lives at fcontent[0x000..0x0FF], .COM image at fcontent[0x100..].
	// The whole buffer is later written to COM_SEG*16 in emulated memory.
	memset(fcontent, 0, sizeof(fcontent));
	fread(fcontent + 0x100, fsize, 1, f);

	err = uc_open(UC_ARCH_X86, UC_MODE_16, &uc);
	if (err)
	{
		fprintf(stderr, "Cannot initialize unicorn\n");
		return 1;
	}

	// Map conventional memory (640 KB) + ROM area
	if (uc_mem_map(uc, 0, 640 * 1024, UC_PROT_ALL) || uc_mem_map(uc, 0xC0000, 0x10000, UC_PROT_READ))
	{
		printf("Failed to write emulation code to memory, quit!\n");
		uc_close(uc);
		return 0;
	}

	// initialize internal settings
	global_init();

	int10_init();
	int15_init();
	int21_init();

	// Build PSP at fcontent[0..0xFF].  Pass seg=0 so psp_setup indexes
	// fcontent[MK_FP(0,0)] = fcontent[0], which is correct for our layout.
	psp_setup(0, fcontent, argc, argv);

	// DS:[0002] = top of available memory in paragraphs (standard: 0x9FFF).
	// Programs read this to check whether enough memory is free.
	((uint16_t*)fcontent)[1] = 0x9FFF;

	// Write PSP + .COM image to emulated memory at COM_SEG*16.
	uc_mem_write(uc, COM_SEG * 16, fcontent, 0x100 + fsize);

	// Set segment registers to the PSP segment, as real DOS does for .COM
	{
		uint32_t seg = COM_SEG;
		uc_reg_write(uc, UC_X86_REG_CS, &seg);
		uc_reg_write(uc, UC_X86_REG_DS, &seg);
		uc_reg_write(uc, UC_X86_REG_ES, &seg);
		uc_reg_write(uc, UC_X86_REG_SS, &seg);
		// SP: real DOS sets this to 0xFFFE for .COM files
		uint32_t sp = 0xFFFE;
		uc_reg_write(uc, UC_X86_REG_SP, &sp);
	}

	// handle interrupt ourself
	uc_hook_add(uc, &trace, UC_HOOK_INTR, hook_intr, NULL, 1, 0);

	// Work around Unicorn real-mode RETF CS-change bug (UC_ERR_FETCH_UNMAPPED)
	uc_hook retf_hook;
	uc_hook_add(uc, &retf_hook, UC_HOOK_INSN, hook_retf, NULL, 1, 0,
				UC_X86_INS_RETF);

	err = uc_emu_start(uc, DOS_ADDR, 0, 0, 0);
	if (err)
	{
		printf("Failed on uc_emu_start() with error returned %u: %s\n",
			   err, uc_strerror(err));
	}

	// Dump emulated memory if requested
	if (dump_path)
	{
		// Probe mapped regions by attempting to read in 4KB chunks across
		// the full 20-bit real-mode address space (0x00000–0xFFFFF).
		const uint32_t PROBE = 4096;
		uint8_t* buf = malloc(PROBE);
		FILE* df = fopen(dump_path, "wb");
		if (!df)
		{
			fprintf(stderr, "dump-mem: cannot open '%s' for writing\n", dump_path);
		}
		else
		{
			uint32_t addr;
			for (addr = 0; addr < 0x100000; addr += PROBE)
			{
				if (uc_mem_read(uc, addr, buf, PROBE) == UC_ERR_OK)
					fwrite(buf, 1, PROBE, df);
				else
					memset(buf, 0, PROBE), fwrite(buf, 1, PROBE, df);
			}
			fclose(df);
			printf("Memory dumped to '%s' (1MB)\n", dump_path);
		}
		free(buf);
	}

	uc_close(uc);

	return exit_code;
}
