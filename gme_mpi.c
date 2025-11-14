#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

/* colors */
#define CLR_MGR   "\033[1;33m"
#define CLR_REQ   "\033[1;36m"
#define CLR_CS    "\033[1;32m"
#define CLR_ERR   "\033[1;31m"
#define CLR_ST    "\033[1;35m"
#define CLR_RST   "\033[0m"

#define NUM_MANAGERS 3
#define NUM_GROUPS 2
#define MAX_QUEUE 128
#define SIM_SECONDS 5.0

/* message tags */
enum { TAG_REQUEST, TAG_OK, TAG_LOCK, TAG_ENTER,
       TAG_RELEASE, TAG_NONEED, TAG_CANCEL,
       TAG_CANCELLED, TAG_FINISHED, TAG_OVER };

/* message payload */
typedef struct {
    int timestamp;
    int rank;
    bool gset[NUM_GROUPS];
    int group;
} Msg;

/* manager/requester states */
typedef enum { M_VACANT, M_WAITLOCK, M_LOCKED, M_RELEASING, M_WAITCANCEL } MState;
typedef enum { R_IDLE, R_WAIT, R_IN, R_OUT } RState;

/* helpers */
static inline int max2(int a, int b) { return a > b ? a : b; }
static inline int higher(int ts1, int r1, int ts2, int r2) {
    if (ts1 < ts2) return 1;
    if (ts1 > ts2) return 0;
    return r1 < r2;
}

/* coterie */
#define COT_SIZE 3
#define QSIZE 2
static const int COT[COT_SIZE][QSIZE] = { {0,1}, {1,2}, {0,2} };

/* queue utilities */
static int best_idx(Msg *q, int n) {
    if (n == 0) return -1;
    int b = 0;
    for (int i = 1; i < n; ++i)
        if (higher(q[i].timestamp, q[i].rank, q[b].timestamp, q[b].rank)) b = i;
    return b;
}
static Msg pop_index(Msg *q, int *n, int i) {
    Msg out = q[i];
    q[i] = q[*n - 1];
    --(*n);
    return out;
}
static void insert_pri(Msg *q, int *n, Msg m) {
    if (*n >= MAX_QUEUE) return;
    int i = *n - 1;
    while (i >= 0) {
        if (higher(q[i].timestamp, q[i].rank, m.timestamp, m.rank)) {
            q[i + 1] = q[i];
            --i;
        } else break;
    }
    q[i + 1] = m;
    ++(*n);
}

/* small sets */
static int in_set(int *s, int n, int x) {
    for (int i = 0; i < n; ++i) if (s[i] == x) return 1;
    return 0;
}
static int remove_set(int *s, int *n, int x) {
    for (int i = 0; i < *n; ++i) {
        if (s[i] == x) { s[i] = s[*n - 1]; --(*n); return 1; }
    }
    return 0;
}

/* pretty printing of group set */
static void fmt_gset(char *buf, bool gset[NUM_GROUPS]) {
    buf[0] = '\0';
    strcat(buf, "{");
    for (int i = 0; i < NUM_GROUPS; ++i) {
        if (gset[i]) {
            char t[8];
            snprintf(t, sizeof(t), " g%d", i);
            strcat(buf, t);
        }
    }
    strcat(buf, " }");
}

/* print queue (manager) */
static void print_queue_int(int mgr, Msg *q, int qn) {
    printf(CLR_MGR "[mgr %d] queue:", mgr);
    if (qn == 0) { printf(" <empty>" CLR_RST "\n"); fflush(stdout); return; }
    for (int i = 0; i < qn; ++i) {
        printf(" (r%d,ts=%d)", q[i].rank, q[i].timestamp);
    }
    printf(CLR_RST "\n");
    fflush(stdout);
}

/**************************************************************************
 * manager role - high-detail logging
 **************************************************************************/
