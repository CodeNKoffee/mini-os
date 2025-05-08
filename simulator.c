#include "simulator.h"
#include <stdarg.h> // For va_list, vsnprintf

// Helper function for logging via callback
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
    // Fallback to stdout if no callback is registered
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    printf("\n"); // Add newline for clarity
    va_end(args);
  }
}

// Helper function for process output via callback
static void sim_output(SystemState *sys, int pid, const char *output)
{
  if (sys->callbacks && sys->callbacks->process_output)
  {
    sys->callbacks->process_output(sys->gui_data, pid, output);
  }
  else
  {
    printf("P%d OUTPUT: %s\n", pid, output); // Fallback
  }
}

// Helper function to notify GUI of state change
static void notify_state_update(SystemState *sys)
{
  if (sys->callbacks && sys->callbacks->state_update)
  {
    sys->callbacks->state_update(sys->gui_data, sys);
  }
}

// --- Internal Function Declarations ---
// (These are now static as they are internal to simulator.c)
static void checkArrivals(SystemState *sys);
static void addToReadyQueue(SystemState *sys, int pid);
static int scheduleNextProcess(SystemState *sys); // Combined scheduler logic
static void interpretInstruction(SystemState *sys, int pid);
static int findVariableMemoryIndex(SystemState *sys, int pid, const char *var, bool findFree);
static void setVariable(SystemState *sys, int pid, const char *var, const char *value);
// getVariable is in simulator.h as it might be useful for GUI display
static ResourceType getResourceTypeFromString(const char *s);
static bool enqueueMutexBlocked(Mutex *m, int pid);
static int dequeueMutexBlocked(SystemState *sys, Mutex *m); // Pass sys for priority checks
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

// ---------------- Implementation ----------------

void initializeSystem(SystemState *sys, SchedulerType type, int rrQuantumVal, GuiCallbacks *callbacks, void *gui_data)
{
  memset(sys, 0, sizeof(SystemState)); // This will initialize wasUnblockedThisCycle to false

  sys->memoryPointer = 0;
  sys->processCount = 0;
  sys->runningProcessID = -1;
  sys->clockCycle = 0;
  sys->needsInput = false;
  sys->simulationComplete = false;

  sys->schedulerType = type;
  sys->rrQuantum = (type == SIM_SCHED_RR) ? (rrQuantumVal > 0 ? rrQuantumVal : 1) : 0;
  // Default MLFQ quanta
  sys->mlfqQuantum[0] = 1;
  sys->mlfqQuantum[1] = 2;
  sys->mlfqQuantum[2] = 4;
  sys->mlfqQuantum[3] = 8;

  sys->callbacks = callbacks;
  sys->gui_data = gui_data;

  // Clear memory (already done by memset, but explicit doesn't hurt)
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

// Returns true on success, false on failure
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

  // Count non-empty lines
  char buf[MAX_LINE_LENGTH];
  int count = 0;
  while (fgets(buf, sizeof(buf), f))
  {
    // Check if line is not just whitespace
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
    return true; // Not a failure, just nothing to load
  }
  if (count > MAX_PROGRAM_LINES)
  {
    sim_log(sys, "Warning: %s has %d lines, truncated to %d.", filename, count, MAX_PROGRAM_LINES);
    count = MAX_PROGRAM_LINES;
  }
  rewind(f);

  // Memory needed: Instructions + Variables + PCB placeholder (optional, not strictly needed if PCB is separate)
  // Let's stick to the original calculation for consistency for now.
  int memNeeded = count + NUM_VARIABLES + PCB_SIZE;
  int lb = allocateMemory(sys, memNeeded);
  if (lb < 0)
  {
    fclose(f);
    return false; // Allocation failed
  }
  int ub = lb + memNeeded - 1; // Inclusive upper bound

  PCB *pcb = &sys->processTable[sys->processCount];
  pcb->processID = sys->processCount;
  pcb->programNumber = getProgramNumberFromFilename(filename);
  pcb->state = NEW;
  pcb->priority = 0; // Default priority, MLFQ will adjust
  pcb->programCounter = 0;
  pcb->memoryLowerBound = lb;
  pcb->memoryUpperBound = ub;
  pcb->arrivalTime = sys->clockCycle;
  pcb->blockedOnResource = (ResourceType)-1; // Use -1 to indicate not blocked
  pcb->quantumRemaining = 0;
  pcb->mlfqLevel = 0; // Start at highest level

  // Load instructions into memory
  int currentMemIdx = lb;
  int linesRead = 0;
  while (linesRead < count && fgets(buf, sizeof(buf), f))
  {
    // Check if line is not just whitespace
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
      continue; // Skip whitespace-only lines

    // Remove trailing newline/carriage return
    buf[strcspn(buf, "\r\n")] = 0;

    if (currentMemIdx <= ub)
    { // Ensure we don't write past allocated bound
      snprintf(sys->memory[currentMemIdx].name, sizeof(sys->memory[0].name), "Inst_%d_%d", pcb->processID, linesRead);
      strncpy(sys->memory[currentMemIdx].value, buf, sizeof(sys->memory[0].value) - 1);
      sys->memory[currentMemIdx].value[sizeof(sys->memory[0].value) - 1] = '\0'; // Ensure null termination
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

  // Initialize variable slots in memory
  int varStartIndex = lb + linesRead; // Start variables right after instructions
  for (int i = 0; i < NUM_VARIABLES; ++i)
  {
    int varMemIdx = varStartIndex + i;
    if (varMemIdx <= ub)
    { // Check bounds
      snprintf(sys->memory[varMemIdx].name, sizeof(sys->memory[0].name), "Var_%d_Free%d", pcb->processID, i);
      sys->memory[varMemIdx].value[0] = '\0'; // Clear value
    }
  }

  // Initialize PCB placeholder memory slots (original logic)
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
  notify_state_update(sys); // Notify GUI about the new process
  return true;
}

PCB *findPCB(SystemState *sys, int pid)
{
  if (pid < 0 || pid >= sys->processCount)
    return NULL;
  return &sys->processTable[pid];
}

// Find the actual number of instruction lines loaded for a process
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
      // Stop counting if we encounter a non-instruction entry
      // This assumes instructions are contiguous at the start.
      break;
    }
  }
  return count;
}

