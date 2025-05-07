// main.c
// A mini-OS simulator supporting FCFS, Round‐Robin, and Multilevel Feedback Queue (MLFQ)

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

// Scheduling policies
typedef enum
{
  SCHED_FCFS,
  SCHED_RR,
  SCHED_MLFQ
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
  RESOURCE_FILE,
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
typedef struct
{
  int processID;
  ProcessState state;
  int priority; // used for resource-unblocking (lower=more urgent)
  int programCounter;
  int memoryLowerBound;
  int memoryUpperBound;
  int arrivalTime;
  ResourceType blockedOnResource;
  int quantumRemaining; // time‐slice left (RR & MLFQ)
  int mlfqLevel;        // current level (0 highest … MLFQ_LEVELS-1 lowest)
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
typedef struct
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
} SystemState;

// Global
SystemState osSystem;
SchedulerType schedulerType;
int rrQuantum;
int mlfqQuantum[MLFQ_LEVELS] = {1, 2, 4, 8};

// Function prototypes
void initializeSystem(SystemState *sys);
int allocateMemory(SystemState *sys, int words);
void loadProgram(SystemState *sys, const char *filename, int arrivalTime);
void runSimulation(SystemState *sys);
PCB *findPCB(SystemState *sys, int pid);
int findInstructionCount(SystemState *sys, int pid);

void checkArrivals(SystemState *sys);
void addToReadyQueue(SystemState *sys, int pid);
int scheduleFCFS(SystemState *sys);

void addToMLFQ(SystemState *sys, int pid, int level);
int scheduleMLFQ(SystemState *sys);

void interpretInstruction(SystemState *sys, int pid);

int findVariableMemoryIndex(SystemState *sys, int pid, const char *var, bool findFree);
void setVariable(SystemState *sys, int pid, const char *var, const char *value);
char *getVariable(SystemState *sys, int pid, const char *var);

void do_print(SystemState *sys, int pid, char *arg1);
void do_assign(SystemState *sys, int pid, char *varName, char *valueOrInput);
void do_writeFile(SystemState *sys, int pid, char *fileVar, char *dataVar);
void do_readFile(SystemState *sys, int pid, char *fileVar);
void do_printFromTo(SystemState *sys, int pid, char *v1, char *v2);
void do_semWait(SystemState *sys, int pid, char *resName);
void do_semSignal(SystemState *sys, int pid, char *resName);

ResourceType getResourceTypeFromString(const char *s);
bool enqueueMutexBlocked(Mutex *m, int pid);
int dequeueMutexBlocked(Mutex *m);
void blockProcess(SystemState *sys, int pid, ResourceType r);
void unblockProcess(SystemState *sys, ResourceType r);

// ---------------- Implementation ----------------

void initializeSystem(SystemState *sys)
{
  sys->memoryPointer = 0;
  sys->processCount = 0;
  sys->runningProcessID = -1;
  sys->clockCycle = 0;
  // Clear memory
  for (int i = 0; i < MEMORY_SIZE; i++)
  {
    sys->memory[i].name[0] = 0;
    sys->memory[i].value[0] = 0;
  }
  // Ready queue
  sys->readyHead = sys->readyTail = sys->readySize = 0;
  // MLFQ queues
  for (int l = 0; l < MLFQ_LEVELS; l++)
  {
    sys->mlfqHead[l] = sys->mlfqTail[l] = sys->mlfqSize[l] = 0;
  }
  // Mutexes
  for (int i = 0; i < NUM_RESOURCES; i++)
  {
    sys->mutexes[i].locked = false;
    sys->mutexes[i].lockingProcessID = -1;
    sys->mutexes[i].head = sys->mutexes[i].tail = sys->mutexes[i].size = 0;
  }
}

int allocateMemory(SystemState *sys, int words)
{
  if (sys->memoryPointer + words > MEMORY_SIZE)
  {
    printf("Error: Out of memory! Requested %d words, available %d.\n",
           words, MEMORY_SIZE - sys->memoryPointer);
    return -1;
  }
  int start = sys->memoryPointer;
  sys->memoryPointer += words;
  return start;
}

void loadProgram(SystemState *sys, const char *filename, int arrivalTime)
{
  if (sys->processCount >= MAX_PROCESSES)
  {
    printf("Error: process table full, cannot load %s\n", filename);
    return;
  }
  FILE *f = fopen(filename, "r");
  if (!f)
  {
    perror("fopen");
    return;
  }

  // count non-empty lines
  char buf[MAX_LINE_LENGTH];
  int count = 0;
  while (fgets(buf, sizeof(buf), f))
  {
    if (strspn(buf, " \t\r\n") < strlen(buf))
      count++;
  }
  if (count == 0)
  {
    printf("Warning: %s is empty.\n", filename);
    fclose(f);
    return;
  }
  if (count > MAX_PROGRAM_LINES)
  {
    printf("Warning: %s truncated to %d lines.\n", filename, MAX_PROGRAM_LINES);
    count = MAX_PROGRAM_LINES;
  }
  rewind(f);

  int memNeeded = count + NUM_VARIABLES + PCB_SIZE;
  int lb = allocateMemory(sys, memNeeded);
  if (lb < 0)
  {
    fclose(f);
    return;
  }
  int ub = lb + memNeeded - 1;

  PCB *pcb = &sys->processTable[sys->processCount];
  pcb->processID = sys->processCount;
  pcb->state = NEW;
  pcb->priority = 0;
  pcb->programCounter = 0;
  pcb->memoryLowerBound = lb;
  pcb->memoryUpperBound = ub;
  pcb->arrivalTime = arrivalTime;
  pcb->blockedOnResource = RESOURCE_FILE; // dummy
  pcb->quantumRemaining = 0;
  pcb->mlfqLevel = 0;

  // load instructions
  int idx = lb, line = 0;
  while (line < count && fgets(buf, sizeof(buf), f))
  {
    if (strspn(buf, " \t\r\n") == strlen(buf))
      continue;
    buf[strcspn(buf, "\r\n")] = 0;
    snprintf(sys->memory[idx].name, 50, "Inst_%d_%d", pcb->processID, line);
    strncpy(sys->memory[idx].value, buf, 50);
    idx++;
    line++;
  }
  fclose(f);

  // init variable slots
  int varStart = lb + count;
  for (int i = 0; i < NUM_VARIABLES && varStart + i <= ub; i++)
  {
    snprintf(sys->memory[varStart + i].name, 50, "Var_%d_Free%d", pcb->processID, i);
    sys->memory[varStart + i].value[0] = 0;
  }

  printf("Loaded P%d: lines=%d, mem=[%d..%d], arrival=%d\n",
         pcb->processID, count, lb, ub, arrivalTime);
  sys->processCount++;
}

PCB *findPCB(SystemState *sys, int pid)
{
  if (pid < 0 || pid >= sys->processCount)
    return NULL;
  return &sys->processTable[pid];
}

int findInstructionCount(SystemState *sys, int pid)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return 0;
  int cnt = 0;
  for (int i = pcb->memoryLowerBound; i <= pcb->memoryUpperBound; i++)
  {
    char prefix[20];
    snprintf(prefix, sizeof(prefix), "Inst_%d_", pid);
    if (strncmp(sys->memory[i].name, prefix, strlen(prefix)) == 0)
      cnt++;
    else
      break;
  }
  return cnt;
}

