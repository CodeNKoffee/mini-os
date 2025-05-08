#include "simulator.h"
#include <stdarg.h>

static void sim_log(SystemState *sys, const char *format, ...)
{
  if (sys->callbacks && sys->callbacks->log_message)
  {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    sys->callbacks->log_message(sys->gui_data, buffer);
  }
  else
  {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    printf("\n");
    va_end(args);
  }
}

static void sim_output(SystemState *sys, int pid, const char *output)
{
  if (sys->callbacks && sys->callbacks->process_output)
  {
    sys->callbacks->process_output(sys->gui_data, pid, output);
  }
  else
  {
    printf("P%d OUTPUT: %s\n", pid, output);
  }
}

static void notify_state_update(SystemState *sys)
{
  if (sys->callbacks && sys->callbacks->state_update)
  {
    sys->callbacks->state_update(sys->gui_data, sys);
  }
}

static void checkArrivals(SystemState *sys);
static void addToReadyQueue(SystemState *sys, int pid);
static int scheduleNextProcess(SystemState *sys);
static void interpretInstruction(SystemState *sys, int pid);
static int findVariableMemoryIndex(SystemState *sys, int pid, const char *var, bool findFree);
static void setVariable(SystemState *sys, int pid, const char *var, const char *value);
static ResourceType getResourceTypeFromString(const char *s);
static bool enqueueMutexBlocked(Mutex *m, int pid);
static int dequeueMutexBlocked(SystemState *sys, Mutex *m);
static void blockProcess(SystemState *sys, int pid, ResourceType r);
static void unblockProcess(SystemState *sys, ResourceType r);
static void do_print(SystemState *sys, int pid, char *arg1);
static void do_assign(SystemState *sys, int pid, char *varName, char *valueOrInput);
static void do_writeFile(SystemState *sys, int pid, char *fileVar, char *dataVar);
static void do_readFile(SystemState *sys, int pid, char *fileVar);
static void do_printFromTo(SystemState *sys, int pid, char *v1, char *v2);
static void do_semWait(SystemState *sys, int pid, char *resName);
static void do_semSignal(SystemState *sys, int pid, char *resName);
static void addToMLFQ(SystemState *sys, int pid, int level);
static int getProgramNumberFromFilename(const char *filename);

void initializeSystem(SystemState *sys, SchedulerType type, int rrQuantumVal, GuiCallbacks *callbacks, void *gui_data)
{
  memset(sys, 0, sizeof(SystemState));

  sys->memoryPointer = 0;
  sys->processCount = 0;
  sys->runningProcessID = -1;
  sys->clockCycle = 0;
  sys->needsInput = false;
  sys->simulationComplete = false;

  sys->schedulerType = type;
  sys->rrQuantum = (type == SIM_SCHED_RR) ? (rrQuantumVal > 0 ? rrQuantumVal : 1) : 0;
  sys->mlfqQuantum[0] = 1;
  sys->mlfqQuantum[1] = 2;
  sys->mlfqQuantum[2] = 4;
  sys->mlfqQuantum[3] = 8;

  sys->callbacks = callbacks;
  sys->gui_data = gui_data;

  for (int i = 0; i < MEMORY_SIZE; i++)
  {
    sys->memory[i].name[0] = 0;
    sys->memory[i].value[0] = 0;
  }
  sys->readyHead = sys->readyTail = sys->readySize = 0;
  for (int l = 0; l < MLFQ_LEVELS; l++)
  {
    sys->mlfqHead[l] = sys->mlfqTail[l] = sys->mlfqSize[l] = 0;
  }
  for (int i = 0; i < NUM_RESOURCES; i++)
  {
    sys->mutexes[i].locked = false;
    sys->mutexes[i].lockingProcessID = -1;
    sys->mutexes[i].head = sys->mutexes[i].tail = sys->mutexes[i].size = 0;
  }
  sim_log(sys, "System initialized (%s, RRQ=%d)",
          type == SIM_SCHED_FCFS ? "FCFS" : type == SIM_SCHED_RR ? "RR"
                                                                : "MLFQ",
          sys->rrQuantum);
  notify_state_update(sys);
}

static int allocateMemory(SystemState *sys, int words)
{
  if (sys->memoryPointer + words > MEMORY_SIZE)
  {
    sim_log(sys, "Error: Out of memory! Requested %d words, available %d.",
            words, MEMORY_SIZE - sys->memoryPointer);
    return -1;
  }
  int start = sys->memoryPointer;
  sys->memoryPointer += words;
  return start;
}