// ------------ Scheduling ------------

static void addToReadyQueue(SystemState *sys, int pid)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return; // Should not happen

  if (sys->readySize >= MAX_QUEUE_SIZE)
  {
    sim_log(sys, "Error: FCFS/RR Ready queue full, dropping P%d", pcb->programNumber);
    // Consider terminating the process?
    pcb->state = TERMINATED; // Mark as terminated if dropped
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

    /* If the chosen queue is already full, keep the project’s
       “spill-to-lower-level” policy unchanged … */
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

    /* ----------  *the only behavioural change*  ---------- */
    sys->mlfqRQ[level][sys->mlfqTail[level]] = pid;          // enqueue
    sys->mlfqTail[level] = (sys->mlfqTail[level] + 1) % MAX_QUEUE_SIZE;
    sys->mlfqSize[level]++;
}

// Combined scheduler: returns PID of next process to run, or -1 if none
static int scheduleNextProcess(SystemState *sys)
{
    if (sys->schedulerType == SIM_SCHED_MLFQ) {
        for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            while (sys->mlfqSize[lvl] > 0) {
                int pid = sys->mlfqRQ[lvl][sys->mlfqHead[lvl]];
                sys->mlfqHead[lvl] = (sys->mlfqHead[lvl] + 1) % MAX_QUEUE_SIZE;
                sys->mlfqSize[lvl]--;
                /* ⬇️  NEW: ignore anything that is no longer READY */
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
    return -1;      // nothing runnable
}

// ------------- Interpreter -------------

static void interpretInstruction(SystemState *sys, int pid)
{
  PCB *pcb = findPCB(sys, pid);
  // Check if PCB exists and process is in RUNNING state (should be, but safety check)
  if (!pcb || pcb->state != RUNNING)
  {
    sim_log(sys, "Error: Attempting to interpret instruction for non-running P%d (State: %d)", pid, pcb ? pcb->state : -1);
    // If it's somehow not running, maybe try to fix state or terminate?
    if (pcb)
      pcb->state = TERMINATED;  // Terminate if in inconsistent state
    sys->runningProcessID = -1; // Ensure CPU becomes idle
    return;
  }

  int instCount = findInstructionCount(sys, pid);

  // Check if Program Counter is valid
  if (pcb->programCounter >= instCount)
  {
    sim_log(sys, "P%d reached end of program (PC=%d, InstCount=%d). Terminating.", pid, pcb->programCounter, instCount);
    pcb->state = TERMINATED;
    // Don't return yet, let the main loop handle termination cleanup this cycle
    return;
  }

  // Fetch instruction from memory
  int memIdx = pcb->memoryLowerBound + pcb->programCounter;
  if (memIdx < pcb->memoryLowerBound || memIdx > pcb->memoryUpperBound)
  {
    sim_log(sys, "Error: P%d Program Counter (%d) resulted in invalid memory index %d. Terminating.", pid, pcb->programCounter, memIdx);
    pcb->state = TERMINATED;
    sys->runningProcessID = -1;
    return;
  }

  // Make a mutable copy of the instruction line for strtok
  char line[MAX_LINE_LENGTH];
  strncpy(line, sys->memory[memIdx].value, MAX_LINE_LENGTH - 1);
  line[MAX_LINE_LENGTH - 1] = '\0'; // Ensure null termination

  // Make another copy for logging, as strtok modifies the string
  char line_copy_for_log[MAX_LINE_LENGTH];
  strncpy(line_copy_for_log, line, MAX_LINE_LENGTH - 1);
  line_copy_for_log[MAX_LINE_LENGTH - 1] = '\0';

  sim_log(sys, "P%d Executing [PC=%d]: %s", pcb->programNumber, pcb->programCounter, line_copy_for_log);

  // Tokenize the instruction line
  char *cmd = strtok(line, " ");
  char *a1 = strtok(NULL, " ");
  char *a2 = strtok(NULL, " ");
  char *a3 = strtok(NULL, " "); // For potential 3-argument instructions like 'assign b readFile a'

  bool error = false;
  bool instruction_completed = true; // Assume completion unless blocked or input needed

  if (!cmd || strlen(cmd) == 0)
  {
    // Empty line or NOP - just advance PC
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
      // assign b readFile a
      // Step 1: read file whose name is in variable a3
      char *filename = getVariable(sys, pid, a3);
      if (!filename)
      {
        error = true;
      }
      else
      {
        do_readFile(sys, pid, a3); // reads file into "file_a3"
        if (pcb->state == TERMINATED)
        {
          error = true;
        }
        else
        {
          // Step 2: assign b = file_a3
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
      // Normal assign (including assign b input)
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
  { // Direct readFile instruction (legacy?)
    if (a1)
      do_readFile(sys, pid, a1); // Reads into "file_<a1>"
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
      // If semWait blocked the process, state will be BLOCKED
      if (pcb->state == BLOCKED)
      {
        instruction_completed = false; // Blocked, don't advance PC
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
    instruction_completed = true; // Error means instruction effect is termination
  }

  // --- Post-instruction processing ---

  // If the instruction completed successfully and didn't block/request input/terminate, advance PC.
  // We check the state *after* the instruction execution attempt.
  if (pcb->state == RUNNING && instruction_completed)
  {
    pcb->programCounter++;

    // Check for program completion *after* advancing PC
    int newInstCount = findInstructionCount(sys, pid); // Recalculate just in case? (unlikely needed)
    if (pcb->programCounter >= newInstCount)
    {
      sim_log(sys, "P%d finished program after instruction (PC=%d, InstCount=%d). Terminating.", pcb->programNumber, pcb->programCounter, newInstCount);
      pcb->state = TERMINATED;
    }
  }
  // State transitions (TERMINATED, BLOCKED) are handled within the instruction handlers
  // or in the main stepSimulation loop after interpretInstruction returns.
}

// -------- Variable Management --------

// Finds memory index for a variable. If findFree is true, returns first free slot if var not found.
static int findVariableMemoryIndex(SystemState *sys, int pid, const char *varName, bool findFree)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return -1;

  int instructionCount = findInstructionCount(sys, pid);
  // Variable storage area starts after instructions and ends before the optional PCB area.
  // Let's refine the bounds based on actual usage.
  int varAreaStart = pcb->memoryLowerBound + instructionCount;
  // The end index should be calculated carefully. Original code used `pcb->memoryUpperBound - PCB_SIZE`.
  // Let's assume variables can use up to `NUM_VARIABLES` slots *after* instructions.
  int varAreaEnd = varAreaStart + NUM_VARIABLES - 1;
  // Ensure varAreaEnd does not exceed the overall process bounds
  if (varAreaEnd > pcb->memoryUpperBound)
  {
    varAreaEnd = pcb->memoryUpperBound;
  }

  char targetFullName[100]; // Increased buffer size
  snprintf(targetFullName, sizeof(targetFullName), "Var_%d_%s", pid, varName);

  int firstFreeSlot = -1;

  for (int i = varAreaStart; i <= varAreaEnd; ++i)
  {
    // Check if this slot holds the variable we're looking for
    if (strcmp(sys->memory[i].name, targetFullName) == 0)
    {
      return i; // Found the variable
    }

    // If looking for a free slot, check if this one qualifies
    if (findFree && firstFreeSlot == -1)
    {
      // A slot is free if its name is empty or starts with "Var_%d_Free"
      char freePrefix[50];
      snprintf(freePrefix, sizeof(freePrefix), "Var_%d_Free", pid);
      if (sys->memory[i].name[0] == '\0' || strncmp(sys->memory[i].name, freePrefix, strlen(freePrefix)) == 0)
      {
        firstFreeSlot = i;
        // Don't return immediately, continue scanning to see if the named variable exists
      }
    }
  }

  // If we finished scanning and were looking for a free slot, return it
  if (findFree)
  {
    return firstFreeSlot;
  }

  // Variable not found, and not looking for a free slot
  return -1;
}

static void setVariable(SystemState *sys, int pid, const char *varName, const char *value)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return; // Should not happen

  // Check for invalid variable names (e.g., empty)
  if (!varName || varName[0] == '\0')
  {
    sim_log(sys, "Error in P%d: Attempt to set variable with empty name.", pcb->programNumber);
    pcb->state = TERMINATED;
    return;
  }
  // Check for potentially problematic names (though less critical now)
  if (strcmp(varName, "input") == 0 || strcmp(varName, "readFile") == 0)
  {
    sim_log(sys, "Warning in P%d: Setting variable with reserved name '%s'.", pcb->programNumber, varName);
  }

  int memIndex = findVariableMemoryIndex(sys, pid, varName, true); // Find existing or first free slot

  if (memIndex < 0)
  {
    sim_log(sys, "Error in P%d: No free memory slot found for variable '%s'. Terminating.", pcb->programNumber, varName);
    pcb->state = TERMINATED;
    return;
  }

  // Update the memory slot
  snprintf(sys->memory[memIndex].name, sizeof(sys->memory[0].name), "Var_%d_%s", pid, varName);
  strncpy(sys->memory[memIndex].value, value, sizeof(sys->memory[0].value) - 1);
  sys->memory[memIndex].value[sizeof(sys->memory[0].value) - 1] = '\0'; // Ensure null termination

  // sim_log(sys, "P%d: Set variable '%s' = '%s' (at mem %d)", pcb->programNumber, varName, value, memIndex);
}

// Get variable value - declared in .h for potential GUI use
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

  int memIndex = findVariableMemoryIndex(sys, pid, varName, false); // Find existing variable only

  if (memIndex < 0)
  {
    // Check if it's the special "file_<filename>" variable from readFile
    // This check might be redundant if readFile always uses setVariable now.
    char fileVarPrefix[64];
    snprintf(fileVarPrefix, sizeof(fileVarPrefix), "file_%s", varName);
    memIndex = findVariableMemoryIndex(sys, pid, fileVarPrefix, false);

    if (memIndex < 0)
    {
      sim_log(sys, "Error in P%d: Variable '%s' not found.", pcb->programNumber, varName);
      pcb->state = TERMINATED;
      return NULL;
    }
    // If found as "file_<filename>", return its value
    return sys->memory[memIndex].value;
  }

  return sys->memory[memIndex].value;
}

// -------- Instruction Handlers --------

static void do_print(SystemState *sys, int pid, char *varName)
{
  char *value = getVariable(sys, pid, varName);
  if (value)
  { // getVariable handles termination on error
    sim_output(sys, pid, value);
  }
}

static void do_assign(SystemState *sys, int pid, char *varName, char *valueOrSource)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return; // Should not happen

  if (strcmp(valueOrSource, "input") == 0)
  {
    // Request input via callback
    if (sys->callbacks && sys->callbacks->request_input)
    {
      sim_log(sys, "P%d needs input for variable '%s'", pcb->programNumber, varName);
      sys->needsInput = true;
      strncpy(sys->inputVarName, varName, sizeof(sys->inputVarName) - 1);
      sys->inputVarName[sizeof(sys->inputVarName) - 1] = '\0';
      sys->inputPid = pid;
      // Execution will pause here for this process. stepSimulation will return.
      // GUI should call request_input, get value, then call provideInput.
      notify_state_update(sys); // Notify GUI it needs input
    }
    else
    {
      // No callback registered - cannot get input. Terminate process.
      sim_log(sys, "Error in P%d: 'assign input' used, but no input callback registered. Terminating.", pcb->programNumber);
      pcb->state = TERMINATED;
    }
  }
  // Handle `assign var readFile fileVar` case is now within interpretInstruction
  else
  {
    // Simple assignment: assign var value
    setVariable(sys, pid, varName, valueOrSource);
  }
}

// Called by GUI after obtaining input via request_input callback
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
    // Clear the flag anyway
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
    // Handle case where input was cancelled or failed (e.g., treat as empty string or error?)
    sim_log(sys, "P%d received NULL input for '%s'. Treating as empty string.", pcb->programNumber, sys->inputVarName);
    setVariable(sys, sys->inputPid, sys->inputVarName, "");
  }

  // Clear the input request flag
  sys->needsInput = false;
  sys->inputPid = -1;
  sys->inputVarName[0] = '\0';

  // If the process was waiting for input, it can now proceed.
  // The next call to stepSimulation will likely execute the next instruction.
  // We might need to potentially put it back in ready queue if context switched?
  // However, the current design keeps it RUNNING but stalled on needsInput.
  // Let's advance the PC here since the 'assign input' instruction is now complete.
  if (pcb->state == RUNNING)
  { // Double check it wasn't terminated by setVariable
    pcb->programCounter++;
    // Check for program completion immediately after input
    int instCount = findInstructionCount(sys, pcb->processID);
    if (pcb->programCounter >= instCount)
    {
      sim_log(sys, "P%d finished program after receiving input (PC=%d, InstCount=%d). Terminating.", pcb->programNumber, pcb->programCounter, instCount);
      pcb->state = TERMINATED;
    }
  }

  notify_state_update(sys); // State changed (variable set)
}

