#include <gtk/gtk.h>
#include "simulator.h"
#include <glib.h>
#include <stdarg.h>

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
  GtkWidget *status_bar;
  GtkWidget *process_list_view;
  GtkWidget *memory_view;

  GtkWidget *input_prompt_box;
  GtkWidget *input_prompt_label;
  GtkWidget *input_entry;
  GtkWidget *submit_input_button;

  GtkWidget *quick_input_entry;
  GtkWidget *quick_input_button;

  SystemState sim_state;
  GuiCallbacks callbacks;

  guint run_timer_id;
  bool is_running;

  int input_process_id;
  char *input_var_name;
  bool input_numeric;

  GtkTextView *process_list_text_view;
  GtkTextBuffer *process_list_buffer;
  GtkTextView *memory_text_view;
  GtkTextBuffer *memory_buffer;
} GuiApp;

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

#define LOG(format, ...) gui_log_message(gui_app, format, ##__VA_ARGS__)

static void gui_add_status_message(GuiApp *gui_app, const char *message)
{
  gui_log_message(gui_app, "%s", message);
  gtk_label_set_text(GTK_LABEL(gui_app->status_bar), message);
}

static void gui_log_message_wrapper(void *gui_data, const char *message)
{
  gui_log_message(gui_data, "%s", message);
}

static void gui_log_message(void *gui_data, const char *format, ...)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  GtkTextIter end;

  char buffer[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  gtk_text_buffer_get_end_iter(gui_app->log_buffer, &end);
  gtk_text_buffer_insert(gui_app->log_buffer, &end, buffer, -1);
  gtk_text_buffer_insert(gui_app->log_buffer, &end, "\n", -1);

  GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(gui_app->log_view));
  if (vadj)
  {
    gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
  }
}

static void gui_process_output(void *gui_data, int pid, const char *output)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  GtkTextIter end;
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "P%d: %s\n", pid, output);

  gtk_text_buffer_get_end_iter(gui_app->process_output_buffer, &end);
  gtk_text_buffer_insert(gui_app->process_output_buffer, &end, buffer, -1);

  GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(gui_app->process_output_view));
  if (vadj)
  {
    gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
  }
}

static void gui_request_input(void *gui_data, int pid, const char *varName)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  gui_request_input_internal(gui_app, pid, varName, FALSE);
}

static gboolean gui_request_input_internal(GuiApp *gui_app, int process_id, const char *var_name, gboolean numeric)
{
  LOG("GUI: Input requested for process %d, variable %s (numeric: %s)",
      process_id, var_name, numeric ? "yes" : "no");

  gui_app->input_process_id = process_id;
  gui_app->input_var_name = g_strdup(var_name);
  gui_app->input_numeric = numeric;

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

  gtk_editable_set_text(GTK_EDITABLE(gui_app->input_entry), "");

  gtk_editable_set_text(GTK_EDITABLE(gui_app->quick_input_entry), "");
  gtk_widget_grab_focus(gui_app->quick_input_entry);

  if (gui_app->is_running && gui_app->run_timer_id > 0)
  {
    g_source_remove(gui_app->run_timer_id);
    gui_app->run_timer_id = 0;
  }

  GtkWidget *input_frame = gtk_widget_get_parent(gui_app->input_prompt_box);
  gtk_widget_set_visible(input_frame, TRUE);

  for (int i = 0; i < 3; i++)
  {
    g_timeout_add(300 * i, (GSourceFunc)flash_input_area, input_frame);
    g_timeout_add(300 * i + 150, (GSourceFunc)unflash_input_area, input_frame);
  }

  // Add status message
  gui_add_status_message(gui_app, "Input required! Please check the input box above.");

  return TRUE;
}

static gboolean flash_input_area(GtkWidget *frame)
{
  gtk_widget_add_css_class(frame, "input-flash");
  return G_SOURCE_REMOVE;
}

static gboolean unflash_input_area(GtkWidget *frame)
{
  gtk_widget_remove_css_class(frame, "input-flash");
  return G_SOURCE_REMOVE;
}

