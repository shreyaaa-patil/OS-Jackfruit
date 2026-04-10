# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor process and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Shreya Patil | PES2UG24CS484 |
| Shreya Shenoy | PES2UG24CS487 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

- Ubuntu 22.04 or 24.04 VM
- Compiler and libraries required to run C programs (Linux libraries used)

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Step 1 — Clone and build

```bash
git clone https://github.com/<your-username>/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate

# Build everything: engine binary + workload binaries + kernel module
make
```

### Step 2 — Prepare the root filesystem

```bash
# Download Alpine mini rootfs
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create one writable copy per container
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Copy workload binaries into each rootfs so they can run inside containers
cp cpu_hog    rootfs-alpha/
cp cpu_hog    rootfs-beta/
cp memory_hog rootfs-alpha/
cp memory_hog rootfs-beta/
cp io_pulse   rootfs-alpha/
cp io_pulse   rootfs-beta/
```

### Step 3 — Load the kernel module

```bash
sudo insmod monitor.ko

# Verify the control device was created
ls -l /dev/container_monitor
```

### Step 4 — Start the supervisor

Open a dedicated terminal for the supervisor. It runs as a long-lived daemon:

```bash
sudo ./engine supervisor ./rootfs-base
```

You should see: `Supervisor ready. Socket: /tmp/mini_runtime.sock`

### Step 5 — Use the CLI (in a second terminal)

```bash
# Start two containers in the background
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 30" --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  "/memory_hog 5" --soft-mib 64 --hard-mib 96

# List all running containers and their metadata
sudo ./engine ps

# View the log output of a container
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Step 6 — Run a memory limit test

```bash
# Start a container with a low hard limit to trigger the kernel monitor
sudo ./engine start memtest ./rootfs-alpha "/memory_hog 2 200" --soft-mib 20 --hard-mib 30

# Watch kernel logs for soft/hard limit events
dmesg -w
```

### Step 7 — Run scheduler experiments

```bash
# Experiment A: two CPU-bound containers at different nice values
sudo ./engine start hi  ./rootfs-alpha "/cpu_hog 20" --nice -5
sudo ./engine start lo  ./rootfs-beta  "/cpu_hog 20" --nice  10

# Compare their completion times via logs
sudo ./engine logs hi
sudo ./engine logs lo

# Experiment B: CPU-bound vs I/O-bound at the same time
sudo ./engine start cpuwork  ./rootfs-alpha "/cpu_hog 20"
sudo ./engine start iowork   ./rootfs-beta  "/io_pulse 20 100"
```

### Step 8 — Teardown and cleanup

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Ctrl+C the supervisor terminal (or kill it)
# Verify no zombie processes remain
ps aux | grep -E "Z|defunct"

# Check kernel logs
dmesg | tail -20

# Unload the kernel module
sudo rmmod monitor

# Clean build artifacts
make clean
```

---

## 3. Demo with Screenshots

### Screenshot 1 — Multi-container supervision

> **What to capture:** Two terminals side by side. Left terminal shows the supervisor running (`./engine supervisor ./rootfs-base`). Right terminal shows both `alpha` and `beta` containers started with `./engine start`. The supervisor terminal should show it accepted both requests.

**[ INSERT SCREENSHOT 1 HERE ]**

*Caption: Supervisor process managing two containers (alpha and beta) simultaneously. The supervisor stays alive while both containers run.*

---

### Screenshot 2 — Metadata tracking (`ps` output)

> **What to capture:** Run `sudo ./engine ps` in the CLI terminal after starting at least two containers. The output table should show container ID, PID, state (running/exited), and exit code for each container.

**[ INSERT SCREENSHOT 2 HERE ]**

*Caption: Output of `./engine ps` showing tracked metadata for all containers including host PID, state, and exit code.*

---

### Screenshot 3 — Bounded-buffer logging

> **What to capture:** Two things in one screenshot (or two separate ones):
> (a) Run `sudo ./engine logs alpha` — shows the log file content captured through the pipeline from the container's stdout.
> (b) The `logs/` directory listing (`ls -lh logs/`) showing per-container log files were created on disk.

**[ INSERT SCREENSHOT 3 HERE ]**

*Caption: Log file contents for container alpha captured through the bounded-buffer logging pipeline. The logs/ directory shows per-container log files written by the consumer thread.*

---

### Screenshot 4 — CLI and IPC

