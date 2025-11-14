/* manabe & park extended gme – strict, cleaned, optimized */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#define NUM_MANAGERS 3
#define NUM_GROUPS 2
#define MAX_QUEUE 128
#define SIM_SECONDS 15.0

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

/* manager states */
typedef enum { M_VACANT, M_WAITLOCK, M_LOCKED, M_RELEASING, M_WAITCANCEL } MState;
/* requester states */
typedef enum { R_IDLE, R_WAIT, R_IN, R_OUT } RState;

/* helpers */
static inline int max2(int a,int b){ return a>b?a:b; }
static inline int higher(int ts1,int r1,int ts2,int r2){
    if(ts1<ts2) return 1;
    if(ts1>ts2) return 0;
    return r1<r2;
}

/* coterie for 3 managers */
#define COT_SIZE 3
#define QSIZE 2
static const int COT[COT_SIZE][QSIZE]={{0,1},{1,2},{0,2}};

/* queue utils */
static int best(Msg*q,int n){
    if(!n) return -1;
    int b=0;
    for(int i=1;i<n;i++)
        if(higher(q[i].timestamp,q[i].rank,q[b].timestamp,q[b].rank)) b=i;
    return b;
}
static Msg pop_idx(Msg*q,int* n,int i){
    Msg x=q[i];
    q[i]=q[*n-1];
    (*n)--;
    return x;
}
static void push_pri(Msg*q,int*n,Msg m){
    if(*n>=MAX_QUEUE) return;
    int i=*n-1;
    while(i>=0){
        if(higher(q[i].timestamp,q[i].rank,m.timestamp,m.rank))
            q[i+1]=q[i], i--;
        else break;
    }
    q[i+1]=m;
    (*n)++;
}

/* set utils */
static int in_set(int*s,int n,int x){
    for(int i=0;i<n;i++) if(s[i]==x) return 1;
    return 0;
}
static int erase(int*s,int* n,int x){
    for(int i=0;i<*n;i++)
        if(s[i]==x){ s[i]=s[*n-1]; (*n)--; return 1; }
    return 0;
}

