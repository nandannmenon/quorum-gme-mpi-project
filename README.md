# Quorum-Based Extended Group Mutual Exclusion in MPI

This project is a **C and MPI implementation** of the distributed algorithm from the paper:

> **"A quorum-based extended group mutual exclusion algorithm without unnecessary blocking"**  
> by *Yoshifumi Manabe* and *JaeHyrk Park*

It simulates a distributed system where multiple **Requester** processes compete for access to a shared resource (a **critical section**).  
Access is managed by a set of **Manager** processes using a **quorum-based system**.

---

## Features

- **Group Mutual Exclusion:** Allows multiple processes from the same group to enter the critical section (CS) concurrently.
- **Mutual Exclusion:** Ensures processes from different groups cannot be in the CS simultaneously — one group must finish completely before another enters.
- **Two-Phase Release:** Implements the full Release / Finished / Over protocol, ensuring the lock is not released until all processes from an active group have exited the CS.
- **Lamport Clocks:** Uses Lamport logical clocks to assign a unique, total-order priority `(timestamp, rank)` to every request.
- **Deadlock Prevention:** Late-arriving (“zombie”) messages from old requests are safely ignored using timestamps, preventing deadlocks and desynchronization.
- **Starvation-Freeness:** Managers use a priority queue, servicing the request with the lowest timestamp first — ensuring fairness and preventing starvation.
- **Cancel Protocol:** Fully implements the Cancel / Cancelled logic, allowing a high-priority request to revoke an OK already sent to a lower-priority one.

---

## How to Run

### Prerequisites

- **C compiler** (e.g., `gcc`)
- **MPI implementation** (e.g., **Open MPI** or **MPICH**)
- **Python 3** (for log visualization)

---

### 1. Compile the C Code

You only need to do this once (or after modifying `gme_mpi.c`):

```bash
mpicc -o gme_mpi gme_mpi.c
```

---

### 2. Run the MPI Simulation & Create Log

Run the MPI program with 6 processes and save the output to `output.log`:

```bash
mpirun -n 6 ./gme_mpi > output.log 2>&1
```

> You can change `-n 6` to `-n 5` or any other number to test your different scenarios.

---

### 3. Run the Python Visualizer

This reads `output.log` and generates an interactive visualization:

```bash
python3 log.py
```

This creates a file named **`visualization.html`** in the same directory.

---

### 4. Open the Visualization in Your Browser

```bash
open visualization.html
```

> On Linux, use `xdg-open visualization.html` instead.

---

### All-in-One Command

Run the entire process — simulation, visualization, and browser view — in one line:

```bash
mpirun -n 6 ./gme_mpi > output.log 2>&1 && python3 log_to_html.py && open visualization.html
```

---

## Example Scenarios

You can test different configurations by changing the number of processes (`-n`) in `mpirun`.

### Test 1: Concurrency (2 Requesters, 1 Group)

```bash
mpirun -n 5 ./gme_mpi
```

**Setup:**
- 3 Managers  
- 2 Requesters → R3[g=0], R4[g=0]

**Expected Behavior:**
- One requester acts as the *pivot*, the other joins with an `ENTER` message.  
- Both show `ENTERING CS` before any `EXITING CS` logs.

---

### Test 2: Mutual Exclusion (3 Requesters, 2 Groups)

```bash
mpirun -n 6 ./gme_mpi
```

**Setup:**
- 3 Managers  
- 3 Requesters → R3[g=0], R4[g=0], R5[g=1]

**Expected Behavior:**
- R3 and R4 (group 0) enter CS concurrently.  
- R5 (group 1) waits until R3 and R4 fully exit (after `OVER`).

---

### Test 3: Cancel Logic

```bash
mpirun -n 6 ./gme_mpi
```

**What to Look For:**
- A higher-priority request (e.g., R3) arrives after an `OK` was sent to a lower-priority one (e.g., R5).  
- Manager logs `Sending CANCEL`.  
- Requester logs `My 'OK' was revoked`.

This confirms proper **cancelation and fairness** handling.

---

## Output Visualization

After the run, the visualization HTML presents:

- **Timeline view** of message exchanges  
- **Grouped color-coded events** per process (Manager / Requester)  
- **Lamport clock order tracking**  
- Clear phases of each request: `REQUEST → OK → ENTER → RELEASE → OVER`  
- Highlighted cancelation and group transitions  

This makes it easy to **visually confirm correctness**, spot deadlocks, or verify group-based concurrency.

---

## Message Flow Diagram

### Normal Request–Grant–Enter–Release Cycle

```text
Requester (Ri)              Manager (Mj)
     |                           |
     | ---- REQUEST(g, ts) --->  |
     |                           |
     | <-------- OK ----------   |
     | ---- ENTER ------------>  |
     | <---- ENTER_ACK --------  |
     | --- RELEASE/FINISHED -->  |
     | <-------- OVER ---------  |
```

---

### Cancel Protocol (When Higher Priority Request Arrives)

```text
Low-Priority R5            Manager M0            High-Priority R3
      |                         |                       |
      | ---- REQUEST(5) ---->   |                       |
      | <---- OK -------------  |                       |
      |                         |<---- REQUEST(3) ----- |
      |                         |---- CANCEL ---------->|
      | <---- CANCELLED ------  |                       |
```

---

## Notes

- Implements all message types:  
  `REQUEST`, `OK`, `CANCEL`, `CANCELLED`, `ENTER`, `RELEASE`, `FINISHED`, `OVER`
- Uses Lamport timestamps `(clock, rank)` for total ordering.  
- Every send/receive is logged with rank and timestamp for reproducibility.  
- Compatible with the **log visualizer** for real-time analysis.  
- Designed for educational and research use in distributed systems.

---

## Reference

**Paper:**  
*Yoshifumi Manabe and JaeHyrk Park*,  
> “A quorum-based extended group mutual exclusion algorithm without unnecessary blocking,”  
> *Proceedings of the 15th International Conference on Parallel and Distributed Systems (ICPADS 2009).*

---

## Author

Developed as an academic simulation of quorum-based distributed coordination in MPI.

