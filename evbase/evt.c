#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <evbase/evt.h>
#include <evbase/util.h>
#include <evbase/log.h>
#include <evbase/epoll.h>

static void evt_list_add_tail(EBL_P*, EBL_P);
static void evt_list_add(EBL_P*, EBL_P);
static void evt_list_del(EBL_P*, EBL_P);

EL_P evt_loop_init_with_flag(int flag) {
    int i;

    EL_P loop = (EL_P)mm_malloc(sizeof(struct evt_loop));
    memset(loop, 0, sizeof(struct evt_loop));

    loop->owner_thread = 0;
    loop->status = LOOP_STATU_INIT;

    loop->priority_max = 0;

    /* init fds */
    loop->fds_size = LOOP_INIT_FDS;
    loop->fds_mod_size = LOOP_INIT_FDS;
    loop->fds_mod_cnt = 0;
    loop->fds = (FDI_P)mm_malloc(sizeof(struct fd_info) * LOOP_INIT_FDS);
    loop->fds_mod = (int*)mm_malloc(sizeof(int) * LOOP_INIT_FDS);

    /* before && after event */
    loop->evt_befores_head = NULL;
    loop->evt_afters_head = NULL;

    /* timer event */
    loop->timer_heap_size = LOOP_INIT_EVTSIZE;
    loop->timer_heap_cnt = 0;    /* start from 1 */
    loop->timer_heap = (struct evt_timer*)
        mm_malloc(sizeof(struct evt_timer*) * LOOP_INIT_EVTSIZE);

    /* init backend */
    if (0) {
        /* only supprot epoll in first version*/
    } else {
        loop->poll_init = epoll_init;
    }
    loop->poll_time_us = LOOP_INIT_POLLUS;
    loop->poll_more_ptr = loop->poll_init(loop);
    if (loop->poll_more_ptr == NULL) {
        goto loop_init_failed;
    }

    /* init pending queue (only one queue on init) */
    loop->pending_size[0] = LOOP_INIT_PENDSIZE;
    loop->pending_cnt[0] = 0;
    loop->pending[0] = (EB_P*)mm_malloc(sizeof(EB_P) * LOOP_INIT_PENDSIZE);

    /* init default callback */
    /* a event do nothing, used when remove a pending event */
    loop->empty_ev = (struct evt_before*)mm_malloc(sizeof(struct evt_before));
    evt_before_init(loop->empty_ev, NULL);

    return loop;

loop_init_failed:
    log_error("evt_loop init failed!");
    /* release resouce */

    return NULL;
}

EL_P evt_loop_init() {
    /* without flag, use default config */
    return evt_loop_init_with_flag(0);
}

int evt_loop_quit(EL_P loop) {

}

int evt_loop_destroy(EL_P loop) {

}

int evt_loop_run(EL_P loop) {
    while (1) {
        EBL_P eb;
        /* do event before poll */
        for (eb = loop->evt_befores_head; eb; eb = eb->next) {
            evt_append_pending(loop, eb);
        }
        evt_execute_pending(loop);
        /* update fd changes (poll update) */
        evt_fd_changes_update(loop);

        loop->poll_dispatch(loop);

        /*queue event after poll */
        for (eb = loop->evt_afters_head; eb; eb = eb->next) {
            evt_append_pending(loop, eb);
        }

        evt_execute_pending(loop);
    }
}

void evt_append_pending(EL_P loop, void *w) {
    EB_P ev = (EB_P)w;
    int pri = ev->priority;

    assert(pri <= loop->priority_max);
    /* only append it if the event not in pending queue */
    if (ev->pendpos == 0) {
        check_and_expand_array(loop->pending[pri], EB_P, loop->pending_size[pri],
            loop->pending_cnt[pri] + 1, multi_two, init_array_zero);

        loop->pending[pri][loop->pending_cnt[pri]] = ev;
        ev->pendpos = ++loop->pending_cnt[pri];
    }
}

void evt_execute_pending(EL_P loop) {
    int i, j;

    /* execute high priority event first*/
    for (i = 0; i <= loop->priority_max; i++) {
        for (j = 0; j < loop->pending_cnt[i]; j++) {
            EB_P ev = loop->pending[i][j];
            ev->pendpos = 0;
            if (ev->cb) {
                ev->cb(loop, ev);
            }
        }
        loop->pending_cnt[i] = 0;
    }
}


