/*
 * This file is part of audio_async_loopback
 * Copyright (c) 2020 Jacob Moroni.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Main AC3 sink implementation. Accepts a pointer to a complete
 * AC3 frame, decodes it, resamples it, and then passes it to
 * the Pulseaudio sink. Like the PCM sink, this also tries to
 * maintain a consistent level in the buffer by dynamically
 * adjusting the sampling rate ratio.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ac3_sink.h"
#include "config.h"

/* Returns the number of space available, in samples. */
static uint32_t buffer_space_avail(struct ac3_sink *inst)
{
    return (AC3_SINK_SAMPLE_BUFFER_SIZE_MASK -
            ((inst->write_idx - inst->read_idx) & AC3_SINK_SAMPLE_BUFFER_SIZE_MASK));
}

/* Returns the current buffer utilization, in samples. */
static uint32_t buffer_used(struct ac3_sink *inst)
{
    return ((inst->write_idx - inst->read_idx) & AC3_SINK_SAMPLE_BUFFER_SIZE_MASK);
}

/* Output thread. Writes data from the intermediate buffer into
 * the Pulseaudio stream in units of AC3_SINK_OUTPUT_CHUNK_SIZE
 * samples.
 */
static void *output_thread(void *arg)
{
    int error;
    uint32_t i;
    float tmp[AC3_SINK_OUTPUT_CHUNK_SIZE];
    struct ac3_sink *inst = (struct ac3_sink *)arg;

    while (1) {
        pthread_mutex_lock(&inst->lock);

        /* Wait for data. */
        while ((buffer_used(inst) < AC3_SINK_OUTPUT_CHUNK_SIZE) && inst->thread_run) {
            pthread_cond_wait(&inst->cond, &inst->lock);
        }

        if (!inst->thread_run) {
            /* Terminate. */
            pthread_mutex_unlock(&inst->lock);
            pthread_exit(NULL);
        }

        /* Copy out one chunk. */
        for (i = 0; i < AC3_SINK_OUTPUT_CHUNK_SIZE; i++) {
            tmp[i] = inst->buffer[inst->read_idx];
            inst->read_idx++;
            inst->read_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;
        }

        pthread_mutex_unlock(&inst->lock);

        if (pa_simple_write(inst->pa_inst, tmp, sizeof(tmp), &error) < 0) {
            printf("Could not write chunk to output stream (error = %d)\n", error);
        }
    }

    /* Not reached. */
    pthread_exit(NULL);
}

/* Calculate a new sampling rate ratio. This should be called
 * before adding a new chunk to the ring buffer, and must be
 * called with the lock held.
 */
static double calculate_rate_ratio(struct ac3_sink *inst)
{
    const int32_t tmp = buffer_used(inst);
    const double mult = AC3_SINK_LOOP_GAIN;
    int32_t offset = AC3_BUFFER_TARGET_SAMPLES - tmp;

    /* Clamp the max offset so that the max rate ratio is
     * purely limited by the gain.
     */
    if (offset < -AC3_BUFFER_TARGET_SAMPLES) {
        offset = -AC3_BUFFER_TARGET_SAMPLES;
    } else if (offset > AC3_BUFFER_TARGET_SAMPLES) {
        offset = AC3_BUFFER_TARGET_SAMPLES;
    }

    return ((mult * offset) + 1.0);
}

/* Open the ac3 sink. */
void ac3_sink_open(struct ac3_sink *inst)
{
    size_t i;
    int error;
    pa_buffer_attr attr;

    static const pa_sample_spec pa_ss = {
        .format = PA_SAMPLE_FLOAT32LE,
        .rate = 48000,
        .channels = 6
    };

    static const pa_channel_map channel_map = {
        .channels = 6,
        .map[0] = PA_CHANNEL_POSITION_FRONT_LEFT,
        .map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT,
        .map[2] = PA_CHANNEL_POSITION_FRONT_CENTER,
        .map[3] = PA_CHANNEL_POSITION_LFE,
#ifdef USE_AC3_SURROUND_MAPPING
        .map[4] = PA_CHANNEL_POSITION_SIDE_LEFT,
        .map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT,
#else
        .map[4] = PA_CHANNEL_POSITION_REAR_LEFT,
        .map[5] = PA_CHANNEL_POSITION_REAR_RIGHT,
#endif
    };

    memset(inst, 0, sizeof(struct ac3_sink));

    pthread_mutex_init(&inst->lock, NULL);
    pthread_cond_init(&inst->cond, NULL);

    /* Open decoder context. */
    av_init_packet(&inst->packet);
    inst->frame = av_frame_alloc();

    /* TODO - Handle all of these failure cases. */

    inst->codec = avcodec_find_decoder(AV_CODEC_ID_AC3);
    if (!inst->codec) {
        printf("Can't find AC3 decoder\n");
    }

    inst->cctx = avcodec_alloc_context3(inst->codec);
    if (!inst->cctx) {
        printf("Couldn't allocate codec context\n");
    }

    if (avcodec_open2(inst->cctx, inst->codec, NULL) < 0) {
        printf("Couldn't open codec\n");
    }

    /* Allocate a separate resampler for each channel. This is done
     * because the resampler expects the channels to be interleaved
     * into one array, but libavcodec gives it to us in separate arrays.
     */
    for (i = 0; i < AC3_SINK_NUM_CHANNELS; i++) {
        inst->rate_converter[i] = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
        if (!inst->rate_converter[i]) {
            printf("Could not create sample rate converter instance\n");
            /* TODO - Handle failure. Program will crash if output is called... */
        }
    }

    /* Configure buffer for low latency. */
    attr.maxlength = AC3_SINK_PA_BUFFER_SIZE;
    attr.tlength = AC3_SINK_PA_BUFFER_SIZE;
    attr.prebuf = AC3_SINK_PA_BUFFER_SIZE;
    attr.minreq = 8;
    attr.fragsize = -1;

    /* Open simple pulseaudio context. */
    inst->pa_inst = pa_simple_new(NULL,
                                  PROGRAM_NAME_STR,
                                  PA_STREAM_PLAYBACK,
                                  NULL,
                                  "Audio Async Loopback",
                                  &pa_ss,
                                  &channel_map,
                                  &attr,
                                  &error);
    if (!inst->pa_inst) {
        printf("Could not open Pulseaudio context (error = %d)\n", error);
        /* TODO - Handle failure. Program will crash if output is called... */
    }

    /* Pre-set these fields as an optimization. Only the required
     * fields get updated in the process call.
     */
    inst->src_data.output_frames = (sizeof(inst->tmp_output_buf[0]) / sizeof(float));
    inst->src_data.end_of_input = 0;
    inst->src_data.src_ratio = 1.0;

    inst->thread_run = true;
    pthread_create(&inst->thread, NULL, output_thread, inst);
    /* TODO - Check return. */
}