bool loadProgram(SystemState *sys, const char *filename)
{
  if (sys->processCount >= MAX_PROCESSES)
  {
    sim_log(sys, "Error: process table full, cannot load %s", filename);
    return false;
  }
  FILE *f = fopen(filename, "r");
  if (!f)
  {
    sim_log(sys, "Error opening program file '%s': %s", filename, strerror(errno));
    return false;
  }

  char buf[MAX_LINE_LENGTH];
  int count = 0;
  while (fgets(buf, sizeof(buf), f))
  {
    bool non_whitespace_found = false;
    for (int i = 0; buf[i] != '\0'; i++)
    {
      if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r')
      {
        non_whitespace_found = true;
        break;
      }
    }
    if (non_whitespace_found)
    {
      count++;
    }
  }

  if (count == 0)
  {
    sim_log(sys, "Warning: %s is empty or contains only whitespace.", filename);
    fclose(f);
    return true;
  }
  if (count > MAX_PROGRAM_LINES)
  {
    sim_log(sys, "Warning: %s has %d lines, truncated to %d.", filename, count, MAX_PROGRAM_LINES);
    count = MAX_PROGRAM_LINES;
  }
  rewind(f);

  int memNeeded = count + NUM_VARIABLES + PCB_SIZE;
  int lb = allocateMemory(sys, memNeeded);
  if (lb < 0)
  {
    fclose(f);
    return false;
  }
  int ub = lb + memNeeded - 1;

  PCB *pcb = &sys->processTable[sys->processCount];
  pcb->processID = sys->processCount;
  pcb->programNumber = getProgramNumberFromFilename(filename);
  pcb->state = NEW;
  pcb->priority = 0;
  pcb->programCounter = 0;
  pcb->memoryLowerBound = lb;
  pcb->memoryUpperBound = ub;
  pcb->arrivalTime = sys->clockCycle;
  pcb->blockedOnResource = (ResourceType)-1;
  pcb->quantumRemaining = 0;
  pcb->mlfqLevel = 0;

  int currentMemIdx = lb;
  int linesRead = 0;
  while (linesRead < count && fgets(buf, sizeof(buf), f))
  {
    bool non_whitespace_found = false;
    for (int i = 0; buf[i] != '\0'; i++)
    {
      if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r')
      {
        non_whitespace_found = true;
        break;
      }
    }
    if (!non_whitespace_found)
      continue;

    buf[strcspn(buf, "\r\n")] = 0;

    if (currentMemIdx <= ub)
    {
      snprintf(sys->memory[currentMemIdx].name, sizeof(sys->memory[0].name), "Inst_%d_%d", pcb->processID, linesRead);
      strncpy(sys->memory[currentMemIdx].value, buf, sizeof(sys->memory[0].value) - 1);
      sys->memory[currentMemIdx].value[sizeof(sys->memory[0].value) - 1] = '\0';
      currentMemIdx++;
      linesRead++;
    }
    else
    {
      sim_log(sys, "Error: Memory overflow while loading instructions for P%d", pcb->processID);
      fclose(f);
      // Should ideally free allocated memory here, but let's keep it simple
      return false;
    }
  }
  fclose(f);

  int varStartIndex = lb + linesRead;
  for (int i = 0; i < NUM_VARIABLES; ++i)
  {
    int varMemIdx = varStartIndex + i;
    if (varMemIdx <= ub)
    {
      snprintf(sys->memory[varMemIdx].name, sizeof(sys->memory[0].name), "Var_%d_Free%d", pcb->processID, i);
      sys->memory[varMemIdx].value[0] = '\0';
    }
  }

  int pcbStartIndex = lb + count + NUM_VARIABLES;
  for (int i = 0; i < PCB_SIZE; ++i)
  {
    int pcbMemIdx = pcbStartIndex + i;
    if (pcbMemIdx <= ub)
    {
      snprintf(sys->memory[pcbMemIdx].name, sizeof(sys->memory[0].name), "PCB_%d_Slot%d", pcb->processID, i);
      sys->memory[pcbMemIdx].value[0] = '\0';
    }
  }

  sim_log(sys, "Loaded P%d: lines=%d, mem=[%d..%d], arrival=%d",
          pcb->programNumber, linesRead, lb, ub, sys->clockCycle);
  sys->processCount++;
  notify_state_update(sys);
  return true;
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

  int count = 0;
  char expectedNamePrefix[50];
  snprintf(expectedNamePrefix, sizeof(expectedNamePrefix), "Inst_%d_", pid);
  size_t prefixLen = strlen(expectedNamePrefix);

  for (int i = pcb->memoryLowerBound; i <= pcb->memoryUpperBound; ++i)
  {
    if (strncmp(sys->memory[i].name, expectedNamePrefix, prefixLen) == 0)
    {
      count++;
    }
    else
    {
      break;
    }
  }
  return count;
}

static void addToReadyQueue(SystemState *sys, int pid)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  if (sys->readySize >= MAX_QUEUE_SIZE)
  {
    sim_log(sys, "Error: FCFS/RR Ready queue full, dropping P%d", pcb->programNumber);
    pcb->state = TERMINATED;
    return;
  }
  pcb->state = READY;
  sys->readyQueue[sys->readyTail] = pid;
  sys->readyTail = (sys->readyTail + 1) % MAX_QUEUE_SIZE;
  sys->readySize++;
}