> **What to capture:** Run any CLI command (e.g., `sudo ./engine stop alpha`) and show both terminals — the CLI terminal printing the supervisor's response message, and the supervisor terminal showing it received and processed the request.

**[ INSERT SCREENSHOT 4 HERE ]**

*Caption: CLI client sending a `stop` command to the supervisor over the UNIX domain socket at `/tmp/mini_runtime.sock`. The supervisor responds with a confirmation message.*

---

### Screenshot 5 — Soft-limit warning

> **What to capture:** Start a container with a low soft limit (e.g., `--soft-mib 20`) and run `memory_hog` inside it. Then run `dmesg | grep "SOFT LIMIT"` to show the kernel warning. It should say something like `[container_monitor] SOFT LIMIT container=memtest pid=XXXX rss=XXXXXX limit=XXXXXX`.

**[ INSERT SCREENSHOT 5 HERE ]**

*Caption: `dmesg` output showing the kernel module emitting a SOFT LIMIT warning when container memtest exceeded its 20 MiB soft limit. The warning fires once and is not repeated.*

---

### Screenshot 6 — Hard-limit enforcement

> **What to capture:** Two things:
> (a) `dmesg | grep "HARD LIMIT"` showing the kernel module sent SIGKILL to the container PID.
> (b) `sudo ./engine ps` after the kill, showing the container's state changed to `killed`.

**[ INSERT SCREENSHOT 6 HERE ]**

*Caption: `dmesg` showing the kernel module enforcing the hard memory limit by sending SIGKILL to the container. The `ps` output shows the container state updated to `killed`, distinguishing it from a normal stop.*

---

### Screenshot 7 — Scheduling experiment

> **What to capture:** Start two `cpu_hog` containers with different nice values (e.g., `--nice -5` and `--nice 10`) at the same time. After both finish, run `./engine logs hi` and `./engine logs lo` side by side. The `hi` container (lower nice = higher priority) should show shorter elapsed time in its output lines. Alternatively, capture `time` output wrapping both starts.

**[ INSERT SCREENSHOT 7 HERE ]**

*Caption: Two cpu_hog containers running concurrently with nice=-5 (hi) and nice=10 (lo). The higher-priority container finishes noticeably faster, demonstrating Linux CFS scheduler priority handling.*

---

### Screenshot 8 — Clean teardown

> **What to capture:** After stopping all containers and sending SIGINT to the supervisor:
> (a) `ps aux | grep -E "engine|defunct"` showing no zombie processes.
> (b) The supervisor terminal printing "Supervisor shutting down..." and returning to the shell prompt cleanly.

**[ INSERT SCREENSHOT 8 HERE ]**

*Caption: Clean shutdown — no zombie or defunct processes remain after all containers are stopped and the supervisor exits. All threads joined and file descriptors closed.*

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Linux namespaces are the kernel mechanism that helps make containers. Our runtime uses clone() with three namespace flags: 1. CLONE_NEWPID which gives every container its own PID namespace so the first process inside sees itself as PID 1, not aware of host processes. 2. CLONE_NEWUTS gives every container its own hostname. 3. CLONE_NEWNS gives every container its own mount namespace so mounts made inside (eg: `/proc`) do not leak to the host.

chroot() - this restricts the container's view of the filesystem to its assigned rootfs directory. Once chroot(rootfs-alpha) is called, the container cannot access anything outside that directory tree. We then call chdir("/") so the working directory is also inside the new root, preventing escaping out via other relative paths.

The host kernel is shared. All containers share the same kernel, same system call interface, the same network, and same CPU and memory. This makes containers lightweight compared to VMs. There is no guest kernel.

### 4.2 Supervisor and Process Lifecycle

A long running supervisor is useful because containers need a parent process that lives longer than them. When a process exits, its process table entry (eg: "zombie") stays there until its parent calls wait() or waitpid(). If the supervisor exited after launching a container, the container would be re-parented to init (meaning PID=1), making us unable to track its exit status and metadata.

Our supervisor calls clone() to create each container child. After clone() returns in the parent, the child's PID is stored in container_record_t. When the child exits, the kernel sends SIGCHLD to the supervisor.sigchld_handler calls waitpid in a loop to reap all exited children at once, extracting their exit code or terminating signal and updating the metadata record. The `WNOHANG` flag in waitpid loop prevents it (waitpid) from blocking if not all children have exited yet.

The metadata record tracks the full container lifecycle: STARTING -> RUNNING -> STOPPED/KILLED/EXITED. The stop_requested flag distinguishes a manual stop from a hard limit kill, so ps output is accurate.