/* Close the ac3 sink. */
void ac3_sink_close(struct ac3_sink *inst)
{
    size_t i;
    int error;

    /* Kill the thread. */
    pthread_mutex_lock(&inst->lock);
    inst->thread_run = false;
    pthread_cond_broadcast(&inst->cond);
    pthread_mutex_unlock(&inst->lock);
    pthread_join(inst->thread, NULL);

    /* Kill Pulseaudio connection. */
    pa_simple_flush(inst->pa_inst, &error);
    pa_simple_free(inst->pa_inst);

    /* Cleanup the rate converter. */
    for (i = 0; i < AC3_SINK_NUM_CHANNELS; i++) {
        src_delete(inst->rate_converter[i]);
    }

    avcodec_close(inst->cctx);
    avcodec_free_context(&inst->cctx);
    av_frame_free(&inst->frame);
}

/* Send a chunk of interleaved left/right s16le ac3 samples
 * to the sink. There's no length argument because this sub-module
 * relies on the top level chunk size anyway...
 */
void ac3_sink_process(struct ac3_sink *inst, uint8_t *data, size_t len)
{
    size_t i;
    int error;
    int got_one;
    uint32_t can_queue;

    inst->packet.data = data;
    inst->packet.size = len;

    error = avcodec_decode_audio4(inst->cctx, inst->frame, &got_one, &inst->packet);
    if (error < 0) {
        printf("Error decoding AC3 frame\n");
        return;
    }

    if (!got_one) {
        printf("No AC3 frame was decoded\n");
        return;
    }

    if (inst->frame->channels != 6) {
        /* Only 5.1 is supported for now. This is mainly because I don't
         * handle all of the other channel mappings yet. I suppose this
         * could be fixed by defining all of the possible mappings and
         * using a lookup table with different ring buffer write routines.
         */
        printf("Only 5.1 is supported right now (channels = %d)\n", inst->frame->channels);
        return;
    }

    /* Resample each channel. */
    for (i = 0; i < AC3_SINK_NUM_CHANNELS; i++) {
        inst->src_data.data_in = (float *)inst->frame->data[i];
        inst->src_data.data_out = inst->tmp_output_buf[i];
        inst->src_data.input_frames = inst->frame->nb_samples;

        /* Resample. */
        if ((error = src_process(inst->rate_converter[i], &inst->src_data))) {
            printf("AC3 sink rate converter error %s\n",  src_strerror(error));
        }
    }

    /* NOTE: The resampler is being called with the same ratio for each channel,
     *       so the number of output frames should be the same for all channels.
     */

    pthread_mutex_lock(&inst->lock);

    inst->src_data.src_ratio = calculate_rate_ratio(inst);

#if DEBUG
    printf("Buffer: %04d    Ratio: %f\n",buffer_used(inst), inst->src_data.src_ratio);
#endif

    /* First, figure out how many samples we can queue. */
    can_queue = buffer_space_avail(inst);

    if (can_queue < (inst->src_data.output_frames_gen * 6)) {
       printf("Can't fit entire frame, so dropping entire frame (%d < %lu)\n",
              can_queue,
              inst->src_data.output_frames_gen * 6);
       pthread_mutex_unlock(&inst->lock);
       return;
    }

    /* Copy into ring buffer, observing the channel mapping. */
    for (i = 0; i < inst->src_data.output_frames_gen; i++) {

        /* Front left. */
        inst->buffer[inst->write_idx] = inst->tmp_output_buf[0][i];
        inst->write_idx++;
        inst->write_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;

        /* Front right. */
        inst->buffer[inst->write_idx] = inst->tmp_output_buf[1][i];
        inst->write_idx++;
        inst->write_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;

        /* Center. */
        inst->buffer[inst->write_idx] = inst->tmp_output_buf[2][i];
        inst->write_idx++;
        inst->write_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;

        /* LFE. */
        inst->buffer[inst->write_idx] = inst->tmp_output_buf[3][i];
        inst->write_idx++;
        inst->write_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;

        /* Rear left. */
        inst->buffer[inst->write_idx] = inst->tmp_output_buf[4][i];
        inst->write_idx++;
        inst->write_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;

        /* Rear right. */
        inst->buffer[inst->write_idx] = inst->tmp_output_buf[5][i];
        inst->write_idx++;
        inst->write_idx &= AC3_SINK_SAMPLE_BUFFER_SIZE_MASK;
    }

    pthread_mutex_unlock(&inst->lock);
    pthread_cond_broadcast(&inst->cond);
}
