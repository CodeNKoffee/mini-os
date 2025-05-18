# MiniSim GUI - Process Scheduling Simulator

This project implements a process scheduling simulator with a graphical user interface (GUI) built using GTK4. The simulator supports three scheduling algorithms: First-Come-First-Serve (FCFS), Round Robin (RR), and Multi-Level Feedback Queue (MLFQ).

## Prerequisites

- GCC compiler
- GTK4 development libraries
- Make

## Building the Project

1. Clean any previous builds:
```bash
make clean
```

2. Build the project:
```bash
make
```

3. Run the simulator:
```bash
./minisimgui
```

## Project Structure

- `gui.c`: Contains the GUI implementation using GTK4
- `simulator.c`: Contains the core simulation logic
- `simulator.h`: Header file with shared declarations
- `main.c`: Terminal-based version of the simulator (not used in GUI version)

## Features

- Visual representation of process states
- Support for three scheduling algorithms:
  - First-Come-First-Serve (FCFS)
  - Round Robin (RR)
  - Multi-Level Feedback Queue (MLFQ)
- Process state visualization
- Memory management visualization
- Resource allocation tracking

## Note

The `main.c` file is provided for reference and contains a terminal-based version of the simulator. The GUI version is the primary interface for this project. 