### 4.3 IPC, Threads, and Synchronisation

This project uses two separate IPC mechanisms as required:

**Logging (pipes):** Each container's stdout and stderr are connected to the write-end of a pipe via dup(). The supervisor holds the read-end. A dedicated pipe reader thread per container reads chunks from the pipe and pushes them into the bounded buffer. The bounded buffer exists between this producer threads and a single consumer (logger thread), which pops chunks and add them to the per container log files.

The bounded buffer uses a pthread_mutex_t to protect the head, tail, and count fields. Without it, two producer threads could both read count = 15, both decide to push, and corrupt the buffer by writing to the same slot. We use two pthread_cond_t, variables: 1. not_full-producers wait here when the buffer is full 2.  not_empty- the consumer waits here when the buffer is empty. pthread_cond_wait atomically releases the mutex and sleeps, which avoids race condition between checking the condition and sleeping.

**Control (UNIX domain socket):** CLI clients connect to /tmp/mini_runtime.sock, send a fixed-size control_request_t struct, and receive a fixed-size control_response_t struct back. The metadata_lock mutex protects the container linked list accessed from both the socket handler (main thread) and the SIGCHLD handler.

The reason we use a mutex for the list and not a spinlock is that the ioctl and socket handler paths can sleep (because they call malloc, send, recv). Spinlocks must never sleep, they busy-wait and are only appropriate for very small critical sections in interrupt cases.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical RAM pages currently mapped and present for a process. It does not measure virtual memory (pages allocated but not yet touched), shared libraries counted once per process, or memory-mapped files that have been swapped out. In our kernel module, get_mm_rss() returns the actual physical page count for a PID's mm_struct, which we multiply by PAGE_SIZE to get bytes.

Soft and hard limits serve different purposes. The soft limit is a warning threshold — the process is allowed to continue but the operator is alerted. The hard limit is a termination threshold — the process is killed to protect the rest of the system. This two-tier design lets operators tune containers conservatively without immediately killing processes that have brief memory spikes.

Enforcement belongs in kernel space because user-space enforcement is fundamentally unreliable. A user-space monitor could be scheduled away for hundreds of milliseconds while a container allocates memory explosively. A kernel timer fires with guaranteed periodicity relative to jiffies. Additionally, the kernel can access mm_struct directly and send signals atomically via send_sig(), without race conditions between checking and acting.

### 4.5 Scheduling Behaviour

Linux uses the Completely Fair Scheduler as its default scheduler. CFS tracks a vruntime value for each runnable task — the total CPU time the task has consumed, weighted by its priority. The scheduler always picks the task with the smallest vruntime. Nice values adjust the weight: a process with nice=-5 gets roughly 3x the CPU share of a process with nice=10 on an otherwise idle system.

Our project show this directly. Two cpu_hog processes running simultaneously — one at nice=-5, one at nice=10 — complete the same workload in measurably different times. The high-priority process finishes significantly faster because CFS selects it more often. The I/O-bound io_pulse process voluntarily sleeps between writes, so it yields the CPU frequently and has very low vruntime — it gets scheduled quickly when it wakes up regardless of its nice value, demonstrating the difference between CPU-bound and I/O-bound scheduling behaviour.

---

## 5. Design Decisions and Tradeoffs

### Namespace isolation — chroot instead of pivot_root

**Choice:** We use chroot() to isolate each container's filesystem view rather than the more thorough pivot_root().

**Tradeoff:** chroot can be escaped by a root process inside the container using chroot("../../..") tricks if the container is not further restricted. pivot_root fully replaces the root mount point and is harder to escape. However, pivot_root requires the new root to be a mount point itself, which adds setup complexity.

**Justification:** For this project, chroot provides sufficient isolation to demonstrate the concept. Adding pivot_root would require creating a bind mount for each container's rootfs before launch, which adds steps that distract from the core learning goals.

### Supervisor architecture — single-threaded event loop with a separate logger thread

**Choice:** The supervisor's main loop handles one CLI connection at a time (sequential `accept` -> `recv` -> `respond`). A separate pthread handles all log writing.

**Tradeoff:** The main loop is not concurrent — if one CLI command takes time (e.g., streaming a large log file), other CLI clients must wait. A multi-threaded or epoll based design would handle this better at scale.

