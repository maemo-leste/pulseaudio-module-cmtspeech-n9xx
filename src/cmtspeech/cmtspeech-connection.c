/*
 * Copyright (C) 2010 Nokia Corporation.
 *
 * Contact: Maemo MMF Audio <mmf-audio@projects.maemo.org>
 *          or Jyri Sarha <jyri.sarha@nokia.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */
#include "cmtspeech-connection.h"

#include <meego/module-voice-api.h>
#include "cmtspeech-mainloop-handler.h"
#include "cmtspeech-sink-input.h"
#include <pulsecore/rtpoll.h>
#include <pulsecore/core-rtclock.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <poll.h>
#include <errno.h>

#include <cmtspeech.h>

/* TODO: Get rid of this and use asyncmsgq instead. */
enum cmt_speech_thread_state {
    CMT_UNINITIALIZED = 0,
    CMT_STARTING,
    CMT_RUNNING,
    CMT_ASK_QUIT,
    CMT_QUIT
};

/* This should be only used for memblock free cb - cmtspeech_free_cb - below
   and it is initialized in cmtspeech_connection_init(). */
static struct userdata *userdata = NULL;

static uint ul_frame_count = 0;

#define CMTSPEECH_CLEANUP_TIMER_TIMEOUT ((pa_usec_t)(5 * PA_USEC_PER_SEC))

enum cmtspeech_cleanup_state_name {
    CMTSPEECH_CLEANUP_TIMER_INACTIVE = 0,
    CMTSPEECH_CLEANUP_TIMER_ACTIVE,
    CMTSPEECH_CLEANUP_IN_PROGRESS
};

enum {
    CMTSPEECH_HANDLER_CLOSE_CONNECTION,
};

typedef struct cmtspeech_handler {
    pa_msgobject parent;
    struct userdata *u;
} cmtspeech_handler;

PA_DEFINE_PRIVATE_CLASS(cmtspeech_handler, pa_msgobject);
#define CMTSPEECH_HANDLER(o) cmtspeech_handler_cast(o)

static void cmtspeech_handler_free(pa_object *o) {
    cmtspeech_handler *h = CMTSPEECH_HANDLER(o);

    pa_log_info("Free called");
    pa_xfree(h);
}

static void close_cmtspeech_on_error(struct userdata *u);

static int cmtspeech_handler_process_msg(pa_msgobject *o, int code, void *ud, int64_t offset, pa_memchunk *chunk) {
    cmtspeech_handler *h = CMTSPEECH_HANDLER(o);
    struct userdata *u;

    cmtspeech_handler_assert_ref(h);
    pa_assert_se(u = h->u);

    switch (code) {
        case CMTSPEECH_HANDLER_CLOSE_CONNECTION:
            pa_log_debug("CMTSPEECH_HANDLER_CLOSE_CONNECTION");
            close_cmtspeech_on_error(u);
            return 0;
        default:
            pa_log_error("Unknown message code %d", code);
            return -1;
    }
}

static pa_msgobject *cmtspeech_handler_new(struct userdata *u) {
    cmtspeech_handler *h;

    pa_assert(u);
    pa_assert(u->core);
    pa_assert_se(h = pa_msgobject_new(cmtspeech_handler));

    h->parent.parent.free = cmtspeech_handler_free;
    h->parent.process_msg = cmtspeech_handler_process_msg;
    h->u = u;

    return (pa_msgobject *)h;
}

/* This is usually called from sink IO-thread */
static void cmtspeech_free_cb(void *p) {
    cmtspeech_t *cmtspeech;

    if (!p)
        return;

    if (!userdata) {
        pa_log_error("userdata not set, cmtspeech buffer %p was not freed!", p);
        return;
    }

    pa_mutex_lock(userdata->cmt_connection.cmtspeech_mutex);
    cmtspeech = userdata->cmt_connection.cmtspeech;
    if (!cmtspeech) {
        pa_log_error("cmtspeech not open, cmtspeech buffer %p was not freed!", p);
    } else {
        int ret;
        cmtspeech_buffer_t *buf = cmtspeech_dl_buffer_find_with_data(cmtspeech, (uint8_t*)p);
        if (buf != NULL) {
            if ((ret = cmtspeech_dl_buffer_release(cmtspeech, buf))) {
                pa_log_error("cmtspeech_dl_buffer_release(%p) failed return value %d.", (void *)buf, ret);
            }
        } else {
            pa_log_error("cmtspeech_dl_buffer_find_with_data() returned NULL, releasing buffer failed.");
        }
    }
    pa_mutex_unlock(userdata->cmt_connection.cmtspeech_mutex);
}

/* Called from sink IO-thread */
/* NOTE: If you ever see a seqfault when accessing these libcmtspeechdata owned
 * memblocks, then just free the cmtframes here after coping them to regular
 * pa_memblocks. The performance penalty should not be too severe. */