/************************ manager ************************/
void manager(int rank,MPI_Datatype T){

    MState st=M_VACANT;
    int lam=0;

    int gm=-1;                   /* current group */
    bool gmset[NUM_GROUPS]={0};  /* pivot group set */
    int pivot=-1;                /* pivot id */
    int pts=-1;                  /* pivot ts */

    Msg Q[MAX_QUEUE];            /* request queue */
    int qn=0;

    Msg okto; okto.rank=-1;      /* last ok target */
    int follower[MAX_QUEUE];     /* followers */
    int fn=0;

    double start=MPI_Wtime();

    while(1){
        if(MPI_Wtime()-start >= SIM_SECONDS) break;

        MPI_Status s; int f=0;
        MPI_Iprobe(MPI_ANY_SOURCE,MPI_ANY_TAG,MPI_COMM_WORLD,&f,&s);
        if(!f){ usleep(5000); continue; }

        Msg m;
        MPI_Recv(&m,1,T,s.MPI_SOURCE,s.MPI_TAG,MPI_COMM_WORLD,&s);
        lam=max2(lam,m.timestamp)+1;

        int src=s.MPI_SOURCE;
        int tag=s.MPI_TAG;

        switch(tag){

        /* request: queue and possibly send ok */
        case TAG_REQUEST:{
            push_pri(Q,&qn,m);

            if(st==M_VACANT){
                int i=best(Q,qn);
                if(i>=0){
                    Msg x=pop_idx(Q,&qn,i);
                    okto=x;
                    Msg ok={x.timestamp,rank,{0},-1};
                    memcpy(ok.gset,x.gset,sizeof(ok.gset));
                    MPI_Send(&ok,1,T,x.rank,TAG_OK,MPI_COMM_WORLD);
                    st=M_WAITLOCK;
                }
            }
            else if(st==M_WAITLOCK && okto.rank>=0){
                if(higher(m.timestamp,m.rank,okto.timestamp,okto.rank)){
                    Msg c={okto.timestamp,rank,{0},-1};
                    MPI_Send(&c,1,T,okto.rank,TAG_CANCEL,MPI_COMM_WORLD);
                    st=M_WAITCANCEL;
                }
            }

            /* send enter if locked and allowed */
            if(st==M_LOCKED && st!=M_RELEASING && st!=M_WAITCANCEL){
                for(int i=0;i<qn;){
                    int outr=higher(Q[i].timestamp,Q[i].rank,pts,pivot);
                    if(Q[i].gset[gm] && !outr){
                        Msg e={Q[i].timestamp,rank,{0},gm};
                        memcpy(e.gset,gmset,sizeof(e.gset));
                        MPI_Send(&e,1,T,Q[i].rank,TAG_ENTER,MPI_COMM_WORLD);
                        if(!in_set(follower,fn,Q[i].rank)) follower[fn++]=Q[i].rank;
                        pop_idx(Q,&qn,i);
                    } else i++;
                }
            }
        } break;

        /* lock: pivot chosen */
        case TAG_LOCK:{
            gm=m.group;
            memcpy(gmset,m.gset,sizeof(gmset));
            pivot=m.rank;
            pts=m.timestamp;
            okto.rank=-1;
            st=M_LOCKED;
            fn=0;

            if(st!=M_RELEASING && st!=M_WAITCANCEL){
                Msg tmp[MAX_QUEUE]; int tn=0;
                for(int i=0;i<qn;i++){
                    int outr=higher(Q[i].timestamp,Q[i].rank,pts,pivot);
                    if(Q[i].gset[gm] && !outr){
                        Msg e={Q[i].timestamp,rank,{0},gm};
                        memcpy(e.gset,gmset,sizeof(e.gset));
                        MPI_Send(&e,1,T,Q[i].rank,TAG_ENTER,MPI_COMM_WORLD);
                        if(!in_set(follower,fn,Q[i].rank)) follower[fn++]=Q[i].rank;
                    } else tmp[tn++]=Q[i];
                }
                qn=tn;
                for(int i=0;i<qn;i++) Q[i]=tmp[i];
            }
        } break;

        /* release: begin releasing phase */
        case TAG_RELEASE:{
            st=M_RELEASING;
            pivot=src;
            pts=m.timestamp;
            if(fn==0){
                Msg fin={pts,rank,{0},-1};
                MPI_Send(&fin,1,T,pivot,TAG_FINISHED,MPI_COMM_WORLD);
            }
        } break;

        /* noneed: follower done */
        case TAG_NONEED:{
            erase(follower,&fn,src);

            if(st==M_RELEASING && fn==0 && pivot>=0){
                Msg fin={pts,rank,{0},-1};
                MPI_Send(&fin,1,T,pivot,TAG_FINISHED,MPI_COMM_WORLD);
            }

            if(st==M_WAITCANCEL &&
               okto.rank==src && okto.timestamp==m.timestamp){
                okto.rank=-1;
                st=M_VACANT;
                int i=best(Q,qn);
                if(i>=0){
                    Msg x=pop_idx(Q,&qn,i);
                    okto=x;
                    Msg ok={x.timestamp,rank,{0},-1};
                    memcpy(ok.gset,x.gset,sizeof(ok.gset));
                    MPI_Send(&ok,1,T,x.rank,TAG_OK,MPI_COMM_WORLD);
                    st=M_WAITLOCK;
                }
            }
        } break;

        /* cancelled ack */
        case TAG_CANCELLED:{
            if(st==M_WAITCANCEL && okto.rank==src){
                okto.rank=-1;
                st=M_VACANT;
                int i=best(Q,qn);
                if(i>=0){
                    Msg x=pop_idx(Q,&qn,i);
                    okto=x;
                    Msg ok={x.timestamp,rank,{0},-1};
                    memcpy(ok.gset,x.gset,sizeof(ok.gset));
                    MPI_Send(&ok,1,T,x.rank,TAG_OK,MPI_COMM_WORLD);
                    st=M_WAITLOCK;
                }
            }
        } break;

        /* over: pivot finished cycle */
        case TAG_OVER:{
            st=M_VACANT;
            gm=-1; pivot=-1; pts=-1; fn=0;
            memset(gmset,0,sizeof(gmset));

            int i=best(Q,qn);
            if(i>=0){
                Msg x=pop_idx(Q,&qn,i);
                okto=x;
                Msg ok={x.timestamp,rank,{0},-1};
                memcpy(ok.gset,x.gset,sizeof(ok.gset));
                MPI_Send(&ok,1,T,x.rank,TAG_OK,MPI_COMM_WORLD);
                st=M_WAITLOCK;
            }
        } break;
        }
    }
}

