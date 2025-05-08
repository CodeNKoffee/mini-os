#include <gtk/gtk.h>
#include <gtk/gtknativedialog.h>
#include "simulator.h"
#include <glib.h>
#include <stdarg.h> // For va_list support

// --- GUI State Structure ---
typedef struct
{
  GtkApplication *app;
  GtkWidget *main_window;
  GtkWidget *log_view;
  GtkTextBuffer *log_buffer;
  GtkWidget *process_output_view;
  GtkTextBuffer *process_output_buffer;
  GtkWidget *step_button;
  GtkWidget *run_button;
  GtkWidget *reset_button;
  GtkDropDown *scheduler_dropdown;
  GtkStringList *scheduler_model;
  GtkWidget *rr_quantum_entry;
  GtkWidget *load_p1_button;
  GtkWidget *load_p2_button;
  GtkWidget *load_p3_button;
  GtkWidget *status_bar;               // To show current cycle, running process etc.
  GtkTextView *process_list_text_view; // NEW - for process/queue details
  GtkTextBuffer *process_list_buffer;  // NEW
  GtkTextView *memory_text_view;       // NEW - for memory map
  GtkTextBuffer *memory_buffer;        // NEW

  // Widgets for embedded input prompt
  GtkWidget *input_prompt_box; // Container for input widgets
  GtkWidget *input_prompt_label;
  GtkWidget *input_entry; // Entry for user input
  GtkWidget *submit_input_button;

  // Quick input field for direct program input
  GtkWidget *quick_input_entry;
  GtkWidget *quick_input_button;

  SystemState sim_state;  // Holds the entire simulator state
  GuiCallbacks callbacks; // Callbacks passed to the simulator

  guint run_timer_id; // Timer ID for continuous run
  bool is_running;    // Flag if simulation is auto-running

  int input_process_id;
  char *input_var_name;
  bool input_numeric;

  GtkWidget *add_process_button; // NEW - To manage sensitivity

} GuiApp; // Renamed from AppData for clarity

// --- Forward Declarations ---
static void gui_log_message(void *gui_data, const char *format, ...);
static void gui_log_message_wrapper(void *gui_data, const char *message);
static void gui_process_output(void *gui_data, int pid, const char *output);
static gboolean gui_request_input_internal(GuiApp *gui_app, int process_id, const char *var_name, gboolean numeric);
static void gui_request_input(void *gui_data, int pid, const char *varName);
static void gui_state_update(void *gui_data, SystemState *sys G_GNUC_UNUSED);
static void update_ui_from_state(GuiApp *gui_app);
static void on_step_button_clicked(GtkButton *button, gpointer user_data);
static void on_run_button_clicked(GtkButton *button, gpointer user_data);
static void on_reset_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data);
static void on_load_program_clicked(GtkButton *button, gpointer user_data);
static gboolean run_simulation_step(gpointer user_data);
static void stop_continuous_run(GuiApp *gui_app);
static void on_scheduler_changed(GtkDropDown *dropdown G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data);
static void on_submit_input_button_clicked(GtkButton *button, gpointer user_data);
static gboolean flash_input_area(GtkWidget *frame);
static gboolean unflash_input_area(GtkWidget *frame);
static void gui_add_status_message(GuiApp *gui_app, const char *message);
static void on_quick_input_button_clicked(GtkButton *button, gpointer user_data);
static char *format_process_list_and_queues(SystemState *sys);
static void on_add_process_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data);
// --- Start: New forward declarations for dialog response handlers ---
static void on_arrival_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data);
static void on_file_chooser_native_response(GtkNativeDialog *native, gint response_id, gpointer user_data);
// --- End: New forward declarations ---

// Macro for logging - defined after forward declarations
#define LOG(format, ...) gui_log_message(gui_app, format, ##__VA_ARGS__)

// --- Utility Functions ---

// Simple status message helper
static void gui_add_status_message(GuiApp *gui_app, const char *message)
{
  gui_log_message(gui_app, "%s", message);
  gtk_label_set_text(GTK_LABEL(gui_app->status_bar), message);
}

// --- Callback Implementations ---

// Simple wrapper for the simulator callback interface
static void gui_log_message_wrapper(void *gui_data, const char *message)
{
  // Just pass the message directly, no formatting
  gui_log_message(gui_data, "%s", message);
}

// Appends a message to the log text view with printf-style formatting
static void gui_log_message(void *gui_data, const char *format, ...)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  GtkTextIter end;

  // Format the message
  char buffer[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  // Append to text buffer
  gtk_text_buffer_get_end_iter(gui_app->log_buffer, &end);
  gtk_text_buffer_insert(gui_app->log_buffer, &end, buffer, -1);
  gtk_text_buffer_insert(gui_app->log_buffer, &end, "\n", -1);

  // Auto-scroll
  GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(gui_app->log_view));
  if (vadj)
  {
    gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
  }
}

// Appends process-specific output to its text view
static void gui_process_output(void *gui_data, int pid, const char *output)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  GtkTextIter end;
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "P%d: %s\n", pid, output);

  gtk_text_buffer_get_end_iter(gui_app->process_output_buffer, &end);
  gtk_text_buffer_insert(gui_app->process_output_buffer, &end, buffer, -1);

  // Auto-scroll
  GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(gui_app->process_output_view));
  if (vadj)
  {
    gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
  }
}

// Wrapper function for the simulator callback
static void gui_request_input(void *gui_data, int pid, const char *varName)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  // Assume non-numeric by default (can be adjusted based on variable name if needed)
  gui_request_input_internal(gui_app, pid, varName, FALSE);
}

