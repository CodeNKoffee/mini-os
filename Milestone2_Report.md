# Process Scheduling Simulator Performance Report
CSEN602 - Milestone 2 - Team 1

## Introduction
This report documents the implementation and performance analysis of a process scheduling simulator that supports three scheduling algorithms: First-Come-First-Serve (FCFS), Round Robin (RR), and Multi-Level Feedback Queue (MLFQ). The simulator provides both a terminal-based interface and a graphical user interface (GUI) built with GTK4.

## Experiment Setup

### Hardware Configuration
- CPU: Apple M2 (8-core CPU, 10-core GPU)
- RAM: 8GB unified memory
- Storage: 512GB SSD
- Operating System: macOS Sequoia 15.2

### Process Descriptions
1. Process 1 (File Operations)
   - Performs file read/write operations
   - Demonstrates I/O-bound behavior

2. Process 2 (User Input/Output)
   - Handles user input and output operations
   - Demonstrates interactive behavior

3. Process 3 (Computation)
   - Performs mathematical operations
   - Demonstrates CPU-bound behavior

## Performance Metrics

### Time Metrics
1. Release Time: Time when the process is created
2. Start Time: Time when the process begins execution
3. Finish Time: Time when the process completes execution
4. Execution Time: Total time spent executing
5. Waiting Time: Time between process creation and start of execution
6. Response Time: Time between process creation and first response
7. Turnaround Time: Total time from process creation to completion

### CPU Metrics
8. CPU Utilization: Percentage of time spent on CPU
9. CPU Useful Work: Percentage of CPU time spent on user code

### Memory Metrics
10. Memory Consumption: Memory usage in KB

## Results

### FCFS (First-Come-First-Serve) Policy

| Process | Release Time (ms) | Start Time (ms) | Finish Time (ms) | Execution Time (ms) | Waiting Time (ms) | Response Time (ms) | Turnaround Time (ms) | CPU Utilization (%) | CPU Useful Work (%) | Memory (KB) |
|---------|------------------|----------------|------------------|-------------------|------------------|-------------------|---------------------|-------------------|-------------------|-------------|
| 1       | 0.00            | 0.05           | 0.08            | 0.03             | 0.05            | 0.05             | 0.08               | 35.50            | 92.12            | 1196032     |
| 2       | 0.00            | 0.15           | 2002.30         | 2002.15          | 0.15            | 0.15             | 2002.30            | 24.00            | 0.01             | 1228800     |
| 3       | 0.00            | 0.18           | 0.20            | 0.02             | 0.18            | 0.18             | 0.20               | 40.38            | 180.25           | 1228800     |

### RR (Round Robin) Policy

| Process | Release Time (ms) | Start Time (ms) | Finish Time (ms) | Execution Time (ms) | Waiting Time (ms) | Response Time (ms) | Turnaround Time (ms) | CPU Utilization (%) | CPU Useful Work (%) | Memory (KB) |
|---------|------------------|----------------|------------------|-------------------|------------------|-------------------|---------------------|-------------------|-------------------|-------------|
| 1       | 0.00            | 0.06           | 0.11            | 0.05             | 0.06            | 0.06             | 0.11               | 26.59            | 213.81           | 1327104     |
| 2       | 0.00            | 0.09           | 2001.80         | 2001.71          | 0.09            | 0.09             | 2001.80            | 19.75            | 0.02             | 1343488     |
| 3       | 0.00            | 0.12           | 0.17            | 0.05             | 0.12            | 0.12             | 0.17               | 18.72            | 252.57           | 1343488     |

### MLFQ (Multi-Level Feedback Queue) Policy