static void do_writeFile(SystemState *sys, int pid, char *fileVar, char *dataVar)
{
  PCB *pcb = findPCB(sys, pid);
  if (!pcb)
    return;

  char *filename = getVariable(sys, pid, fileVar);
  if (!filename)
  { /* Error logged by getVariable */
    return;
  }

  char *data = getVariable(sys, pid, dataVar);
  if (!data)
  { /* Error logged by getVariable */
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
  { /* Error logged by getVariable */
    return;
  }

  FILE *f = fopen(filename, "r");
  if (!f)
  {
    sim_log(sys, "Error in P%d: Cannot open file '%s' for reading: %s. Terminating.", pcb->programNumber, filename, strerror(errno));
    pcb->state = TERMINATED;
    return;
  }

  char content[500] = ""; // Buffer to hold file content
  char lineBuffer[MAX_LINE_LENGTH];
  size_t currentLen = 0;
  size_t maxLen = sizeof(content) - 1; // Leave space for null terminator

  while (fgets(lineBuffer, sizeof(lineBuffer), f))
  {
    size_t lineLen = strlen(lineBuffer);
    if (currentLen + lineLen > maxLen)
    {
      sim_log(sys, "Warning in P%d: File '%s' content truncated during read.", pcb->programNumber, filename);
      strncat(content, lineBuffer, maxLen - currentLen);
      currentLen = maxLen;
      break; // Stop reading
    }
    strcat(content, lineBuffer);
    currentLen += lineLen;
  }
  fclose(f);
  content[currentLen] = '\0'; // Ensure null termination

  // Store the content in a variable named "file_<originalVarName>"
  char resultVarName[100]; // Ensure large enough buffer
  snprintf(resultVarName, sizeof(resultVarName), "file_%s", fileVar);

  setVariable(sys, pid, resultVarName, content);
  // Log message happens in setVariable if successful, or error reported if fails
  // sim_log(sys, "P%d read from '%s' into variable '%s'", pid, filename, resultVarName);
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

  // Use strtol for safer conversion than atoi
  char *endptr1, *endptr2;
  long val1 = strtol(s1, &endptr1, 10);
  long val2 = strtol(s2, &endptr2, 10);

  // Basic check if conversion was successful (doesn't catch all errors)
  if (*endptr1 != '\0' || *endptr2 != '\0')
  {
    sim_log(sys, "Error in P%d: printFromTo requires numeric values for '%s' ('%s') and '%s' ('%s').", pcb->programNumber, v1, s1, v2, s2);
    findPCB(sys, pid)->state = TERMINATED;
    return;
  }

  // Build the output string
  char outputBuffer[512]; // Reasonably large buffer
  outputBuffer[0] = '\0';
  int remaining = sizeof(outputBuffer) - 1; // Space including null terminator

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

  // Remove trailing space if any
  size_t len = strlen(outputBuffer);
  if (len > 0 && outputBuffer[len - 1] == ' ')
  {
    outputBuffer[len - 1] = '\0';
  }

  sim_output(sys, pid, outputBuffer);
}

// -------- Semaphore / Mutex Operations --------

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
  // Add more resources here if needed
  return (ResourceType)-1; // Invalid resource name
}