// Shows the embedded input prompt area (internal implementation)
static gboolean gui_request_input_internal(GuiApp *gui_app, int process_id, const char *var_name, gboolean numeric)
{
  // Add LOG message for debugging
  LOG("GUI: Input requested for process %d, variable %s (numeric: %s)",
      process_id, var_name, numeric ? "yes" : "no");

  // Set input state
  gui_app->input_process_id = process_id;
  gui_app->input_var_name = g_strdup(var_name);
  gui_app->input_numeric = numeric;

  // Update prompt with specific message
  char *prompt_message;
  if (numeric)
  {
    prompt_message = g_strdup_printf("<b>Enter a NUMBER for process %d, variable %s:</b>", process_id, var_name);
  }
  else
  {
    prompt_message = g_strdup_printf("<b>Enter a STRING for process %d, variable %s:</b>", process_id, var_name);
  }
  gtk_label_set_markup(GTK_LABEL(gui_app->input_prompt_label), prompt_message);
  g_free(prompt_message);

  // Clear previous input
  gtk_editable_set_text(GTK_EDITABLE(gui_app->input_entry), "");

  // Clear the quick input field too and prepare it
  gtk_editable_set_text(GTK_EDITABLE(gui_app->quick_input_entry), "");
  gtk_widget_grab_focus(gui_app->quick_input_entry);

  // If auto-running, stop the timer
  if (gui_app->is_running && gui_app->run_timer_id > 0)
  {
    g_source_remove(gui_app->run_timer_id);
    gui_app->run_timer_id = 0;
    // Note: we keep is_running=true so we can resume after input
  }

  // Make the input prompt visible and grab focus to the entry
  GtkWidget *input_frame = gtk_widget_get_parent(gui_app->input_prompt_box);
  gtk_widget_set_visible(input_frame, TRUE);

  // Flash the input area a couple of times to draw attention
  for (int i = 0; i < 3; i++)
  {
    g_timeout_add(300 * i, (GSourceFunc)flash_input_area, input_frame);
    g_timeout_add(300 * i + 150, (GSourceFunc)unflash_input_area, input_frame);
  }

  // Add status message
  gui_add_status_message(gui_app, "Input required! Please check the input box above.");

  return TRUE;
}

// Helper functions for flashing the input area (RE-ADD definitions)
static gboolean flash_input_area(GtkWidget *frame)
{
  gtk_widget_add_css_class(frame, "input-flash");
  return G_SOURCE_REMOVE; // Timer should run only once
}

static gboolean unflash_input_area(GtkWidget *frame)
{
  gtk_widget_remove_css_class(frame, "input-flash");
  return G_SOURCE_REMOVE; // Timer should run only once
}

// Handler for the Submit Input button
static void on_submit_input_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  // Get text from entry
  const char *input_text = gtk_editable_get_text(GTK_EDITABLE(gui_app->input_entry));

  // Pass input to simulator (simulator handles NULL if needed)
  provideInput(&gui_app->sim_state, input_text);

  // Clear entry and hide input area
  gtk_editable_set_text(GTK_EDITABLE(gui_app->input_entry), "");

  // Hide the input frame, not just the box
  GtkWidget *input_frame = gtk_widget_get_parent(gui_app->input_prompt_box);
  gtk_widget_set_visible(input_frame, FALSE);

  // Update UI (re-enables controls, updates status)
  update_ui_from_state(gui_app);
}

// Called by the simulator when state changes significantly
static void gui_state_update(void *gui_data, SystemState *sys G_GNUC_UNUSED)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  // This function itself runs on the main GTK thread.
  // It can directly call UI update functions.
  update_ui_from_state(gui_app);
}

// --- UI Update Function ---