static void on_submit_input_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  const char *input_text = gtk_editable_get_text(GTK_EDITABLE(gui_app->input_entry));

  provideInput(&gui_app->sim_state, input_text);

  gtk_editable_set_text(GTK_EDITABLE(gui_app->input_entry), "");

  GtkWidget *input_frame = gtk_widget_get_parent(gui_app->input_prompt_box);
  gtk_widget_set_visible(input_frame, FALSE);

  update_ui_from_state(gui_app);
}

static void gui_state_update(void *gui_data, SystemState *sys G_GNUC_UNUSED)
{
  GuiApp *gui_app = (GuiApp *)gui_data;
  update_ui_from_state(gui_app);
}

static void update_ui_from_state(GuiApp *gui_app)
{
  SystemState *sys = &gui_app->sim_state;
  char status_text[200];
  const char *running_status = "Idle";
  bool is_waiting_for_input = sys->needsInput;

  bool sim_complete = isSimulationComplete(sys);
  bool has_processes = sys->processCount > 0;
  bool can_step = !sim_complete && !gui_app->is_running && !is_waiting_for_input && has_processes;
  bool can_run = !sim_complete && !gui_app->is_running && !is_waiting_for_input && has_processes;
  bool can_reset = !gui_app->is_running && !is_waiting_for_input;
  bool can_load = !gui_app->is_running && !is_waiting_for_input && (sys->processCount < MAX_PROCESSES);
  bool can_change_sched = !gui_app->is_running && !is_waiting_for_input && sys->processCount == 0;

  if (is_waiting_for_input)
  {
    running_status = gtk_label_get_text(GTK_LABEL(gui_app->input_prompt_label));
  }
  else if (sys->runningProcessID >= 0)
  {
    PCB *pcb = findPCB(sys, sys->runningProcessID);
    if (pcb)
    {
      char running_status_buf[100];
      if (sys->schedulerType == SIM_SCHED_MLFQ)
      {
        snprintf(running_status_buf, sizeof(running_status_buf), "Running: P%d (L%d, Q%d)", sys->runningProcessID, pcb->mlfqLevel, pcb->quantumRemaining);
      }
      else if (sys->schedulerType == SIM_SCHED_RR)
      {
        snprintf(running_status_buf, sizeof(running_status_buf), "Running: P%d (Q%d)", sys->runningProcessID, pcb->quantumRemaining);
      }
      else
      {
        snprintf(running_status_buf, sizeof(running_status_buf), "Running: P%d", sys->runningProcessID);
      }
      running_status = running_status_buf;
    }
  }

  snprintf(status_text, sizeof(status_text), "Cycle: %d | %s | %s",
          sys->clockCycle,
          sys->schedulerType == SIM_SCHED_FCFS ? "FCFS" : sys->schedulerType == SIM_SCHED_RR ? "RR"
                                                                                              : "MLFQ",
          running_status);
  gtk_label_set_text(GTK_LABEL(gui_app->status_bar), status_text);

  if (is_waiting_for_input)
  {
    GtkWidget *parent_box = gtk_widget_get_parent(gui_app->quick_input_entry);
    if (parent_box)
    {
      GtkWidget *label = gtk_widget_get_first_child(parent_box);
      if (GTK_IS_LABEL(label))
      {
        char label_text[100];
        PCB *pcb = findPCB(sys, sys->inputPid);
        if (pcb)
        {
          snprintf(label_text, sizeof(label_text), "Input for P%d, var %s:",
                  pcb->programNumber, sys->inputVarName);
        }
        else
        {
          snprintf(label_text, sizeof(label_text), "Input for unknown process, var %s:",
                  sys->inputVarName);
        }
        gtk_label_set_text(GTK_LABEL(label), label_text);
      }
    }

    gtk_widget_grab_focus(gui_app->quick_input_entry);
  }
  else
  {
    GtkWidget *parent_box = gtk_widget_get_parent(gui_app->quick_input_entry);
    if (parent_box)
    {
      GtkWidget *label = gtk_widget_get_first_child(parent_box);
      if (GTK_IS_LABEL(label))
      {
        gtk_label_set_text(GTK_LABEL(label), "Input Value:");
      }
    }
  }

  gtk_widget_set_sensitive(gui_app->step_button, can_step);
  gtk_widget_set_sensitive(gui_app->run_button, can_run);
  gtk_button_set_label(GTK_BUTTON(gui_app->run_button), gui_app->is_running ? "_Pause" : "_Run");
  gtk_widget_set_sensitive(gui_app->reset_button, can_reset);
  gtk_widget_set_sensitive(gui_app->load_p1_button, can_load);
  gtk_widget_set_sensitive(gui_app->load_p2_button, can_load);
  gtk_widget_set_sensitive(gui_app->load_p3_button, can_load);
  gtk_widget_set_sensitive(GTK_WIDGET(gui_app->scheduler_dropdown), can_change_sched);

  gtk_widget_set_sensitive(gui_app->quick_input_button, is_waiting_for_input);
  if (is_waiting_for_input)
  {
    gtk_widget_add_css_class(gui_app->quick_input_entry, "input-flash");
  }
  else
  {
    gtk_widget_remove_css_class(gui_app->quick_input_entry, "input-flash");
  }

  guint selected_scheduler_index = gtk_drop_down_get_selected(gui_app->scheduler_dropdown);
  gtk_widget_set_sensitive(gui_app->rr_quantum_entry, can_change_sched && (selected_scheduler_index == 1));

  char *process_queue_info = format_process_list_and_queues(sys);
  if (gui_app->process_list_buffer)
  {
    gtk_text_buffer_set_text(gui_app->process_list_buffer, process_queue_info ? process_queue_info : "", -1);
  }

  char memory_map_text[MEMORY_SIZE * 150 + 1];
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

    int written = snprintf(line_buffer, sizeof(line_buffer), "W%02d: [%-25.25s] = [%-25.25s]\n",
                          i, name_display, value_display);
    if (current_map_len + written < sizeof(memory_map_text))
    {
      strcat(memory_map_text, line_buffer);
      current_map_len += written;
    }
    else
    {
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
}

static void on_step_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  if (!gui_app->is_running && !isSimulationComplete(&gui_app->sim_state) && !gui_app->sim_state.needsInput)
  {
    stepSimulation(&gui_app->sim_state);
  }
}