// ------------ Scheduling ------------

void addToReadyQueue(SystemState *sys, int pid)
{
  if (sys->readySize >= MAX_QUEUE_SIZE)
  {
    printf("Error: Ready queue full, dropping P%d\n", pid);
    return;
  }
  PCB *pcb = findPCB(sys, pid);
  pcb->state = READY;
  sys->readyQueue[sys->readyTail] = pid;
  sys->readyTail = (sys->readyTail + 1) % MAX_QUEUE_SIZE;
  sys->readySize++;
}

int scheduleFCFS(SystemState *sys)
{
  if (sys->readySize == 0)
    return -1;
  int pid = sys->readyQueue[sys->readyHead];
  sys->readyHead = (sys->readyHead + 1) % MAX_QUEUE_SIZE;
  sys->readySize--;
  return pid;
}

void addToMLFQ(SystemState *sys, int pid, int level)
{
  if (level < 0 || level >= MLFQ_LEVELS)
    return;
  if (sys->mlfqSize[level] >= MAX_QUEUE_SIZE)
  {
    printf("Error: MLFQ level %d full, dropping P%d\n", level, pid);
    return;
  }
  PCB *pcb = findPCB(sys, pid);
  pcb->state = READY;
  pcb->mlfqLevel = level;
  pcb->priority = level;
  sys->mlfqRQ[level][sys->mlfqTail[level]] = pid;
  sys->mlfqTail[level] = (sys->mlfqTail[level] + 1) % MAX_QUEUE_SIZE;
  sys->mlfqSize[level]++;
}

