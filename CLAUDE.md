- In all interactions, be extremely concise and sacrifice grammar for the sake of concision.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A raycaster engine implemented in bare-metal C targeting the **DE1-SoC FPGA board** running a **RISC-V RV32IM (Nios V/g)** soft-core processor. Can also be simulated in-browser via the CPUlator emulator at `https://cpulator.01xz.net/?sys=rv32-de1soc`.

## Build Commands

Requires the **Intel FPGA Quartus Lite v23.1** toolchain (Windows/Cygwin). On Windows, use `gmake.bat <TARGET>` to invoke the Quartus-bundled make.

| Command | Description |
|---|---|
| `make COMPILE` | Cross-compile `raycaster.c` → `raycaster.elf` (RISC-V RV32IM) |
| `make CLEAN` | Remove build artifacts |
| `make OBJDUMP` | Disassemble with source (`riscv32-unknown-elf-objdump -d -S`) |
| `make SYMBOLS` | Print symbol table |
| `make DE1-SoC` | Flash to DE1-SoC via JTAG |
| `make GDB_SERVER` + `make GDB_CLIENT` | Start JTAG GDB server then attach client |
| `make TERMINAL` | Open JTAG UART console |

Compiler flags: `-march=rv32im_zicsr -mabi=ilp32 -g -O1 -ffunction-sections -fno-inline -gdwarf-2`. Stack at `0x4000000`.

## Architecture

All logic lives in `raycaster.c` (~405 lines). `address_map.h` defines all memory-mapped peripheral addresses.

### Hardware Peripherals
- **VGA display**: 320×240, RGB 5:6:5. Pixel buffer is memory-mapped; row stride is 1024 bytes (512 shorts) even though only 320 pixels are used per row.
- **Double buffering**: Front buffer in FPGA SRAM (`0x08000000`), back buffer in SDRAM (`0x01000000`). Swapped via VGA controller at `0xFF203020`.
- **PS/2 keyboard**: Memory-mapped FIFO at `0xFF200100`.

### Plans
- At the end of each plan, give me a list of unresolved questions to answer, if any. Make the questions extremely concise. Sacrifice grammar for the sake of concision.
