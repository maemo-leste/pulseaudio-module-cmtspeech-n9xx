/*
 * This file is part of pulseaudio-meego
 *
 * Copyright (C) 2008, 2009 Nokia Corporation. All rights reserved.
 *
 * Contact: Maemo Multimedia <multimedia@maemo.org>
 *
 * This software, including documentation, is protected by copyright
 * controlled by Nokia Corporation. All rights are reserved.
 *
 * Copying, including reproducing, storing, adapting or translating,
 * any or all of this material requires the prior written consent of
 * Nokia Corporation. This material also contains confidential
 * information which may not be disclosed to others without the prior
 * written consent of Nokia.
 */
#include "module-voice-userdata.h"
#include "module-meego-voice-symdef.h"

#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sample-util.h>

#include "voice-convert.h"
#include "voice-aep-sink-input.h"
#include "voice-hw-sink-input.h"
#include "voice-hw-source-output.h"
#include "voice-raw-sink.h"
#include "voice-raw-source.h"
#include "voice-voip-sink.h"
#include "voice-voip-source.h"
#include "voice-util.h"
#include "voice-aep-ear-ref.h"
#include "voice-mainloop-handler.h"
#include "module-voice-api.h"

#include "proplist-meego.h"

PA_MODULE_AUTHOR("Jyri Sarha");
PA_MODULE_DESCRIPTION("Nokia voice module");
PA_MODULE_USAGE("voice_sink_name=<name for the voice sink> "
                "voice_source_name=<name for the voice source> "
                "master_sink=<sink to connect to> "
                "master_source=<source to connect to> "
                "raw_sink=<name for raw sink> "
                "raw_source=<name for raw source> "
                "max_hw_frag_size=<maximum fragment size of master sink and source in usecs>");
PA_MODULE_VERSION(PACKAGE_VERSION) ;


static const char* const valid_modargs[] = {
    "voice_sink_name",
    "voice_source_name",
    "master_sink",
    "master_source",
    "raw_sink_name",
    "raw_source_name",
    "max_hw_frag_size",
    NULL,
};

static void voice_update_volumes(struct userdata *u) {
    const pa_cvolume *cvol;

    pa_assert(u);
    pa_assert(u->master_sink);

    cvol = &u->master_sink->real_volume;

    if (voice_voip_source_active(u))
        pa_hook_fire(u->hooks[HOOK_CALL_VOLUME], (void*)cvol);
    else
        pa_hook_fire(u->hooks[HOOK_VOLUME], (void*)cvol);
}

static void sink_subscribe_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;
    pa_sink *sink;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE))
        return;

    if (!(sink = pa_idxset_get_by_index(c->sinks, idx)))
        return;

    if (sink != u->master_sink) {
        //pa_log_debug("Not our sink %s (!= %s)", sink->name, u->master_sink->name);
        return;
    }

    voice_update_volumes(u);
}

static int set_hooks(struct userdata *u) {
    pa_assert(u);

    u->algorithm = algorithm_hook_get(u->core);

    u->hooks[HOOK_HW_SINK_PROCESS]              = algorithm_hook_init(u->algorithm, VOICE_HOOK_HW_SINK_PROCESS);
    u->hooks[HOOK_NARROWBAND_EAR_EQU_MONO]      = algorithm_hook_init(u->algorithm, VOICE_HOOK_NARROWBAND_EAR_EQU_MONO);
    u->hooks[HOOK_NARROWBAND_MIC_EQ_MONO]       = algorithm_hook_init(u->algorithm, VOICE_HOOK_NARROWBAND_MIC_EQ_MONO);
    u->hooks[HOOK_WIDEBAND_MIC_EQ_MONO]         = algorithm_hook_init(u->algorithm, VOICE_HOOK_WIDEBAND_MIC_EQ_MONO);
    u->hooks[HOOK_XPROT_MONO]                   = algorithm_hook_init(u->algorithm, VOICE_HOOK_XPROT_MONO);
    u->hooks[HOOK_VOLUME]                       = algorithm_hook_init(u->algorithm, VOICE_HOOK_VOLUME);
    u->hooks[HOOK_CALL_VOLUME]                  = algorithm_hook_init(u->algorithm, VOICE_HOOK_CALL_VOLUME);
    u->hooks[HOOK_CALL_BEGIN]                   = algorithm_hook_init(u->algorithm, VOICE_HOOK_CALL_BEGIN);
    u->hooks[HOOK_CALL_END]                     = algorithm_hook_init(u->algorithm, VOICE_HOOK_CALL_END);
    u->hooks[HOOK_AEP_DOWNLINK]                 = algorithm_hook_init(u->algorithm, VOICE_HOOK_AEP_DOWNLINK);
    u->hooks[HOOK_AEP_UPLINK]                   = algorithm_hook_init(u->algorithm, VOICE_HOOK_AEP_UPLINK);

    return 0;
}