static void addToMLFQ(SystemState *sys, int pid, int level)
{
    if (level < 0 || level >= MLFQ_LEVELS) {
        sim_log(sys, "Error: Invalid MLFQ level %d for P%d", level, pid);
        return;
    }

    if (sys->mlfqSize[level] >= MAX_QUEUE_SIZE) {
        sim_log(sys, "Warning: MLFQ level %d full, trying next level for P%d",
                level, pid);
        if (level + 1 < MLFQ_LEVELS)
            addToMLFQ(sys, pid, level + 1);
        else {
            sim_log(sys, "Error: All MLFQ levels full - terminating P%d", pid);
            PCB *pcb = findPCB(sys, pid);
            if (pcb) pcb->state = TERMINATED;
        }
        return;
    }

    PCB *pcb = findPCB(sys, pid);
    if (!pcb) return;

    pcb->state     = READY;
    pcb->mlfqLevel = level;
    pcb->priority  = level;

    sys->mlfqRQ[level][sys->mlfqTail[level]] = pid;
    sys->mlfqTail[level] = (sys->mlfqTail[level] + 1) % MAX_QUEUE_SIZE;
    sys->mlfqSize[level]++;
}

static int scheduleNextProcess(SystemState *sys)
{
    if (sys->schedulerType == SIM_SCHED_MLFQ) {
        for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            while (sys->mlfqSize[lvl] > 0) {
                int pid = sys->mlfqRQ[lvl][sys->mlfqHead[lvl]];
                sys->mlfqHead[lvl] = (sys->mlfqHead[lvl] + 1) % MAX_QUEUE_SIZE;
                sys->mlfqSize[lvl]--;
                if (findPCB(sys, pid)->state == READY)
                    return pid;
            }
        }
    } else {
        while (sys->readySize > 0) {
            int pid = sys->readyQueue[sys->readyHead];
            sys->readyHead = (sys->readyHead + 1) % MAX_QUEUE_SIZE;
            sys->readySize--;
            if (findPCB(sys, pid)->state == READY)
                return pid;
        }
    }
    return -1;
}

static void interpretInstruction(SystemState *sys, int pid)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb || pcb->state != RUNNING)
  {
    sim_log(sys, "Error: Attempting to interpret instruction for non-running P%d (State: %d)", pid, pcb ? pcb->state : -1);
    if (pcb)
      pcb->state = TERMINATED;
    sys->runningProcessID = -1;
    return;
  }

  int instCount = findInstructionCount(sys, pid);

  if (pcb->programCounter >= instCount)
  {
    sim_log(sys, "P%d reached end of program (PC=%d, InstCount=%d). Terminating.", pid, pcb->programCounter, instCount);
    pcb->state = TERMINATED;
    return;
  }

  int memIdx = pcb->memoryLowerBound + pcb->programCounter;
  if (memIdx < pcb->memoryLowerBound || memIdx > pcb->memoryUpperBound)
  {
    sim_log(sys, "Error: P%d Program Counter (%d) resulted in invalid memory index %d. Terminating.", pid, pcb->programCounter, memIdx);
    pcb->state = TERMINATED;
    sys->runningProcessID = -1;
    return;
  }

  char line[MAX_LINE_LENGTH];
  strncpy(line, sys->memory[memIdx].value, MAX_LINE_LENGTH - 1);
  line[MAX_LINE_LENGTH - 1] = '\0';

  char line_copy_for_log[MAX_LINE_LENGTH];
  strncpy(line_copy_for_log, line, MAX_LINE_LENGTH - 1);
  line_copy_for_log[MAX_LINE_LENGTH - 1] = '\0';

  sim_log(sys, "P%d Executing [PC=%d]: %s", pcb->programNumber, pcb->programCounter, line_copy_for_log);

  char *cmd = strtok(line, " ");
  char *a1 = strtok(NULL, " ");
  char *a2 = strtok(NULL, " ");
  char *a3 = strtok(NULL, " ");

  bool error = false;
  bool instruction_completed = true;

  if (!cmd || strlen(cmd) == 0)
  {
    sim_log(sys, "P%d: NOP instruction", pcb->programNumber);
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
      sim_log(sys, "Error in P%d: assign requires variable name and value/source.", pcb->programNumber);
      error = true;
    }
    else if (strcmp(a2, "readFile") == 0 && a3)
    {
      char *filename = getVariable(sys, pid, a3);
      if (!filename)
      {
        error = true;
      }
      else
      {
        do_readFile(sys, pid, a3);
        if (pcb->state == TERMINATED)
        {
          error = true;
        }
        else
        {
          char tempVarName[64];
          snprintf(tempVarName, sizeof(tempVarName), "file_%s", a3);
          char *content = getVariable(sys, pid, tempVarName);
          if (!content)
          {
            sim_log(sys, "Error in P%d: readFile intermediate variable %s not found after read.", pcb->programNumber, tempVarName);
            error = true;
          }
          else
          {
            setVariable(sys, pid, a1, content);
            if (pcb->state == TERMINATED)
              error = true;
          }
        }
      }
    }
    else
    {
      do_assign(sys, pid, a1, a2);
      if (sys->needsInput && sys->inputPid == pid)
        instruction_completed = false;
      if (pcb->state == TERMINATED)
        error = true;
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
    {
      do_semWait(sys, pid, a1);
      if (pcb->state == BLOCKED)
      {
        instruction_completed = false;
      }
    }
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
    sim_log(sys, "Error in P%d: Unknown command '%s'", pcb->programNumber, cmd ? cmd : "<null>");
    error = true;
  }

  if (error)
  {
    sim_log(sys, "Error processing instruction for P%d. Terminating.", pcb->programNumber);
    pcb->state = TERMINATED;
    instruction_completed = true;
  }

  if (pcb->state == RUNNING && instruction_completed)
  {
    pcb->programCounter++;

    int newInstCount = findInstructionCount(sys, pid); // Recalculate just in case? (unlikely needed)
    if (pcb->programCounter >= newInstCount)
    {
      sim_log(sys, "P%d finished program after instruction (PC=%d, InstCount=%d). Terminating.", pcb->programNumber, pcb->programCounter, newInstCount);
      pcb->state = TERMINATED;
    }
  }
}