void manager_role(int rank, MPI_Datatype M) {
    MState state = M_VACANT;
    int lamport = 0;

    int gm = -1;
    bool gmset[NUM_GROUPS] = {0};
    int pivot = -1;
    int pivot_ts = -1;

    Msg queue[MAX_QUEUE];
    int qn = 0;

    Msg ok_sent; ok_sent.rank = -1; ok_sent.timestamp = -1;
    int followers[MAX_QUEUE]; int fn = 0;

    printf(CLR_MGR "[mgr %d] starting manager role\n" CLR_RST, rank); fflush(stdout);
    double start = MPI_Wtime();

    while (1) {
        if (MPI_Wtime() - start >= SIM_SECONDS) {
            printf(CLR_MGR "[mgr %d] sim time elapsed -> exiting\n" CLR_RST, rank);
            fflush(stdout);
            break;
        }

        MPI_Status st; int flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &st);
        if (!flag) { usleep(3000); continue; }

        Msg msg;
        MPI_Recv(&msg, 1, M, st.MPI_SOURCE, st.MPI_TAG, MPI_COMM_WORLD, &st);
        int src = st.MPI_SOURCE;
        int tag = st.MPI_TAG;
        lamport = max2(lamport, msg.timestamp) + 1;

        /* detailed receipt log */
        char gbuf[32]; fmt_gset(gbuf, msg.gset);
        printf(CLR_MGR "[mgr %d] recv tag=%d from %d (msg.ts=%d, gset=%s) state=%d lam=%d\n" CLR_RST,
               rank, tag, src, msg.timestamp, gbuf, state, lamport);
        fflush(stdout);

        switch (tag) {

            case TAG_REQUEST: {
                /* insert and log queue */
                insert_pri(queue, &qn, msg);
                printf(CLR_MGR "[mgr %d] inserted request (r%d,ts=%d) -> queue size=%d\n" CLR_RST,
                       rank, msg.rank, msg.timestamp, qn);
                print_queue_int(rank, queue, qn);

                /* vacancy -> send OK to highest priority */
                if (state == M_VACANT) {
                    int idx = best_idx(queue, qn);
                    if (idx >= 0) {
                        Msg sel = pop_index(queue, &qn, idx);
                        ok_sent = sel;
                        Msg ok; ok.timestamp = sel.timestamp; ok.rank = rank;
                        memcpy(ok.gset, sel.gset, sizeof(ok.gset)); ok.group = -1;
                        ++lamport;
                        MPI_Send(&ok, 1, M, sel.rank, TAG_OK, MPI_COMM_WORLD);
                        printf(CLR_MGR "[mgr %d] send OK -> r%d (ok.ts=%d) lam=%d\n" CLR_RST,
                               rank, sel.rank, sel.timestamp, lamport);
                        fflush(stdout);
                        state = M_WAITLOCK;
                    }
                }
                /* waitlock cancellation check */
                else if (state == M_WAITLOCK && ok_sent.rank >= 0) {
                    if (higher(msg.timestamp, msg.rank, ok_sent.timestamp, ok_sent.rank)) {
                        Msg c; c.timestamp = ok_sent.timestamp; c.rank = rank; c.group = -1;
                        ++lamport;
                        MPI_Send(&c, 1, M, ok_sent.rank, TAG_CANCEL, MPI_COMM_WORLD);
                        printf(CLR_MGR CLR_ERR "[mgr %d] sent CANCEL -> r%d (old.ts=%d) lam=%d\n" CLR_RST,
                               rank, ok_sent.rank, ok_sent.timestamp, lamport);
                        fflush(stdout);
                        state = M_WAITCANCEL;
                    }
                }

                /* if locked and allowed, send ENTERs */
                if (state == M_LOCKED && state != M_RELEASING && state != M_WAITCANCEL) {
                    int i = 0;
                    while (i < qn) {
                        bool compatible = queue[i].gset[gm];
                        bool outranks_pivot = higher(queue[i].timestamp, queue[i].rank, pivot_ts, pivot);
                        if (compatible && !outranks_pivot) {
                            Msg ent; ent.timestamp = queue[i].timestamp; ent.rank = rank;
                            ent.group = gm; memcpy(ent.gset, gmset, sizeof(gmset));
                            ++lamport;
                            MPI_Send(&ent, 1, M, queue[i].rank, TAG_ENTER, MPI_COMM_WORLD);
                            printf(CLR_MGR "[mgr %d] sent ENTER -> r%d group=%d (ent.ts=%d) lam=%d\n" CLR_RST,
                                   rank, queue[i].rank, gm, ent.timestamp, lamport);
                            fflush(stdout);

                            if (!in_set(followers, fn, queue[i].rank)) followers[fn++] = queue[i].rank;
                            pop_index(queue, &qn, i);
                            continue;
                        }
                        ++i;
                    }
                }
                break;
            }

            case TAG_LOCK: {
                /* pivot announces lock */
                gm = msg.group; memcpy(gmset, msg.gset, sizeof(gmset));
                pivot = msg.rank; pivot_ts = msg.timestamp;
                ok_sent.rank = -1;
                state = M_LOCKED;
                fn = 0;

                printf(CLR_MGR "[mgr %d] LOCK from r%d group=%d ts=%d state->LOCKED\n" CLR_RST,
                       rank, pivot, gm, pivot_ts);
                fflush(stdout);
                print_queue_int(rank, queue, qn);

                /* send ENTER to queued compatible requests (if allowed) */
                if (state != M_RELEASING && state != M_WAITCANCEL) {
                    Msg tmp[MAX_QUEUE]; int tn = 0;
                    for (int i = 0; i < qn; ++i) {
                        bool compatible = queue[i].gset[gm];
                        bool outranks_pivot = higher(queue[i].timestamp, queue[i].rank, pivot_ts, pivot);
                        if (compatible && !outranks_pivot) {
                            Msg ent = { queue[i].timestamp, rank, {0}, gm };
                            memcpy(ent.gset, gmset, sizeof(gmset));
                            ++lamport;
                            MPI_Send(&ent, 1, M, queue[i].rank, TAG_ENTER, MPI_COMM_WORLD);
                            printf(CLR_MGR "[mgr %d] sent ENTER -> r%d group=%d (ent.ts=%d) lam=%d\n" CLR_RST,
                                   rank, queue[i].rank, gm, ent.timestamp, lamport);
                            fflush(stdout);
                            if (!in_set(followers, fn, queue[i].rank)) followers[fn++] = queue[i].rank;
                        } else {
                            tmp[tn++] = queue[i];
                        }
                    }
                    qn = tn;
                    for (int i = 0; i < qn; ++i) queue[i] = tmp[i];
                    print_queue_int(rank, queue, qn);
                }
                break;
            }

            case TAG_RELEASE: {
                /* pivot begins releasing */
                state = M_RELEASING;
                pivot = src;
                pivot_ts = msg.timestamp;
                printf(CLR_MGR "[mgr %d] RELEASE from r%d ts=%d state->RELEASING\n" CLR_RST,
                       rank, pivot, pivot_ts);
                fflush(stdout);

                if (fn == 0) {
                    Msg fin = { pivot_ts, rank, {0}, -1 };
                    ++lamport;
                    MPI_Send(&fin, 1, M, pivot, TAG_FINISHED, MPI_COMM_WORLD);
                    printf(CLR_MGR "[mgr %d] no followers -> FINISHED to r%d (ts=%d) lam=%d\n" CLR_RST,
                           rank, pivot, pivot_ts, lamport);
                    fflush(stdout);
                }
                break;
            }

            case TAG_NONEED: {
                /* follower indicates no need */
                printf(CLR_MGR "[mgr %d] NONEED from r%d (msg.ts=%d)\n" CLR_RST, rank, src, msg.timestamp);
                fflush(stdout);
                remove_set(followers, &fn, src);
                printf(CLR_MGR "[mgr %d] follower removed -> remaining=%d\n" CLR_RST, rank, fn);
                fflush(stdout);

                if (state == M_RELEASING && fn == 0 && pivot >= 0) {
                    Msg fin = { pivot_ts, rank, {0}, -1 };
                    ++lamport;
                    MPI_Send(&fin, 1, M, pivot, TAG_FINISHED, MPI_COMM_WORLD);
                    printf(CLR_MGR "[mgr %d] all followers done -> FINISHED to r%d lam=%d\n" CLR_RST,
                           rank, pivot, lamport);
                    fflush(stdout);
                }

                /* cancellation match */
                if (state == M_WAITCANCEL && ok_sent.rank == src && ok_sent.timestamp == msg.timestamp) {
                    ok_sent.rank = -1;
                    state = M_VACANT;
                    printf(CLR_MGR "[mgr %d] NONEED matched cancelled ok -> VACANT\n" CLR_RST, rank);
                    fflush(stdout);

                    int i = best_idx(queue, qn);
                    if (i >= 0) {
                        Msg sel = pop_index(queue, &qn, i);
                        ok_sent = sel;
                        Msg ok = { sel.timestamp, rank, {0}, -1 };
                        memcpy(ok.gset, sel.gset, sizeof(ok.gset));
                        ++lamport;
                        MPI_Send(&ok, 1, M, sel.rank, TAG_OK, MPI_COMM_WORLD);
                        printf(CLR_MGR "[mgr %d] send OK -> r%d (after noneed) lam=%d\n" CLR_RST,
                               rank, sel.rank, lamport);
                        fflush(stdout);
                        state = M_WAITLOCK;
                    }
                }
                break;
            }

            case TAG_CANCELLED: {
                printf(CLR_MGR "[mgr %d] CANCELLED ack from r%d\n" CLR_RST, rank, src); fflush(stdout);
                if (state == M_WAITCANCEL && ok_sent.rank == src) {
                    ok_sent.rank = -1;
                    state = M_VACANT;
                    int i = best_idx(queue, qn);
                    if (i >= 0) {
                        Msg sel = pop_index(queue, &qn, i);
                        ok_sent = sel;
                        Msg ok = { sel.timestamp, rank, {0}, -1 };
                        memcpy(ok.gset, sel.gset, sizeof(ok.gset));
                        ++lamport;
                        MPI_Send(&ok, 1, M, sel.rank, TAG_OK, MPI_COMM_WORLD);
                        printf(CLR_MGR "[mgr %d] send OK -> r%d (after cancelled) lam=%d\n" CLR_RST,
                               rank, sel.rank, lamport);
                        fflush(stdout);
                        state = M_WAITLOCK;
                    }
                }
                break;
            }

            case TAG_FINISHED: {
                /* unexpected for manager but log */
                printf(CLR_MGR "[mgr %d] unexpected FINISHED from %d (ignored)\n" CLR_RST, rank, src); fflush(stdout);
                break;
            }

            case TAG_OVER: {
                /* pivot completed cycle and informs managers */
                state = M_VACANT;
                gm = -1; pivot = -1; pivot_ts = -1; fn = 0; memset(gmset, 0, sizeof(gmset));
                printf(CLR_MGR "[mgr %d] OVER received -> VACANT\n" CLR_RST, rank); fflush(stdout);

                int i = best_idx(queue, qn);
                if (i >= 0) {
                    Msg sel = pop_index(queue, &qn, i);
                    ok_sent = sel;
                    Msg ok = { sel.timestamp, rank, {0}, -1 };
                    memcpy(ok.gset, sel.gset, sizeof(ok.gset));
                    ++lamport;
                    MPI_Send(&ok, 1, M, sel.rank, TAG_OK, MPI_COMM_WORLD);
                    printf(CLR_MGR "[mgr %d] send OK -> r%d (after over) lam=%d\n" CLR_RST,
                           rank, sel.rank, lamport);
                    fflush(stdout);
                    state = M_WAITLOCK;
                }
                break;
            }

            default:
                printf(CLR_ERR "[mgr %d] unknown tag %d from %d\n" CLR_RST, rank, tag, src); fflush(stdout);
                break;
        }
    }

    printf(CLR_MGR "[mgr %d] exiting manager\n" CLR_RST, rank); fflush(stdout);
}