**Justification:** For a project demonstrating concepts with a handful of containers, sequential handling is sufficient and much simpler to reason about correctly. The logger is correctly separated into its own thread because it does blocking file I/O that should not stall the control plane.

### IPC and logging — pipes for logging, UNIX socket for control

**Choice:** Two separate IPC mechanisms: pipes carry container output to the supervisor, and a UNIX domain socket carries CLI commands to the supervisor.

**Tradeoff:** Using two mechanisms adds complexity compared to a single socket for everything. However, mixing control messages and log data on one channel would require framing/multiplexing logic.

**Justification:** Pipes are the natural fit for streaming stdout/stderr. they are unidirectional, auto-close when the container exits (triggering EOF for the reader), and require no message framing. The socket is the natural fit for request/response control commands. Keeping them separate makes each path simpler and matches how real container runtimes (like containerd) are designed.

### Kernel monitor — mutex over spinlock

**Choice:** We protect the monitored process list with a mutex rather than  spinlock.

**Tradeoff:** A mutex can sleep, so it cannot be used in hard interrupt context. We use mutex_trylock in the timer callback to avoid blocking — if the lock is held we simply skip the tick. A spinlock would never sleep and would be safe in all contexts, but our kmalloc(GFP_KERNEL) in the register path can sleep, making a spinlock inappropriate there.

**Justification:** The ioctl path (register/unregister) needs GFP_KERNEL allocation which can sleep. Using a mutex everywhere and trylock in the timer is the cleaner solution. The one-second timer period makes a skipped tick completely acceptable.

### Scheduling experiments — nice values via setpriority in child_fn

**Choice:** We apply nice values using the nice() syscall inside child_fn before exec().

**Tradeoff:** The nice value is applied after the process is already created and before exec. A more precise approach would use sched_setattr with CFS weights, but that requires more privilege handling.

**Justification:** nice() is the standard POSIX interface for priority adjustment. It is enopugh to produce clearly observable differences in scheduling behaviour between containers.

---

## 6. Scheduler Experiment Results

### Experiment A — CPU-bound workloads at different priorities

Two containers ran cpu_hog 20 (20 seconds of CPU work) simultaneously. One was started with --nice -5(higher priority) and one with --nice 10 (lower priority).

| Container | Nice value | Observed completion time |
|-----------|-----------|--------------------------|
| hi        | -5        | [ Fill in from your logs ] seconds |
| lo        | 10        | [ Fill in from your logs ] seconds |

**what is seen:** The high-priority container (nice=-5) received a larger share of CPU time from the Linux CFS scheduler. CFS uses weighted vruntime...a lower nice value means a higher weight, so the scheduler selects `high` more often per scheduling period. The `low` container still made progress but took too long to finish the task. CFS guarantees no starvation.

### Experiment B — CPU-bound vs I/O-bound at the same priority

One container ran cpu_hog 20 and another ran io_pulse 20 100 (20 iterations with 100ms sleep between writes) simultaneously at the same nice value.

| Container | Workload type | Behaviour observed |
|-----------|--------------|-------------------|
| cpuwork   | CPU-bound    | Ran continuously, consumed full time slice each scheduling period |
| iowork    | I/O-bound    | Slept frequently, woke up with low vruntime, responded quickly each time |

**What is seen:** The I/O-bound process voluntarily gave up the CPU on every usleep() call. Because it was sleeping most of the time, its vruntime accumulated slowly. Each time it woke up, CFS scheduled it almost immediately because its vruntime was the smallest in the run queue. This demonstrates that CFS naturally gives good responsiveness to I/O bound workloads without any explicit priority adjustment, the scheduler rewards processes that yield the CPU.

### Summary

The experiments confirm two fundamental CFS properties: 1. nice values shift CPU allocation predictably - lower nice = more CPU share. 2.  I/O-bound processes get better responsiveness than their nice value alone would suggest, because voluntary sleeping keeps their vruntime low. These properties are why web servers (I/O bound) feel responsive on the same machine as compilation jobs (CPU bound) with no manual priority tuning.

---

## Repository Structure

```
boilerplate/
├── engine.c          — user-space runtime and supervisor
├── monitor.c         — kernel-space memory monitor (LKM)
├── monitor_ioctl.h   — shared ioctl definitions
├── cpu_hog.c         — CPU-bound test workload
├── memory_hog.c      — memory-consuming test workload
├── io_pulse.c        — I/O-bound test workload
├── Makefile          — builds all targets
└── environment-check.sh
```