// Enqueues PID into mutex blocked queue (simple FIFO for now)
static bool enqueueMutexBlocked(Mutex *m, int pid)
{
  if (m->size >= MAX_QUEUE_SIZE)
  {
    return false; // Queue full
  }
  m->blockedQueue[m->tail] = pid;
  m->tail = (m->tail + 1) % MAX_QUEUE_SIZE;
  m->size++;
  return true;
}

// Dequeues PID from mutex blocked queue based on priority
static int dequeueMutexBlocked(SystemState *sys, Mutex *m)
{
  if (m->size == 0)
  {
    return -1; // Queue empty
  }

  // Find the process with the highest priority (lowest priority value)
  int bestPid = -1;
  int bestPriority = 9999;   // Assume higher number means lower priority
  int bestIndexInQueue = -1; // Index within the blockedQueue array
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

  // If no suitable process found (shouldn't happen if size > 0 and PCBs exist)
  if (bestPid == -1)
  {
    sim_log(sys, "Error: Could not find highest priority process in mutex queue (size %d)", m->size);
    // As a fallback, dequeue the head (FIFO)
    bestPid = m->blockedQueue[m->head];
    m->head = (m->head + 1) % MAX_QUEUE_SIZE;
    m->size--;
    return bestPid;
  }

  // Remove the highest priority process from the queue
  // This involves shifting elements if the dequeued item is not the head.
  if (bestIndexInQueue == m->head)
  {
    // Simple case: removing the head
    m->head = (m->head + 1) % MAX_QUEUE_SIZE;
    m->size--; // Decrement size here
  }
  else
  {
    // More complex: remove element from middle/tail
    // Rebuild the queue internally excluding the bestPid (safer approach).
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
    m->size = 0; // Reset size before adding back
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
    // Ensure the currently running process is cleared if it's the one terminating
    if (sys->runningProcessID == pid)
    {
      sys->runningProcessID = -1;
    }
    return;
  }

  pcb->state = BLOCKED;
  pcb->blockedOnResource = r;

  // If the currently running process is the one being blocked, CPU becomes idle
  if (sys->runningProcessID == pid)
  {
    sys->runningProcessID = -1;
  }

  sim_log(sys, "P%d BLOCKED on resource %d", pcb->programNumber, r);
  notify_state_update(sys); // State changed
}