int scheduleMLFQ(SystemState *sys)
{
  for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++)
  {
    if (sys->mlfqSize[lvl] > 0)
    {
      int pid = sys->mlfqRQ[lvl][sys->mlfqHead[lvl]];
      sys->mlfqHead[lvl] = (sys->mlfqHead[lvl] + 1) % MAX_QUEUE_SIZE;
      sys->mlfqSize[lvl]--;
      return pid;
    }
  }
  return -1;
}

// ------------- Interpreter -------------

void interpretInstruction(SystemState *sys, int pid)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb || pcb->state != RUNNING)
  {
    printf("Error: P%d not runnable\n", pid);
    return;
  }
  int instCount = findInstructionCount(sys, pid);
  if (pcb->programCounter >= instCount)
  {
    // terminate
    pcb->state = TERMINATED;
    printf("P%d done\n", pid);
    return;
  }
  int memIdx = pcb->memoryLowerBound + pcb->programCounter;
  char line[MAX_LINE_LENGTH];
  strncpy(line, sys->memory[memIdx].value, MAX_LINE_LENGTH);
  printf("P%d executing [%s]\n", pid, line);

  // tokenize
  char *cmd = strtok(line, " ");
  char *a1 = strtok(NULL, " ");
  char *a2 = strtok(NULL, " ");
  char *a3 = strtok(NULL, " ");

  bool error = false;
  if (!cmd)
  { /*NOP*/
  }
  else if (strcmp(cmd, "print") == 0)
  {
    if (a1)
      do_print(sys, pid, a1);
    else
      error = true;
  }
  else if (strcmp(cmd, "assign") == 0)
  {
    if (!a1 || !a2)
    {
      printf("Error in P%d: assign needs at least 2 args\n", pid);
      findPCB(sys, pid)->state = TERMINATED;
    }
    else if (strcmp(a2, "input") == 0)
    {
      // interactive input
      do_assign(sys, pid, a1, a2);
    }
    else if (strcmp(a2, "readFile") == 0)
    {
      // assign b readFile a  → do_readFile(a), then b = file_a
      if (!a3)
      {
        printf("Error in P%d: assign readFile missing source var\n", pid);
        findPCB(sys, pid)->state = TERMINATED;
      }
      else
      {
        // perform the read under the existing file‐mutex
        do_readFile(sys, pid, a3);
        // build the auto‐generated var name "file_<a3>"
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "file_%s", a3);
        char *cnt = getVariable(sys, pid, tmp);
        if (cnt)
        {
          setVariable(sys, pid, a1, cnt);
        }
        else
        {
          printf("Error in P%d: readFile did not create %s\n",
                 pid, tmp);
          findPCB(sys, pid)->state = TERMINATED;
        }
      }
    }
    else
    {
      // literal assignment
      setVariable(sys, pid, a1, a2);
    }
  }
  else if (strcmp(cmd, "writeFile") == 0)
  {
    if (a1 && a2)
      do_writeFile(sys, pid, a1, a2);
    else
      error = true;
  }
  else if (strcmp(cmd, "readFile") == 0)
  {
    if (a1)
      do_readFile(sys, pid, a1);
    else
      error = true;
  }
  else if (strcmp(cmd, "printFromTo") == 0)
  {
    if (a1 && a2)
      do_printFromTo(sys, pid, a1, a2);
    else
      error = true;
  }
  else if (strcmp(cmd, "semWait") == 0)
  {
    if (a1)
      do_semWait(sys, pid, a1);
    else
      error = true;
  }
  else if (strcmp(cmd, "semSignal") == 0)
  {
    if (a1)
      do_semSignal(sys, pid, a1);
    else
      error = true;
  }
  else
  {
    printf("Unknown cmd '%s' in P%d\n", cmd, pid);
    error = true;
  }
  if (error)
  {
    printf("Error in P%d, terminating.\n", pid);
    pcb->state = TERMINATED;
  }
}