static int findVariableMemoryIndex(SystemState *sys, int pid, const char *varName, bool findFree)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return -1;

  int instructionCount = findInstructionCount(sys, pid);
  int varAreaStart = pcb->memoryLowerBound + instructionCount;
  int varAreaEnd = varAreaStart + NUM_VARIABLES - 1;
  if (varAreaEnd > pcb->memoryUpperBound)
  {
    varAreaEnd = pcb->memoryUpperBound;
  }

  char targetFullName[100];
  snprintf(targetFullName, sizeof(targetFullName), "Var_%d_%s", pid, varName);

  int firstFreeSlot = -1;

  for (int i = varAreaStart; i <= varAreaEnd; ++i)
  {
    if (strcmp(sys->memory[i].name, targetFullName) == 0)
    {
      return i;
    }

    if (findFree && firstFreeSlot == -1)
    {
      char freePrefix[50];
      snprintf(freePrefix, sizeof(freePrefix), "Var_%d_Free", pid);
      if (sys->memory[i].name[0] == '\0' || strncmp(sys->memory[i].name, freePrefix, strlen(freePrefix)) == 0)
      {
        firstFreeSlot = i;
      }
    }
  }

  if (findFree)
  {
    return firstFreeSlot;
  }

  return -1;
}

static void setVariable(SystemState *sys, int pid, const char *varName, const char *value)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  if (!varName || varName[0] == '\0')
  {
    sim_log(sys, "Error in P%d: Attempt to set variable with empty name.", pcb->programNumber);
    pcb->state = TERMINATED;
    return;
  }
  if (strcmp(varName, "input") == 0 || strcmp(varName, "readFile") == 0)
  {
    sim_log(sys, "Warning in P%d: Setting variable with reserved name '%s'.", pcb->programNumber, varName);
  }

  int memIndex = findVariableMemoryIndex(sys, pid, varName, true);

  if (memIndex < 0)
  {
    sim_log(sys, "Error in P%d: No free memory slot found for variable '%s'. Terminating.", pcb->programNumber, varName);
    pcb->state = TERMINATED;
    return;
  }

  snprintf(sys->memory[memIndex].name, sizeof(sys->memory[0].name), "Var_%d_%s", pid, varName);
  strncpy(sys->memory[memIndex].value, value, sizeof(sys->memory[0].value) - 1);
  sys->memory[memIndex].value[sizeof(sys->memory[0].value) - 1] = '\0';

}

char *getVariable(SystemState *sys, int pid, const char *varName)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return NULL;

  if (!varName || varName[0] == '\0')
  {
    sim_log(sys, "Error in P%d: Attempt to get variable with empty name.", pcb->programNumber);
    pcb->state = TERMINATED;
    return NULL;
  }

  int memIndex = findVariableMemoryIndex(sys, pid, varName, false);

  if (memIndex < 0)
  {
    char fileVarPrefix[64];
    snprintf(fileVarPrefix, sizeof(fileVarPrefix), "file_%s", varName);
    memIndex = findVariableMemoryIndex(sys, pid, fileVarPrefix, false);

    if (memIndex < 0)
    {
      sim_log(sys, "Error in P%d: Variable '%s' not found.", pcb->programNumber, varName);
      pcb->state = TERMINATED;
      return NULL;
    }
    return sys->memory[memIndex].value;
  }

  return sys->memory[memIndex].value;
}