// Updates all relevant UI elements based on the current sim_state
static void update_ui_from_state(GuiApp *gui_app)
{
  SystemState *sys = &gui_app->sim_state;
  char status_text[256];
  const char *scheduler_name = "Unknown";

  // 1. Update Process List and Queue Views
  char *process_queue_info = format_process_list_and_queues(sys);
  if (gui_app->process_list_buffer)
  {
    gtk_text_buffer_set_text(gui_app->process_list_buffer, process_queue_info ? process_queue_info : "", -1);
  }

  // 2. Update Memory View
  char memory_map_text[MEMORY_SIZE * 150 + 1]; // Ensure enough space +1 for null terminator
  memory_map_text[0] = '\0';
  char line_buffer[150];
  size_t current_map_len = 0;

  for (int i = 0; i < MEMORY_SIZE; ++i)
  {
    char name_display[51];
    char value_display[51];
    strncpy(name_display, sys->memory[i].name, 50);
    name_display[50] = '\0';
    strncpy(value_display, sys->memory[i].value, 50);
    value_display[50] = '\0';

    int written = snprintf(line_buffer, sizeof(line_buffer), "W%02d: [%s] = [%s]\n",
                           i, name_display, value_display);
    if (current_map_len + written < sizeof(memory_map_text))
    {
      strcat(memory_map_text, line_buffer);
      current_map_len += written;
    }
    else
    {
      // Buffer full, append truncation message if possible
      const char *trunc_msg = "... (memory map truncated) ...\n";
      if (sizeof(memory_map_text) - current_map_len > strlen(trunc_msg))
      {
        strcat(memory_map_text, trunc_msg);
      }
      break;
    }
  }
  if (gui_app->memory_buffer)
  {
    gtk_text_buffer_set_text(gui_app->memory_buffer, memory_map_text, -1);
  }

  // 3. Update Status Bar
  switch (sys->schedulerType)
  {
  case SIM_SCHED_FCFS:
    scheduler_name = "FCFS";
    break;
  case SIM_SCHED_RR:
    scheduler_name = "Round Robin";
    break;
  case SIM_SCHED_MLFQ:
    scheduler_name = "MLFQ";
    break;
  }
  char running_process_str[50] = "CPU Idle";
  if (sys->runningProcessID != -1)
  {
    PCB *pcb = findPCB(sys, sys->runningProcessID);
    if (pcb)
    {
      snprintf(running_process_str, sizeof(running_process_str), "P%d Running", pcb->programNumber);
    }
    else
    {
      snprintf(running_process_str, sizeof(running_process_str), "P%d Invalid", sys->runningProcessID);
    }
  }

  if (sys->simulationComplete)
  {
    snprintf(status_text, sizeof(status_text), "Simulation Complete! Clock: %d | Scheduler: %s",
             sys->clockCycle, scheduler_name);
  }
  else if (sys->needsInput)
  {
    PCB *input_pcb = findPCB(sys, sys->inputPid);
    snprintf(status_text, sizeof(status_text), "Paused for Input (P%d for %s) | Clock: %d | Scheduler: %s",
             input_pcb ? input_pcb->programNumber : sys->inputPid, sys->inputVarName, sys->clockCycle, scheduler_name);
  }
  else
  {
    snprintf(status_text, sizeof(status_text), "%s | Clock: %d | Scheduler: %s",
             running_process_str, sys->clockCycle, scheduler_name);
  }
  if (gui_app->status_bar)
  {
    gtk_label_set_text(GTK_LABEL(gui_app->status_bar), status_text);
  }

  // 4. Update Button and Widget Sensitivity
  bool sim_can_progress = !sys->simulationComplete && !sys->needsInput;
  bool can_load_new_process = !gui_app->is_running && (sys->processCount < MAX_PROCESSES);

  if (gui_app->step_button)
    gtk_widget_set_sensitive(gui_app->step_button, sim_can_progress);
  if (gui_app->run_button)
  {
    gtk_widget_set_sensitive(gui_app->run_button, sim_can_progress);
    gtk_button_set_label(GTK_BUTTON(gui_app->run_button), gui_app->is_running ? "_Pause" : "_Run");
    // Add/remove CSS class for run/pause state visual feedback if desired
    if (gui_app->is_running)
    {
      gtk_widget_remove_css_class(gui_app->run_button, "run-button");
      gtk_widget_add_css_class(gui_app->run_button, "stop-button"); // Assumes a "stop-button" class is defined for pause
    }
    else
    {
      gtk_widget_remove_css_class(gui_app->run_button, "stop-button");
      gtk_widget_add_css_class(gui_app->run_button, "run-button");
    }
  }
  if (gui_app->reset_button)
    gtk_widget_set_sensitive(gui_app->reset_button, !gui_app->is_running); // Can reset if not auto-running
  if (gui_app->add_process_button)
    gtk_widget_set_sensitive(gui_app->add_process_button, can_load_new_process);

  // Scheduler and Quantum entry sensitivity
  // Only allow changing scheduler if no processes are loaded OR if not auto-running and simulation is not complete/paused
  bool can_change_scheduler = (sys->processCount == 0) || (!gui_app->is_running && !sys->simulationComplete && !sys->needsInput);
  if (gui_app->scheduler_dropdown)
    gtk_widget_set_sensitive(GTK_WIDGET(gui_app->scheduler_dropdown), can_change_scheduler);
  if (gui_app->rr_quantum_entry)
    gtk_widget_set_sensitive(gui_app->rr_quantum_entry, can_change_scheduler && (sys->schedulerType == SIM_SCHED_RR));

  // Sensitivity of quick input button (distinct from the main input prompt)
  if (gui_app->quick_input_button)
    gtk_widget_set_sensitive(gui_app->quick_input_button, sys->needsInput);
  if (gui_app->quick_input_entry)
    gtk_widget_set_sensitive(gui_app->quick_input_entry, sys->needsInput);
  if (sys->needsInput && gui_app->quick_input_entry)
  {
    gtk_widget_add_css_class(gui_app->quick_input_entry, "input-flash");
  }
  else if (gui_app->quick_input_entry)
  {
    gtk_widget_remove_css_class(gui_app->quick_input_entry, "input-flash");
  }
}

// --- Button Handlers ---

static void on_step_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  if (!gui_app->is_running && !isSimulationComplete(&gui_app->sim_state) && !gui_app->sim_state.needsInput)
  {
    stepSimulation(&gui_app->sim_state);
    // state update is triggered via callback
  }
}

static void on_run_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  if (gui_app->is_running)
  {
    // Pause
    stop_continuous_run(gui_app);
  }
  else
  {
    // Start running
    if (!isSimulationComplete(&gui_app->sim_state) && !gui_app->sim_state.needsInput)
    {
      gui_app->is_running = true;
      update_ui_from_state(gui_app); // Update button label to Pause
      // Add a timer to call stepSimulation periodically
      gui_app->run_timer_id = g_timeout_add(100, run_simulation_step, gui_app); // 100ms interval
    }
  }
  // update_ui_from_state(gui_app); // Update sensitivity etc.
}

static void stop_continuous_run(GuiApp *gui_app)
{
  if (gui_app->is_running)
  {
    if (gui_app->run_timer_id > 0)
    {
      g_source_remove(gui_app->run_timer_id);
      gui_app->run_timer_id = 0;
    }
    gui_app->is_running = false;
    update_ui_from_state(gui_app); // Update button label and sensitivity
  }
}

