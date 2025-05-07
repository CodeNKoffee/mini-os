#ifndef SIMULATOR_H
#define SIMULATOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#define MEMORY_SIZE 60
#define MAX_PROGRAM_LINES 50
#define MAX_LINE_LENGTH 100
#define NUM_VARIABLES 3
#define PCB_SIZE 5 // for storing PCB fields in memory (optional)
#define MAX_PROCESSES 10
#define MAX_QUEUE_SIZE 10
#define MLFQ_LEVELS 4
#define NUM_RESOURCES 3 // file, userInput, userOutput

// Forward declaration for GUI interaction callbacks
typedef struct GuiCallbacks GuiCallbacks;

// Scheduling policies
typedef enum
{
    SIM_SCHED_FCFS,
    SIM_SCHED_RR,
    SIM_SCHED_MLFQ
} SchedulerType;

// Process states
typedef enum
{
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
} ProcessState;

// Resources for semaphores
typedef enum
{
    RESOURCE_FILE = 0, // Ensure explicit numbering for safety
    RESOURCE_USER_INPUT,
    RESOURCE_USER_OUTPUT
} ResourceType;

// A memory word can hold a name and a value
typedef struct
{
    char name[50];
    char value[50];
} MemoryWord;

// Process Control Block
// In simulator.h, modify the PCB struct:
typedef struct
{
    int processID;
    int programNumber; // Add this line
    ProcessState state;
    int priority;
    int programCounter;
    int memoryLowerBound;
    int memoryUpperBound;
    int arrivalTime;
    ResourceType blockedOnResource;
    int quantumRemaining;
    int mlfqLevel;
} PCB;

// Mutex with a FIFO + priority‐based blocked queue
typedef struct
{
    bool locked;
    int lockingProcessID;
    int blockedQueue[MAX_QUEUE_SIZE];
    int head, tail, size;
} Mutex;

// Overall system state
typedef struct SystemState SystemState; // Forward declaration
struct SystemState
{
    MemoryWord memory[MEMORY_SIZE];
    int memoryPointer;

    PCB processTable[MAX_PROCESSES];
    int processCount;

    Mutex mutexes[NUM_RESOURCES];

    // FCFS/RR ready queue
    int readyQueue[MAX_QUEUE_SIZE];
    int readyHead, readyTail, readySize;

    // MLFQ ready queues per level
    int mlfqRQ[MLFQ_LEVELS][MAX_QUEUE_SIZE];
    int mlfqHead[MLFQ_LEVELS], mlfqTail[MLFQ_LEVELS], mlfqSize[MLFQ_LEVELS];

    int runningProcessID;
    int clockCycle;

    SchedulerType schedulerType;
    int rrQuantum;
    int mlfqQuantum[MLFQ_LEVELS];

    // Flag to indicate if simulation requires user input
    bool needsInput;
    char inputVarName[50]; // Variable name needing input
    int inputPid;          // Process needing input

    // GUI Interaction
    GuiCallbacks *callbacks; // Pointer to GUI callback functions
    void *gui_data;          // Pointer to GUI specific data

    // Flag indicating if the simulation has completed
    bool simulationComplete;

    bool wasUnblockedThisCycle[MAX_PROCESSES]; // Track processes unblocked this cycle
};

// Structure to hold function pointers for GUI interaction
struct GuiCallbacks
{
    // Called when the simulator needs to log a message
    void (*log_message)(void *gui_data, const char *message);
    // Called when the 'print' instruction is executed
    void (*process_output)(void *gui_data, int pid, const char *output);
    // Called when the 'assign input' instruction requires user input
    // This function should *display* the input dialog asynchronously.
    // The actual input should be passed back later via provideInput().
    void (*request_input)(void *gui_data, int pid, const char *varName);
    // Called when the state changes significantly (e.g., process state, queues)
    // GUI should query SystemState to update its display
    void (*state_update)(void *gui_data, SystemState *sys);
};

// Function prototypes
void initializeSystem(SystemState *sys, SchedulerType type, int rrQuantumVal, GuiCallbacks *callbacks, void *gui_data);
bool loadProgram(SystemState *sys, const char *filename);
void stepSimulation(SystemState *sys); // Executes one cycle or one event
bool isSimulationComplete(SystemState *sys);
PCB *findPCB(SystemState *sys, int pid);
int findInstructionCount(SystemState *sys, int pid);
char *getVariable(SystemState *sys, int pid, const char *var);
void provideInput(SystemState *sys, const char *input); // Call after request_input

// These internal functions likely won't be called directly by GUI but need declaration if simulator.c is split
// void checkArrivals(SystemState *sys);
// void addToReadyQueue(SystemState *sys, int pid);
// int scheduleFCFS(SystemState *sys);
// void addToMLFQ(SystemState *sys, int pid, int level);
// int scheduleMLFQ(SystemState *sys);
// void interpretInstruction(SystemState *sys, int pid);
// ResourceType getResourceTypeFromString(const char *s);
// void blockProcess(SystemState *sys, int pid, ResourceType r);
// void unblockProcess(SystemState *sys, ResourceType r);

#endif // SIMULATOR_H