static void unblockProcess(SystemState *sys, ResourceType r)
{
  Mutex *m = &sys->mutexes[r];
  if (m->size == 0)
  {
    return; // No process waiting on this resource
  }

  // Dequeue the highest priority waiting process
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
  pcb->blockedOnResource = (ResourceType)-1; // Mark as not blocked

  // Mark this process as unblocked this cycle
  sys->wasUnblockedThisCycle[pidToUnblock] = true;

  // Add the unblocked process to the appropriate ready queue
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
    // Associate priority with the process *before* blocking (MLFQ level)
    pcb->priority = (sys->schedulerType == SIM_SCHED_MLFQ) ? pcb->mlfqLevel : 0;
    blockProcess(sys, pid, r);
    // blockProcess sets runningProcessID to -1 if needed and notifies GUI
  }
  else
  {
    m->locked = true;
    m->lockingProcessID = pid;
    sim_log(sys, "P%d acquired resource %d.", pcb->programNumber, r);
    // Process continues, PC will advance normally
    notify_state_update(sys); // State changed (mutex locked)
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
    // Now unblock the highest priority waiting process, if any
    unblockProcess(sys, r); // unblockProcess handles adding to ready queue & notify
  }
  else
  {
    // Trying to signal a resource not held or not locked
    sim_log(sys, "Error in P%d: Illegal semSignal on resource %d (Locked: %d, Holder: P%d). Terminating.",
            pcb->programNumber, r, m->locked, m->lockingProcessID);
    pcb->state = TERMINATED;
  }
  // No need to call notify_state_update here if unblockProcess already did.
  // But call it if no process was unblocked to reflect mutex state change.
  if (m->size == 0)
  { // If no process was unblocked
    notify_state_update(sys);
  }
}