// -------- Variable Management --------

int findVariableMemoryIndex(SystemState *sys, int pid, const char *var, bool findFree)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return -1;
  int instCnt = findInstructionCount(sys, pid);
  int start = pcb->memoryLowerBound + instCnt;
  int end = pcb->memoryUpperBound - PCB_SIZE;
  char full[50];
  snprintf(full, sizeof(full), "Var_%d_%s", pid, var);
  int firstFree = -1;
  for (int i = start; i <= end; i++)
  {
    if (strcmp(sys->memory[i].name, full) == 0)
      return i;
    if (findFree)
    {
      char prefix[20];
      snprintf(prefix, sizeof(prefix), "Var_%d_Free", pid);
      if ((sys->memory[i].name[0] == 0 ||
           strncmp(sys->memory[i].name, prefix, strlen(prefix)) == 0) &&
          firstFree < 0)
        firstFree = i;
    }
  }
  return findFree ? firstFree : -1;
}

void setVariable(SystemState *sys, int pid, const char *var, const char *value)
{
  int idx = findVariableMemoryIndex(sys, pid, var, true);
  PCB *pcb = findPCB(sys, pid);
  if (idx < 0)
  {
    printf("Var store error in P%d\n", pid);
    pcb->state = TERMINATED;
    return;
  }
  snprintf(sys->memory[idx].name, 50, "Var_%d_%s", pid, var);
  strncpy(sys->memory[idx].value, value, 50);
}

char *getVariable(SystemState *sys, int pid, const char *var)
{
  int idx = findVariableMemoryIndex(sys, pid, var, false);
  PCB *pcb = findPCB(sys, pid);
  if (idx < 0)
  {
    printf("Var lookup error '%s' in P%d\n", var, pid);
    pcb->state = TERMINATED;
    return NULL;
  }
  return sys->memory[idx].value;
}

// -------- Instruction Handlers --------

void do_print(SystemState *sys, int pid, char *arg1)
{
  char *v = getVariable(sys, pid, arg1);
  if (v)
    printf("P%d OUTPUT: %s\n", pid, v);
}

void do_assign(SystemState *sys, int pid, char *varName, char *valueOrInput)
{
  if (strcmp(valueOrInput, "input") == 0)
  {
    char buf[50];
    do
    {
      printf("P%d: enter value for <%s>: ", pid, varName);
      fflush(stdout);
    } while (!fgets(buf, sizeof(buf), stdin) || buf[0] == '\n');
    buf[strcspn(buf, "\r\n")] = 0;
    setVariable(sys, pid, varName, buf);
  }
  else
  {
    setVariable(sys, pid, varName, valueOrInput);
  }
}