static int unset_hooks(struct userdata *u) {
    pa_assert(u);

    algorithm_hook_done(u->algorithm, VOICE_HOOK_HW_SINK_PROCESS);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_NARROWBAND_EAR_EQU_MONO);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_NARROWBAND_MIC_EQ_MONO);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_WIDEBAND_MIC_EQ_MONO);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_XPROT_MONO);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_VOLUME);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_CALL_VOLUME);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_CALL_BEGIN);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_CALL_END);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_AEP_DOWNLINK);
    algorithm_hook_done(u->algorithm, VOICE_HOOK_AEP_UPLINK);

    algorithm_hook_unref(u->algorithm);
    u->algorithm = NULL;

    return 0;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *master_sink_name;
    const char *master_source_name;
    const char *raw_sink_name;
    const char *raw_source_name;
    const char *voice_sink_name;
    const char *voice_source_name;
    const char *max_hw_frag_size_str;
    int max_hw_frag_size = 3840;
    pa_sink *master_sink;
    pa_source *master_source;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    master_sink_name = pa_modargs_get_value(ma, "master_sink", NULL);
    master_source_name = pa_modargs_get_value(ma, "master_source", NULL);

    raw_sink_name = pa_modargs_get_value(ma, "raw_sink_name", "sink.voice.raw");
    raw_source_name = pa_modargs_get_value(ma, "raw_source_name", "source.voice.raw");
    voice_sink_name = pa_modargs_get_value(ma, "voice_sink_name", "sink.voice");
    voice_source_name = pa_modargs_get_value(ma, "voice_source_name", "source.voice");
    max_hw_frag_size_str = pa_modargs_get_value(ma, "max_hw_frag_size", "3840");

    pa_log_debug("Got arguments: master_sink=\"%s\" master_source=\"%s\" "
		 "raw_sink_name=\"%s\" raw_source_name=\"%s\" max_hw_frag_size=\"%s\".",
		 master_sink_name, master_source_name,
		 raw_sink_name, raw_source_name,
		 max_hw_frag_size_str);

    if (!(master_sink = pa_namereg_get(m->core, master_sink_name, PA_NAMEREG_SINK))) {
        pa_log("Master sink \"%s\" not found", master_sink_name);
        goto fail;
    }

    if (!(master_source = pa_namereg_get(m->core, master_source_name, PA_NAMEREG_SOURCE))) {
        pa_log("Master source \"%s\" not found", master_source_name);
        goto fail;
    }

    if (master_sink->sample_spec.format != master_source->sample_spec.format &&
	master_sink->sample_spec.rate != master_source->sample_spec.rate &&
	master_sink->sample_spec.channels != master_source->sample_spec.channels) {
	pa_log("Master source and sink must have same sample spec");
	goto fail;
    }

    if (pa_atoi(max_hw_frag_size_str, &max_hw_frag_size) < 0 ||
	max_hw_frag_size < 960 ||
	max_hw_frag_size > 128*960) {
	pa_log("Bad value for max_hw_frag_size: %s", max_hw_frag_size_str);
	goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->modargs = ma;
    u->core = m->core;
    u->module = m;
    u->master_sink = master_sink;
    u->master_source = master_source;

    set_hooks(u);

    u->mainloop_handler = voice_mainloop_handler_new(u);

    u->ul_timing_advance = 500; // = 500 micro seconds, seems to be a good default value

    pa_channel_map_init_mono(&u->mono_map);
    pa_channel_map_init_stereo(&u->stereo_map);

    u->hw_sample_spec.format = PA_SAMPLE_S16NE;
    u->hw_sample_spec.rate = SAMPLE_RATE_HW_HZ;
    u->hw_sample_spec.channels = 2;

    u->hw_mono_sample_spec.format = PA_SAMPLE_S16NE;
    u->hw_mono_sample_spec.rate = SAMPLE_RATE_HW_HZ;
    u->hw_mono_sample_spec.channels = 1;

    u->aep_sample_spec.format = PA_SAMPLE_S16NE;
    u->aep_sample_spec.rate = SAMPLE_RATE_AEP_HZ;
    u->aep_sample_spec.channels = 1;
    pa_channel_map_init_mono(&u->aep_channel_map);
    // The result is rounded down incorrectly thus +1
    u->aep_fragment_size = pa_usec_to_bytes(PERIOD_AEP_USECS+1, &u->aep_sample_spec);
    u->aep_hw_fragment_size = pa_usec_to_bytes(PERIOD_AEP_USECS+1, &u->hw_sample_spec);
    u->hw_fragment_size = pa_usec_to_bytes(PERIOD_MASTER_USECS+1, &u->hw_sample_spec);
    u->hw_fragment_size_max = max_hw_frag_size;
    if (0 != (u->hw_fragment_size_max % u->hw_fragment_size))
        u->hw_fragment_size_max += u->hw_fragment_size - (u->hw_fragment_size_max % u->hw_fragment_size);
    u->aep_hw_mono_fragment_size = pa_usec_to_bytes(PERIOD_AEP_USECS+1, &u->hw_mono_sample_spec);
    u->hw_mono_fragment_size = pa_usec_to_bytes(PERIOD_MASTER_USECS+1, &u->hw_mono_sample_spec);

    u->voice_ul_fragment_size = pa_usec_to_bytes(PERIOD_CMT_USECS+1, &u->aep_sample_spec);
    pa_silence_memchunk_get(&u->core->silence_cache,
			    u->core->mempool,
			    &u->aep_silence_memchunk,
			    & u->aep_sample_spec,
			    u->aep_fragment_size);

    voice_memchunk_pool_load(u);

    if (voice_init_raw_sink(u, raw_sink_name))
	goto fail;
    pa_sink_put(u->raw_sink);

    if (voice_init_voip_sink(u, voice_sink_name))
	goto fail;
    pa_sink_put(u->voip_sink);

    if (voice_init_aep_sink_input(u))
	goto fail;

    u->call_state_tracker = pa_call_state_tracker_get(m->core);

    pa_atomic_store(&u->mixer_state, PROP_MIXER_TUNING_PRI);
    pa_call_state_tracker_set_active(u->call_state_tracker, FALSE);
    u->alt_mixer_compensation = PA_VOLUME_NORM;

    if (voice_init_hw_sink_input(u))
	goto fail;

    u->sink_temp_buff = pa_xmalloc(2*u->hw_fragment_size_max);
    u->sink_temp_buff_len = 2*u->hw_fragment_size_max;

    if (voice_init_raw_source(u, raw_source_name))
	goto fail;
    pa_source_put(u->raw_source);

    if (voice_init_voip_source(u, voice_source_name))
	goto fail;
    pa_source_put(u->voip_source);

    if (voice_init_hw_source_output(u))
        goto fail;

    /* TODO: Guess we should use max_hw_frag_size here */
    u->hw_source_memblockq = // 8 * 5ms = 40ms
	pa_memblockq_new(0, 2*u->hw_fragment_size_max, 0, pa_frame_size(&u->hw_sample_spec), 0, 0, 0, NULL);

    u->ul_memblockq =
	pa_memblockq_new(0, 2*u->voice_ul_fragment_size, 0, pa_frame_size(&u->aep_sample_spec), 0, 0, 0, NULL);

    u->dl_sideinfo_queue = pa_queue_new();

    u->ul_deadline = 0;

    u->linear_q15_master_volume_L = INT16_MAX;
    u->linear_q15_master_volume_R = INT16_MAX;

    voice_aep_ear_ref_init(u);

    if (voice_convert_init(u))
	goto fail;

    request_parameter_updates("voice", (pa_hook_cb_t)voice_parameter_cb, PA_HOOK_NORMAL, u);
    request_parameter_updates("alsa", (pa_hook_cb_t)alsa_parameter_cb, PA_HOOK_NORMAL, u);
    request_parameter_updates("aep", (pa_hook_cb_t)aep_parameter_cb, PA_HOOK_LATE, u);

    /*         aep-s-i                                            */
    /* voip-sink ---\                 hw-sink-input               */
    /*                > optimized mix -------------> master-sink  */
    /*                |                                           */
    /*             raw-sink                                       */

    /*                                                  */
    /* voip-src  <---       hw-source-output            */
    /*                < mux <------------- master-src   */
    /*  raw-src  <---                                   */

    pa_source_output_put(u->hw_source_output);
    pa_sink_input_put(u->hw_sink_input);
    pa_sink_input_put(u->aep_sink_input);

    u->sink_subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK, sink_subscribe_cb, u);

    return 0;

fail:
    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u = m->userdata;
    unsigned i;

    assert(m);

    if (!u)
        return;

    if (u->call_state_tracker)
        pa_call_state_tracker_unref(u->call_state_tracker);

    voice_clear_up(u);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    unset_hooks(u);

    pa_xfree(u);
}
