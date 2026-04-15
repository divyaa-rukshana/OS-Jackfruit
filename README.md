# Multi-Container Runtime with Kernel Memory Monitor

---

## 1. Team Information

* **Name:** A U Divyaa Rukshana
* **SRN:** PES2UG24AM002
* **Name:** Ashmitha Sri Anand
* **SRN:** PES2UG24AM031

---

## 2. Project Description

This project implements a lightweight Linux container runtime in C along with a kernel-space memory monitoring module. The system supports running multiple containers simultaneously, managing them using a long-running supervisor process, and enforcing memory limits using a Linux Kernel Module (LKM).

### Key Features:

* Multi-container execution with namespace isolation
* Supervisor-based container lifecycle management
* CLI interface for control and monitoring
* Bounded-buffer logging system for container output
* Kernel module for enforcing soft and hard memory limits
* Scheduling experiments using Linux process priorities

---

## 3. Build, Load, and Run Instructions

### Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build Project

```bash
make
```

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

### Run Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

### CLI Commands

```bash
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

### Run Experiments

```bash
./cpu_hog
./memory_hog
```

### Cleanup

```bash
sudo rmmod monitor
```

---

## 4. Implementation Approach

### Task 1: Multi-Container Runtime

* Used `clone()` with flags: `CLONE_NEWPID`, `CLONE_NEWUTS`, `CLONE_NEWNS`
* Implemented a long-running supervisor process to manage containers
* Maintained container metadata using structured records
* Used `chroot()` for filesystem isolation
* Mounted `/proc` inside each container
* Ensured proper child reaping using `waitpid()`

---

### Task 2: CLI and Supervisor Communication

* Implemented IPC using UNIX domain sockets
* CLI sends commands to supervisor and receives responses
* Supported commands: `start`, `run`, `ps`, `logs`, `stop`
* Handled signals (`SIGCHLD`, `SIGINT`, `SIGTERM`) for lifecycle management

---

### Task 3: Bounded-Buffer Logging

* Captured container stdout and stderr using pipes
* Implemented circular bounded buffer
* Producer thread reads from pipes
* Consumer thread writes logs to files
* Used mutex and condition variables for synchronization
* Ensured no data loss and proper shutdown of threads

---

### Task 4: Kernel Memory Monitor

* Implemented Linux Kernel Module with `/dev/container_monitor`
* Used `ioctl` for communication with user space
* Maintained linked list of monitored processes
* Used mutex for safe access
* Soft limit → logs warning using `printk`
* Hard limit → terminates process using `SIGKILL`

---

### Task 5: Scheduling Experiments

* Used CPU-bound (`cpu_hog`) and memory workloads
* Adjusted priorities using `nice`
* Measured execution time and CPU allocation differences

---

### Task 6: Resource Cleanup

* Reaped child processes using `waitpid()`
* Closed all file descriptors
* Ensured threads exit cleanly
* Freed kernel memory on module unload
* Verified no zombie processes remain

---

## 5. Demo (Screenshots)

1. **Multi-container supervision**
   *Two containers running under one supervisor*
   <img width="914" height="345" alt="multi" src="https://github.com/user-attachments/assets/ceee472c-53b3-4891-bfd5-0e1a76c680d5" />

2. **Metadata tracking (`ps`)**
   *Displays container ID, PID, and state*
   <img width="914" height="345" alt="multi" src="https://github.com/user-attachments/assets/02484a5a-3b6c-47df-b74d-e09707cfe65c" />

3. **Bounded-buffer logging**
   *Logs captured and stored in files*
   <img width="911" height="324" alt="dmesg_tail" src="https://github.com/user-attachments/assets/c03b19ea-3a9e-48e8-9bdd-44ce776f764e" />

 

5. **CLI and IPC**
   *Command sent from CLI and handled by supervisor*
   <img width="911" height="146" alt="cli_ipc" src="https://github.com/user-attachments/assets/5521629a-9934-4fde-a8b6-e8782a36e82f" />
   <img width="910" height="90" alt="engine_supervisor" src="https://github.com/user-attachments/assets/d3461099-448a-46e5-b0c7-608ad970acff" />



6. **Soft-limit warning**
   *Kernel logs showing memory warning*
   <img width="911" height="324" alt="dmesg_tail" src="https://github.com/user-attachments/assets/168e32a4-c61b-43e0-9ea0-b684a0f4c3d9" />


7. **Hard-limit enforcement**
   *Container killed after exceeding memory limit*
   <img width="909" height="698" alt="sudo_dmesg_tail_-n" src="https://github.com/user-attachments/assets/fd9ab12a-3e9b-4931-af16-b7fee69c1577" />


9. **Scheduling experiment**
   *Two containers running concurrently under the supervisor, the Linux scheduler allocates CPU time differently, demonstrating fairness and scheduling behavior*
   <img width="914" height="345" alt="multi" src="https://github.com/user-attachments/assets/80d5da1e-2ae6-4f1a-8816-daeef4c5e751" />


10. **Clean teardown**
   *No zombie processes after shutdown*
   <img width="916" height="306" alt="clean" src="https://github.com/user-attachments/assets/5ca16411-4683-4663-887c-5cdcbacd8e1f" />
   <img width="910" height="352" alt="rmmod_monitor" src="https://github.com/user-attachments/assets/324b4047-ced5-4627-a28c-b3066fb572a6" />


---

## 6. Engineering Analysis

### Isolation Mechanisms

Containers use PID, UTS, and mount namespaces for isolation. `chroot()` ensures filesystem separation. However, all containers share the same kernel.

---

### Supervisor and Process Lifecycle

A persistent supervisor manages container creation, tracking, and termination. It ensures proper signal handling and avoids zombie processes.

---

### IPC, Threads, and Synchronization

Two IPC mechanisms:

* Control: UNIX sockets (CLI ↔ supervisor)
* Logging: Pipes (container → supervisor)

Synchronization:

* Mutex for shared data
* Condition variables for buffer control

---

### Memory Management and Enforcement

RSS represents actual physical memory usage. Soft limits generate warnings, while hard limits enforce termination. Kernel-level enforcement ensures accuracy and control.

---

### Scheduling Behavior

Linux scheduler prioritizes processes based on `nice` values. Lower nice values receive more CPU time, demonstrating fairness and responsiveness.

---

## 7. Design Decisions and Tradeoffs

### Namespace Isolation

* Used `chroot()`
* ✔ Simple
* ✘ Less secure than `pivot_root()`

---

### IPC Design

* Used UNIX domain sockets
* ✔ Reliable bidirectional communication
* ✘ Slightly more complex than FIFO

---

### Logging System

* Bounded buffer with threads
* ✔ Efficient and non-blocking
* ✘ Requires synchronization

---

### Kernel Monitor

* Linked list for tracking processes
* ✔ Simple design
* ✘ Requires locking for safety

---

### Scheduling Experiments

* Used `nice` values
* ✔ Easy to implement
* ✘ Limited control over scheduler internals

---

## 8. Scheduler Experiment Results

| Workload  | Nice Value | Observation      |
| --------- | ---------- | ---------------- |
| CPU Hog A | -5         | Faster execution |
| CPU Hog B | 10         | Slower execution |

### Comparison

* Lower nice value resulted in higher CPU allocation
* CPU-bound processes dominated CPU usage
* Confirms Linux scheduler prioritization and fairness

---

## 9. Conclusion

This project demonstrates key Operating System concepts including process isolation, IPC, synchronization, kernel-user interaction, and scheduling behavior through a practical implementation.

---