// Timer callback for continuous run
static gboolean run_simulation_step(gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  if (!gui_app->is_running)
    return G_SOURCE_REMOVE; // Stop if paused externally

  if (isSimulationComplete(&gui_app->sim_state) || gui_app->sim_state.needsInput)
  {
    // Stop running if complete or needs input
    stop_continuous_run(gui_app);
    return G_SOURCE_REMOVE; // Remove the timer
  }

  stepSimulation(&gui_app->sim_state);
  // gui_state_update callback handles UI refresh

  return G_SOURCE_CONTINUE; // Keep timer running
}

static void on_reset_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  stop_continuous_run(gui_app);

  // Get selected scheduler and quantum
  SchedulerType type = SIM_SCHED_FCFS; // Default
  guint selected_index = gtk_drop_down_get_selected(gui_app->scheduler_dropdown);
  if (selected_index == 1)
    type = SIM_SCHED_RR;
  else if (selected_index == 2)
    type = SIM_SCHED_MLFQ;

  int rr_quantum = 1;
  if (type == SIM_SCHED_RR)
  {
    rr_quantum = atoi(gtk_editable_get_text(GTK_EDITABLE(gui_app->rr_quantum_entry)));
    if (rr_quantum < 1)
      rr_quantum = 1;
  }

  // Re-initialize the simulator state
  initializeSystem(&gui_app->sim_state, type, rr_quantum, &gui_app->callbacks, gui_app);

  // Clear log views
  gtk_text_buffer_set_text(gui_app->log_buffer, "", -1);
  gtk_text_buffer_set_text(gui_app->process_output_buffer, "", -1);

  gui_log_message(gui_app, "System Reset.");

  // Update UI (buttons will be enabled/disabled appropriately)
  update_ui_from_state(gui_app);
}

static void on_load_program_clicked(GtkButton *button, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  const char *label = gtk_button_get_label(button);
  const char *filename = NULL;
  int arrival = 0;

  if (strstr(label, "P1"))
  {
    filename = "Program_1.txt";
    arrival = 0;
  }
  else if (strstr(label, "P2"))
  {
    filename = "Program_2.txt";
    arrival = 1;
  }
  else if (strstr(label, "P3"))
  {
    filename = "Program_3.txt";
    arrival = 2;
  }

  if (filename)
  {
    gui_log_message(gui_app, "Attempting to load program %s with arrival time %d...", filename, arrival);
    bool success = loadProgram(&gui_app->sim_state, filename, arrival);

    if (!success)
    {
      gui_log_message(gui_app, "Load failed for %s.", filename);
    }
    update_ui_from_state(gui_app);
  }
}

// Callback for when the scheduler dropdown selection changes
static void on_scheduler_changed(GtkDropDown *dropdown G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  update_ui_from_state(gui_app); // Update sensitivity of quantum entry
}

// Handler for the Quick Input button
static void on_quick_input_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  // Get text from entry
  const char *input_text = gtk_editable_get_text(GTK_EDITABLE(gui_app->quick_input_entry));

  // Only provide input if the simulation is waiting for input
  if (gui_app->sim_state.needsInput)
  {
    // Pass input to simulator
    provideInput(&gui_app->sim_state, input_text);

    // Log the input action
    gui_log_message(gui_app, "Input provided: %s", input_text);

    // If auto-running was interrupted by input request, resume it
    if (gui_app->is_running && gui_app->run_timer_id == 0)
    {
      gui_app->run_timer_id = g_timeout_add(100, run_simulation_step, gui_app);
    }

    // Update UI
    update_ui_from_state(gui_app);

    // Hide the input frame since we've provided input
    GtkWidget *input_frame = gtk_widget_get_parent(gui_app->input_prompt_box);
    gtk_widget_set_visible(input_frame, FALSE);
  }
  else
  {
    // Inform the user that input is not currently needed
    gui_log_message(gui_app, "Input not required at this time");
  }

  // Clear the quick input field
  gtk_editable_set_text(GTK_EDITABLE(gui_app->quick_input_entry), "");
}