static void do_print(SystemState *sys, int pid, char *varName)
{
  char *value = getVariable(sys, pid, varName);
  if (value)
  {
    sim_output(sys, pid, value);
  }
}

static void do_assign(SystemState *sys, int pid, char *varName, char *valueOrSource)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  if (strcmp(valueOrSource, "input") == 0)
  {
    if (sys->callbacks && sys->callbacks->request_input)
    {
      sim_log(sys, "P%d needs input for variable '%s'", pcb->programNumber, varName);
      sys->needsInput = true;
      strncpy(sys->inputVarName, varName, sizeof(sys->inputVarName) - 1);
      sys->inputVarName[sizeof(sys->inputVarName) - 1] = '\0';
      sys->inputPid = pid;
      notify_state_update(sys);
    }
    else
    {
      sim_log(sys, "Error in P%d: 'assign input' used, but no input callback registered. Terminating.", pcb->programNumber);
      pcb->state = TERMINATED;
    }
  }
  else
  {
    setVariable(sys, pid, varName, valueOrSource);
  }
}

void provideInput(SystemState *sys, const char *input)
{
  if (!sys->needsInput || sys->inputPid < 0)
  {
    sim_log(sys, "Warning: provideInput called when no input was pending.");
    return;
  }

  PCB *pcb = findPCB(sys, sys->inputPid);
  if (!pcb || pcb->state != RUNNING)
  {
    sim_log(sys, "Warning: provideInput called for P%d which is not in RUNNING state.", sys->inputPid);
    sys->needsInput = false;
    sys->inputPid = -1;
    return;
  }

  sim_log(sys, "P%d received input '%s' for variable '%s'", pcb->programNumber, input ? input : "<NULL>", sys->inputVarName);

  if (input)
  {
    setVariable(sys, sys->inputPid, sys->inputVarName, input);
  }
  else
  {
    sim_log(sys, "P%d received NULL input for '%s'. Treating as empty string.", pcb->programNumber, sys->inputVarName);
    setVariable(sys, sys->inputPid, sys->inputVarName, "");
  }

  sys->needsInput = false;
  sys->inputPid = -1;
  sys->inputVarName[0] = '\0';

  if (pcb->state == RUNNING)
  {
    pcb->programCounter++;
    int instCount = findInstructionCount(sys, pcb->processID);
    if (pcb->programCounter >= instCount)
    {
      sim_log(sys, "P%d finished program after receiving input (PC=%d, InstCount=%d). Terminating.", pcb->programNumber, pcb->programCounter, instCount);
      pcb->state = TERMINATED;
    }
  }

  notify_state_update(sys);
}

static void do_writeFile(SystemState *sys, int pid, char *fileVar, char *dataVar)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  char *filename = getVariable(sys, pid, fileVar);
  if (!filename)
  {
    return;
  }

  char *data = getVariable(sys, pid, dataVar);
  if (!data)
  {
    return;
  }

  FILE *f = fopen(filename, "w");
  if (!f)
  {
    sim_log(sys, "Error in P%d: Cannot open file '%s' for writing: %s. Terminating.", pcb->programNumber, filename, strerror(errno));
    pcb->state = TERMINATED;
    return;
  }

  fprintf(f, "%s", data);
  fclose(f);
  sim_log(sys, "P%d wrote to file '%s'", pcb->programNumber, filename);
}

static void do_readFile(SystemState *sys, int pid, char *fileVar)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  char *filename = getVariable(sys, pid, fileVar);
  if (!filename)
  {
    return;
  }

  FILE *f = fopen(filename, "r");
  if (!f)
  {
    sim_log(sys, "Error in P%d: Cannot open file '%s' for reading: %s. Terminating.", pcb->programNumber, filename, strerror(errno));
    pcb->state = TERMINATED;
    return;
  }

  char content[500] = "";
  char lineBuffer[MAX_LINE_LENGTH];
  size_t currentLen = 0;
  size_t maxLen = sizeof(content) - 1;

  while (fgets(lineBuffer, sizeof(lineBuffer), f))
  {
    size_t lineLen = strlen(lineBuffer);
    if (currentLen + lineLen > maxLen)
    {
      sim_log(sys, "Warning in P%d: File '%s' content truncated during read.", pcb->programNumber, filename);
      strncat(content, lineBuffer, maxLen - currentLen);
      currentLen = maxLen;
      break;
    }
    strcat(content, lineBuffer);
    currentLen += lineLen;
  }
  fclose(f);
  content[currentLen] = '\0';

  char resultVarName[100];
  snprintf(resultVarName, sizeof(resultVarName), "file_%s", fileVar);

  setVariable(sys, pid, resultVarName, content);
}