int cmtspeech_buffer_to_memchunk(struct userdata *u, cmtspeech_buffer_t *buf, pa_memchunk *chunk) {
    pa_assert_fp(u);
    pa_assert_fp(chunk);
    pa_assert_fp(buf);

    if (!buf->data) {
        pa_log_warn("No data in cmtspeech_buffer");
        if (cmtspeech_dl_buffer_release(u->cmt_connection.cmtspeech, buf))
            pa_log_warn("cmtspeech_dl_buffer_release() failed");
        return -1;
    }

    chunk->memblock = pa_memblock_new_user(u->core->mempool, buf->data, (size_t) buf->size, cmtspeech_free_cb, buf->data, true);
    chunk->index = CMTSPEECH_DATA_HEADER_LEN;
    chunk->length = buf->count - CMTSPEECH_DATA_HEADER_LEN;

    return 0;
}

/* cmtspeech thread */
static inline
int push_cmtspeech_buffer_to_dl_queue(struct userdata *u, cmtspeech_dl_buf_t *buf) {
    pa_assert_fp(u);
    pa_assert_fp(buf);

    if (pa_asyncq_push(u->cmt_connection.dl_frame_queue, (void *)buf, false)) {
        int ret;
        struct cmtspeech_connection *c = &u->cmt_connection;

        pa_log_error("Failed to push dl frame to asyncq");
        pa_mutex_lock(c->cmtspeech_mutex);
        if ((ret = cmtspeech_dl_buffer_release(u->cmt_connection.cmtspeech, buf)))
            pa_log_error("cmtspeech_dl_buffer_release(%p) failed return value %d.", (void *)buf, ret);
        pa_mutex_unlock(c->cmtspeech_mutex);
        return -1;
    }

    ONDEBUG_TOKENS(fprintf(stderr, "D"));
    return 0;
}

/* cmtspeech thread */
static void update_uplink_frame_timing(struct userdata *u, cmtspeech_event_t *cmtevent) {
    int deadline_us;
    int64_t usec;

    pa_log_debug("msec= %d usec=%d rtclock=%d.%09ld",
                 (int)cmtevent->msg.timing_config_ntf.msec,
                 (int)cmtevent->msg.timing_config_ntf.usec,
                 (int)cmtevent->msg.timing_config_ntf.tstamp.tv_sec,
                 cmtevent->msg.timing_config_ntf.tstamp.tv_nsec);

    deadline_us = (cmtevent->msg.timing_config_ntf.msec % 20) * 1000 + cmtevent->msg.timing_config_ntf.usec;

    usec = ((int64_t) cmtevent->msg.timing_config_ntf.tstamp.tv_sec * 1000000) +
        (cmtevent->msg.timing_config_ntf.tstamp.tv_nsec/1000) + deadline_us;

    pa_log_debug("deadline at %" PRIi64 " (%d usec from msg receival)", usec, deadline_us);

    if (u->source && PA_SOURCE_IS_LINKED(u->source->state))
        pa_asyncmsgq_post(u->source->asyncmsgq, PA_MSGOBJECT(u->source),
                          VOICE_SOURCE_SET_UL_DEADLINE, NULL, usec, NULL, NULL);
    else
        pa_log_error("No destination where to send timing info");
}

static void reset_call_stream_states(struct userdata *u) {
    struct cmtspeech_connection *c = &u->cmt_connection;

    pa_assert(u);

    if (c->streams_created) {
        pa_log_warn("DL/UL streams existed at reset, closing");
        pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                          CMTSPEECH_MAINLOOP_HANDLER_DELETE_STREAMS, NULL, 0, NULL, NULL);
        c->streams_created = false;
    }
    if (c->playback_running) {
        pa_log_warn("DL stream was open, closing");
        c->playback_running = false;
    }
    if (c->record_running) {
        pa_log_warn("UL stream was open, closing");
        c->record_running = false;
        ul_frame_count = 0;
    }
}