// Add this function before update_ui_from_state
static char *format_process_list_and_queues(SystemState *sys)
{
  static char buffer[4096]; // Static buffer to hold the formatted text
  char temp[512];           // Temporary buffer for individual process info
  int offset = 0;

  // Process List Header
  offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                     "=== Process List ===\n"
                     "PID\tState\t\tPriority\tMemory\t\tPC\n"
                     "----------------------------------------\n");

  // List all processes
  for (int i = 0; i < sys->processCount; i++)
  {
    PCB *pcb = &sys->processTable[i];
    const char *state_str;
    switch (pcb->state)
    {
    case NEW:
      state_str = "NEW";
      break;
    case READY:
      state_str = "READY";
      break;
    case RUNNING:
      state_str = "RUNNING";
      break;
    case BLOCKED:
      state_str = "BLOCKED";
      break;
    case TERMINATED:
      state_str = "TERMINATED";
      break;
    default:
      state_str = "UNKNOWN";
    }

    snprintf(temp, sizeof(temp), "P%d\t%s\t\t%d\t\t[%d-%d]\t%d\n",
             pcb->programNumber,
             state_str,
             pcb->priority,
             pcb->memoryLowerBound,
             pcb->memoryUpperBound,
             pcb->programCounter);
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s", temp);
  }

  // Queue Section Header
  offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                     "\n=== Queue Section ===\n");

  // Running Process
  if (sys->runningProcessID >= 0)
  {
    PCB *pcb = findPCB(sys, sys->runningProcessID);
    if (pcb)
    {
      char *current_inst = "";
      if (pcb->programCounter < findInstructionCount(sys, pcb->processID))
      {
        current_inst = sys->memory[pcb->memoryLowerBound + pcb->programCounter].value;
      }
      offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                         "Running: P%d\n"
                         "Current Instruction: %s\n"
                         "Time in CPU: %d cycles\n",
                         pcb->programNumber,
                         current_inst,
                         sys->clockCycle - pcb->arrivalTime);
    }
  }

  // Ready Queue
  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nReady Queue:\n");
  if (sys->schedulerType == SIM_SCHED_MLFQ)
  {
    // MLFQ Ready Queues
    for (int level = 0; level < MLFQ_LEVELS; level++)
    {
      if (sys->mlfqSize[level] > 0)
      {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "Level %d: ", level);
        int idx = sys->mlfqHead[level];
        for (int i = 0; i < sys->mlfqSize[level]; i++)
        {
          int pid = sys->mlfqRQ[level][idx];
          PCB *pcb = findPCB(sys, pid);
          if (pcb)
          {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "P%d ", pcb->programNumber);
          }
          idx = (idx + 1) % MAX_QUEUE_SIZE;
        }
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");
      }
    }
  }
  else
  {
    // FCFS/RR Ready Queue
    if (sys->readySize > 0)
    {
      int idx = sys->readyHead;
      for (int i = 0; i < sys->readySize; i++)
      {
        int pid = sys->readyQueue[idx];
        PCB *pcb = findPCB(sys, pid);
        if (pcb)
        {
          offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                             "P%d ", pcb->programNumber);
        }
        idx = (idx + 1) % MAX_QUEUE_SIZE;
      }
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");
    }
  }

  // Mutex Holder Info - ADDED SECTION
  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== Mutex Status ===\n");
  for (int r = 0; r < NUM_RESOURCES; r++)
  {
    const char *res_name_held;
    switch (r)
    {
    case RESOURCE_FILE:
      res_name_held = "File";
      break;
    case RESOURCE_USER_INPUT:
      res_name_held = "User Input";
      break;
    case RESOURCE_USER_OUTPUT:
      res_name_held = "User Output";
      break;
    default:
      res_name_held = "Unknown";
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s: ", res_name_held);
    if (sys->mutexes[r].locked)
    {
      PCB *holder_pcb = findPCB(sys, sys->mutexes[r].lockingProcessID);
      if (holder_pcb)
      {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "Held by P%d. Waiting: ", holder_pcb->programNumber);
      }
      else
      {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "Held by P%d (Error: PCB not found). Waiting: ", sys->mutexes[r].lockingProcessID);
      }
    }
    else
    {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "(Not locked). Waiting: ");
    }
    // List waiting processes for this mutex
    if (sys->mutexes[r].size > 0)
    {
      int idx_wait = sys->mutexes[r].head;
      for (int i_wait = 0; i_wait < sys->mutexes[r].size; i_wait++)
      {
        int pid_wait = sys->mutexes[r].blockedQueue[idx_wait];
        PCB *pcb_wait = findPCB(sys, pid_wait);
        if (pcb_wait)
        {
          offset += snprintf(buffer + offset, sizeof(buffer) - offset, "P%d ", pcb_wait->programNumber);
        }
        idx_wait = (idx_wait + 1) % MAX_QUEUE_SIZE;
      }
    }
    else
    {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "(none)");
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");
  }
  // END OF ADDED SECTION - Blocked Processes list can be removed if this is sufficient

  return buffer;
}

// Helper struct to pass filepath and gui_app to arrival_dialog_response
typedef struct
{
  GuiApp *app_data;
  char *filepath;
} ArrivalDialogLoadData;

// Response handler for the arrival time dialog
static void on_arrival_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
  ArrivalDialogLoadData *load_data = (ArrivalDialogLoadData *)user_data;
  GuiApp *gui_app = load_data->app_data;

  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    GtkWidget *content_area = gtk_dialog_get_content_area(dialog); // Deprecated but part of original structure
    GtkWidget *arrival_hbox = gtk_widget_get_first_child(content_area);
    GtkWidget *arrival_spin_button = gtk_widget_get_last_child(arrival_hbox);

    if (GTK_IS_SPIN_BUTTON(arrival_spin_button))
    {
      int arrival_time = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(arrival_spin_button));
      gui_log_message(gui_app, "Loading program '%s' with arrival time %d...", load_data->filepath, arrival_time);
      bool success = loadProgram(&gui_app->sim_state, load_data->filepath, arrival_time);
      if (!success)
      {
        gui_log_message(gui_app, "Load failed for %s.", load_data->filepath);
      }
      update_ui_from_state(gui_app);
    }
  }
  g_free(load_data->filepath); // Free the duplicated filepath
  g_slice_free(ArrivalDialogLoadData, load_data);
  gtk_window_destroy(GTK_WINDOW(dialog));
}