static void on_run_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  if (gui_app->is_running)
  {
    stop_continuous_run(gui_app);
  }
  else
  {
    if (!isSimulationComplete(&gui_app->sim_state) && !gui_app->sim_state.needsInput)
    {
      gui_app->is_running = true;
      update_ui_from_state(gui_app);
      gui_app->run_timer_id = g_timeout_add(100, run_simulation_step, gui_app);
    }
  }
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
    update_ui_from_state(gui_app);
  }
}

static gboolean run_simulation_step(gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  if (!gui_app->is_running)
    return G_SOURCE_REMOVE;

  if (isSimulationComplete(&gui_app->sim_state) || gui_app->sim_state.needsInput)
  {
    stop_continuous_run(gui_app);
    return G_SOURCE_REMOVE;
  }

  stepSimulation(&gui_app->sim_state);

  return G_SOURCE_CONTINUE;
}

static void on_reset_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  stop_continuous_run(gui_app);

  SchedulerType type = SIM_SCHED_FCFS;
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

  initializeSystem(&gui_app->sim_state, type, rr_quantum, &gui_app->callbacks, gui_app);

  gtk_text_buffer_set_text(gui_app->log_buffer, "", -1);
  gtk_text_buffer_set_text(gui_app->process_output_buffer, "", -1);

  gui_log_message(gui_app, "System Reset.");

  update_ui_from_state(gui_app);
}

static void on_scheduler_changed(GtkDropDown *dropdown G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;
  update_ui_from_state(gui_app);
}