/* cmtspeech thread */
static int mainloop_cmtspeech(struct userdata *u) {
    int retsockets = 0;
    struct cmtspeech_connection *c = &u->cmt_connection;
    struct pollfd *pollfd;

    pa_assert(u);

    if (!c->cmt_poll_item)
        return 0;

    if (pa_atomic_load(&u->cmtspeech_server_status))
        pa_rtpoll_set_timer_absolute(c->rtpoll, pa_rtclock_now() + CMTSPEECH_CLEANUP_TIMER_TIMEOUT);

    pollfd = pa_rtpoll_item_get_pollfd(c->cmt_poll_item, NULL);
    if (pollfd->revents & POLLIN) {
        cmtspeech_t *cmtspeech;
        int flags = 0, i = CMTSPEECH_CTRL_LEN;
        int res;

        /* locking note: hot path lock */
        pa_mutex_lock(c->cmtspeech_mutex);

        cmtspeech = c->cmtspeech;

        res = cmtspeech_check_pending(cmtspeech, &flags);
        if (res >= 0)
            retsockets = 1;

        pa_mutex_unlock(c->cmtspeech_mutex);

        if (res > 0) {
            if (flags & CMTSPEECH_EVENT_CONTROL) {
                cmtspeech_event_t cmtevent;

                /* locking note: this path is taken only very rarely */
                pa_mutex_lock(c->cmtspeech_mutex);

                i = cmtspeech_read_event(cmtspeech, &cmtevent);

                pa_mutex_unlock(c->cmtspeech_mutex);

                pa_log_debug("read cmtspeech event: state %d -> %d (type %d, ret %d).",
                             cmtevent.prev_state, cmtevent.state, cmtevent.msg_type, i);

                if (i != 0) {
                    pa_log_error("ERROR: unable to read event.");

                } else if (cmtevent.prev_state == CMTSPEECH_STATE_DISCONNECTED &&
                           cmtevent.state == CMTSPEECH_STATE_CONNECTED) {
                    pa_log_debug("call starting.");
                    reset_call_stream_states(u);

                    pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                      CMTSPEECH_MAINLOOP_HANDLER_CREATE_STREAMS, NULL, 0, NULL, NULL);

                    c->streams_created = true;
                } else if (cmtevent.prev_state == CMTSPEECH_STATE_CONNECTED &&
                           cmtevent.state == CMTSPEECH_STATE_ACTIVE_DL &&
                           cmtevent.msg_type == CMTSPEECH_SPEECH_CONFIG_REQ) {
                    pa_log_notice("speech start: srate=%u, format=%u, stream=%u",
                                  cmtevent.msg.speech_config_req.sample_rate,
                                  cmtevent.msg.speech_config_req.data_format,
                                  cmtevent.msg.speech_config_req.speech_data_stream);

                     /* Ul is turned on when timing information is received */

                    pa_log_debug("enabling DL");
                    pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                      CMTSPEECH_MAINLOOP_HANDLER_CMT_DL_CONNECT, NULL, 0, NULL, NULL);
                    c->playback_running = true;

                     // start waiting for first dl frame
                     c->first_dl_frame_received = false;
                } else if (cmtevent.prev_state == CMTSPEECH_STATE_ACTIVE_DLUL &&
                           cmtevent.state == CMTSPEECH_STATE_ACTIVE_DL &&
                           cmtevent.msg_type == CMTSPEECH_SPEECH_CONFIG_REQ) {

                    pa_log_notice("speech update: srate=%u, format=%u, stream=%u",
                                  cmtevent.msg.speech_config_req.sample_rate,
                                  cmtevent.msg.speech_config_req.data_format,
                                  cmtevent.msg.speech_config_req.speech_data_stream);

                } else if (cmtevent.prev_state == CMTSPEECH_STATE_ACTIVE_DL &&
                           cmtevent.state == CMTSPEECH_STATE_ACTIVE_DLUL) {
                    pa_log_debug("enabling UL");

                    pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                    CMTSPEECH_MAINLOOP_HANDLER_CMT_UL_CONNECT, NULL, 0, NULL, NULL);
                    c->record_running = true;

                } else if (cmtevent.state == CMTSPEECH_STATE_ACTIVE_DLUL &&
                           cmtevent.msg_type == CMTSPEECH_TIMING_CONFIG_NTF) {
                    update_uplink_frame_timing(u, &cmtevent);
                    pa_log_debug("updated UL timing params");

                } else if ((cmtevent.prev_state == CMTSPEECH_STATE_ACTIVE_DL ||
                            cmtevent.prev_state == CMTSPEECH_STATE_ACTIVE_DLUL) &&
                           cmtevent.state == CMTSPEECH_STATE_CONNECTED) {
                    pa_log_notice("speech stop: stream=%u",
                                  cmtevent.msg.speech_config_req.speech_data_stream);
                    pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                      CMTSPEECH_MAINLOOP_HANDLER_CMT_DL_DISCONNECT, NULL, 0, NULL, NULL);
                    c->playback_running = false;
                    pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                      CMTSPEECH_MAINLOOP_HANDLER_CMT_UL_DISCONNECT, NULL, 0, NULL, NULL);
                    c->record_running = false;
                    ul_frame_count = 0;

                } else if (cmtevent.prev_state == CMTSPEECH_STATE_CONNECTED &&
                         cmtevent.state == CMTSPEECH_STATE_DISCONNECTED) {
                    pa_log_debug("call terminated.");
                    pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                      CMTSPEECH_MAINLOOP_HANDLER_DELETE_STREAMS, NULL, 0, NULL, NULL);
                    c->streams_created = false;
                    reset_call_stream_states(u);

                } else if (cmtevent.msg_type == CMTSPEECH_EVENT_RESET) {
                    pa_log_warn("modem reset detected");
                    close_cmtspeech_on_error(u);
                    /* cmtspeech handle now null so return immediately */
                    return retsockets;

                } else {
                    pa_log_error("Unrecognized cmtspeech event: state %d -> %d (type %d, ret %d).",
                                 cmtevent.prev_state, cmtevent.state, cmtevent.msg_type, i);
                    if (cmtevent.state == CMTSPEECH_STATE_DISCONNECTED)
                        reset_call_stream_states(u);
                }
            }

            /* step: check for SSI data events */
            if (flags & CMTSPEECH_EVENT_DL_DATA) {
                cmtspeech_buffer_t *buf;
                static int counter = 0;
                bool cmtspeech_active = false;

                counter++;
                if (counter < 10)
                    pa_log_debug("SSI: DL frame available, read %d bytes.", i);

                /* locking note: another hot path lock */
                pa_mutex_lock(c->cmtspeech_mutex);
                cmtspeech_active = cmtspeech_is_active(c->cmtspeech);
                i = cmtspeech_dl_buffer_acquire(cmtspeech, &buf);
                pa_mutex_unlock(c->cmtspeech_mutex);

                if (i < 0) {
                    pa_log_error("Invalid DL frame received, cmtspeech_dl_buffer_acquire returned %d", i);
                } else {
                    if (counter < 10 )
                        pa_log_debug("DL (audio len %d) frame's first bytes %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                                     buf->count - CMTSPEECH_DATA_HEADER_LEN,
                                     buf->data[0], buf->data[1], buf->data[1], buf->data[3],
                                     buf->data[4], buf->data[5], buf->data[6], buf->data[7]);

                    if (c->playback_running) {
                        if (c->first_dl_frame_received != true) {
                            c->first_dl_frame_received = true;
                            pa_log_debug("DL frame received, turn DL routing on...");
                        }
                        (void)push_cmtspeech_buffer_to_dl_queue(u, buf);

                    } else if (cmtspeech_active != true) {
                        pa_log_debug("DL frame received before ACTIVE_DL state, dropping...");
                    }
                }
            }
        }
    } else {
        /* pollfd timer expired and no events. */
        if (pa_atomic_cmpxchg(&u->cmtspeech_cleanup_state, CMTSPEECH_CLEANUP_TIMER_ACTIVE, CMTSPEECH_CLEANUP_IN_PROGRESS)) {
            pa_mutex_lock(c->cmtspeech_mutex);
            if (!pa_atomic_load(&u->cmtspeech_server_status) && c->cmtspeech) {
                if (u->server_inactive_timeout <= pa_rtclock_now()) {
                    pa_log_debug("cmtspeech cleanup timer checking server status.");
                    if (cmtspeech_is_active(c->cmtspeech)) {
                        pa_log_debug("cmtspeech still active, forcing cleanup");
                        pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                          CMTSPEECH_MAINLOOP_HANDLER_CMT_DL_DISCONNECT, NULL, 0, NULL, NULL);
                        pa_asyncmsgq_post(pa_thread_mq_get()->outq, u->mainloop_handler,
                                          CMTSPEECH_MAINLOOP_HANDLER_CMT_UL_DISCONNECT, NULL, 0, NULL, NULL);
                        cmtspeech_state_change_error(c->cmtspeech);
                    }
                    pa_rtpoll_set_timer_disabled(c->rtpoll);
                    pa_atomic_store(&u->cmtspeech_cleanup_state, CMTSPEECH_CLEANUP_TIMER_INACTIVE);
                    pa_log_debug("cmtspeech cleanup timer inactive in cmtspeech mainloop.");
                } else {
                    pa_rtpoll_set_timer_relative(c->rtpoll, CMTSPEECH_CLEANUP_TIMER_TIMEOUT);
                    pa_atomic_store(&u->cmtspeech_cleanup_state, CMTSPEECH_CLEANUP_TIMER_ACTIVE);
                    pa_log_debug("cmtspeech cleanup timer timeout updated in cmtspeech mainloop.");
                }
            } else {
                pa_rtpoll_set_timer_disabled(c->rtpoll);
                pa_atomic_store(&u->cmtspeech_cleanup_state, CMTSPEECH_CLEANUP_TIMER_INACTIVE);
                pa_log_debug("cmtspeech cleanup timer inactive in cmtspeech mainloop (call active or cmtspeech closed).");
            }
            pa_mutex_unlock(c->cmtspeech_mutex);
        } else if (!pa_atomic_load(&u->cmtspeech_server_status) && c->cmtspeech == NULL) {
            pa_log_debug("cmtspeech cleanup timer inactive in cmtspeech mainloop (2).");
            pa_rtpoll_set_timer_disabled(c->rtpoll);
        }
    }

    return retsockets;
}