void do_writeFile(SystemState *sys, int pid, char *fileVar, char *dataVar)
{
  char *fname = getVariable(sys, pid, fileVar);
  char *data = getVariable(sys, pid, dataVar);
  if (!fname || !data)
    return;
  FILE *f = fopen(fname, "w");
  if (!f)
  {
    perror("writeFile");
    findPCB(sys, pid)->state = TERMINATED;
    return;
  }
  fprintf(f, "%s", data);
  fclose(f);
  printf("P%d wrote '%s' to %s\n", pid, data, fname);
}

void do_readFile(SystemState *sys, int pid, char *fileVar)
{
  char *fname = getVariable(sys, pid, fileVar);
  if (!fname)
    return;

  FILE *f = fopen(fname, "r");
  if (!f)
  {
    perror("readFile");
    findPCB(sys, pid)->state = TERMINATED;
    return;
  }

  char buf[MAX_LINE_LENGTH];
  char content[500] = "";
  while (fgets(buf, sizeof(buf), f))
  {
    strncat(content, buf, sizeof(content) - strlen(content) - 1);
  }
  fclose(f);

  // Build a valid variable name: "file_<originalVarName>"
  char varName[64];
  snprintf(varName, sizeof(varName), "file_%s", fileVar);

  setVariable(sys, pid, varName, content);
  printf("P%d read '%s' -> var %s\n", pid, content, varName);
}

void do_printFromTo(SystemState *sys, int pid, char *v1, char *v2)
{
  char *s1 = getVariable(sys, pid, v1), *s2 = getVariable(sys, pid, v2);
  if (!s1 || !s2)
    return;
  int a = atoi(s1), b = atoi(s2);
  printf("P%d OUTPUT: ", pid);
  if (a <= b)
    for (int x = a; x <= b; x++)
      printf("%d ", x);
  else
    for (int x = a; x >= b; x--)
      printf("%d ", x);
  printf("\n");
}

ResourceType getResourceTypeFromString(const char *s)
{
  if (strcmp(s, "file") == 0)
    return RESOURCE_FILE;
  if (strcmp(s, "userInput") == 0)
    return RESOURCE_USER_INPUT;
  if (strcmp(s, "userOutput") == 0)
    return RESOURCE_USER_OUTPUT;
  return -1;
}

bool enqueueMutexBlocked(Mutex *m, int pid)
{
  if (m->size >= MAX_QUEUE_SIZE)
    return false;
  m->blockedQueue[m->tail] = pid;
  m->tail = (m->tail + 1) % MAX_QUEUE_SIZE;
  m->size++;
  return true;
}

int dequeueMutexBlocked(Mutex *m)
{
  if (m->size == 0)
    return -1;
  int pid = m->blockedQueue[m->head];
  m->head = (m->head + 1) % MAX_QUEUE_SIZE;
  m->size--;
  return pid;
}

void blockProcess(SystemState *sys, int pid, ResourceType r)
{
  PCB *pcb = findPCB(sys, pid);
  Mutex *m = &sys->mutexes[r];
  if (!enqueueMutexBlocked(m, pid))
  {
    printf("Mutex %d queue full, killing P%d\n", r, pid);
    pcb->state = TERMINATED;
    return;
  }
  pcb->state = BLOCKED;
  pcb->blockedOnResource = r;
  sys->runningProcessID = -1;
  printf("P%d BLOCKED on res %d\n", pid, r);
}

