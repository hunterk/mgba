#include "gba.h"

#include "debugger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const char* GBA_CANNOT_MMAP = "Could not map memory";

static int32_t GBALoad32(struct ARMMemory* memory, uint32_t address);
static int16_t GBALoad16(struct ARMMemory* memory, uint32_t address);
static uint16_t GBALoadU16(struct ARMMemory* memory, uint32_t address);
static int8_t GBALoad8(struct ARMMemory* memory, uint32_t address);
static uint8_t GBALoadU8(struct ARMMemory* memory, uint32_t address);

static void GBAStore32(struct ARMMemory* memory, uint32_t address, int32_t value);
static void GBAStore16(struct ARMMemory* memory, uint32_t address, int16_t value);
static void GBAStore8(struct ARMMemory* memory, uint32_t address, int8_t value);

static void GBASwi16(struct ARMBoard* board, int immediate);
static void GBASwi32(struct ARMBoard* board, int immediate);

static void GBASetActiveRegion(struct ARMMemory* memory, uint32_t region);
static void GBAHitStub(struct ARMBoard* board, uint32_t opcode);

static void _GBAIOWrite(struct GBA* gba, uint32_t address, uint16_t value);

void GBAInit(struct GBA* gba) {
	gba->errno = GBA_NO_ERROR;
	gba->errstr = 0;

	ARMInit(&gba->cpu);

	gba->memory.p = gba;
	GBAMemoryInit(&gba->memory);
	ARMAssociateMemory(&gba->cpu, &gba->memory.d);

	gba->board.p = gba;
	GBABoardInit(&gba->board);
	ARMAssociateBoard(&gba->cpu, &gba->board.d);

	ARMReset(&gba->cpu);
}

void GBADeinit(struct GBA* gba) {
	GBAMemoryDeinit(&gba->memory);
}

void GBAMemoryInit(struct GBAMemory* memory) {
	memory->d.load32 = GBALoad32;
	memory->d.load16 = GBALoad16;
	memory->d.loadU16 = GBALoadU16;
	memory->d.load8 = GBALoad8;
	memory->d.loadU8 = GBALoadU8;
	memory->d.store32 = GBAStore32;
	memory->d.store16 = GBAStore16;
	memory->d.store8 = GBAStore8;

	memory->bios = 0;
	memory->wram = mmap(0, SIZE_WORKING_RAM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	memory->iwram = mmap(0, SIZE_WORKING_IRAM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	memory->rom = 0;
	memset(memory->io, 0, sizeof(memory->io));

	if (!memory->wram || !memory->iwram) {
		GBAMemoryDeinit(memory);
		memory->p->errno = GBA_OUT_OF_MEMORY;
		memory->p->errstr = GBA_CANNOT_MMAP;
	}

	memory->d.activeRegion = 0;
	memory->d.activeMask = 0;
	memory->d.setActiveRegion = GBASetActiveRegion;
}

void GBAMemoryDeinit(struct GBAMemory* memory) {
	munmap(memory->wram, SIZE_WORKING_RAM);
	munmap(memory->iwram, SIZE_WORKING_IRAM);
}

void GBABoardInit(struct GBABoard* board) {
	board->d.reset = GBABoardReset;
	board->d.swi16 = GBASwi16;
	board->d.swi32 = GBASwi32;
	board->d.hitStub = GBAHitStub;
}

void GBABoardReset(struct ARMBoard* board) {
	struct ARMCore* cpu = board->cpu;
	ARMSetPrivilegeMode(cpu, MODE_IRQ);
	cpu->gprs[ARM_SP] = SP_BASE_IRQ;
	ARMSetPrivilegeMode(cpu, MODE_SUPERVISOR);
	cpu->gprs[ARM_SP] = SP_BASE_SUPERVISOR;
	ARMSetPrivilegeMode(cpu, MODE_SYSTEM);
	cpu->gprs[ARM_SP] = SP_BASE_SYSTEM;
}

void GBAAttachDebugger(struct GBA* gba, struct ARMDebugger* debugger) {
	ARMDebuggerInit(debugger, &gba->cpu);
	gba->debugger = debugger;
}

void GBALoadROM(struct GBA* gba, int fd) {
	gba->memory.rom = mmap(0, SIZE_CART0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FILE, fd, 0);
	// TODO: error check
}

static void GBASetActiveRegion(struct ARMMemory* memory, uint32_t address) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_BIOS:
		memory->activeRegion = gbaMemory->bios;
		memory->activeMask = 0;
		break;
	case BASE_WORKING_RAM:
		memory->activeRegion = gbaMemory->wram;
		memory->activeMask = SIZE_WORKING_RAM - 1;
		break;
	case BASE_WORKING_IRAM:
		memory->activeRegion = gbaMemory->iwram;
		memory->activeMask = SIZE_WORKING_IRAM - 1;
		break;
	case BASE_CART0:
	case BASE_CART0_EX:
	case BASE_CART1:
	case BASE_CART1_EX:
	case BASE_CART2:
	case BASE_CART2_EX:
		memory->activeRegion = gbaMemory->rom;
		memory->activeMask = SIZE_CART0 - 1;
		break;
	default:
		memory->activeRegion = 0;
		memory->activeMask = 0;
		break;
	}
}