static void on_quick_input_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  const char *input_text = gtk_editable_get_text(GTK_EDITABLE(gui_app->quick_input_entry));

  if (gui_app->sim_state.needsInput)
  {
    provideInput(&gui_app->sim_state, input_text);

    gui_log_message(gui_app, "Input provided: %s", input_text);

    if (gui_app->is_running && gui_app->run_timer_id == 0)
    {
      gui_app->run_timer_id = g_timeout_add(100, run_simulation_step, gui_app);
    }

    update_ui_from_state(gui_app);

    GtkWidget *input_frame = gtk_widget_get_parent(gui_app->input_prompt_box);
    gtk_widget_set_visible(input_frame, FALSE);
  }
  else
  {
    gui_log_message(gui_app, "Input not required at this time");
  }

  gtk_editable_set_text(GTK_EDITABLE(gui_app->quick_input_entry), "");
}

static char *format_process_list_and_queues(SystemState *sys)
{
  static char buffer[4096];
  char temp[512];
  int offset = 0;

  // Process List Header
  offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                    "=== Process List ===\n"
                    "PID\tState\t\tPriority\tMemory\t\tPC\n"
                    "----------------------------------------\n");

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

  offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                    "\n=== Queue Section ===\n");

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

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nReady Queue:\n");
  if (sys->schedulerType == SIM_SCHED_MLFQ)
  {
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

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nBlocked Processes:\n");
  for (int r = 0; r < NUM_RESOURCES; r++)
  {
    if (sys->mutexes[r].size > 0)
    {
      const char *res_name;
      switch (r)
      {
      case RESOURCE_FILE:
        res_name = "File";
        break;
      case RESOURCE_USER_INPUT:
        res_name = "User Input";
        break;
      case RESOURCE_USER_OUTPUT:
        res_name = "User Output";
        break;
      default:
        res_name = "Unknown";
      }
      offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                        "%s: ", res_name);
      int idx = sys->mutexes[r].head;
      for (int i = 0; i < sys->mutexes[r].size; i++)
      {
        int pid = sys->mutexes[r].blockedQueue[idx];
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

  return buffer;
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
    bool success = loadProgram(&gui_app->sim_state, filename);
    if (!success)
    {
      gui_log_message(gui_app, "Load failed for %s.", filename);
    }
    update_ui_from_state(gui_app);
  }
}

static void activate(GtkApplication *app, gpointer user_data)
{
  GuiApp *gui_app = (GuiApp *)user_data;

  gui_app->main_window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(gui_app->main_window), "Mini OS Simulator");
  gtk_window_set_default_size(GTK_WINDOW(gui_app->main_window), 800, 700);

  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_window_set_child(GTK_WINDOW(gui_app->main_window), main_vbox);

  GtkWidget *control_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_start(control_hbox, 5);
  gtk_widget_set_margin_end(control_hbox, 5);
  gtk_widget_set_margin_top(control_hbox, 5);
  gtk_box_append(GTK_BOX(main_vbox), control_hbox);

  const char *schedulers[] = {"FCFS", "Round Robin", "MLFQ", NULL};
  gui_app->scheduler_model = gtk_string_list_new(schedulers);
  gui_app->scheduler_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(gui_app->scheduler_model), NULL));
  gtk_drop_down_set_selected(gui_app->scheduler_dropdown, 0); // Default FCFS
  gtk_box_append(GTK_BOX(control_hbox), GTK_WIDGET(gui_app->scheduler_dropdown));
  // Connect scheduler change to enable/disable quantum entry
  g_signal_connect(gui_app->scheduler_dropdown, "notify::selected", G_CALLBACK(on_scheduler_changed), gui_app);

  GtkWidget *rr_label = gtk_label_new(" RR Q:");
  gui_app->rr_quantum_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(gui_app->rr_quantum_entry), "2"); // Use gtk_editable_set_text
  gtk_box_append(GTK_BOX(control_hbox), rr_label);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->rr_quantum_entry);

  gui_app->load_p1_button = gtk_button_new_with_label("Load P1");
  gui_app->load_p2_button = gtk_button_new_with_label("Load P2");
  gui_app->load_p3_button = gtk_button_new_with_label("Load P3");
  // Ensure these signal connects are also uncommented:
  g_signal_connect(gui_app->load_p1_button, "clicked", G_CALLBACK(on_load_program_clicked), gui_app);
  g_signal_connect(gui_app->load_p2_button, "clicked", G_CALLBACK(on_load_program_clicked), gui_app);
  g_signal_connect(gui_app->load_p3_button, "clicked", G_CALLBACK(on_load_program_clicked), gui_app);
  gtk_box_append(GTK_BOX(control_hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  gtk_box_append(GTK_BOX(control_hbox), gui_app->load_p1_button);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->load_p2_button);
  gtk_box_append(GTK_BOX(control_hbox), gui_app->load_p3_button);

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

  gui_app->input_prompt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_start(gui_app->input_prompt_box, 10);
  gtk_widget_set_margin_end(gui_app->input_prompt_box, 10);
  gtk_widget_set_margin_top(gui_app->input_prompt_box, 10);
  gtk_widget_set_margin_bottom(gui_app->input_prompt_box, 10);

  GtkWidget *input_frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(input_frame, "input-frame");
  gtk_frame_set_child(GTK_FRAME(input_frame), gui_app->input_prompt_box);

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

  GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand(hpaned, TRUE);
  gtk_box_append(GTK_BOX(main_vbox), hpaned);

  GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_paned_set_start_child(GTK_PANED(hpaned), left_vbox);
  gtk_paned_set_resize_start_child(GTK_PANED(hpaned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(hpaned), FALSE);

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
  gtk_box_append(GTK_BOX(left_vbox), process_list_frame); // Add the new view first

  GtkWidget *memory_view_frame = gtk_frame_new("Memory Map (60 Words)");
  GtkWidget *memory_view_scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(memory_view_scrolled, TRUE); // Allow vertical expansion
  gui_app->memory_text_view = GTK_TEXT_VIEW(gtk_text_view_new());
  gui_app->memory_buffer = gtk_text_view_get_buffer(gui_app->memory_text_view);
  gtk_text_view_set_editable(gui_app->memory_text_view, FALSE);
  gtk_text_view_set_cursor_visible(gui_app->memory_text_view, FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(gui_app->memory_text_view), "monospace-memory-view"); // Apply CSS class
  gtk_text_view_set_wrap_mode(gui_app->memory_text_view, GTK_WRAP_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(memory_view_scrolled), GTK_WIDGET(gui_app->memory_text_view));
  gtk_frame_set_child(GTK_FRAME(memory_view_frame), memory_view_scrolled);
  gtk_box_append(GTK_BOX(left_vbox), memory_view_frame); // Add Memory View after Process List

  GtkWidget *right_vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_end_child(GTK_PANED(hpaned), right_vpaned);
  gtk_paned_set_resize_end_child(GTK_PANED(hpaned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(hpaned), FALSE);

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

int main(int argc, char **argv)
{
  GuiApp gui_app;
  int status;

  memset(&gui_app, 0, sizeof(GuiApp));
  gui_app.is_running = false;
  gui_app.run_timer_id = 0;

  gui_app.callbacks.log_message = gui_log_message_wrapper;
  gui_app.callbacks.process_output = gui_process_output;
  gui_app.callbacks.request_input = gui_request_input;
  gui_app.callbacks.state_update = gui_state_update;

  gui_app.app = gtk_application_new("com.example.minisimulator", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gui_app.app, "activate", G_CALLBACK(activate), &gui_app);

  status = g_application_run(G_APPLICATION(gui_app.app), argc, argv);

  g_object_unref(gui_app.app);
  if (gui_app.scheduler_model)
  {
    g_object_unref(gui_app.scheduler_model);
  }

  return status;
}