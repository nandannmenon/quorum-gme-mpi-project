# Quorum-Based Extended Group Mutual Exclusion in MPI

This project is a **C + MPI implementation** of the distributed algorithm from the paper:

> **"A quorum-based extended group mutual exclusion algorithm without unnecessary blocking"**  
> by *Yoshifumi Manabe* and *JaeHyrk Park*

It simulates a distributed system where multiple **Requester** processes attempt to access a shared critical section (CS).  
Access is coordinated by a set of **Manager** processes using Lamport clocks and a quorum-based protocol.

---

## Features

- **Group Mutual Exclusion**  
  Multiple processes from the **same group** may enter the CS at the same time.

- **Mutual Exclusion Between Groups**  
  Processes from **different groups** cannot be in the CS simultaneously.

- **Two-Phase (“Release / Finished / Over”) Protocol**  
  Ensures the CS is not reassigned until *all members* of the currently-entering group have exited.

- **Lamport Logical Clocks**  
  Each request gets a timestamp ensuring a **globally consistent priority order**.

- **Deadlock-Free**  
  Late or stale (old timestamp) replies are ignored, preventing message confusion or deadlock.

- **Starvation-Free**  
  Managers maintain a priority queue based on `(timestamp, rank)`, always granting access to the oldest request.

- **Cancel / Cancelled Protocol**  
  A higher-priority request can revoke an OK previously given to a lower-priority one ― matching the paper's logic.

---

## How to Run

### Prerequisites

- A C compiler (`gcc`, `clang`, etc.)
- An MPI implementation (e.g., **OpenMPI**, **MPICH**)

---

### 1. Compile the Program

```bash
mpicc -o gme_mpi gme_mpi.c
```

---

### 2. Run with MPI

Example using 6 processes:

```bash
mpirun -n 6 ./gme_mpi
```

> You can change `-n` to test different combinations of managers and requesters.

---

## Example Scenarios

### **Scenario 1 — Concurrency inside the same group**

```bash
mpirun -n 5 ./gme_mpi
```

**Setup**  
- 3 Managers  
- 2 Requesters in group 0 → R3[g=0], R4[g=0]

**Expected**  
Both may enter CS together:  
- One becomes pivot (gets OK + LOCK)  
- Second receives `ENTER` from managers  

---

### **Scenario 2 — Mutual exclusion between groups**

```bash
mpirun -n 6 ./gme_mpi
```

**Setup**  
- 3 Managers  
- Requesters:  
  - R3 → group 0  
  - R4 → group 0  
  - R5 → group 1  

**Expected**  
- R3 & R4 enter CS together  
- R5 waits until R3 and R4 are completely done  
- Only after `OVER` messages are processed does R5 enter

---

### **Scenario 3 — Cancel Protocol**

**Trigger**  
A higher-priority requester arrives after a manager has already sent `OK` to a lower-priority one.

**Expected Log Events**  
- Manager sends: `CANCEL`  
- Requester replies: `CANCELLED`  
- Manager reissues `OK` to the higher-priority request

This ensures fairness and prevents circular waits.

---

## Message Flow Summary

### Normal Request → Enter → Release → Over Cycle

```
Requester (Ri)              Manager (Mj)
     |                           |
     | ---- REQUEST -----------> |
     | <-------- OK ------------ |
     | ---- LOCK / ENTER ----->  |
     | <-------- ENTER --------- |
     | ---- RELEASE -----------> |
     | <-------- FINISHED ------ |
     | <-------- OVER ---------- |
```

---

### Cancel Protocol

```
Low-Priority R5            Manager M0            High-Priority R3
      |                         |                       |
      | ---- REQUEST(5) ---->   |                       |
      | <---- OK -------------  |                       |
      |                         |<---- REQUEST(3) ----- |
      |                         |---- CANCEL ---------->|
      | <---- CANCELLED ------  |                       |
```

---

## Message Types Implemented

- `REQUEST`  
- `OK`  
- `LOCK`  
- `ENTER`  
- `RELEASE`  
- `NONEED`  
- `FINISHED`  
- `OVER`  
- `CANCEL`  
- `CANCELLED`

All messages include Lamport timestamps, requester rank, group information, and group-set bitmasks.

---

## Reference

**Paper:**  
*Yoshifumi Manabe and JaeHyrk Park*  
> “A quorum-based extended group mutual exclusion algorithm without unnecessary blocking.”  
> *ICPADS 2004.*

---

## Author

Developed as part of a distributed systems academic project demonstrating quorum-based coordination and extended group mutual exclusion in MPI.