static void do_printFromTo(SystemState *sys, int pid, char *v1, char *v2)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  char *s1 = getVariable(sys, pid, v1);
  if (!s1)
    return;
  char *s2 = getVariable(sys, pid, v2);
  if (!s2)
    return;

  char *endptr1, *endptr2;
  long val1 = strtol(s1, &endptr1, 10);
  long val2 = strtol(s2, &endptr2, 10);

  if (*endptr1 != '\0' || *endptr2 != '\0')
  {
    sim_log(sys, "Error in P%d: printFromTo requires numeric values for '%s' ('%s') and '%s' ('%s').", pcb->programNumber, v1, s1, v2, s2);
    findPCB(sys, pid)->state = TERMINATED;
    return;
  }

  char outputBuffer[512];
  outputBuffer[0] = '\0';
  int remaining = sizeof(outputBuffer) - 1;

  if (val1 <= val2)
  {
    for (long i = val1; i <= val2; ++i)
    {
      int written = snprintf(outputBuffer + strlen(outputBuffer), remaining, "%ld ", i);
      if (written < 0 || written >= remaining)
      {
        sim_log(sys, "Warning in P%d: printFromTo output truncated.", pcb->programNumber);
        break;
      }
      remaining -= written;
    }
  }
  else
  {
    for (long i = val1; i >= val2; --i)
    {
      int written = snprintf(outputBuffer + strlen(outputBuffer), remaining, "%ld ", i);
      if (written < 0 || written >= remaining)
      {
        sim_log(sys, "Warning in P%d: printFromTo output truncated.", pcb->programNumber);
        break;
      }
      remaining -= written;
    }
  }

  size_t len = strlen(outputBuffer);
  if (len > 0 && outputBuffer[len - 1] == ' ')
  {
    outputBuffer[len - 1] = '\0';
  }

  sim_output(sys, pid, outputBuffer);
}

static ResourceType getResourceTypeFromString(const char *s)
{
  if (!s)
    return (ResourceType)-1;
  if (strcmp(s, "file") == 0)
    return RESOURCE_FILE;
  if (strcmp(s, "userInput") == 0)
    return RESOURCE_USER_INPUT;
  if (strcmp(s, "userOutput") == 0)
    return RESOURCE_USER_OUTPUT;
  return (ResourceType)-1;
}

static bool enqueueMutexBlocked(Mutex *m, int pid)
{
  if (m->size >= MAX_QUEUE_SIZE)
  {
    return false;
  }
  m->blockedQueue[m->tail] = pid;
  m->tail = (m->tail + 1) % MAX_QUEUE_SIZE;
  m->size++;
  return true;
}

static int dequeueMutexBlocked(SystemState *sys, Mutex *m)
{
  if (m->size == 0)
  {
    return -1;
  }

  int bestPid = -1;
  int bestPriority = 9999;
  int bestIndexInQueue = -1;
  int currentQueueIndex = m->head;

  for (int i = 0; i < m->size; ++i)
  {
    int pid = m->blockedQueue[currentQueueIndex];
    PCB *pcb = findPCB(sys, pid);

    if (pcb && pcb->priority < bestPriority)
    {
      bestPriority = pcb->priority;
      bestPid = pid;
      bestIndexInQueue = currentQueueIndex;
    }

    currentQueueIndex = (currentQueueIndex + 1) % MAX_QUEUE_SIZE;
  }

  if (bestPid == -1)
  {
    sim_log(sys, "Error: Could not find highest priority process in mutex queue (size %d)", m->size);
    bestPid = m->blockedQueue[m->head];
    m->head = (m->head + 1) % MAX_QUEUE_SIZE;
    m->size--;
    return bestPid;
  }

  if (bestIndexInQueue == m->head)
  {
    m->head = (m->head + 1) % MAX_QUEUE_SIZE;
    m->size--;
  }
  else
  {
    int tempQueue[MAX_QUEUE_SIZE];
    int tempCount = 0;
    int scanIdx = m->head;
    for (int k = 0; k < m->size; k++)
    {
      int currentPid = m->blockedQueue[scanIdx];
      if (currentPid != bestPid)
      {
        tempQueue[tempCount++] = currentPid;
      }
      scanIdx = (scanIdx + 1) % MAX_QUEUE_SIZE;
    }
    // Copy back from tempQueue
    m->head = 0;
    m->size = 0;
    m->tail = 0;
    for (int k = 0; k < tempCount; k++)
    {
      m->blockedQueue[m->tail] = tempQueue[k];
      m->tail = (m->tail + 1) % MAX_QUEUE_SIZE;
      m->size++;
    }
  }

  return bestPid;
}

static void blockProcess(SystemState *sys, int pid, ResourceType r)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;
  Mutex *m = &sys->mutexes[r];

  if (!enqueueMutexBlocked(m, pid))
  {
    sim_log(sys, "Error: Mutex queue for resource %d full. Cannot block P%d. Terminating.", r, pcb->programNumber);
    pcb->state = TERMINATED;
    if (sys->runningProcessID == pid)
    {
      sys->runningProcessID = -1;
    }
    return;
  }

  pcb->state = BLOCKED;
  pcb->blockedOnResource = r;

  if (sys->runningProcessID == pid)
  {
    sys->runningProcessID = -1;
  }

  sim_log(sys, "P%d BLOCKED on resource %d", pcb->programNumber, r);
  notify_state_update(sys);
}