// Response handler for GtkFileChooserNative
static void on_file_chooser_native_response(GtkNativeDialog *native, gint response_id, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  if (response_id == GTK_RESPONSE_ACCEPT)
  {
    GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
    char *filepath = g_file_get_path(file);
    g_object_unref(file);

    // Now prompt for arrival time
    GtkWidget *arrival_dialog = gtk_dialog_new_with_buttons(
        "Set Arrival Time",
        GTK_WINDOW(gui_app->main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_ACCEPT,
        "_Cancel", GTK_RESPONSE_REJECT,
        NULL);
    // gtk_dialog_get_content_area is deprecated, but part of original structure of arrival dialog
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(arrival_dialog));
    GtkWidget *arrival_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *arrival_label = gtk_label_new("Arrival Time (cycles):");
    GtkAdjustment *adjustment = gtk_adjustment_new(0.0, 0.0, 1000.0, 1.0, 5.0, 0.0);
    GtkWidget *arrival_spin_button = gtk_spin_button_new(adjustment, 1.0, 0);

    gtk_box_append(GTK_BOX(arrival_hbox), arrival_label);
    gtk_box_append(GTK_BOX(arrival_hbox), arrival_spin_button);
    gtk_widget_set_halign(arrival_hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(arrival_hbox, 10);
    gtk_widget_set_margin_bottom(arrival_hbox, 10);
    gtk_widget_set_margin_start(arrival_hbox, 10);
    gtk_widget_set_margin_end(arrival_hbox, 10);
    gtk_box_append(GTK_BOX(content_area), arrival_hbox);
    // gtk_widget_show_all(arrival_dialog); // gtk_widget_show should be enough for a dialog
    gtk_widget_set_visible(arrival_dialog, TRUE);

    ArrivalDialogLoadData *load_data = g_slice_new(ArrivalDialogLoadData);
    load_data->app_data = gui_app;
    load_data->filepath = filepath; // Ownership of filepath passes to load_data, will be freed in on_arrival_dialog_response

    g_signal_connect(arrival_dialog, "response", G_CALLBACK(on_arrival_dialog_response), load_data);
    gtk_window_set_modal(GTK_WINDOW(arrival_dialog), TRUE);
    gtk_widget_show(GTK_WIDGET(arrival_dialog));
  }
  else
  {
    // User cancelled file chooser - no action needed, dialog closes itself
  }
  // gtk_native_dialog_destroy(native); // Native dialogs manage their own lifecycle after present
}

// Modified handler for "Add Process File..." button
static void on_add_process_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  GtkFileChooserNative *native = gtk_file_chooser_native_new(
      "Open Program File",
      GTK_WINDOW(gui_app->main_window),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Open",    // accept_label
      "_Cancel"); // cancel_label

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Text files (.txt)");
  gtk_file_filter_add_pattern(filter, "*.txt");
  // gtk_file_chooser_add_filter is for GtkFileChooser, for GtkFileChooserNative, use set_filter
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(native), filter);
  g_object_unref(filter); // filter is owned by the chooser now, unref our initial hold

  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(native), TRUE);
  g_signal_connect(native, "response", G_CALLBACK(on_file_chooser_native_response), gui_app);
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
  g_object_unref(native); // Release our reference, the dialog manages its own lifecycle
}

// --- Application Activation (UI Setup) ---