int32_t GBALoad32(struct ARMMemory* memory, uint32_t address) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_BIOS:
		break;
	case BASE_WORKING_RAM:
		return gbaMemory->wram[(address & (SIZE_WORKING_RAM - 1)) >> 2];
	case BASE_WORKING_IRAM:
		return gbaMemory->iwram[(address & (SIZE_WORKING_IRAM - 1)) >> 2];
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
	case BASE_CART0_EX:
	case BASE_CART1:
	case BASE_CART1_EX:
	case BASE_CART2:
	case BASE_CART2_EX:
		return gbaMemory->rom[(address & (SIZE_CART0 - 1)) >> 2];
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}

	return 0;
}

int16_t GBALoad16(struct ARMMemory* memory, uint32_t address) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_BIOS:
		break;
	case BASE_WORKING_RAM:
		return ((int16_t*) gbaMemory->wram)[(address & (SIZE_WORKING_RAM - 1)) >> 1];
	case BASE_WORKING_IRAM:
		return ((int16_t*) gbaMemory->iwram)[(address & (SIZE_WORKING_IRAM - 1)) >> 1];
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
	case BASE_CART0_EX:
	case BASE_CART1:
	case BASE_CART1_EX:
	case BASE_CART2:
	case BASE_CART2_EX:
		return ((int16_t*) gbaMemory->rom)[(address & (SIZE_CART0 - 1)) >> 1];
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}

	return 0;
}

uint16_t GBALoadU16(struct ARMMemory* memory, uint32_t address) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_BIOS:
		break;
	case BASE_WORKING_RAM:
		return ((uint16_t*) gbaMemory->wram)[(address & (SIZE_WORKING_RAM - 1)) >> 1];
	case BASE_WORKING_IRAM:
		return ((uint16_t*) gbaMemory->iwram)[(address & (SIZE_WORKING_IRAM - 1)) >> 1];
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
	case BASE_CART0_EX:
	case BASE_CART1:
	case BASE_CART1_EX:
	case BASE_CART2:
	case BASE_CART2_EX:
		return ((uint16_t*) gbaMemory->rom)[(address & (SIZE_CART0 - 1)) >> 1];
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}

	return 0;
}

int8_t GBALoad8(struct ARMMemory* memory, uint32_t address) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_BIOS:
		break;
	case BASE_WORKING_RAM:
		return ((int8_t*) gbaMemory->wram)[address & (SIZE_WORKING_RAM - 1)];
	case BASE_WORKING_IRAM:
		return ((int8_t*) gbaMemory->iwram)[address & (SIZE_WORKING_IRAM - 1)];
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
	case BASE_CART0_EX:
	case BASE_CART1:
	case BASE_CART1_EX:
	case BASE_CART2:
	case BASE_CART2_EX:
		return ((int8_t*) gbaMemory->rom)[address & (SIZE_CART0 - 1)];
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}

	return 0;
}