static void unblockProcess(SystemState *sys, ResourceType r)
{
  Mutex *m = &sys->mutexes[r];
  if (m->size == 0)
  {
    return;
  }

  int pidToUnblock = dequeueMutexBlocked(sys, m);

  if (pidToUnblock < 0)
  {
    sim_log(sys, "Error: Mutex %d queue not empty but dequeue failed.", r);
    return;
  }

  PCB *pcb = findPCB(sys, pidToUnblock);
  if (!pcb)
  {
    sim_log(sys, "Error: Dequeued PID %d from mutex %d but PCB not found.", pidToUnblock, r);
    return;
  }

  pcb->state = READY;
  pcb->blockedOnResource = (ResourceType)-1;

  sys->wasUnblockedThisCycle[pidToUnblock] = true;

  if (sys->schedulerType == SIM_SCHED_MLFQ)
  {
    addToMLFQ(sys, pidToUnblock, pcb->mlfqLevel);
  }
  else
  {
    addToReadyQueue(sys, pidToUnblock);
  }

  sim_log(sys, "P%d UNBLOCKED from resource %d, added to ready queue.", pcb->programNumber, r);
  notify_state_update(sys);
}

static void do_semWait(SystemState *sys, int pid, char *resName)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  ResourceType r = getResourceTypeFromString(resName);
  if (r < 0 || r >= NUM_RESOURCES)
  {
    sim_log(sys, "Error in P%d: semWait invalid resource name '%s'. Terminating.", pcb->programNumber, resName ? resName : "<null>");
    pcb->state = TERMINATED;
    return;
  }

  Mutex *m = &sys->mutexes[r];

  if (m->locked)
  {
    sim_log(sys, "P%d requests locked resource %d. Blocking.", pcb->programNumber, r);
    pcb->priority = (sys->schedulerType == SIM_SCHED_MLFQ) ? pcb->mlfqLevel : 0;
    blockProcess(sys, pid, r);
  }
  else
  {
    m->locked = true;
    m->lockingProcessID = pid;
    sim_log(sys, "P%d acquired resource %d.", pcb->programNumber, r);
    notify_state_update(sys);
  }
}

static void do_semSignal(SystemState *sys, int pid, char *resName)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  ResourceType r = getResourceTypeFromString(resName);
  if (r < 0 || r >= NUM_RESOURCES)
  {
    sim_log(sys, "Error in P%d: semSignal invalid resource name '%s'. Terminating.", pcb->programNumber, resName ? resName : "<null>");
    pcb->state = TERMINATED;
    return;
  }

  Mutex *m = &sys->mutexes[r];

  if (m->locked && m->lockingProcessID == pid)
  {
    m->locked = false;
    m->lockingProcessID = -1;
    sim_log(sys, "P%d released resource %d.", pcb->programNumber, r);
    unblockProcess(sys, r);
  }
  else
  {
    sim_log(sys, "Error in P%d: Illegal semSignal on resource %d (Locked: %d, Holder: P%d). Terminating.",
            pcb->programNumber, r, m->locked, m->lockingProcessID);
    pcb->state = TERMINATED;
  }
  if (m->size == 0)
  {
    notify_state_update(sys);
  }
}

static void checkArrivals(SystemState *sys)
{
  for (int i = 0; i < sys->processCount; i++)
  {
    if (sys->wasUnblockedThisCycle[i])
    {
      PCB *pcb = &sys->processTable[i];
      if (pcb->state == READY)
      {
        sys->wasUnblockedThisCycle[i] = false;
      }
    }
  }

  for (int i = 0; i < sys->processCount; i++)
  {
    PCB *pcb = &sys->processTable[i];
    if (pcb->state == NEW && pcb->arrivalTime <= sys->clockCycle)
    {
      sim_log(sys, "Clock %d: P%d arrived.", sys->clockCycle, pcb->programNumber);
      pcb->state = READY;
      if (sys->schedulerType == SIM_SCHED_MLFQ)
      {
        addToMLFQ(sys, i, 0);
      }
      else
      {
        addToReadyQueue(sys, i);
      }
      notify_state_update(sys);
    }
  }
}

// ------ Simulation Step ------

bool isSimulationComplete(SystemState *sys)
{
  if (sys->processCount == 0)
  {
    return false;
  }

  if (sys->simulationComplete)
  {
    return true;
  }

  int completedOrTerminatedCount = 0;
  for (int i = 0; i < sys->processCount; ++i)
  {
    if (sys->processTable[i].state == TERMINATED)
    {
      completedOrTerminatedCount++;
    }
  }

  bool complete = (completedOrTerminatedCount == sys->processCount);
  if (complete)
  {
    sys->simulationComplete = true;
  }
  return complete;
}

