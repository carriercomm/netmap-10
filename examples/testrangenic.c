/**
 *
 * author: db
 * 2014-04-21 by db
 *
 *
 */

#define NETMAP_WITH_LIBS
#define _GNU_SOURCE 

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>

#include <sys/mman.h> /* PROT_* */
#include <sys/ioctl.h> /* ioctl */
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/socket.h> /* sockaddr.. */
#include <arpa/inet.h> /* ntohs */

#include <pthread.h>
#include <sys/epoll.h>

#include <net/if.h>	/* ifreq */
#include <net/ethernet.h>

#include <netinet/if_ether.h>
#include <netinet/in.h> /* sockaddr_in */
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "net/netmap.h"
#include "net/netmap_user.h"

char *version = "$ID$";
int verbose = 0;

static int do_abort = 0;

static void sigint_h(int sig)
{
    (void)sig;  /* UNUSED */
    do_abort = 1;
    signal(SIGINT, SIG_DFL);
}

/* set the thread affinity. */
static int setaffinity(pthread_t me, int i)
{
    cpu_set_t cpumask;

    if (i == -1)
        return 0;

    /* Set thread affinity affinity.*/
    CPU_ZERO(&cpumask);
    CPU_SET(i, &cpumask);

    if (pthread_setaffinity_np(me, sizeof(cpu_set_t), &cpumask) != 0) {
        D("Unable to set affinity: %s", strerror(errno));
        return 1;
    }
    return 0;
}

struct thread_args_t {
    struct nm_desc *desc;
    int affinity;
    int mode;
    pthread_t thread;
    unsigned long long volatile count;
};

//#define NO_SWAP
static int process_rings(struct netmap_ring *rxring, 
        u_int limit)
{
    u_int j,m = 0;

    /* print a warning if any of the ring flags is set (e.g. NM_REINIT) */
    j = rxring->cur; /* RX */
    m = nm_ring_space(rxring);
    if (m < limit)
        limit = m;
    m = 0;
    while (limit-- > 0) {
        m ++;
        j = nm_ring_next(rxring, j);
    }

    rxring->head = rxring->cur = j;

    return (m);
}

static unsigned long long g_rings_counts[16]={0};
static int rx(struct thread_args_t *arg, struct nm_desc *src, u_int limit)
{
    struct netmap_ring *rxring;
    u_int si = src->first_rx_ring;
    while (si <= src->last_rx_ring) {
        rxring = NETMAP_RXRING(src->nifp, si);
        if (nm_ring_empty(rxring)) {
            si++;
            continue;
        }
        g_rings_counts[si] ++;

        u_int m = process_rings(rxring, limit);
        arg->count += m;
    }

    return (1);
}

void *run(void* args)
{
    u_int burst = 1024, wait_link = 4;

    struct thread_args_t *targ = (struct thread_args_t *)args;

    struct nm_desc *desc = (struct nm_desc*)targ->desc;

    if (setaffinity(targ->thread, targ->affinity))
    {
        D("setaffinity failed.");
    }

    int efd = epoll_create(1);
    if (efd < 0 )
    {
        D("epoll create failed.");
        exit(1);
    }

    struct epoll_event event, events;
    memset(&event.data, 0, sizeof(event.data));
    event.data.fd = desc->fd; 
    event.events = EPOLLIN;
    epoll_ctl(efd, EPOLL_CTL_ADD, desc->fd, &event);

    D("Wait %d secs for link to come up...", wait_link);
    sleep(wait_link);
    D("Ready to go ....%d", desc->fd);

    /* main loop */
    signal(SIGINT, sigint_h);
    while (!do_abort) {
        if (epoll_wait(efd, &events, 1, 200) > 0 ) 
        {
            rx(targ, desc, burst);
        }
    }

    return NULL;
}

#define MAX_RINGS 8
#define MAX_EVENTS MAX_RINGS

void main_loop_statistics(struct thread_args_t *targs, int channel_nums) 
{
    int times = 0;
    int index = 0;
    unsigned long long pre_total = 0;
    while(!do_abort) {
#define INTERVAL 1
        sleep(INTERVAL);
        unsigned long long total = 0;

        for (index = 0; index < channel_nums; index ++)
        {
            total += targs[index].count;
        }

        printf("ring pkt counts: ");
        for (index = 0; index < MAX_RINGS; index ++)
        {
            printf("%llu ", g_rings_counts[index]);
        }

        printf("\n%d recv_count: %lld\n", 
                times++, ((total - pre_total)/INTERVAL));
        
        fflush(stdout);

        pre_total = total;
    }
}


int main(int argc, char **argv)
{
    int index = 0;
    int channel_nums = 4;
    int member = 2;
    const char *ifname = NULL;

    struct thread_args_t targs[MAX_RINGS]; 
    memset(targs, 0x0, sizeof(targs));

    if (argc < 2 || argc > 3) {
        printf("Usage:%s [interface] <channel nums>\n", argv[1]);
        return 1;
    }

    ifname = argv[1];
    if (argc == 3)
    {
        channel_nums = atoi(argv[2]);
    }

    if (channel_nums < 1 || channel_nums > MAX_RINGS 
            || (MAX_RINGS % channel_nums) != 0 )
    {
        printf("channel nums error.\n");
        return 1;
    }

    member = MAX_RINGS / channel_nums;
    for (index = 0; index < channel_nums; index++)
    {   
        char buff[64];
        memset(buff,0x0, 64);

        struct thread_args_t *thread_arg = &targs[index];

        unsigned short start = index * member;
        unsigned short end   = start + member - 1;
        snprintf(buff, 63, "netmap:%s+%d.%d", ifname, start, end);
        
        thread_arg->desc = nm_open(buff, NULL, NETMAP_NO_TX_POLL | NETMAP_DO_RX_POLL, NULL);
        if ( thread_arg->desc == NULL) {
            D("cannot open eth0");
            return 1;
        }
        
        printf("%d first_rx_ring:%d last_rx_rings:%d ", index,
                            thread_arg->desc->first_rx_ring,
                            thread_arg->desc->last_rx_ring);
        printf(" first_tx_ring:%d last_tx_rings:%d\n", 
                            thread_arg->desc->first_tx_ring,
                            thread_arg->desc->last_tx_ring);

        thread_arg->affinity = index;
        pthread_create(&(thread_arg->thread), NULL, run, (void*)thread_arg);
        pthread_detach(thread_arg->thread);
    }

    main_loop_statistics(targs, channel_nums);

    for (index = 0; index < channel_nums; index ++)
    {
        nm_close(targs[index].desc); 
    }

    D("exiting");
    return (0);
}