/* cmtspeech thread */
static int check_cmtspeech_connection(struct cmtspeech_connection *c) {
    static uint counter = 0;

    if (c->cmtspeech)
        return 0;

    /* locking note: not on the hot path */

    pa_mutex_lock(c->cmtspeech_mutex);

    c->cmtspeech = cmtspeech_open();

    pa_mutex_unlock(c->cmtspeech_mutex);

    if (!c->cmtspeech) {
        if (counter++ < 5)
            pa_log_error("cmtspeech_open() failed");
        return -1;
    } else if (counter > 0) {
        pa_log_debug("cmtspeech_open() OK");
        counter = 0;
    }
    return 0;
}

/* cmtspeech thread */
static void pollfd_update(struct cmtspeech_connection *c) {
    if (c->cmt_poll_item) {
        pa_rtpoll_item_free(c->cmt_poll_item);
        c->cmt_poll_item = NULL;
    }
    if (c->cmtspeech) {
        pa_rtpoll_item *i = pa_rtpoll_item_new(c->rtpoll, PA_RTPOLL_NEVER, 1);
        struct pollfd *pollfd = pa_rtpoll_item_get_pollfd(i, NULL);
        /* locking note: a hot path lock */
        pa_mutex_lock(c->cmtspeech_mutex);
        pollfd->fd = cmtspeech_descriptor(c->cmtspeech);
        pa_mutex_unlock(c->cmtspeech_mutex);
        pollfd->events = POLLIN;
        pollfd->revents = 0;

        c->cmt_poll_item = i;

    } else {
        pa_log_debug("No cmtspeech connection");
    }

    if (c->thread_state_poll_item) {
        pa_rtpoll_item_free(c->thread_state_poll_item);
        c->thread_state_poll_item = NULL;
    }

    c->thread_state_poll_item = pa_rtpoll_item_new_fdsem(c->rtpoll, PA_RTPOLL_NORMAL, c->thread_state_change);
}