void stepSimulation(SystemState *sys)
{
  if (isSimulationComplete(sys))
  {
    sim_log(sys, "Simulation already complete.");
    return;
  }

  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    sys->wasUnblockedThisCycle[i] = false;
  }

  if (sys->needsInput)
  {
    sim_log(sys, "Simulation paused, waiting for input for P%d.", sys->inputPid);
    return;
  }

  sim_log(sys, "--- Clock Cycle %d ---", sys->clockCycle);

  checkArrivals(sys);

  bool needToSchedule = false;
  if (sys->runningProcessID >= 0)
  {
    PCB *runningPCB = findPCB(sys, sys->runningProcessID);
    if (!runningPCB || runningPCB->state != RUNNING)
    {
      sim_log(sys, "Warning: Running PID %d is not in RUNNING state (%d). CPU becoming idle.", sys->runningProcessID, runningPCB ? runningPCB->state : -1);
      sys->runningProcessID = -1;
      needToSchedule = true;
    }
    else
    {
      if (sys->schedulerType == SIM_SCHED_RR && runningPCB->quantumRemaining <= 0)
      {
        sim_log(sys, "P%d RR quantum expired.", runningPCB->programNumber);
        runningPCB->state = READY;
        addToReadyQueue(sys, sys->runningProcessID);
        sys->runningProcessID = -1;
        needToSchedule = true;
        notify_state_update(sys);
      }
      else if (sys->schedulerType == SIM_SCHED_MLFQ && runningPCB->quantumRemaining <= 0)
      {
        sim_log(sys, "P%d MLFQ quantum expired at level %d.", runningPCB->programNumber, runningPCB->mlfqLevel);
        runningPCB->state = READY;
        int nextLevel = (runningPCB->mlfqLevel < MLFQ_LEVELS - 1) ? runningPCB->mlfqLevel + 1 : runningPCB->mlfqLevel;
        sim_log(sys, "P%d demoted to level %d.", runningPCB->programNumber, nextLevel);
        addToMLFQ(sys, sys->runningProcessID, nextLevel);
        sys->runningProcessID = -1;
        needToSchedule = true;
        notify_state_update(sys);
      }
    }
  }
  else
  {
    needToSchedule = true;
  }

  if (needToSchedule && sys->runningProcessID < 0)
  {
    int nextPid = scheduleNextProcess(sys);
    if (nextPid >= 0)
    {
      sys->runningProcessID = nextPid;
      PCB *newlyScheduledPCB = findPCB(sys, nextPid);
      if (newlyScheduledPCB)
      {
        newlyScheduledPCB->state = RUNNING;

        if (sys->schedulerType == SIM_SCHED_RR)
        {
          newlyScheduledPCB->quantumRemaining = sys->rrQuantum;
        }
        else if (sys->schedulerType == SIM_SCHED_MLFQ)
        {
          newlyScheduledPCB->quantumRemaining = sys->mlfqQuantum[newlyScheduledPCB->mlfqLevel];
        }
        sim_log(sys, "Scheduler: Dispatching P%d (Level: %d, Quantum: %d)",
                newlyScheduledPCB->programNumber, newlyScheduledPCB->mlfqLevel, newlyScheduledPCB->quantumRemaining);
        notify_state_update(sys);
      }
      else
      {
        sim_log(sys, "Error: Scheduled PID %d not found!", nextPid);
        sys->runningProcessID = -1;
      }
    }
    else
    {
      sim_log(sys, "Scheduler: CPU Idle - No ready processes.");
      sys->runningProcessID = -1;
      notify_state_update(sys);
    }
  }

  if (sys->runningProcessID >= 0)
  {
    PCB *currentPCB = findPCB(sys, sys->runningProcessID);
    if (currentPCB && currentPCB->state == RUNNING && !sys->needsInput)
    {
      if (sys->schedulerType == SIM_SCHED_RR || sys->schedulerType == SIM_SCHED_MLFQ)
      {
        currentPCB->quantumRemaining--;
      }

      interpretInstruction(sys, sys->runningProcessID);

      if (currentPCB->state == TERMINATED)
      {
        sim_log(sys, "P%d terminated during execution.", currentPCB->programNumber);
        isSimulationComplete(sys);
        sys->runningProcessID = -1;
        notify_state_update(sys);
      }
    }
  }

  sys->clockCycle++;

  if (isSimulationComplete(sys))
  {
    sim_log(sys, "Simulation Complete at Clock Cycle %d.", sys->clockCycle);
    notify_state_update(sys);
  }
}

static int getProgramNumberFromFilename(const char *filename)
{
  if (strstr(filename, "Program_1.txt"))
  {
    return 1;
  }
  else if (strstr(filename, "Program_2.txt"))
  {
    return 2;
  }
  else if (strstr(filename, "Program_3.txt"))
  {
    return 3;
  }
  return 0;
}