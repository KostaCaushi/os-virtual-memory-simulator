# OS Virtual Memory Simulator

A virtual memory simulator written in C that models core operating system memory-management concepts.  
This project was built to deepen understanding of how paging, page tables, and page-replacement algorithms work at a low level.

## Features
- Virtual memory paging simulation
- Page tables
- Page replacement algorithms:
  - FIFO (First-In, First-Out)
  - LRU (Least Recently Used)
- Configurable number of memory frames
- Trace-driven memory access simulation
- Tracks page faults and memory access behavior
- Implemented in C with a Makefile build system

## Project Structure
src/        C source code
traces/     Memory access trace files
Makefile    Build configuration
