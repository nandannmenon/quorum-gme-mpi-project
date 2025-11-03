# Extended Group Mutual Exclusion in MPI

This project is a **C and MPI implementation** of the distributed algorithm from the paper:

> **"A quorum-based extended group mutual exclusion algorithm without unnecessary blocking"**  
> by *Yoshifumi Manabe* and *JaeHyrk Park*

It simulates a distributed system where multiple **Requester** processes compete for access to a shared resource (a **critical section**).  
Access is managed by a set of **Manager** processes using a **quorum-based system**.

---

## Features

### Group Mutual Exclusion
Allows multiple processes **from the same group** to enter the critical section (CS) concurrently.

### Mutual Exclusion
Ensures **processes from different groups** cannot be in the CS simultaneously — one group must finish completely before another enters.

### Two-Phase Release
Implements the full **Release / Finished / Over** protocol, ensuring the lock is not released until all processes from an active group have exited the CS.

### Lamport Clocks
Uses **Lamport logical clocks** to assign a unique, total-order priority `(timestamp, rank)` to every request.

### Deadlock Prevention
Late-arriving (“zombie”) messages from old requests are safely ignored using timestamps, preventing deadlocks and desynchronization.

### Starvation-Freeness
Managers use a **priority queue**, servicing the request with the *lowest timestamp first* — ensuring fairness and preventing starvation.

### Cancel Protocol
Fully implements the **Cancel / Cancelled** logic, allowing a high-priority request to revoke an `OK` already sent to a lower-priority one.

---

## How to Run

### Prerequisites

- A C compiler (e.g., `gcc`)
- An MPI implementation (e.g., **Open MPI** or **MPICH**)

---

### 1. Compilation

Compile the program using the MPI compiler wrapper:

```bash
mpicc -o gme_mpi gme_mpi.c
```

---

### 2. Program Configuration

Program behavior is controlled by constants inside **`gme_mpi.c`**.

#### Managers

```c
#define NUM_MANAGERS 3
```

The first 3 processes (`rank 0, 1, 2`) always act as **Managers**.

#### Requesters

Group assignments are hardcoded in the `requester_role()` function:

| Rank | Group |
|------|--------|
| 3, 4 | 0 |
| 5+   | 1 |

---

## Example Scenarios

You can test different configurations by changing the number of processes (`-n`) in `mpirun`.

---

### Test 1: Concurrency (2 Requesters, 1 Group)

Proves that two requesters from the same group can enter the CS **concurrently**.

```bash
mpirun -n 5 ./gme_mpi
```

**Setup:**
- 3 Managers
- 2 Requesters → R3[g=0], R4[g=0]

**Expected behavior:**
- One requester enters as the *pivot*.
- The other joins with an `ENTER` message.
- Both log `ENTERING CS` before any `EXITING CS` messages appear.

---

### Test 2: Mutual Exclusion (3 Requesters, 2 Groups)

Proves that a requester from another group must **wait** until the current group fully exits.

```bash
mpirun -n 6 ./gme_mpi
```

**Setup:**
- 3 Managers
- 3 Requesters → R3[g=0], R4[g=0], R5[g=1]

**Expected behavior:**
- R3 and R4 (group 0) enter CS concurrently.
- R5 (group 1) sends `REQUEST(g=1)` → Enqueued & waits.
- Only after R3 and R4 finish (including sending `OVER`), R5 is dequeued and allowed to enter.

---

### Test 3: Cancel Logic

Demonstrates the **Cancel / Cancelled** protocol when competing requests race.

```bash
mpirun -n 6 ./gme_mpi
```

**What to look for:**
- A high-priority request (e.g., R3) arrives after an `OK` was sent to a lower-priority one (e.g., R5).
- Managers log:  
  `Sending CANCEL`
- Requester logs:  
  `My 'OK' was revoked`

This ensures consistency and prevents lower-priority processes from entering CS incorrectly.

---

## Message Flow Diagram

### Normal Request–Grant–Enter–Release Cycle

```text
Requester (Ri)             Manager (Mj)
     |                          |
     | ---- REQUEST(g, ts) ---> |
     |                          |
     | <-------- OK ----------  |
     |                          |
     | ---- ENTER ------------> |
     |                          |
     | <---- ENTER_ACK -------- |
     |                          |
     | --- RELEASE/FINISHED --> |
     |                          |
     | <-------- OVER --------- |
     |                          |
```

Multiple requesters from the **same group** may reach the `ENTER` stage concurrently.

---

### Cancel Protocol (When Higher Priority Request Arrives)

```text
Low-Priority R5           Manager M0           High-Priority R3
       |                       |                       |
       | ---- REQUEST(5) ----> |                       |
       | <---- OK -------------|                       |
       |                       |<---- REQUEST(3) ----- |
       |                       |---- CANCEL ---------->|
       | <---- CANCELLED ------|                       |
```

This ensures fairness: the **lower-priority** request (R5) backs off, and **R3** proceeds.

---

## Notes

- Implements all message types:  
  `REQUEST`, `OK`, `CANCEL`, `CANCELLED`, `ENTER`, `RELEASE`, `FINISHED`, `OVER`
- Uses Lamport timestamps `(clock, rank)` to order events globally.
- Logs each send/receive with rank and clock for reproducibility.
- Designed for educational and research use to visualize distributed coordination.

---

## Reference

**Paper:**  
*Yoshifumi Manabe and JaeHyrk Park*,  
> “A quorum-based extended group mutual exclusion algorithm without unnecessary blocking,”  
> *Proceedings of the 15th International Conference on Parallel and Distributed Systems (ICPADS 2009).*

---

## Author

Developed as an academic simulation of quorum-based distributed coordination in MPI.

---