/**************************************************************************
 * requester role - high-detail logs
 **************************************************************************/
void requester_role(int rank, MPI_Datatype M) {
    RState state = R_IDLE;
    int lamport = 0;
    int my_ts = 0;

    bool gset[NUM_GROUPS] = {0};
    if (rank == NUM_MANAGERS) gset[0] = true;
    else if (rank == NUM_MANAGERS + 1) { gset[0] = true; gset[1] = true; }
    else gset[1] = true;

    int quorum[QSIZE]; int qn = 0;
    int ok_count = 0; Msg ok_list[MAX_QUEUE]; int ok_ln = 0;
    int finished_count = 0;

    int mask = 0;
    for (int i = 0; i < NUM_GROUPS; ++i) if (gset[i]) mask |= (1 << i);

    printf(CLR_REQ "[req %d] starting requester role gset=", rank);
    for (int i = 0; i < NUM_GROUPS; ++i) if (gset[i]) printf(" g%d", i);
    printf(CLR_RST "\n"); fflush(stdout);

    double start = MPI_Wtime();

    while (1) {
        if (MPI_Wtime() - start >= SIM_SECONDS) {
            printf(CLR_REQ "[req %d] sim time elapsed -> exiting\n" CLR_RST, rank);
            fflush(stdout);
            break;
        }

        if (state == R_IDLE) {
            sleep(1);
            my_ts = ++lamport;
            ok_count = 0; ok_ln = 0; finished_count = 0;

            /* choose deterministic quorum */
            int chosen = (rank + mask) % COT_SIZE;
            qn = QSIZE;
            for (int i = 0; i < qn; ++i) quorum[i] = COT[chosen][i];

            Msg req; req.timestamp = my_ts; req.rank = rank; memcpy(req.gset, gset, sizeof(gset)); req.group = -1;

            printf(CLR_REQ "[req %d] state idle->wait request# ts=%d chosen_quorum=%d members={", rank, my_ts, chosen);
            for (int i = 0; i < qn; ++i) printf(" %d", quorum[i]);
            printf(" }\n" CLR_RST); fflush(stdout);

            for (int i = 0; i < qn; ++i) {
                ++lamport;
                req.timestamp = my_ts; /* message contains request timestamp */
                MPI_Send(&req, 1, M, quorum[i], TAG_REQUEST, MPI_COMM_WORLD);
                printf(CLR_REQ "[req %d] sent REQUEST(ts=%d) -> mgr %d lam=%d\n" CLR_RST,
                       rank, my_ts, quorum[i], lamport);
                fflush(stdout);
            }
            state = R_WAIT;
        }

        MPI_Status st; int flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &st);
        if (!flag) { usleep(3000); continue; }

        Msg msg;
        MPI_Recv(&msg, 1, M, st.MPI_SOURCE, st.MPI_TAG, MPI_COMM_WORLD, &st);
        int src = st.MPI_SOURCE;
        int tag = st.MPI_TAG;
        lamport = max2(lamport, msg.timestamp) + 1;

        char gbuf[32]; fmt_gset(gbuf, msg.gset);
        printf(CLR_REQ "[req %d] recv tag=%d from %d (msg.ts=%d gset=%s) state=%d lam=%d\n" CLR_RST,
               rank, tag, src, msg.timestamp, gbuf, state, lamport);
        fflush(stdout);

        if (state == R_WAIT) {
            /* ignore old replies */
            if (msg.timestamp != my_ts && (tag == TAG_OK || tag == TAG_ENTER || tag == TAG_CANCEL || tag == TAG_FINISHED)) {
                printf(CLR_REQ "[req %d] ignoring old reply tag=%d from %d (msg.ts=%d != my_ts=%d)\n" CLR_RST,
                       rank, tag, src, msg.timestamp, my_ts);
                fflush(stdout);
                continue;
            }

            if (tag == TAG_OK) {
                ok_list[ok_ln++] = msg;
                ++ok_count;
                printf(CLR_REQ "[req %d] OK from mgr %d (ok.ts=%d) (%d/%d)\n" CLR_RST,
                       rank, src, msg.timestamp, ok_count, qn);
                fflush(stdout);

                if (ok_count == qn) {
                    /* decide group (paper: arbitrary) */
                    int chosen_group = -1;
                    for (int g = 0; g < NUM_GROUPS; ++g) if (gset[g]) { chosen_group = g; break; }
                    if (chosen_group < 0) chosen_group = 0;

                    Msg lock; lock.timestamp = my_ts; lock.rank = rank; lock.group = chosen_group;
                    memcpy(lock.gset, gset, sizeof(gset));
                    ++lamport;
                    for (int i = 0; i < qn; ++i) {
                        MPI_Send(&lock, 1, M, quorum[i], TAG_LOCK, MPI_COMM_WORLD);
                        printf(CLR_REQ "[req %d] sent LOCK(group=%d,ts=%d) -> mgr %d lam=%d\n" CLR_RST,
                               rank, chosen_group, my_ts, quorum[i], lamport);
                        fflush(stdout);
                    }

                    printf(CLR_REQ "[req %d] pivot entering CS group=%d ts=%d\n" CLR_RST, rank, chosen_group, my_ts);
                    fflush(stdout);

                    state = R_IN;
                    printf(CLR_CS "[req %d] in-crit-section (pivot) start\n" CLR_RST, rank); fflush(stdout);
                    sleep(2);
                    printf(CLR_CS "[req %d] in-crit-section (pivot) end\n" CLR_RST, rank); fflush(stdout);

                    /* two-phase release */
                    Msg rel; rel.timestamp = my_ts; rel.rank = rank; rel.group = chosen_group;
                    ++lamport;
                    for (int i = 0; i < qn; ++i) {
                        MPI_Send(&rel, 1, M, quorum[i], TAG_RELEASE, MPI_COMM_WORLD);
                        printf(CLR_REQ "[req %d] sent RELEASE(ts=%d) -> mgr %d lam=%d\n" CLR_RST,
                               rank, my_ts, quorum[i], lamport);
                        fflush(stdout);
                    }
                    state = R_OUT;
                    finished_count = 0;
                }
            }

            else if (tag == TAG_ENTER) {
                int g = msg.group;
                int enter_ts = msg.timestamp;
                printf(CLR_REQ "[req %d] received ENTER from mgr %d grant group=%d (ent.ts=%d)\n" CLR_RST,
                       rank, src, g, enter_ts);
                fflush(stdout);

                /* notify all quorum members with NONEED using enter_ts */
                Msg nd; nd.timestamp = enter_ts; nd.rank = rank; nd.group = g;
                memcpy(nd.gset, gset, sizeof(gset));
                ++lamport;
                for (int i = 0; i < qn; ++i) {
                    MPI_Send(&nd, 1, M, quorum[i], TAG_NONEED, MPI_COMM_WORLD);
                    printf(CLR_REQ "[req %d] sent NONEED(ts=%d) -> mgr %d lam=%d\n" CLR_RST,
                           rank, nd.timestamp, quorum[i], lamport);
                    fflush(stdout);
                }

                /* follower enters CS immediately */
                state = R_IN;
                printf(CLR_CS "[req %d] in-crit-section (follower) start group=%d\n" CLR_RST, rank, g);
                fflush(stdout);
                sleep(2);
                printf(CLR_CS "[req %d] in-crit-section (follower) end group=%d\n" CLR_RST, rank, g);
                fflush(stdout);

                /* send NONEED again on exit */
                ++lamport;
                for (int i = 0; i < qn; ++i) {
                    MPI_Send(&nd, 1, M, quorum[i], TAG_NONEED, MPI_COMM_WORLD);
                    printf(CLR_REQ "[req %d] sent NONEED (exit ts=%d) -> mgr %d lam=%d\n" CLR_RST,
                           rank, nd.timestamp, quorum[i], lamport);
                    fflush(stdout);
                }

                state = R_IDLE;
            }

            else if (tag == TAG_CANCEL) {
                printf(CLR_ERR "[req %d] received CANCEL from mgr %d -> sending CANCELLED and retry\n" CLR_RST,
                       rank, src);
                fflush(stdout);

                Msg cancelled; cancelled.timestamp = my_ts; cancelled.rank = rank; cancelled.group = -1;
                ++lamport;
                MPI_Send(&cancelled, 1, M, src, TAG_CANCELLED, MPI_COMM_WORLD);
                printf(CLR_REQ "[req %d] sent CANCELLED -> mgr %d lam=%d\n" CLR_RST,
                       rank, src, lamport);
                fflush(stdout);

                /* retry later with new timestamp */
                state = R_IDLE;
                sleep(1);
            }
        }

        else if (state == R_OUT) {
            if (tag == TAG_FINISHED && msg.timestamp == my_ts) {
                ++finished_count;
                printf(CLR_REQ "[req %d] received FINISHED from mgr %d (%d/%d)\n" CLR_RST,
                       rank, src, finished_count, qn);
                fflush(stdout);

                if (finished_count == qn) {
                    Msg over; over.timestamp = my_ts; over.rank = rank; over.group = -1;
                    ++lamport;
                    for (int i = 0; i < qn; ++i) {
                        MPI_Send(&over, 1, M, quorum[i], TAG_OVER, MPI_COMM_WORLD);
                        printf(CLR_REQ "[req %d] sent OVER(ts=%d) -> mgr %d lam=%d\n" CLR_RST,
                               rank, my_ts, quorum[i], lamport);
                        fflush(stdout);
                    }
                    state = R_IDLE;
                }
            } else {
                printf(CLR_REQ "[req %d] ignoring tag=%d from %d in OUT state\n" CLR_RST, rank, tag, src); fflush(stdout);
            }
        } /* end R_OUT handling */
    } /* end loop */
}