void unblockProcess(SystemState *sys, ResourceType r)
{
  Mutex *m = &sys->mutexes[r];
  if (m->size == 0)
    return;
  // priority-based selection
  int bestPid = -1, bestPrio = 999, bestIdx = -1;
  for (int i = 0, idx = m->head; i < m->size; i++)
  {
    int pid = m->blockedQueue[idx];
    PCB *pcb = findPCB(sys, pid);
    if (pcb && pcb->priority < bestPrio)
    {
      bestPrio = pcb->priority;
      bestPid = pid;
      bestIdx = idx;
    }
    idx = (idx + 1) % MAX_QUEUE_SIZE;
  }
  if (bestPid < 0)
    return;
  // remove from queue
  int idx = bestIdx;
  for (int k = 0; k < m->size - 1; k++)
  {
    int next = (idx + 1) % MAX_QUEUE_SIZE;
    m->blockedQueue[idx] = m->blockedQueue[next];
    idx = next;
  }
  m->tail = (m->tail - 1 + MAX_QUEUE_SIZE) % MAX_QUEUE_SIZE;
  m->size--;

  PCB *pcb = findPCB(sys, bestPid);
  pcb->state = READY;
  pcb->blockedOnResource = -1;
  // enqueue into proper ready queue
  if (schedulerType == SCHED_MLFQ)
  {
    addToMLFQ(sys, bestPid, pcb->mlfqLevel);
  }
  else
  {
    addToReadyQueue(sys, bestPid);
  }
  printf("P%d UNBLOCKED from res %d\n", bestPid, (int)r);
}

void do_semWait(SystemState *sys, int pid, char *resName)
{
  ResourceType r = getResourceTypeFromString(resName);
  if (r < 0)
  {
    printf("semWait bad res '%s'\n", resName);
    findPCB(sys, pid)->state = TERMINATED;
    return;
  }
  Mutex *m = &sys->mutexes[r];
  if (m->locked)
  {
    blockProcess(sys, pid, r);
  }
  else
  {
    m->locked = true;
    m->lockingProcessID = pid;
    printf("P%d acquired res %d\n", pid, (int)r);
  }
}

void do_semSignal(SystemState *sys, int pid, char *resName)
{
  ResourceType r = getResourceTypeFromString(resName);
  if (r < 0)
  {
    printf("semSignal bad res '%s'\n", resName);
    findPCB(sys, pid)->state = TERMINATED;
    return;
  }
  Mutex *m = &sys->mutexes[r];
  if (m->locked && m->lockingProcessID == pid)
  {
    m->locked = false;
    m->lockingProcessID = -1;
    printf("P%d released res %d\n", pid, (int)r);
    unblockProcess(sys, r);
  }
  else
  {
    printf("P%d illegal semSignal on res %d\n", pid, (int)r);
    findPCB(sys, pid)->state = TERMINATED;
  }
}

// ------ Arrival & Simulation Loop ------

void checkArrivals(SystemState *sys)
{
  for (int i = 0; i < sys->processCount; i++)
  {
    PCB *pcb = &sys->processTable[i];
    if (pcb->state == NEW && pcb->arrivalTime <= sys->clockCycle)
    {
      if (schedulerType == SCHED_MLFQ)
        addToMLFQ(sys, i, 0);
      else
        addToReadyQueue(sys, i);
      pcb->state = READY;
      printf("Clock %d: P%d arrived\n", sys->clockCycle, i);
    }
  }
}