// ------ Arrival Check ------

static void checkArrivals(SystemState *sys)
{
  // First, handle any processes that were unblocked this cycle
  for (int i = 0; i < sys->processCount; i++)
  {
    if (sys->wasUnblockedThisCycle[i])
    {
      PCB *pcb = &sys->processTable[i];
      if (pcb->state == READY)
      {
        // These processes are already in the ready queue, no need to add them again
        sys->wasUnblockedThisCycle[i] = false; // Reset the flag
      }
    }
  }

  // Then handle new arrivals
  for (int i = 0; i < sys->processCount; i++)
  {
    PCB *pcb = &sys->processTable[i];
    if (pcb->state == NEW && pcb->arrivalTime <= sys->clockCycle)
    {
      sim_log(sys, "Clock %d: P%d arrived.", sys->clockCycle, pcb->programNumber);
      pcb->state = READY;
      if (sys->schedulerType == SIM_SCHED_MLFQ)
      {
        addToMLFQ(sys, i, 0); // New processes start at highest priority level
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
  // If no processes have been loaded, the simulation is not complete.
  if (sys->processCount == 0)
  {
    return false;
  }

  // If already marked complete, return true
  if (sys->simulationComplete)
  {
    return true;
  }

  // Check if all loaded processes are terminated
  int completedOrTerminatedCount = 0;
  for (int i = 0; i < sys->processCount; ++i)
  {
    if (sys->processTable[i].state == TERMINATED)
    {
      completedOrTerminatedCount++;
    }
  }

  // Simulation is complete only if all loaded processes are terminated
  bool complete = (completedOrTerminatedCount == sys->processCount);
  if (complete)
  {
    sys->simulationComplete = true; // Mark as complete
  }
  return complete;
}

// Executes one logical step/cycle of the simulation
void stepSimulation(SystemState *sys)
{
  if (isSimulationComplete(sys))
  {
    sim_log(sys, "Simulation already complete.");
    return;
  }

  // Reset unblocked flags at the start of each cycle
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    sys->wasUnblockedThisCycle[i] = false;
  }

  // Check if waiting for input - if so, do nothing until input provided
  if (sys->needsInput)
  {
    sim_log(sys, "Simulation paused, waiting for input for P%d.", sys->inputPid);
    return;
  }

  sim_log(sys, "--- Clock Cycle %d ---", sys->clockCycle);

  // 1. Check for new arrivals and add them to ready queue(s)
  checkArrivals(sys);

  // 2. Check if the currently running process finished its quantum or execution
  bool needToSchedule = false;
  if (sys->runningProcessID >= 0)
  {
    PCB *runningPCB = findPCB(sys, sys->runningProcessID);
    if (!runningPCB || runningPCB->state != RUNNING)
    {
      // This case indicates an inconsistency, maybe the process got blocked/terminated externally?
      sim_log(sys, "Warning: Running PID %d is not in RUNNING state (%d). CPU becoming idle.", sys->runningProcessID, runningPCB ? runningPCB->state : -1);
      sys->runningProcessID = -1;
      needToSchedule = true;
    }
    else
    {
      // Quantum handling for RR and MLFQ
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
        // Demote process: move to next lower level, or stay at lowest if already there
        int nextLevel = (runningPCB->mlfqLevel < MLFQ_LEVELS - 1) ? runningPCB->mlfqLevel + 1 : runningPCB->mlfqLevel;
        sim_log(sys, "P%d demoted to level %d.", runningPCB->programNumber, nextLevel);
        addToMLFQ(sys, sys->runningProcessID, nextLevel);
        sys->runningProcessID = -1;
        needToSchedule = true;
        notify_state_update(sys);
      }
      // No else needed - if quantum not expired, process continues
    }
  }
  else
  {
    // CPU is idle, definitely need to schedule
    needToSchedule = true;
  }

  // 3. Schedule a new process if CPU is idle (or became idle)
  if (needToSchedule && sys->runningProcessID < 0)
  {
    int nextPid = scheduleNextProcess(sys);
    if (nextPid >= 0)
    {
      sys->runningProcessID = nextPid;
      PCB *newlyScheduledPCB = findPCB(sys, nextPid);
      if (newlyScheduledPCB)
      { // Should always be found
        newlyScheduledPCB->state = RUNNING;

        // Assign quantum based on scheduler type
        if (sys->schedulerType == SIM_SCHED_RR)
        {
          newlyScheduledPCB->quantumRemaining = sys->rrQuantum;
        }
        else if (sys->schedulerType == SIM_SCHED_MLFQ)
        {
          // Quantum based on the level it's scheduled from
          newlyScheduledPCB->quantumRemaining = sys->mlfqQuantum[newlyScheduledPCB->mlfqLevel];
          // Priority is already set by addToMLFQ
        }
        sim_log(sys, "Scheduler: Dispatching P%d (Level: %d, Quantum: %d)",
                newlyScheduledPCB->programNumber, newlyScheduledPCB->mlfqLevel, newlyScheduledPCB->quantumRemaining);
        notify_state_update(sys); // State changed
      }
      else
      {
        sim_log(sys, "Error: Scheduled PID %d not found!", nextPid);
        sys->runningProcessID = -1; // Go back to idle
      }
    }
    else
    {
      // No process ready to run
      sim_log(sys, "Scheduler: CPU Idle - No ready processes.");
      sys->runningProcessID = -1; // Ensure it remains -1
      notify_state_update(sys);   // State potentially changed if arrivals happened but no scheduling
    }
  }

  // 4. Execute one instruction for the running process
  if (sys->runningProcessID >= 0)
  {
    PCB *currentPCB = findPCB(sys, sys->runningProcessID);
    if (currentPCB && currentPCB->state == RUNNING && !sys->needsInput)
    { // Ensure it's still running and not waiting for input
      // Decrement quantum *before* executing instruction for RR/MLFQ
      if (sys->schedulerType == SIM_SCHED_RR || sys->schedulerType == SIM_SCHED_MLFQ)
      {
        currentPCB->quantumRemaining--;
        // Log remaining quantum? Maybe too verbose.
      }

      interpretInstruction(sys, sys->runningProcessID);

      // Post-instruction checks: Did it terminate or block?
      if (currentPCB->state == TERMINATED)
      {
        sim_log(sys, "P%d terminated during execution.", currentPCB->programNumber);
        // Check completion status after termination
        isSimulationComplete(sys);  // Update the flag
        sys->runningProcessID = -1; // CPU becomes idle
        notify_state_update(sys);
      }
      else if (currentPCB->state == BLOCKED)
      {
        // interpretInstruction already logged blocking and blockProcess set runningProcessID to -1
        // No further action needed here, blockProcess calls notify_state_update
      }
      // If still RUNNING, quantum check will happen at the start of the *next* cycle
    }
  }

  // 5. Increment clock cycle
  sys->clockCycle++;

  // 6. Final check for overall simulation completion
  if (isSimulationComplete(sys))
  {
    sim_log(sys, "Simulation Complete at Clock Cycle %d.", sys->clockCycle);
    notify_state_update(sys); // Notify GUI of final state
  }

  // Safety break (optional, remove if confident)
  // if (sys->clockCycle > 1000) {
  //     sim_log(sys, "Safety break triggered at cycle 1000.");
  //     sys->simulationComplete = true;
  //     notify_state_update(sys);
  //     return;
  // }
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