/**************************************************************************
 * main
 **************************************************************************/
int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank, world;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);

    if (world <= NUM_MANAGERS) {
        if (rank == 0) fprintf(stderr, "need at least %d managers + 1 requester\n", NUM_MANAGERS);
        MPI_Finalize();
        return EXIT_FAILURE;
    }

    /* create MPI type for Msg */
    MPI_Datatype MT;
    int bl[4] = {1,1,NUM_GROUPS,1};
    MPI_Aint disp[4];
    MPI_Datatype ty[4] = {MPI_INT, MPI_INT, MPI_C_BOOL, MPI_INT};
    Msg tmp;
    MPI_Aint base;
    MPI_Get_address(&tmp, &base);
    MPI_Get_address(&tmp.timestamp, &disp[0]);
    MPI_Get_address(&tmp.rank, &disp[1]);
    MPI_Get_address(&tmp.gset[0], &disp[2]);
    MPI_Get_address(&tmp.group, &disp[3]);
    for (int i = 0; i < 4; ++i) disp[i] = MPI_Aint_diff(disp[i], base);

    MPI_Type_create_struct(4, bl, disp, ty, &MT);
    MPI_Type_commit(&MT);

    if (rank < NUM_MANAGERS) manager_role(rank, MT);
    else requester_role(rank, MT);

    MPI_Type_free(&MT);
    MPI_Finalize();
    return 0;
}
