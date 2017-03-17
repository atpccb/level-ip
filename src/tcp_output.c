#include "syshead.h"
#include "utils.h"
#include "tcp.h"
#include "ip.h"
#include "skbuff.h"
#include "timer.h"

static void tcp_retransmission_timeout(uint32_t ts, void *arg);

static struct sk_buff *tcp_alloc_skb(int size)
{
    struct sk_buff *skb = alloc_skb(size + ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN);
    skb_reserve(skb, size + ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN);
    skb->protocol = IP_TCP;
    skb->dlen = size;

    return skb;
}

static int tcp_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;

    skb_push(skb, tsk->tcp_header_len);

    struct tcphdr *thdr = (struct tcphdr *)skb->data;

    thdr->sport = sk->sport;
    thdr->dport = sk->dport;
    thdr->seq = tcb->seq;
    thdr->ack_seq = tcb->rcv_nxt;
    thdr->hl = 5;
    thdr->rsvd = 0;
    thdr->win = tcb->rcv_wnd;
    thdr->csum = 0;
    thdr->urp = 0;

    tcphdr_dbg("Output", thdr);

    /* Store sequence information into the socket buffer */
    skb->seq = tcb->seq;
    skb->end_seq = tcb->seq + skb->dlen;
    
    thdr->sport = htons(thdr->sport);
    thdr->dport = htons(thdr->dport);
    thdr->seq = htonl(thdr->seq);
    thdr->ack_seq = htonl(thdr->ack_seq);
    thdr->win = htons(thdr->win);
    thdr->csum = htons(thdr->csum);
    thdr->urp = htons(thdr->urp);
    thdr->csum = tcp_v4_checksum(skb, htonl(sk->saddr), htonl(sk->daddr));
    
    return ip_output(sk, skb);
}

static int tcp_queue_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    int rc = 0;
    
    pthread_mutex_lock(&sk->write_queue.lock);
    skb_queue_tail(&sk->write_queue, skb);
    rc = tcp_transmit_skb(sk, skb);
    pthread_mutex_unlock(&sk->write_queue.lock);

    return rc;
}

int tcp_send_finack(struct sock *sk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0);

    th = tcp_hdr(skb);
    th->fin = 1;
    th->ack = 1;

    rc = tcp_transmit_skb(sk, skb);

    free_skb(skb);

    return rc;
}

int tcp_send_synack(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT) {
        print_err("TCP synack: Socket was not in correct state (SYN_SENT)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0);
    th = tcp_hdr(skb);

    th->syn = 1;
    th->ack = 1;

    rc = tcp_transmit_skb(sk, skb);
    free_skb(skb);

    return rc;
}

/* Routine for timer-invoked delayed acknowledgment */
void tcp_send_delack(uint32_t ts, void *arg)
{
    struct sock *sk = (struct sock *) arg;
    struct tcp_sock *tsk = tcp_sk(sk);

    tcp_release_delack_timer(tsk);

    tcp_send_ack(sk);
}

int tcp_send_ack(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;
    
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0);
    
    th = tcp_hdr(skb);
    th->ack = 1;

    rc = tcp_transmit_skb(sk, skb);
    free_skb(skb);

    return rc;
}

static int tcp_send_syn(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT && sk->state != TCP_CLOSE && sk->state != TCP_LISTEN) {
        print_err("Socket was not in correct state (closed or listen)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;

    skb = tcp_alloc_skb(0);
    th = tcp_hdr(skb);

    sk->state = TCP_SYN_SENT;
    th->syn = 1;
    
    return tcp_queue_transmit_skb(sk, skb);
}

int tcp_send_fin(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;

    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0);
    
    th = tcp_hdr(skb);
    th->fin = 1;
    th->ack = 1;
    tcb->snd_nxt++;

    rc = tcp_queue_transmit_skb(sk, skb);

    return rc;
}

void tcp_select_initial_window(uint32_t *rcv_wnd)
{
    *rcv_wnd = 29200;
}

/**
 * TCP connection retransmission timeout
 */
static void tcp_connect_rto(struct tcp_sock *tsk)
{
    struct sock *sk = &tsk->sk;

    if (sk->state != TCP_ESTABLISHED) {
        if (tsk->backoff > TCP_CONN_RETRIES) {
            tsk->sk.err = -ETIMEDOUT;
            tcp_set_state(sk, TCP_CLOSE);
            wait_wakeup(&tsk->sk.sock->sleep);
            //free_socket
        } else {
            tsk->backoff++;
            tsk->retransmit = timer_add(TCP_SYN_BACKOFF << tsk->backoff, &tcp_retransmission_timeout, tsk);
        }
    } else {
        print_err("TCP connect RTO triggered even when Established\n");
    }
}