/**
 * Closes the cmtspeech instance after an unrecoverable
 * error has been detected.
 *
 * In most cases, the connection to the modem has been lost and its
 * state is unknown. As a recovery mechanism, we close the library
 * instance and restart from a known state.
 */
/* cmtspeech thread */
static void close_cmtspeech_on_error(struct userdata *u)
{
    struct cmtspeech_connection *c = &u->cmt_connection;
    bool was_active = c->streams_created;

    pa_assert(u);

    pa_log_debug("closing the modem instance");

    reset_call_stream_states(u);

    if (u->sink_input && PA_SINK_INPUT_IS_LINKED(u->sink_input->state) &&
        u->sink_input->sink && u->sink_input->sink->asyncmsgq) {
        pa_assert_se(pa_asyncmsgq_send(u->sink_input->sink->asyncmsgq, PA_MSGOBJECT(u->sink_input),
                                       PA_SINK_INPUT_MESSAGE_FLUSH_DL, NULL, 0, NULL) == 0);
    } else {
        cmtspeech_buffer_t *buf;
        pa_log_debug("DL stream not connected. Flushing the queue locally");
        while((buf = pa_asyncq_pop(c->dl_frame_queue, 0))) {
            if (cmtspeech_dl_buffer_release(c->cmtspeech, buf)) {
                pa_log_error("Freeing cmtspeech buffer failed!");
            }
        }
    }

    pa_mutex_lock(c->cmtspeech_mutex);
    if (was_active == true)
        pa_log_error("closing modem instance when interface still active");
    if (cmtspeech_close(c->cmtspeech))
        pa_log_error("cmtspeech_close() failed");
    c->cmtspeech = NULL;
    pa_mutex_unlock(c->cmtspeech_mutex);
}