/************************ requester ************************/
void requester(int rank,MPI_Datatype T){

    RState st=R_IDLE;
    int lam=0;
    int myts=0;

    bool gs[NUM_GROUPS]={0};
    if(rank==NUM_MANAGERS) gs[0]=1;
    else if(rank==NUM_MANAGERS+1){ gs[0]=1; gs[1]=1; }
    else gs[1]=1;

    int Q[QSIZE], Qn=0;
    int okc=0;
    Msg okv[MAX_QUEUE];
    int okn=0;
    int finc=0;

    int mask=0;
    for(int i=0;i<NUM_GROUPS;i++) if(gs[i]) mask|=(1<<i);

    double start=MPI_Wtime();

    while(1){
        if(MPI_Wtime()-start>=SIM_SECONDS) break;

        if(st==R_IDLE){
            sleep(1);
            myts=++lam;
            okc=0; okn=0; finc=0;

            int ch=(rank+mask)%COT_SIZE;
            Qn=QSIZE;
            for(int i=0;i<Qn;i++) Q[i]=COT[ch][i];

            Msg r={myts,rank,{0},-1};
            memcpy(r.gset,gs,sizeof(gs));
            for(int i=0;i<Qn;i++)
                MPI_Send(&r,1,T,Q[i],TAG_REQUEST,MPI_COMM_WORLD);

            st=R_WAIT;
        }

        MPI_Status s; int f=0;
        MPI_Iprobe(MPI_ANY_SOURCE,MPI_ANY_TAG,MPI_COMM_WORLD,&f,&s);
        if(!f){ usleep(5000); continue; }

        Msg m;
        MPI_Recv(&m,1,T,s.MPI_SOURCE,s.MPI_TAG,MPI_COMM_WORLD,&s);
        lam=max2(lam,m.timestamp)+1;

        int src=s.MPI_SOURCE;
        int tag=s.MPI_TAG;

        if(st==R_WAIT){

            if(m.timestamp!=myts &&
               (tag==TAG_OK||tag==TAG_ENTER||tag==TAG_CANCEL||tag==TAG_FINISHED))
                continue;

            if(tag==TAG_OK){
                okv[okn++]=m;
                if(++okc == Qn){
                    int g=-1;
                    for(int i=0;i<NUM_GROUPS;i++) if(gs[i]){ g=i; break; }
                    if(g<0) g=0;

                    Msg l={myts,rank,{0},g};
                    memcpy(l.gset,gs,sizeof(gs));
                    for(int i=0;i<Qn;i++)
                        MPI_Send(&l,1,T,Q[i],TAG_LOCK,MPI_COMM_WORLD);

                    st=R_IN;
                    sleep(2);

                    Msg rel={myts,rank,{0},g};
                    for(int i=0;i<Qn;i++)
                        MPI_Send(&rel,1,T,Q[i],TAG_RELEASE,MPI_COMM_WORLD);

                    st=R_OUT;
                    finc=0;
                }
            }
            else if(tag==TAG_ENTER){
                int g=m.group;
                int ets=m.timestamp;

                Msg nd={ets,rank,{0},g};

                for(int i=0;i<Qn;i++)
                    MPI_Send(&nd,1,T,Q[i],TAG_NONEED,MPI_COMM_WORLD);

                st=R_IN;
                sleep(2);

                for(int i=0;i<Qn;i++)
                    MPI_Send(&nd,1,T,Q[i],TAG_NONEED,MPI_COMM_WORLD);

                st=R_IDLE;
            }
            else if(tag==TAG_CANCEL){
                Msg c={myts,rank,{0},-1};
                MPI_Send(&c,1,T,src,TAG_CANCELLED,MPI_COMM_WORLD);
                st=R_IDLE;
                sleep(1);
            }
        }
        else if(st==R_OUT){
            if(tag==TAG_FINISHED && m.timestamp==myts){
                if(++finc == Qn){
                    Msg o={myts,rank,{0},-1};
                    for(int i=0;i<Qn;i++)
                        MPI_Send(&o,1,T,Q[i],TAG_OVER,MPI_COMM_WORLD);
                    st=R_IDLE;
                }
            }
        }
    }
}

/************************ main ************************/
int main(int argc,char**argv){
    MPI_Init(&argc,&argv);

    int r,n;
    MPI_Comm_rank(MPI_COMM_WORLD,&r);
    MPI_Comm_size(MPI_COMM_WORLD,&n);

    if(n<=NUM_MANAGERS){
        if(r==0) fprintf(stderr,"need at least %d managers + 1 requester\n",NUM_MANAGERS);
        MPI_Finalize();
        return 1;
    }

    MPI_Datatype T;
    int bl[4]={1,1,NUM_GROUPS,1};
    MPI_Aint ds[4],b;
    MPI_Datatype ty[4]={MPI_INT,MPI_INT,MPI_C_BOOL,MPI_INT};
    Msg tmp;

    MPI_Get_address(&tmp,&b);
    MPI_Get_address(&tmp.timestamp,&ds[0]);
    MPI_Get_address(&tmp.rank,&ds[1]);
    MPI_Get_address(&tmp.gset[0],&ds[2]);
    MPI_Get_address(&tmp.group,&ds[3]);
    for(int i=0;i<4;i++) ds[i]-=b;

    MPI_Type_create_struct(4,bl,ds,ty,&T);
    MPI_Type_commit(&T);

    if(r<NUM_MANAGERS) manager(r,T);
    else requester(r,T);

    MPI_Type_free(&T);
    MPI_Finalize();
    return 0;
}
