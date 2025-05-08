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
#define PCB_SIZE 5
#define MAX_PROCESSES 10
#define MAX_QUEUE_SIZE 10
#define MLFQ_LEVELS 4
#define NUM_RESOURCES 3

typedef struct GuiCallbacks GuiCallbacks;

typedef enum
{
    SIM_SCHED_FCFS,
    SIM_SCHED_RR,
    SIM_SCHED_MLFQ
} SchedulerType;

typedef enum
{
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
} ProcessState;

typedef enum
{
    RESOURCE_FILE = 0,
    RESOURCE_USER_INPUT,
    RESOURCE_USER_OUTPUT
} ResourceType;

typedef struct
{
    char name[50];
    char value[50];
} MemoryWord;

typedef struct
{
    int processID;
    int programNumber;
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

typedef struct
{
    bool locked;
    int lockingProcessID;
    int blockedQueue[MAX_QUEUE_SIZE];
    int head, tail, size;
} Mutex;

typedef struct SystemState SystemState;
struct SystemState
{
    MemoryWord memory[MEMORY_SIZE];
    int memoryPointer;

    PCB processTable[MAX_PROCESSES];
    int processCount;

    Mutex mutexes[NUM_RESOURCES];

    int readyQueue[MAX_QUEUE_SIZE];
    int readyHead, readyTail, readySize;

    int mlfqRQ[MLFQ_LEVELS][MAX_QUEUE_SIZE];
    int mlfqHead[MLFQ_LEVELS], mlfqTail[MLFQ_LEVELS], mlfqSize[MLFQ_LEVELS];

    int runningProcessID;
    int clockCycle;

    SchedulerType schedulerType;
    int rrQuantum;
    int mlfqQuantum[MLFQ_LEVELS];

    bool needsInput;
    char inputVarName[50];
    int inputPid;

    GuiCallbacks *callbacks;
    void *gui_data;

    bool simulationComplete;

    bool wasUnblockedThisCycle[MAX_PROCESSES];
};

struct GuiCallbacks
{
    void (*log_message)(void *gui_data, const char *message);
    void (*process_output)(void *gui_data, int pid, const char *output);
    void (*request_input)(void *gui_data, int pid, const char *varName);
    void (*state_update)(void *gui_data, SystemState *sys);
};

void initializeSystem(SystemState *sys, SchedulerType type, int rrQuantumVal, GuiCallbacks *callbacks, void *gui_data);
bool loadProgram(SystemState *sys, const char *filename);
void stepSimulation(SystemState *sys);
bool isSimulationComplete(SystemState *sys);
PCB *findPCB(SystemState *sys, int pid);
int findInstructionCount(SystemState *sys, int pid);
char *getVariable(SystemState *sys, int pid, const char *var);
void provideInput(SystemState *sys, const char *input);

#endif