void runSimulation(SystemState *sys)
{
  int completed = 0;
  printf("Starting simulation with %s scheduler\n",
         schedulerType == SCHED_FCFS ? "FCFS" : schedulerType == SCHED_RR ? "RR"
                                                                          : "MLFQ");

  while (completed < sys->processCount)
  {
    printf("\n--- Cycle %d ---\n", sys->clockCycle);
    checkArrivals(sys);

    // Dispatch if CPU idle
    if (sys->runningProcessID < 0)
    {
      int next;
      if (schedulerType == SCHED_MLFQ)
        next = scheduleMLFQ(sys);
      else
        next = scheduleFCFS(sys);
      if (next >= 0)
      {
        PCB *pcb = findPCB(sys, next);
        pcb->state = RUNNING;
        if (schedulerType == SCHED_RR)
        {
          pcb->quantumRemaining = rrQuantum;
        }
        else if (schedulerType == SCHED_MLFQ)
        {
          pcb->priority = pcb->mlfqLevel;
          pcb->quantumRemaining = mlfqQuantum[pcb->mlfqLevel];
        }
        sys->runningProcessID = next;
        printf("Scheduler: P%d → RUNNING\n", next);
      }
      else
      {
        printf("Scheduler: CPU idle\n");
      }
    }
    else
    {
      printf("CPU continues P%d\n", sys->runningProcessID);
    }

    // Execute one instruction
    if (sys->runningProcessID >= 0)
    {
      PCB *pcb = findPCB(sys, sys->runningProcessID);
      interpretInstruction(sys, pcb->processID);

      // handle post‐execution
      if (pcb->state == TERMINATED)
      {
        printf("P%d terminated\n", pcb->processID);
        completed++;
        sys->runningProcessID = -1;
      }
      else if (pcb->state == BLOCKED)
      {
        // already moved to blocked queue
      }
      else if (pcb->state == RUNNING)
      {
        // advance PC
        pcb->programCounter++;

        // Check if process has finished after incrementing PC
        int instCount = findInstructionCount(sys, pcb->processID);
        if (pcb->programCounter >= instCount)
        {
          pcb->state = TERMINATED;
          printf("P%d done\n", pcb->processID);
          printf("P%d terminated\n", pcb->processID);
          completed++;
          sys->runningProcessID = -1;
        }
        else
        {
          // quantum handling
          if (schedulerType == SCHED_RR)
          {
            pcb->quantumRemaining--;
            if (pcb->quantumRemaining <= 0)
            {
              printf("P%d quantum expired\n", pcb->processID);
              pcb->state = READY;
              addToReadyQueue(sys, pcb->processID);
              sys->runningProcessID = -1;
            }
          }
          else if (schedulerType == SCHED_MLFQ)
          {
            pcb->quantumRemaining--;
            if (pcb->quantumRemaining <= 0)
            {
              printf("P%d MLFQ quantum expired at level %d\n",
                     pcb->processID, pcb->mlfqLevel);
              pcb->state = READY;
              int nl = pcb->mlfqLevel < MLFQ_LEVELS - 1 ? pcb->mlfqLevel + 1 : pcb->mlfqLevel;
              addToMLFQ(sys, pcb->processID, nl);
              sys->runningProcessID = -1;
            }
          }
        }
      }
    }

    sys->clockCycle++;
    if (sys->clockCycle > 1000)
    {
      printf("Safety break\n");
      break;
    }
  }
  printf("\nSimulation complete in %d cycles\n", sys->clockCycle);
}

// --------------------- main ---------------------

int main()
{
  char line[128];
  int choice;

  // 1) Choose scheduler
  printf("Choose scheduler (1=FCFS, 2=RR, 3=MLFQ): ");
  if (!fgets(line, sizeof(line), stdin))
  {
    fprintf(stderr, "Input error\n");
    return 1;
  }
  choice = atoi(line);
  if (choice < 1 || choice > 3)
    choice = 1; // default FCFS

  // 2) If RR, ask for quantum
  if (choice == 2)
  {
    schedulerType = SCHED_RR;
    printf("Enter RR quantum: ");
    if (!fgets(line, sizeof(line), stdin))
    {
      fprintf(stderr, "Input error\n");
      return 1;
    }
    rrQuantum = atoi(line);
    if (rrQuantum < 1)
      rrQuantum = 1;
  }
  else if (choice == 3)
  {
    schedulerType = SCHED_MLFQ;
  }
  else
  {
    schedulerType = SCHED_FCFS;
  }

  // 3) Initialize & load
  initializeSystem(&osSystem);
  loadProgram(&osSystem, "Program_1.txt", 0);
  loadProgram(&osSystem, "Program_2.txt", 1);
  loadProgram(&osSystem, "Program_3.txt", 2);

  // 4) Run
  runSimulation(&osSystem);
  return 0;
}