static void tcp_notify_user(struct sock *sk)
{
    struct tcp_sock *tsk = (struct tcp_sock *) sk;

    switch (sk->state) {
    case TCP_CLOSE_WAIT:
        wait_wakeup(&sk->sock->sleep);
        break;
    }
}

static void tcp_retransmission_timeout(uint32_t ts, void *arg)
{
    struct tcp_sock *tsk = (struct tcp_sock *) arg;
    struct sock *sk = &tsk->sk;

    pthread_mutex_lock(&sk->write_queue.lock);

    tcp_release_rto_timer(tsk);

    struct sk_buff *skb = write_queue_head(sk);
    if (!skb) {
        tcp_notify_user(sk);
        goto unlock;
    }

    struct tcphdr *th = tcp_hdr(skb);
    skb_reset_header(skb);
    tcp_transmit_skb(sk, skb);
    
    if (th->syn) {
        tcp_connect_rto(tsk);
    } else {
        tsk->retransmit = timer_add(500, &tcp_retransmission_timeout, tsk);
    }

    if (th->fin) {
        tcp_handle_fin_state(sk);
    }

unlock:
    pthread_mutex_unlock(&sk->write_queue.lock);
}

int tcp_connect(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    
    tsk->tcp_header_len = sizeof(struct tcphdr);
    tcb->iss = generate_iss();
    tcb->snd_wnd = 0;
    tcb->snd_wl1 = 0;

    tcb->snd_una = tcb->iss;
    tcb->snd_up = tcb->iss;
    tcb->snd_nxt = tcb->iss + 1;
    tcb->rcv_nxt = 0;
    tcb->seq = tcb->iss;

    tcp_select_initial_window(&tsk->tcb.rcv_wnd);
    
    tsk->retransmit = timer_add(TCP_SYN_BACKOFF << tsk->backoff, &tcp_retransmission_timeout, tsk);

    return tcp_send_syn(sk);
}

int tcp_send(struct tcp_sock *tsk, const void *buf, int len)
{
    struct sk_buff *skb;
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *th;
    int ret = -1;

    skb = tcp_alloc_skb(len);
    skb_push(skb, len);
    memcpy(skb->data, buf, len);

    th = tcp_hdr(skb);
    th->ack = 1;
    th->psh = 1;
    tcb->seq = tcb->snd_nxt;
    tcb->snd_nxt += len;

    ret = tcp_queue_transmit_skb(&tsk->sk, skb);

    tsk->retransmit = timer_add(500, &tcp_retransmission_timeout, tsk);

    ret -= (ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN);

    if (ret != len) {
        return -1;
    }

    return ret;
}

int tcp_send_reset(struct tcp_sock *tsk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb *tcb;
    int rc = 0;

    skb = tcp_alloc_skb(0);
    th = tcp_hdr(skb);
    tcb = &tsk->tcb;

    th->rst = 1;
    tcb->seq = tcb->snd_nxt;

    rc = tcp_transmit_skb(&tsk->sk, skb);
    free_skb(skb);

    return rc;
}

int tcp_send_challenge_ack(struct sock *sk, struct sk_buff *skb)
{
    // TODO: implement me
    return 0;
}

int tcp_queue_fin(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct sk_buff *skb;
    struct tcphdr *th;
    int ready = 0;
    int rc = 0;

    skb = tcp_alloc_skb(0);
    th = tcp_hdr(skb);

    th->fin = 1;
    th->ack = 1;
    
    pthread_mutex_lock(&sk->write_queue.lock);

    ready = skb_queue_empty(&sk->write_queue);
    skb_queue_tail(&sk->write_queue, skb);

    if (ready) {
        tcp_release_rto_timer(tsk);
        tsk->retransmit = timer_add(500, &tcp_retransmission_timeout, tsk);
        /* If nothing in write queue, send FIN immediately */
        tcb->snd_nxt++;
        rc = tcp_transmit_skb(sk, skb);

        tcp_handle_fin_state(sk);
    }

    pthread_mutex_unlock(&sk->write_queue.lock);

    return rc;
}