/* cmtspeech thread */
static void thread_func(void *udata) {
    struct userdata *u = udata;
    struct cmtspeech_connection *c = &u->cmt_connection;

    pa_assert(u);

    pa_log_debug("cmtspeech thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority - 1);

    pa_thread_mq_install(&c->thread_mq);

    c->cmtspeech = cmtspeech_open();

    pa_assert_se(pa_atomic_cmpxchg(&c->thread_state, CMT_STARTING, CMT_RUNNING));

    while(1) {
        int ret;

        if (check_cmtspeech_connection(c)) {
            pa_log("Failed to open the cmtspeech device, waiting 60 seconds before trying again.");
            pa_rtpoll_set_timer_relative(c->rtpoll, 60 * 1000 * 1000);
        }

        pollfd_update(c);

        if (0 > (ret = pa_rtpoll_run(c->rtpoll))) {
            pa_log_error("running rtpoll failed (%d) (fd %d)", ret, cmtspeech_descriptor(c->cmtspeech));
            close_cmtspeech_on_error(u);
        }

        if (pa_atomic_load(&c->thread_state) == CMT_ASK_QUIT) {
            pa_log_debug("cmtspeech thread quiting");
            goto finish;
        }

        /* note: cmtspeech can be closed in DBus thread */
        if (c->cmtspeech == NULL) {
            continue;
        }

        if (0 > mainloop_cmtspeech(u)) {
            goto fail;
        }

    }


/**/
fail:
    pa_log_error("Trying to unload myself");
    pa_asyncmsgq_post(c->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);

    pa_log_debug("Waiting for quit command...");
    pa_fdsem_wait(c->thread_state_change);
    pa_assert(pa_atomic_load(&c->thread_state) == CMT_ASK_QUIT);

finish:
    close_cmtspeech_on_error(u);

    pa_assert_se(pa_atomic_cmpxchg(&c->thread_state, CMT_ASK_QUIT, CMT_QUIT));

    pa_log_debug("cmtspeech thread ended");
}

static int priv_cmtspeech_to_pa_prio(int cmtspprio)
{
    if (cmtspprio == CMTSPEECH_TRACE_ERROR)
       return PA_LOG_ERROR;

    if (cmtspprio == CMTSPEECH_TRACE_INFO)
       return PA_LOG_INFO;

    return PA_LOG_DEBUG;
}

static void priv_cmtspeech_trace_handler_f(int priority, const char *message, va_list args)
{
    pa_log_levelv_meta(priv_cmtspeech_to_pa_prio(priority),
                      "libcmtspeechdata",
                      0,
                      NULL,
                      message,
                      args);
}

/* Main thread */
int cmtspeech_connection_init(struct userdata *u)
{
    struct cmtspeech_connection *c = &u->cmt_connection;

    pa_assert(u);
    pa_assert(!userdata); /* To make sure we are the only instance running. */

    /* Initialized static pointer for memblock free function */
    userdata = u;

    c->cmt_handler = cmtspeech_handler_new(u);
    pa_atomic_store(&c->thread_state, CMT_STARTING);
    c->thread_state_change = pa_fdsem_new();
    c->rtpoll = pa_rtpoll_new();
    c->cmt_poll_item = NULL;
    pa_thread_mq_init(&c->thread_mq, u->core->mainloop, c->rtpoll);
    c->dl_frame_queue = pa_asyncq_new(4);

    c->cmtspeech = NULL;
    c->cmtspeech_mutex = pa_mutex_new(false, false);

    cmtspeech_init();
    cmtspeech_trace_toggle(CMTSPEECH_TRACE_ERROR, true);
    cmtspeech_trace_toggle(CMTSPEECH_TRACE_INFO, true);
    cmtspeech_trace_toggle(CMTSPEECH_TRACE_STATE_CHANGE, true);
    cmtspeech_trace_toggle(CMTSPEECH_TRACE_IO, true);
    cmtspeech_trace_toggle(CMTSPEECH_TRACE_DEBUG, true);
    cmtspeech_set_trace_handler(priv_cmtspeech_trace_handler_f);

    c->call_ul = false;
    c->call_dl = false;
    c->call_emergency = false;

    c->first_dl_frame_received = false;
    c->record_running = false;
    c->playback_running = false;
    c->streams_created = false;

    if (!(c->thread = pa_thread_new("cmtspeech", thread_func, u))) {
        pa_log_error("Failed to create thread.");
        pa_atomic_store(&c->thread_state, CMT_QUIT);
        cmtspeech_connection_unload(u);
        return -1;
    }

    return 0;
}

/* Main thread */
void cmtspeech_connection_unload(struct userdata *u)
{
    struct cmtspeech_connection *c = &u->cmt_connection;

    pa_assert(u);

    switch (pa_atomic_load(&c->thread_state)) {
        default:
            pa_log_error("Undefined thread_state value: %d", pa_atomic_load(&c->thread_state));
            // fall trough
        case CMT_UNINITIALIZED:
            pa_log_debug("No CMT connection to unload");
            return;
        case CMT_STARTING:
            while (pa_atomic_load(&c->thread_state) == CMT_STARTING) {
                pa_log_debug("CMT connection not up yet, waiting...");
                usleep(200000);
            }
            // fall trough
        case CMT_RUNNING:
            pa_assert_se(pa_atomic_cmpxchg(&c->thread_state, CMT_RUNNING, CMT_ASK_QUIT));
            pa_fdsem_post(c->thread_state_change);
            // fall trough
        case CMT_ASK_QUIT:
            while (pa_atomic_load(&c->thread_state) == CMT_ASK_QUIT) {
                pa_log_debug("Waiting for CMT connection thread to quit...");
                usleep(200000);
            }
            pa_log_debug("cmtspeech thread has ended");
            // fall trough
        case CMT_QUIT:
            break;
    }

    pa_atomic_store(&c->thread_state, CMT_UNINITIALIZED);
    if (c->cmt_handler) {
        c->cmt_handler->parent.free((pa_object *)c->cmt_handler);
        c->cmt_handler = NULL;
    }
    if (c->thread_state_change) {
        pa_fdsem_free(c->thread_state_change);
        c->thread_state_change = NULL;
    }
    pa_rtpoll_free(c->rtpoll);
    c->rtpoll = NULL;
    pa_thread_mq_done(&c->thread_mq);

    if (c->cmtspeech) {
        pa_log_error("CMT speech connection up when shutting down");
    }
    pa_asyncq_free(c->dl_frame_queue, NULL);
    pa_mutex_free(c->cmtspeech_mutex);
    userdata = NULL;
    pa_log_debug("CMT connection unloaded");
}

/**
 * Sends an UL frame using SSI audio interface 'sal'.
 *
 * Return zero on success, -1 on error.
 */
/* Source IO-thread */
int cmtspeech_send_ul_frame(struct userdata *u, uint8_t *buf, size_t bytes)
{
    cmtspeech_buffer_t *salbuf;
    int res = -1;
    struct cmtspeech_connection *c = &u->cmt_connection;

    pa_assert(u);

    /* locking note: hot path lock */
    pa_mutex_lock(c->cmtspeech_mutex);

    if (!c->cmtspeech) {
        pa_mutex_unlock(c->cmtspeech_mutex);
        return -EIO;
    }

    if (cmtspeech_is_active(c->cmtspeech) == true)
        res = cmtspeech_ul_buffer_acquire(c->cmtspeech, &salbuf);

    if (res == 0) {
        if (ul_frame_count++ < 10)
            pa_log_debug("Sending ul frame # %d", ul_frame_count);

        /* note: 'bytes' must match the fixed size of frames */
        pa_assert(bytes == (size_t)salbuf->pcount);
        memcpy(salbuf->payload, buf, bytes);
        res = cmtspeech_ul_buffer_release(c->cmtspeech, salbuf);
        if (res < 0) {
          pa_log_error("cmtspeech_ul_buffer_release(%p) failed return value %d.", (void *)salbuf, res);
          if (res == -EIO) {
              /* note: a severe error has occured, close the modem
               *       instance */
              pa_mutex_unlock(c->cmtspeech_mutex);
              pa_log_error("A severe error has occured, close the modem instance.");
              close_cmtspeech_on_error(u);
              pa_mutex_lock(c->cmtspeech_mutex);
          }
        }
        ONDEBUG_TOKENS(fprintf(stderr, "U"));
    } else {
        static uint count = 0;
        if (count++ < 10)
            pa_log_error("cmtspeech_ul_buffer_acquire failed %d", res);
    }

    pa_mutex_unlock(c->cmtspeech_mutex);

    return res;
}

/* This is called form pulseaudio main thread. */
DBusHandlerResult cmtspeech_dbus_filter(DBusConnection *conn, DBusMessage *msg, void *arg)
{
    DBusMessageIter args;
    int type;
    struct userdata *u = arg;
    struct cmtspeech_connection *c = &u->cmt_connection;

    pa_assert(u);

    DBusError dbus_error;
    dbus_error_init(&dbus_error);

    if (dbus_message_is_signal(msg, CMTSPEECH_DBUS_CSCALL_CONNECT_IF, CMTSPEECH_DBUS_CSCALL_CONNECT_SIG)) {
        dbus_bool_t ulflag, dlflag, emergencyflag;

        dbus_message_get_args(msg, &dbus_error,
                              DBUS_TYPE_BOOLEAN, &ulflag,
                              DBUS_TYPE_BOOLEAN, &dlflag,
                              DBUS_TYPE_BOOLEAN, &emergencyflag,
                              DBUS_TYPE_INVALID);

        if (dbus_error_is_set(&dbus_error) != true) {
            pa_log_debug("received AudioConnect with params %d, %d, %d", ulflag, dlflag, emergencyflag);

            c->call_ul = (ulflag == true ? true : false);
            c->call_dl = (dlflag == true ? true : false);
            c->call_emergency = (emergencyflag == true ? true : false);

            /* note: very rarely taken code path */
            pa_mutex_lock(c->cmtspeech_mutex);
            if (c->cmtspeech)
                cmtspeech_state_change_call_connect(c->cmtspeech, dlflag == true);
            pa_mutex_unlock(c->cmtspeech_mutex);

        } else
            pa_log_error("received %s with invalid parameters", CMTSPEECH_DBUS_CSCALL_CONNECT_SIG);

        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_signal(msg, CMTSPEECH_DBUS_CSCALL_STATUS_IF, CMTSPEECH_DBUS_CSCALL_STATUS_SIG)) {
        pa_log_debug("Received ServerStatus");

        if (dbus_message_iter_init(msg, &args) == true) {
            type = dbus_message_iter_get_arg_type(&args);
            dbus_bool_t val;
            if (type == DBUS_TYPE_BOOLEAN) {
                dbus_message_iter_get_basic(&args, &val);

                pa_log_debug("Set ServerStatus to %d.", val == true);

                /* note: very rarely taken code path */
                pa_mutex_lock(c->cmtspeech_mutex);
                if (c->cmtspeech) {
                    cmtspeech_state_change_call_status(c->cmtspeech, val == true);
                    if (val) {
                        /* Call in progress, pause cleanup timer. */
                        pa_atomic_store(&u->cmtspeech_server_status, 1);
                        if (pa_atomic_cmpxchg(&u->cmtspeech_cleanup_state, CMTSPEECH_CLEANUP_TIMER_ACTIVE,
                                                                           CMTSPEECH_CLEANUP_TIMER_INACTIVE)) {
                            pa_log_warn("cmtspeech cleanup timer changed to inactive in DBus thread.");
                        }
                    } else {
                        /* Call ended, set cleanup timer timeout. */
                        u->server_inactive_timeout = pa_rtclock_now() + CMTSPEECH_CLEANUP_TIMER_TIMEOUT;
                        if (pa_atomic_cmpxchg(&u->cmtspeech_cleanup_state, CMTSPEECH_CLEANUP_TIMER_INACTIVE,
                                                                           CMTSPEECH_CLEANUP_TIMER_ACTIVE)) {
                            pa_log_debug("cmtspeech cleanup timer timeout set in DBus thread.");
                        } else {
                            pa_log_debug("cmtspeech cleanup timer is already active or cleanup in progress.");
                        }
                        pa_atomic_store(&u->cmtspeech_server_status, 0);
                    }
                }
                pa_mutex_unlock(c->cmtspeech_mutex);
            } else
                pa_log_warn("received %s with invalid arguments.", CMTSPEECH_DBUS_CSCALL_STATUS_SIG);
        } else
            pa_log_error("received %s with invalid parameters", CMTSPEECH_DBUS_CSCALL_STATUS_SIG);

        return DBUS_HANDLER_RESULT_HANDLED;

    } else if (dbus_message_is_signal(msg, CMTSPEECH_DBUS_PHONE_SSC_STATE_IF, CMTSPEECH_DBUS_PHONE_SSC_STATE_SIG)) {
        const char* modemstate = NULL;

        dbus_message_get_args(msg, &dbus_error,
                              DBUS_TYPE_STRING, &modemstate,
                              DBUS_TYPE_INVALID);

        if (dbus_error_is_set(&dbus_error) != true) {
            pa_log_debug("modem state change: %s", modemstate);
        }
    } else if (dbus_message_is_signal(msg, OFONO_DBUS_VOICECALL_IF, OFONO_DBUS_VOICECALL_CHANGE_SIG)) {
        pa_log_debug("Received voicecall change");
        if (dbus_message_iter_init(msg, &args) == true) {
            if ((type = dbus_message_iter_get_arg_type(&args)) == DBUS_TYPE_STRING) {
                const char* callstr;
                dbus_message_iter_get_basic(&args,&callstr);
                if (strcmp(callstr,"State") == 0) {
                    pa_log_debug("Received voicecall state change");
                    if (dbus_message_iter_next (&args) == true) {
                        DBusMessageIter callstate;
                        const char* callstatestr;
                        dbus_bool_t val = false;
                        dbus_message_iter_recurse(&args,&callstate);
                        dbus_message_iter_get_basic(&callstate,&callstatestr);
                        if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_ACTIVE) == 0) {
                            pa_log_debug("Call active");val = true;
                        } else if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_ALERTING) == 0) {
                            pa_log_debug("Call alerting");
                            val = true;
                        } else if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_HELD) == 0) {
                            pa_log_debug("Call held");
                            val = true;
                        } else if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_WAITING) == 0) {
                            pa_log_debug("Call waiting");
                            val = true;
                        } else if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_INCOMING) == 0) {
                            pa_log_debug("Incoming call");
                            val = false;
                        } else if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_DIALING) == 0) {
                            pa_log_debug("Dialing out");
                            val = false;
                        } else if (strcmp(callstatestr,OFONO_DBUS_VOICECALL_DISCONNECTED) == 0) {
                            pa_log_debug("Call disconnected");
                            val = false;
                        }

                        pa_log_debug("Set ServerStatus to %d.", val == true);
                        /* note: very rarely taken code path */
                        pa_mutex_lock(c->cmtspeech_mutex);
                        if (c->cmtspeech)
                            cmtspeech_state_change_call_status(c->cmtspeech, val == true);
                        pa_mutex_unlock(c->cmtspeech_mutex);

                        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
                    }
                }
            }
        }
        pa_log_error("Received %s with invalid parameters", OFONO_DBUS_VOICECALL_CHANGE_SIG);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