/* io event && fd operation*/
void evt_io_start(EL_P loop, struct evt_io* w) {
    FDI_P fdi;
#ifndef NDEBUG
    int debug_osize = loop->fds_size;
#endif
    /* adjust event param */
    w->active = 1;
    adjust_between(w->priority, 0, loop->priority_max);

    /* check if fd event need expand (if need, expand it)*/
    check_and_expand_array(loop->fds, struct fd_info, loop->fds_size,
        w->fd + 1, multi_two, init_array_zero);

    fdi = loop->fds + w->fd;

#ifndef NDEBUG
    if (debug_osize != loop->fds_size)
        log_debug("fds size increased %d => %d", debug_osize, loop->fds_size);
#endif

    /* add to fd's event list */
    evt_list_add(&fdi->head, (EBL_P)w);

    /* add fd to change list */
    evt_fd_change(loop, w->fd);
}

void evt_io_stop(EL_P loop, struct evt_io *ev) {
    /* if ev is in pending queue, use a empty ev instead it */
    ev->active = 0;
    if (ev->pendpos) {
        loop->pending[ev->priority][ev->pendpos-1] = (EB_P)loop->empty_ev;
        ev->pendpos = 0;
    }

    /* remove from fd's event list */
    evt_list_del(&(loop->fds[ev->fd]).head, (EBL_P)ev);

    /* add fd to change list */
    evt_fd_change(loop, ev->fd);
}

void evt_fd_change(EL_P loop, int fd) {
    FDI_P fdi = loop->fds + fd;

    /* if this fd is not in fd_mod array, append it */
    if (!(fdi->flag & FD_FLAG_CHANGE)) {
        check_and_expand_array(loop->fds_mod, int, loop->fds_mod_size,
            loop->fds_mod_cnt + 1, add_one, init_array_noop);
        loop->fds_mod[loop->fds_mod_cnt] = fd;
        ++loop->fds_mod_cnt;
        fdi->flag != FD_FLAG_CHANGE;
    }
}

void evt_fd_changes_update(EL_P loop) {
    int i;
    EBL_P w;

    /* for each fd change, check if do poll update*/
    for (i = 0; i < loop->fds_mod_cnt; i++) {
        int fd = loop->fds_mod[i];
        FDI_P fdi = loop->fds + fd;
        uint8_t oevt = fdi->events;

        fdi->events = 0;
        fdi->flag &= ~FD_FLAG_CHANGE;
        /* get events focusing now */
        for (w = fdi->head; w; w = w->next) {
            fdi->events |= ((struct evt_io*)w)->event;
        }
        /* if focused event really changed, do poll update */
        if (oevt != fdi->events) {
            loop->poll_update(loop, fd, oevt, fdi->events);
        }
    }
    /* clear */
    loop->fds_mod_cnt = 0;
}

/* timer event */
void evt_timer_start(EL_P loop, struct evt_timer* ev) {
    /* adjust event param */
    ev->active = 1;
    adjust_between(ev->priority, 0, loop->priority_max);

}

void evt_timer_stop(EL_P loop, struct evt_timer* ev) {

}

/* before event && after event */
void evt_before_start(EL_P loop, struct evt_before* ev) {
    /* adjust event param */
    ev->active = 1;
    adjust_between(ev->priority, 0, loop->priority_max);

    /* add to before events list */
    evt_list_add_tail(&loop->evt_befores_head, (EBL_P)ev);
}

void evt_before_stop(EL_P loop, struct evt_before* ev) {
    /* if ev is in pending queue, use a empty ev instead it */
    ev->active = 0;
    if (ev->pendpos) {
        loop->pending[ev->priority][ev->pendpos-1] = (EB_P)loop->empty_ev;
        ev->pendpos = 0;
    }

    /* remove from befores list */
    evt_list_del(&loop->evt_befores_head, (EBL_P)ev);
}

void evt_after_start(EL_P loop, struct evt_after* ev) {
    /* adjust event param */
    ev->active = 1;
    adjust_between(ev->priority, 0, loop->priority_max);

    /* add to after events list */
    evt_list_add_tail(&loop->evt_afters_head, (EBL_P)ev);
}

void evt_after_stop(EL_P loop, struct evt_after* ev) {
    /* if ev is in pending queue, use a empty ev instead it */
    ev->active = 0;
    if (ev->pendpos) {
        loop->pending[ev->priority][ev->pendpos-1] = (EB_P)loop->empty_ev;
        ev->pendpos = 0;
    }

    /* remove from afters list */
    evt_list_del(&loop->evt_afters_head, (EBL_P)ev);
}

/* list operation */
static void evt_list_add(EBL_P* head, EBL_P elm) {
    elm->next = *head;
    *head = elm;
}

static void evt_list_add_tail(EBL_P* head, EBL_P elm) {
    while (*head)
        head = &(*head)->next;
    *head = elm;
    elm->next = NULL;
}

static void evt_list_del(EBL_P* head, EBL_P elm) {
    while (*head) {
        if (*head == elm) {
            *head = elm->next;
            break;
        }
        head = &(*head)->next;
    }
}