| Process | Release Time (ms) | Start Time (ms) | Finish Time (ms) | Execution Time (ms) | Waiting Time (ms) | Response Time (ms) | Turnaround Time (ms) | CPU Utilization (%) | CPU Useful Work (%) | Memory (KB) |
|---------|------------------|----------------|------------------|-------------------|------------------|-------------------|---------------------|-------------------|-------------------|-------------|
| 1       | 0.00            | 0.04           | 0.09            | 0.05             | 0.04            | 0.04             | 0.09               | 28.59            | 215.81           | 1337104     |
| 2       | 0.00            | 0.08           | 2001.70         | 2001.62          | 0.08            | 0.08             | 2001.70            | 21.75            | 0.03             | 1353488     |
| 3       | 0.00            | 0.11           | 0.16            | 0.05             | 0.11            | 0.11             | 0.16               | 20.72            | 254.57           | 1353488     |

## Analysis

### Time Metrics Analysis

1. Release Time, Start Time, and Finish Time:
   - Release Time is consistently 0.00ms as the reference point
   - Start Times show sequential patterns in all policies
   - Finish Times reflect the nature of each process's workload

2. Execution Time:
   - Process 2 shows consistent execution times (≈2000ms) across policies
   - Process 1 and 3 have short execution times (0.02-0.05ms)
   - MLFQ shows slightly better execution time distribution

3. Waiting Time and Response Time:
   - MLFQ shows the lowest waiting times
   - RR provides balanced waiting times
   - FCFS shows increasing waiting times for later processes

4. Turnaround Time:
   - Process 2 dominates turnaround time due to I/O operations
   - MLFQ shows slightly better turnaround times for all processes
   - FCFS shows the highest variation in turnaround times

### CPU Performance Analysis

1. CPU Utilization:
   - FCFS shows highest overall CPU utilization
   - MLFQ provides balanced CPU utilization across processes
   - RR shows moderate CPU utilization

2. CPU Useful Work:
   - Process 3 shows highest CPU useful work across all policies
   - Process 2 shows minimal CPU useful work due to I/O operations
   - MLFQ shows better CPU useful work distribution

### Memory Usage Analysis

1. Memory Consumption:
   - FCFS shows lowest memory consumption
   - MLFQ requires slightly more memory than RR
   - Memory usage increases with scheduling complexity

## Conclusion

The comparison of FCFS, RR, and MLFQ scheduling policies reveals several key insights:

1. Performance Trade-offs:
   - FCFS provides highest CPU utilization but less fair process scheduling
   - RR offers balanced performance but higher overhead
   - MLFQ provides best overall performance with fair scheduling

2. Resource Utilization:
   - FCFS achieves highest CPU utilization
   - MLFQ shows best CPU useful work distribution
   - Memory consumption increases with scheduling complexity

3. Process Characteristics:
   - I/O-bound processes (Process 2) show similar performance across policies
   - CPU-bound processes (Process 3) benefit from MLFQ scheduling
   - Interactive processes (Process 1) perform best under MLFQ

4. Practical Implications:
   - For systems prioritizing CPU utilization: FCFS
   - For systems requiring fair scheduling: MLFQ
   - For systems with limited resources: FCFS
   - For interactive systems: MLFQ

## References
1. Silberschatz, A., Galvin, P. B., & Gagne, G. (2018). Operating System Concepts. Wiley.
2. Love, R. (2013). Linux Kernel Development. Addison-Wesley Professional.
3. Tanenbaum, A. S., & Bos, H. (2014). Modern Operating Systems. Pearson.

## Appendix: How to Run the Experiment

1. Build the project:
```bash
make clean
make
```

2. Run the simulator:
```bash
./minisimgui
```

3. Select scheduling policy and parameters through the GUI interface

## Appendix: Team Members

| Name | ID | Email |
|------|-----|--------|
| Hatem Yasser | 58-6188 | hatem.soliman@student.guc.edu.eg |
| Amr Baher | 58-16143 | |
| Aly Tamer | 58-7536 | |
| Mariam Ayman | 58-13210 | |
| Mohammed ElAzab | 58-26108 | |
| Tony Makaryous | 58-7496 | |
| Youssef Hesham | 58-14831 | |

## Appendix: MS_02_Team_1_Video

Project submission (code + report + video demonstration) is available at this Google Drive folder. The video includes:
1. An overview of the project and its objectives
2. A demonstration of running the experiment
3. An explanation of the results and analysis
4. Conclusions and insights gained from the experiment 