uint8_t GBALoadU8(struct ARMMemory* memory, uint32_t address) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_BIOS:
		break;
	case BASE_WORKING_RAM:
		return ((uint8_t*) gbaMemory->wram)[address & (SIZE_WORKING_RAM - 1)];
		break;
	case BASE_WORKING_IRAM:
		return ((uint8_t*) gbaMemory->iwram)[address & (SIZE_WORKING_IRAM - 1)];
		break;
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
	case BASE_CART0_EX:
	case BASE_CART1:
	case BASE_CART1_EX:
	case BASE_CART2:
	case BASE_CART2_EX:
		return ((uint8_t*) gbaMemory->rom)[address & (SIZE_CART0 - 1)];
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}

	return 0;
}

void GBAStore32(struct ARMMemory* memory, uint32_t address, int32_t value) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_WORKING_RAM:
		gbaMemory->wram[(address & (SIZE_WORKING_RAM - 1)) >> 2] = value;
		break;
	case BASE_WORKING_IRAM:
		gbaMemory->iwram[(address & (SIZE_WORKING_IRAM - 1)) >> 2] = value;
		break;
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
		break;
	case BASE_CART2_EX:
		break;
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}
}

void GBAStore16(struct ARMMemory* memory, uint32_t address, int16_t value) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_WORKING_RAM:
		((int16_t*) gbaMemory->wram)[(address & (SIZE_WORKING_RAM - 1)) >> 1] = value;
		break;
	case BASE_WORKING_IRAM:
		((int16_t*) gbaMemory->iwram)[(address & (SIZE_WORKING_IRAM - 1)) >> 1] = value;
		break;
	case BASE_IO:
		_GBAIOWrite(gbaMemory->p, address & (SIZE_IO - 1), value);
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
		break;
	case BASE_CART2_EX:
		break;
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}
}

void GBAStore8(struct ARMMemory* memory, uint32_t address, int8_t value) {
	struct GBAMemory* gbaMemory = (struct GBAMemory*) memory;

	switch (address & ~OFFSET_MASK) {
	case BASE_WORKING_RAM:
		((int8_t*) gbaMemory->wram)[address & (SIZE_WORKING_RAM - 1)] = value;
		break;
	case BASE_WORKING_IRAM:
		((int8_t*) gbaMemory->iwram)[address & (SIZE_WORKING_IRAM - 1)] = value;
		break;
	case BASE_IO:
		break;
	case BASE_PALETTE_RAM:
		break;
	case BASE_VRAM:
		break;
	case BASE_OAM:
		break;
	case BASE_CART0:
		break;
	case BASE_CART2_EX:
		break;
	case BASE_CART_SRAM:
		break;
	default:
		break;
	}
}

static void GBASwi16(struct ARMBoard* board, int immediate) {
	switch (immediate) {
	default:
		GBALog(GBA_LOG_STUB, "Stub software interrupt: %02x", immediate);
	}
}

static void GBASwi32(struct ARMBoard* board, int immediate) {
	GBASwi32(board, immediate >> 8);
}

void GBALog(int level, const char* format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf("\n");
}

void GBAHitStub(struct ARMBoard* board, uint32_t opcode) {
	GBALog(GBA_LOG_STUB, "Stub opcode: %08x", opcode);
	struct GBABoard* gbaBoard = (struct GBABoard*) board;
	if (!gbaBoard->p->debugger) {
		abort();
	} else {
		ARMDebuggerEnter(gbaBoard->p->debugger);
	}
}

static void _GBAIOWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	switch (address) {
	default:
		GBALog(GBA_LOG_STUB, "Stub I/O register: %03x", address);
		break;
	}
}