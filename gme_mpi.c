/*
 * MPI IMPLEMENTATION OF:
 * A quorum-based extended group mutual exclusion algorithm
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()

// --- 1. Configuration ---
#define NUM_MANAGERS 3
#define NUM_GROUPS 2
#define MAX_QUEUE_SIZE 10

// --- 2. Message Tags ---
typedef enum {
    TAG_REQUEST, TAG_OK, TAG_LOCK, TAG_ENTER,
    TAG_RELEASE, TAG_NONEED, 
    TAG_CANCEL,    
    TAG_CANCELLED, 
    TAG_FINISHED, TAG_OVER
} MessageTag;

// --- 3. Message Payload Struct ---
typedef struct {
    int timestamp; // This is the ID/priority of the request
    int rank;
    int group;
} MsgData;

// --- 4. Manager Logic ---

typedef enum { M_VACANT, M_WAITLOCK, M_LOCKED, M_RELEASING, M_WAITCANCEL } ManagerStatus;

int max(int a, int b) { return (a > b) ? a : b; }

// (find_highest_priority - no changes)
int find_highest_priority(MsgData* queue, int count) {
    if (count == 0) return -1;
    int min_ts = queue[0].timestamp;
    int min_idx = 0;
    for (int i = 1; i < count; i++) {
        if (queue[i].timestamp < min_ts) {
            min_ts = queue[i].timestamp;
            min_idx = i;
        } else if (queue[i].timestamp == min_ts && queue[i].rank < queue[min_idx].rank) {
            min_ts = queue[i].timestamp;
            min_idx = i;
        }
    }
    return min_idx;
}

// (dequeue_priority_item - no changes)
MsgData dequeue_priority_item(MsgData* queue, int* count, int idx) {
    MsgData req = queue[idx];
    queue[idx] = queue[*count - 1];
    (*count)--;
    return req;
}

// (try_service_queue - no changes)
void try_service_queue(int rank, ManagerStatus* status, MsgData* queue, 
                       int* queue_count, MsgData* sentok_to, 
                       int* lamport_clock, MPI_Datatype mpi_msg_type) 
{
    if (*status == M_VACANT && *queue_count > 0) {
        int prio_idx = find_highest_priority(queue, *queue_count);
        MsgData next_req = dequeue_priority_item(queue, queue_count, prio_idx);
        printf("[Manager %d]: Status is VACANT. Dequeuing priority request from %d (ts %d). (Queue size: %d)\n",
               rank, next_req.rank, next_req.timestamp, *queue_count);
        *sentok_to = next_req;
        MsgData ok_msg;
        ok_msg.rank = rank;
        ok_msg.timestamp = next_req.timestamp;
        (*lamport_clock)++;
        MPI_Send(&ok_msg, 1, mpi_msg_type, next_req.rank, TAG_OK, MPI_COMM_WORLD);
        *status = M_WAITLOCK;
    }
}


// (manager_role - no changes from Step 8)
void manager_role(int rank, int world_size, MPI_Datatype mpi_msg_type) {
    ManagerStatus status = M_VACANT;
    int current_group = -1;
    int lamport_clock = 0;
    MsgData sentok_to;
    sentok_to.timestamp = -1;
    sentok_to.rank = -1;
    int using_set[MAX_QUEUE_SIZE];
    int using_count = 0;
    int pivot_releaser_rank = -1;
    MsgData request_queue[MAX_QUEUE_SIZE];
    int queue_count = 0;
    int group_queue[MAX_QUEUE_SIZE];
    printf("[Manager %d]: Online.\n", rank);

    while (1) {
        MsgData msg;
        int source_rank;
        int tag;
        MPI_Status mpi_status;
        MPI_Recv(&msg, 1, mpi_msg_type, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpi_status);
        source_rank = mpi_status.MPI_SOURCE;
        tag = mpi_status.MPI_TAG;
        lamport_clock = max(lamport_clock, msg.timestamp) + 1;

        switch (tag) {
            case TAG_REQUEST:
            {
                printf("[Manager %d]: Received REQUEST(g=%d) from %d (ts %d).\n", 
                       rank, msg.group, source_rank, msg.timestamp);
                if (status == M_WAITLOCK) {
                    int new_has_priority = 0;
                    if (msg.timestamp < sentok_to.timestamp) {
                        new_has_priority = 1;
                    } else if (msg.timestamp == sentok_to.timestamp && msg.rank < sentok_to.rank) {
                        new_has_priority = 1;
                    }
                    if (new_has_priority) {
                        printf("[Manager %d]: New request from %d (ts %d) has priority over %d (ts %d). Sending CANCEL.\n",
                               rank, msg.rank, msg.timestamp, sentok_to.rank, sentok_to.timestamp);
                        MsgData cancel_msg;
                        cancel_msg.rank = rank;
                        cancel_msg.timestamp = sentok_to.timestamp;
                        lamport_clock++;
                        MPI_Send(&cancel_msg, 1, mpi_msg_type, sentok_to.rank, TAG_CANCEL, MPI_COMM_WORLD);
                        status = M_WAITCANCEL;
                    }
                }
                if (queue_count >= MAX_QUEUE_SIZE) { break; }
                request_queue[queue_count] = msg;
                group_queue[queue_count] = msg.group;
                queue_count++;
                if (status != M_LOCKED) {
                    printf("[Manager %d]: Enqueued request from %d. (Queue size: %d)\n", 
                           rank, source_rank, queue_count);
                }
                if (status == M_LOCKED && msg.group == current_group) {
                    queue_count--;
                    printf("[Manager %d]: Status is LOCKED(g=%d) and groups match. Sending ENTER to %d.\n",
                           rank, current_group, source_rank);
                    MsgData enter_msg;
                    enter_msg.rank = rank;
                    enter_msg.group = current_group;
                    enter_msg.timestamp = msg.timestamp;
                    lamport_clock++;
                    MPI_Send(&enter_msg, 1, mpi_msg_type, source_rank, TAG_ENTER, MPI_COMM_WORLD);
                    using_set[using_count++] = source_rank;
                }
                try_service_queue(rank, &status, request_queue, 
                                  &queue_count, &sentok_to, 
                                  &lamport_clock, mpi_msg_type);
                break;
            } 
            case TAG_LOCK:
                current_group = msg.group;
                printf("[Manager %d]: Received LOCK(g=%d) from %d (ts %d).\n", 
                       rank, current_group, source_rank, msg.timestamp);
                if (status == M_WAITLOCK && source_rank == sentok_to.rank && msg.timestamp == sentok_to.timestamp) {
                    status = M_LOCKED;
                    sentok_to.rank = -1;
                } else if (status == M_WAITCANCEL && source_rank == sentok_to.rank) {
                    printf("[Manager %d]: WARNING: Received LOCK from %d after sending CANCEL. Honoring LOCK.\n", rank, source_rank);
                    status = M_LOCKED;
                    sentok_to.rank = -1;
                }
                int new_queue_count = 0;
                MsgData new_request_queue[MAX_QUEUE_SIZE];
                int new_group_queue[MAX_QUEUE_SIZE];
                for (int i = 0; i < queue_count; i++) {
                    MsgData queued_req = request_queue[i];
                    if (queued_req.group == current_group) {
                        printf("[Manager %d]: Found compatible waiter %d (ts %d) in queue. Sending ENTER(g=%d).\n",
                               rank, queued_req.rank, queued_req.timestamp, current_group);
                        MsgData enter_msg;
                        enter_msg.rank = rank;
                        enter_msg.group = current_group;
                        enter_msg.timestamp = queued_req.timestamp;
                        lamport_clock++;
                        MPI_Send(&enter_msg, 1, mpi_msg_type, queued_req.rank, TAG_ENTER, MPI_COMM_WORLD);
                        using_set[using_count++] = queued_req.rank;
                    } else {
                        new_request_queue[new_queue_count] = queued_req;
                        new_group_queue[new_queue_count] = group_queue[i];
                        new_queue_count++;
                    }
                }
                queue_count = new_queue_count;
                for(int i=0; i < queue_count; i++) {
                    request_queue[i] = new_request_queue[i];
                    group_queue[i] = new_group_queue[i];
                }
                break;
            case TAG_RELEASE:
                printf("[Manager %d]: Received RELEASE from pivot %d (ts %d). Entering RELEASING state.\n", 
                       rank, source_rank, msg.timestamp);
                status = M_RELEASING;
                pivot_releaser_rank = source_rank;
                if (using_count == 0) {
                    printf("[Manager %d]: `using_set` is empty. Sending FINISHED to %d.\n", 
                           rank, pivot_releaser_rank);
                    MsgData finish_msg;
                    finish_msg.rank = rank;
                    finish_msg.timestamp = msg.timestamp;
                    lamport_clock++;
                    MPI_Send(&finish_msg, 1, mpi_msg_type, pivot_releaser_rank, TAG_FINISHED, MPI_COMM_WORLD);
                }
                break;
            case TAG_NONEED:
                printf("[Manager %d]: Received NONEED from %d (ts %d).\n", rank, source_rank, msg.timestamp);
                if (status == M_WAITCANCEL && sentok_to.rank == source_rank && sentok_to.timestamp == msg.timestamp) {
                    printf("[Manager %d]: IGNORED NONEED from %d, was expecting CANCELLED.\n", rank, source_rank);
                }
                for (int i = 0; i < using_count; i++) {
                    if (using_set[i] == source_rank) {
                        using_set[i] = using_set[using_count - 1];
                        using_count--;
                        break;
                    }
                }
                if (status == M_RELEASING && using_count == 0) {
                    printf("[Manager %d]: `using_set` is now empty. Sending FINISHED to pivot %d.\n", 
                           rank, pivot_releaser_rank);
                    MsgData finish_msg;
                    finish_msg.rank = rank;
                    finish_msg.timestamp = lamport_clock; 
                    lamport_clock++;
                    MPI_Send(&finish_msg, 1, mpi_msg_type, pivot_releaser_rank, TAG_FINISHED, MPI_COMM_WORLD);
                }
                break;
            case TAG_CANCELLED:
                printf("[Manager %d]: Received CANCELLED from %d (for ts %d).\n", rank, source_rank, msg.timestamp);
                if (status == M_WAITCANCEL && source_rank == sentok_to.rank) {
                    sentok_to.rank = -1;
                    status = M_VACANT;
                    try_service_queue(rank, &status, request_queue, 
                                      &queue_count, &sentok_to, 
                                      &lamport_clock, mpi_msg_type);
                }
                break;
            case TAG_OVER:
                printf("[Manager %d]: Received OVER from pivot %d (ts %d). Setting status to VACANT.\n", 
                       rank, source_rank, msg.timestamp);
                status = M_VACANT;
                pivot_releaser_rank = -1;
                current_group = -1;
                try_service_queue(rank, &status, request_queue, 
                                  &queue_count, &sentok_to, 
                                  &lamport_clock, mpi_msg_type);
                break;
            default:
                printf("[Manager %d]: Received unhandled tag %d from %d.\n", rank, tag, source_rank);
        }
    }
}

// --- 5. Requester Logic ---

typedef enum { R_IDLE, R_WAIT, R_IN_CS, R_OUT_CS } RequesterStatus;

void requester_role(int rank, int world_size, MPI_Datatype mpi_msg_type) {
    RequesterStatus status = R_IDLE;
    int ok_replies = 0;
    int finished_replies = 0;
    int lamport_clock = 0;
    int my_timestamp = 0; 
    int quorum_size = (int)(NUM_MANAGERS / 2) + 1;
    int quorum[quorum_size];
    for(int i = 0; i < quorum_size; i++) {
        quorum[i] = i; 
    }
    int my_group;
    if (rank == 3 || rank == 4) {
        my_group = 0;
    } else {
        my_group = 1;
    }
    printf("[Requester %d]: Online. My group is %d. My quorum is %d managers.\n", 
           rank, my_group, quorum_size);

    while (1) {
        if (status == R_IDLE) {
            printf("[Requester %d]: Want to enter CS for group %d.\n", rank, my_group);
            lamport_clock++;
            my_timestamp = lamport_clock;
            status = R_WAIT;
            ok_replies = 0;
            finished_replies = 0;
            MsgData request_msg;
            request_msg.rank = rank;
            request_msg.group = my_group;
            request_msg.timestamp = my_timestamp;
            for (int i = 0; i < quorum_size; i++) {
                MPI_Send(&request_msg, 1, mpi_msg_type, quorum[i], TAG_REQUEST, MPI_COMM_WORLD);
            }
            printf("[Requester %d]: Sent REQUEST with ts %d.\n", rank, my_timestamp);
        }
        
        MsgData msg;
        int source;
        int tag;
        MPI_Status mpi_status;
        MPI_Recv(&msg, 1, mpi_msg_type, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpi_status);
        source = mpi_status.MPI_SOURCE;
        tag = mpi_status.MPI_TAG;
        lamport_clock = max(lamport_clock, msg.timestamp) + 1;
        
        // --- THIS IS THE FINAL FIX ---
        // We *only* handle messages based on our current state.
        
        if (status == R_WAIT) {
            // --- Zombie message filter ---
            if (msg.timestamp != my_timestamp) {
                if (tag == TAG_OK || tag == TAG_ENTER || tag == TAG_CANCEL) {
                    printf("[Requester %d]: In R_WAIT, IGNORING ZOMBIE MSG (tag %d) from %d (ts %d != my_ts %d)\n",
                           rank, tag, source, msg.timestamp, my_timestamp);
                    continue; // Discard
                }
            }

            if (tag == TAG_OK) {
                printf("[Requester %d]: Received OK from %d (for ts %d).\n", rank, source, msg.timestamp);
                ok_replies++;
                
                if (ok_replies == quorum_size) {
                    printf("[Requester %d]: Got all %d OKs. Sending LOCK(g=%d).\n", 
                           rank, quorum_size, my_group);
                    MsgData lock_msg;
                    lock_msg.rank = rank;
                    lock_msg.group = my_group;
                    lock_msg.timestamp = my_timestamp;
                    lamport_clock++;
                    for (int i = 0; i < quorum_size; i++) {
                        MPI_Send(&lock_msg, 1, mpi_msg_type, quorum[i], TAG_LOCK, MPI_COMM_WORLD);
                    }
                    printf("\n>>>>>>>>>> [Requester %d]: ENTERING CS (as pivot for g=%d)\n\n", rank, my_group);
                    status = R_IN_CS;
                    sleep(2);
                    printf("\n<<<<<<<<<< [Requester %d]: EXITING CS (as pivot for g=%d)\n\n", rank, my_group);
                    status = R_OUT_CS;
                    MsgData release_msg;
                    release_msg.rank = rank;
                    release_msg.timestamp = my_timestamp;
                    printf("[Requester %d]: Sending RELEASE.\n", rank);
                    lamport_clock++;
                    for (int i = 0; i < quorum_size; i++) {
                        MPI_Send(&release_msg, 1, mpi_msg_type, quorum[i], TAG_RELEASE, MPI_COMM_WORLD);
                    }
                    printf("[Requester %d]: Waiting for FINISHED from %d managers...\n", rank, quorum_size);
                }
            
            } else if (tag == TAG_ENTER) {
                int entered_group = msg.group;
                printf("[Requester %d]: Received ENTER(g=%d) from %d (for ts %d).\n", 
                       rank, entered_group, source, msg.timestamp);
                status = R_IN_CS; 
                printf("\n>>>>>>>>>> [Requester %d]: ENTERING CS (via Enter for g=%d)\n\n", rank, entered_group);
                sleep(2);
                printf("\n<<<<<<<<<< [Requester %d]: EXITING CS (via Enter for g=%d)\n\n", rank, entered_group);
                MsgData noneed_msg;
                noneed_msg.rank = rank;
                noneed_msg.timestamp = my_timestamp;
                printf("[Requester %d]: Sending NONEED to all quorum members.\n", rank);
                lamport_clock++;
                for (int i = 0; i < quorum_size; i++) {
                    MPI_Send(&noneed_msg, 1, mpi_msg_type, quorum[i], TAG_NONEED, MPI_COMM_WORLD);
                }
                status = R_IDLE;
            
            // --- FIX: `CANCEL` is now handled *inside* R_WAIT ---
            } else if (tag == TAG_CANCEL) {
                printf("[Requester %d]: Received CANCEL from %d (for ts %d). My 'OK' was revoked.\n",
                       rank, source, msg.timestamp);
                
                MsgData cancelled_msg;
                cancelled_msg.rank = rank;
                cancelled_msg.timestamp = my_timestamp;
                
                lamport_clock++;
                MPI_Send(&cancelled_msg, 1, mpi_msg_type, source, TAG_CANCELLED, MPI_COMM_WORLD);
                
                // Reset state to wait for new OKs
                status = R_WAIT;
                ok_replies = 0;
            }
        
        } else if (status == R_OUT_CS) {
            // --- Pivot's "wait for finished" state ---
            if (tag == TAG_FINISHED) {
                printf("[Requester %d]: Received FINISHED from %d.\n", rank, source);
                finished_replies++;
                
                if (finished_replies == quorum_size) {
                    printf("[Requester %d]: Got all FINISHED. Sending OVER.\n", rank);
                    MsgData over_msg;
                    over_msg.rank = rank;
                    over_msg.timestamp = my_timestamp;
                    lamport_clock++;
                    for (int i = 0; i < quorum_size; i++) {
                        MPI_Send(&over_msg, 1, mpi_msg_type, quorum[i], TAG_OVER, MPI_COMM_WORLD);
                    }
                    status = R_IDLE;
                }
            } else {
                // --- FIX: We are in R_OUT_CS, so we *ignore* any late-arriving CANCEL ---
                printf("[Requester %d]: In R_OUT_CS, ignoring tag %d from %d (ts %d). My commit won.\n", 
                       rank, tag, source, msg.timestamp);
            }
        
        } else if (status == R_IDLE) {
            printf("[Requester %d]: In R_IDLE, ignoring tag %d from %d.\n", rank, tag, source);
            printf("[Requester %d]: CS procedure complete. Idling for 5 seconds.\n", rank);
            sleep(5);
        }
    }
}

// --- 6. Main Function ---

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int world_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    
    int world_size;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    if (world_size <= NUM_MANAGERS) { /*...*/ MPI_Finalize(); return 1; }

    // --- Create the custom MPI Datatype ---
    MPI_Datatype mpi_msg_type;
    int blocks[3] = {1, 1, 1};
    MPI_Aint displacements[3];
    MPI_Datatype types[3] = {MPI_INT, MPI_INT, MPI_INT};
    
    MsgData dummy_msg;
    MPI_Aint base_address;
    MPI_Get_address(&dummy_msg, &base_address);
    MPI_Get_address(&dummy_msg.timestamp, &displacements[0]);
    MPI_Get_address(&dummy_msg.rank, &displacements[1]);
    MPI_Get_address(&dummy_msg.group, &displacements[2]);
    
    displacements[0] = MPI_Aint_diff(displacements[0], base_address);
    displacements[1] = MPI_Aint_diff(displacements[1], base_address);
    displacements[2] = MPI_Aint_diff(displacements[2], base_address);
    
    MPI_Type_create_struct(3, blocks, displacements, types, &mpi_msg_type);
    MPI_Type_commit(&mpi_msg_type);
    // --- End of Datatype Creation ---


    if (world_rank < NUM_MANAGERS) {
        manager_role(world_rank, world_size, mpi_msg_type);
    } else {
        requester_role(world_rank, world_size, mpi_msg_type);
    }

    MPI_Type_free(&mpi_msg_type);
    MPI_Finalize();
    return 0;
}