static void activate(GtkApplication *app, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  // Main Window
  gui_app->main_window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(gui_app->main_window), "Mini OS Simulator");
  gtk_window_set_default_size(GTK_WINDOW(gui_app->main_window), 800, 700);

  // Main Box (Vertical)
  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(gui_app->main_window), main_vbox);

  // --- Control Panel (Horizontal Box) ---
  GtkWidget *control_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_start(control_hbox, 5);
  gtk_widget_set_margin_end(control_hbox, 5);
  gtk_widget_set_margin_top(control_hbox, 5);
  gtk_box_append(GTK_BOX(main_vbox), control_hbox);

  // Scheduler Selection (Using GtkDropDown)
  const char *schedulers[] = {"FCFS", "Round Robin", "MLFQ", NULL};
  gui_app->scheduler_model = gtk_string_list_new(schedulers);
  gui_app->scheduler_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(gui_app->scheduler_model), NULL));
  gtk_drop_down_set_selected(gui_app->scheduler_dropdown, 0); // Default FCFS
  gtk_box_append(GTK_BOX(control_hbox), GTK_WIDGET(gui_app->scheduler_dropdown));
  g_signal_connect(gui_app->scheduler_dropdown, "notify::selected", G_CALLBACK(on_scheduler_changed), gui_app);

  // RR Quantum Entry
  GtkWidget *rr_label = gtk_label_new(" RR Q:");
  gui_app->rr_quantum_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(gui_app->rr_quantum_entry), "2");
  gtk_box_append(GTK_BOX(control_hbox), rr_label);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->rr_quantum_entry);

  // --- Start: Comment out old Load P1/P2/P3 buttons ---
  /*
  gui_app->load_p1_button = gtk_button_new_with_label("Load P1");
  gui_app->load_p2_button = gtk_button_new_with_label("Load P2");
  gui_app->load_p3_button = gtk_button_new_with_label("Load P3");
  g_signal_connect(gui_app->load_p1_button, "clicked", G_CALLBACK(on_load_program_clicked), gui_app);
  g_signal_connect(gui_app->load_p2_button, "clicked", G_CALLBACK(on_load_program_clicked), gui_app);
  g_signal_connect(gui_app->load_p3_button, "clicked", G_CALLBACK(on_load_program_clicked), gui_app);
  gtk_box_append(GTK_BOX(control_hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  gtk_box_append(GTK_BOX(control_hbox), gui_app->load_p1_button);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->load_p2_button);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->load_p3_button);
  */
  // --- End: Comment out old Load P1/P2/P3 buttons ---

  // --- Start: Add new "Add Process File..." button ---
  gui_app->add_process_button = gtk_button_new_with_label("Add Process File...");
  g_signal_connect(gui_app->add_process_button, "clicked", G_CALLBACK(on_add_process_clicked), gui_app);
  gtk_box_append(GTK_BOX(control_hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  gtk_box_append(GTK_BOX(control_hbox), gui_app->add_process_button);
  // --- End: Add new "Add Process File..." button ---

  // Simulation Control Buttons
  gui_app->step_button = gtk_button_new_with_label("Step");
  gui_app->run_button = gtk_button_new_with_label("Run");
  gui_app->reset_button = gtk_button_new_with_label("Reset");
  g_signal_connect(gui_app->step_button, "clicked", G_CALLBACK(on_step_button_clicked), gui_app);
  g_signal_connect(gui_app->run_button, "clicked", G_CALLBACK(on_run_button_clicked), gui_app);
  g_signal_connect(gui_app->reset_button, "clicked", G_CALLBACK(on_reset_button_clicked), gui_app);

  gtk_box_append(GTK_BOX(control_hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  gtk_box_append(GTK_BOX(control_hbox), gui_app->step_button);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->run_button);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->reset_button);

  // --- Quick Input Box ---
  GtkWidget *quick_input_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_start(quick_input_hbox, 5);
  gtk_widget_set_margin_end(quick_input_hbox, 5);
  gtk_widget_set_margin_top(quick_input_hbox, 5);
  gtk_widget_set_margin_bottom(quick_input_hbox, 5);
  gtk_box_append(GTK_BOX(main_vbox), quick_input_hbox);

  GtkWidget *quick_input_label = gtk_label_new("Input Value:");
  gui_app->quick_input_entry = gtk_entry_new();
  gtk_widget_set_hexpand(gui_app->quick_input_entry, TRUE);
  gui_app->quick_input_button = gtk_button_new_with_label("Submit");
  gtk_widget_add_css_class(gui_app->quick_input_button, "suggested-action");

  g_signal_connect(gui_app->quick_input_button, "clicked", G_CALLBACK(on_quick_input_button_clicked), gui_app);
  g_signal_connect(gui_app->quick_input_entry, "activate", G_CALLBACK(on_quick_input_button_clicked), gui_app); // Allow Enter key

  gtk_box_append(GTK_BOX(quick_input_hbox), quick_input_label);
  gtk_box_append(GTK_BOX(quick_input_hbox), gui_app->quick_input_entry);
  gtk_box_append(GTK_BOX(quick_input_hbox), gui_app->quick_input_button);

  // --- Input Prompt Area (Initially Hidden) ---
  gui_app->input_prompt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_start(gui_app->input_prompt_box, 10);
  gtk_widget_set_margin_end(gui_app->input_prompt_box, 10);
  gtk_widget_set_margin_top(gui_app->input_prompt_box, 10);
  gtk_widget_set_margin_bottom(gui_app->input_prompt_box, 10);

  // Create a frame around the input prompt for better visibility
  GtkWidget *input_frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(input_frame, "input-frame");
  gtk_frame_set_child(GTK_FRAME(input_frame), gui_app->input_prompt_box);

  // Input prompt components with better styling
  gui_app->input_prompt_label = gtk_label_new("Input Required:");
  gtk_widget_set_margin_start(gui_app->input_prompt_label, 5);
  gtk_widget_set_margin_end(gui_app->input_prompt_label, 5);

  gui_app->input_entry = gtk_entry_new();
  gtk_widget_set_hexpand(gui_app->input_entry, TRUE);
  gtk_widget_set_size_request(gui_app->input_entry, 200, 30); // Make entry bigger

  gui_app->submit_input_button = gtk_button_new_with_label("Submit Input");
  gtk_widget_add_css_class(gui_app->submit_input_button, "suggested-action"); // Gives emphasis styling

  g_signal_connect(gui_app->submit_input_button, "clicked", G_CALLBACK(on_submit_input_button_clicked), gui_app);
  g_signal_connect(gui_app->input_entry, "activate", G_CALLBACK(on_submit_input_button_clicked), gui_app); // Allow Enter key

  gtk_box_append(GTK_BOX(gui_app->input_prompt_box), gui_app->input_prompt_label);
  gtk_box_append(GTK_BOX(gui_app->input_prompt_box), gui_app->input_entry);
  gtk_box_append(GTK_BOX(gui_app->input_prompt_box), gui_app->submit_input_button);
  gtk_widget_set_visible(input_frame, FALSE); // Hide initially
  gtk_box_append(GTK_BOX(main_vbox), input_frame);

  // --- Paned View for State and Logs ---
  GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand(hpaned, TRUE);
  gtk_box_append(GTK_BOX(main_vbox), hpaned);

  // Left Pane (State Views - Needs more work)
  GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_start_child(GTK_PANED(hpaned), left_vbox);
  gtk_paned_set_resize_start_child(GTK_PANED(hpaned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(hpaned), FALSE);

  // Process List/Queue View (using GtkTextView)
  GtkWidget *process_list_frame = gtk_frame_new("Process & Queue Status");
  GtkWidget *process_list_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(process_list_scrolled, TRUE); // Allow vertical expansion
  gui_app->process_list_text_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gui_app->process_list_buffer = gtk_text_view_get_buffer(gui_app->process_list_text_view);
  gtk_text_view_set_editable(gui_app->process_list_text_view, FALSE);
  gtk_text_view_set_cursor_visible(gui_app->process_list_text_view, FALSE);
  gtk_text_view_set_wrap_mode(gui_app->process_list_text_view, GTK_WRAP_WORD_CHAR); // Wrap long lines
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(process_list_scrolled), GTK_WIDGET(gui_app->process_list_text_view));
  gtk_frame_set_child(GTK_FRAME(process_list_frame), process_list_scrolled);
  gtk_box_append(GTK_BOX(left_vbox), process_list_frame);

  // Memory View (using GtkTextView)
  GtkWidget *memory_view_frame = gtk_frame_new("Memory Map (60 Words)");
  GtkWidget *memory_view_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(memory_view_scrolled, TRUE); // Allow vertical expansion
  gui_app->memory_text_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gui_app->memory_buffer = gtk_text_view_get_buffer(gui_app->memory_text_view);
  gtk_text_view_set_editable(gui_app->memory_text_view, FALSE);
  gtk_text_view_set_cursor_visible(gui_app->memory_text_view, FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(gui_app->memory_text_view), "monospace-memory-view"); // Add CSS class
  gtk_text_view_set_wrap_mode(gui_app->memory_text_view, GTK_WRAP_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(memory_view_scrolled), GTK_WIDGET(gui_app->memory_text_view));
  gtk_frame_set_child(GTK_FRAME(memory_view_frame), memory_view_scrolled);
  gtk_box_append(GTK_BOX(left_vbox), memory_view_frame);

  // Right Pane (Logs and Output - Vertical Paned)
  GtkWidget *right_vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_end_child(GTK_PANED(hpaned), right_vpaned);
  gtk_paned_set_resize_end_child(GTK_PANED(hpaned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(hpaned), FALSE);

  // Log View (Top of Right Pane)
  GtkWidget *log_frame = gtk_frame_new("Simulation Log");
  GtkWidget *log_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(log_scrolled, TRUE);
  gtk_widget_set_vexpand(log_scrolled, TRUE);
  gui_app->log_view = gtk_text_view_new();
  gui_app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui_app->log_view));
  gtk_text_view_set_editable(GTK_TEXT_VIEW(gui_app->log_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(gui_app->log_view), FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scrolled), gui_app->log_view);
  gtk_frame_set_child(GTK_FRAME(log_frame), log_scrolled);
  gtk_paned_set_start_child(GTK_PANED(right_vpaned), log_frame);
  gtk_paned_set_resize_start_child(GTK_PANED(right_vpaned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(right_vpaned), FALSE);

  // Process Output View (Bottom of Right Pane)
  GtkWidget *output_frame = gtk_frame_new("Process Output (print command)");
  GtkWidget *output_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(output_scrolled, TRUE);
  gtk_widget_set_vexpand(output_scrolled, TRUE);
  gui_app->process_output_view = gtk_text_view_new();
  gui_app->process_output_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui_app->process_output_view));
  gtk_text_view_set_editable(GTK_TEXT_VIEW(gui_app->process_output_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(gui_app->process_output_view), FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(output_scrolled), gui_app->process_output_view);
  gtk_frame_set_child(GTK_FRAME(output_frame), output_scrolled);
  gtk_paned_set_end_child(GTK_PANED(right_vpaned), output_frame);
  gtk_paned_set_resize_end_child(GTK_PANED(right_vpaned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(right_vpaned), FALSE);

  // --- Status Bar ---
  gui_app->status_bar = gtk_label_new("Status: Initializing...");
  gtk_widget_set_halign(gui_app->status_bar, GTK_ALIGN_START);
  gtk_widget_set_margin_start(gui_app->status_bar, 5);
  gtk_widget_set_margin_end(gui_app->status_bar, 5);
  gtk_widget_set_margin_bottom(gui_app->status_bar, 5);
  gtk_box_append(GTK_BOX(main_vbox), gui_app->status_bar);

  // Initial Reset to setup simulator state
  on_reset_button_clicked(NULL, gui_app);

  // Create and set CSS provider
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider,
                                    "button.run-button { background: #4CAF50; color: white; font-weight: bold; }\n"
                                    "button.run-button:hover { background: #45a049; }\n"
                                    "button.stop-button { background: #f44336; color: white; font-weight: bold; }\n"
                                    "button.stop-button:hover { background: #d32f2f; }\n"
                                    "button.step-button { background: #2196F3; color: white; font-weight: bold; }\n"
                                    "button.step-button:hover { background: #0b7dda; }\n"
                                    "button.submit-button { background: #ff9800; color: white; font-weight: bold; padding: 5px 10px; }\n"
                                    "button.submit-button:hover { background: #e68a00; }\n"
                                    ".input-frame { border: 2px solid #ff9800; background-color: #fff3e0; border-radius: 5px; padding: 10px; margin: 10px; }\n"
                                    ".input-flash { border: 2px solid #f44336; background-color: #ffebee; border-radius: 3px; }\n"
                                    "entry.input-flash { color: #d32f2f; font-weight: bold; }\n"
                                    ".monospace-memory-view { font-family: Monospace; font-size: small; }\n");
  gtk_style_context_add_provider_for_display(
      gtk_widget_get_display(gui_app->main_window),
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  gtk_widget_set_visible(gui_app->main_window, TRUE);
}

// --- Main Function ---

int main(int argc, char **argv)
{
  GuiApp gui_app;
  int status;

  // Initialize GUI App structure
  memset(&gui_app, 0, sizeof(GuiApp));
  gui_app.is_running = false;
  gui_app.run_timer_id = 0;

  // Setup simulator callbacks
  gui_app.callbacks.log_message = gui_log_message_wrapper;
  gui_app.callbacks.process_output = gui_process_output;
  gui_app.callbacks.request_input = gui_request_input;
  gui_app.callbacks.state_update = gui_state_update;

  // Create GTK application
  gui_app.app = gtk_application_new("com.example.minisimulator", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gui_app.app, "activate", G_CALLBACK(activate), &gui_app);

  // Run the application
  status = g_application_run(G_APPLICATION(gui_app.app), argc, argv);

  // Clean up
  g_object_unref(gui_app.app);
  if (gui_app.scheduler_model)
  {
    g_object_unref(gui_app.scheduler_model); // Free the string list model
  }

  return status;
}