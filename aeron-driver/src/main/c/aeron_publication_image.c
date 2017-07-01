/*
 * Copyright 2014 - 2017 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "concurrent/aeron_term_rebuilder.h"
#include "util/aeron_error.h"
#include "aeron_publication_image.h"

int aeron_publication_image_create(
    aeron_publication_image_t **image,
    aeron_receive_channel_endpoint_t *endpoint,
    aeron_driver_context_t *context,
    int64_t correlation_id,
    int32_t session_id,
    int32_t stream_id,
    int32_t initial_term_id,
    int32_t active_term_id,
    int32_t initial_term_offset,
    aeron_position_t *rcv_hwm_position,
    aeron_position_t *rcv_pos_position,
    aeron_congestion_control_strategy_t *congestion_control,
    struct sockaddr_storage *control_address,
    struct sockaddr_storage *source_address,
    int32_t term_buffer_length,
    int32_t sender_mtu_length,
    bool is_reliable,
    aeron_system_counters_t *system_counters)
{
    char path[AERON_MAX_PATH];
    int path_length =
        aeron_publication_image_location(
            path,
            sizeof(path),
            context->aeron_dir,
            endpoint->conductor_fields.udp_channel->canonical_form,
            session_id,
            stream_id,
            correlation_id);
    aeron_publication_image_t *_image = NULL;
    const uint64_t usable_fs_space = context->usable_fs_space_func(context->aeron_dir);
    const uint64_t log_length = AERON_LOGBUFFER_COMPUTE_LOG_LENGTH(term_buffer_length);

    *image = NULL;

    if (usable_fs_space < log_length)
    {
        aeron_set_err(ENOSPC, "Insufficient usable storage for new log of length=%d in %s", log_length, context->aeron_dir);
        return -1;
    }

    if (aeron_alloc((void **)&_image, sizeof(aeron_publication_image_t)) < 0)
    {
        aeron_set_err(ENOMEM, "%s", "Could not allocate publication image");
        return -1;
    }

    _image->log_file_name = NULL;
    if (aeron_alloc((void **)(&_image->log_file_name), (size_t)path_length) < 0)
    {
        aeron_free(_image);
        aeron_set_err(ENOMEM, "%s", "Could not allocate publication image log_file_name");
        return -1;
    }

    if (context->map_raw_log_func(&_image->mapped_raw_log, path, context->term_buffer_sparse_file, (uint64_t)term_buffer_length) < 0)
    {
        aeron_free(_image->log_file_name);
        aeron_free(_image);
        aeron_set_err(aeron_errcode(), "error mapping network raw log %s: %s", path, aeron_errmsg());
        return -1;
    }
    _image->map_raw_log_close_func = context->map_raw_log_close_func;

    strncpy(_image->log_file_name, path, path_length);
    _image->log_file_name_length = (size_t)path_length;
    _image->log_meta_data = (aeron_logbuffer_metadata_t *)(_image->mapped_raw_log.log_meta_data.addr);

    _image->log_meta_data->initialTerm_id = initial_term_id;
    _image->log_meta_data->mtu_length = sender_mtu_length;
    _image->log_meta_data->correlation_id = correlation_id;
    _image->log_meta_data->time_of_last_status_message = 0;
    _image->log_meta_data->end_of_stream_position = INT64_MAX;
    aeron_logbuffer_fill_default_header(
        _image->mapped_raw_log.log_meta_data.addr, session_id, stream_id, initial_term_id);

    _image->endpoint = endpoint;
    _image->congestion_control = congestion_control;
    _image->nano_clock = context->nano_clock;
    _image->epoch_clock = context->epoch_clock;
    _image->conductor_fields.subscribeable.array = NULL;
    _image->conductor_fields.subscribeable.length = 0;
    _image->conductor_fields.subscribeable.capacity = 0;
    _image->conductor_fields.managed_resource.registration_id = correlation_id;
    _image->conductor_fields.managed_resource.clientd = _image;
    _image->conductor_fields.managed_resource.incref = NULL;
    _image->conductor_fields.managed_resource.decref = NULL;
    _image->conductor_fields.has_reached_end_of_life = false;
    _image->conductor_fields.status = AERON_PUBLICATION_IMAGE_STATUS_INIT;
    _image->conductor_fields.time_of_last_activity_ns = 0;
    _image->conductor_fields.liveness_timeout_ns = context->image_liveness_timeout_ns;
    _image->session_id = session_id;
    _image->stream_id = stream_id;
    _image->rcv_hwm_position.counter_id = rcv_hwm_position->counter_id;
    _image->rcv_hwm_position.value_addr = rcv_hwm_position->value_addr;
    _image->rcv_pos_position.counter_id = rcv_pos_position->counter_id;
    _image->rcv_pos_position.value_addr = rcv_pos_position->value_addr;
    _image->initial_term_id = initial_term_id;
    _image->term_length_mask = (int32_t)term_buffer_length - 1;
    _image->position_bits_to_shift = (size_t)aeron_number_of_trailing_zeroes((int32_t)term_buffer_length);
    _image->mtu_length = sender_mtu_length;
    _image->last_sm_change_number = -1;

    memcpy(&_image->control_address, control_address, sizeof(_image->control_address));
    memcpy(&_image->source_address, source_address, sizeof(_image->source_address));

    _image->heartbeats_received_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_HEARTBEATS_RECEIVED);
    _image->flow_control_under_runs_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_FLOW_CONTROL_UNDER_RUNS);
    _image->flow_control_over_runs_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_FLOW_CONTROL_OVER_RUNS);
    _image->status_messages_sent_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_STATUS_MESSAGES_SENT);

    const int64_t initial_position =
        aeron_logbuffer_compute_position(
            active_term_id, initial_term_offset, _image->position_bits_to_shift, initial_term_id);

    _image->begin_sm_change = -1;
    _image->end_sm_change = -1;
    _image->next_sm_position = initial_position;
    _image->next_sm_receiver_window_length =
        _image->congestion_control->initial_window_length(_image->congestion_control->state);
    _image->conductor_fields.clean_position = initial_position;

    aeron_counter_set_ordered(_image->rcv_hwm_position.value_addr, initial_position);
    aeron_counter_set_ordered(_image->rcv_pos_position.value_addr, initial_position);

    *image = _image;
    return 0;
}

int aeron_publication_image_close(aeron_counters_manager_t *counters_manager, aeron_publication_image_t *image)
{
    if (NULL != image)
    {
        aeron_subscribeable_t *subscribeable = &image->conductor_fields.subscribeable;

        aeron_counters_manager_free(counters_manager, (int32_t)image->rcv_hwm_position.counter_id);
        aeron_counters_manager_free(counters_manager, (int32_t)image->rcv_pos_position.counter_id);

        for (size_t i = 0, length = subscribeable->length; i < length; i++)
        {
            aeron_counters_manager_free(counters_manager, (int32_t)subscribeable->array[i].counter_id);
        }

        aeron_free(subscribeable->array);

        image->map_raw_log_close_func(&image->mapped_raw_log);
        image->congestion_control->fini(image->congestion_control);
        aeron_free(image->log_file_name);
    }

    aeron_free(image);
    return 0;
}

void aeron_publication_image_clean_buffer_to(aeron_publication_image_t *image, int64_t new_clean_position)
{
    const int64_t clean_position = image->conductor_fields.clean_position;
    const size_t bytes_for_cleaning = (size_t)(new_clean_position - clean_position);
    const size_t dirty_term_index = aeron_logbuffer_index_by_position(clean_position, image->position_bits_to_shift);
    const int32_t term_offset = (int32_t)(clean_position & image->term_length_mask);
    const size_t bytes_left_in_term = image->term_length_mask + 1 - (size_t)term_offset;
    const size_t length = bytes_for_cleaning < bytes_left_in_term ? bytes_for_cleaning : bytes_left_in_term;

    if (length > 0)
    {
        memset(image->mapped_raw_log.term_buffers[dirty_term_index].addr + term_offset, 0, length);
        image->conductor_fields.clean_position = clean_position + (int64_t)length;
    }
}

void aeron_publication_image_track_rebuild(
    aeron_publication_image_t *image, int64_t now_ns, int64_t status_message_timeout)
{
    int64_t min_sub_pos = INT64_MAX;
    int64_t max_sub_pos = INT64_MIN;

    for (size_t i = 0, length = image->conductor_fields.subscribeable.length; i < length; i++)
    {
        int64_t position = aeron_counter_get_volatile(image->conductor_fields.subscribeable.array[i].value_addr);

        min_sub_pos = (position < min_sub_pos) ? (position) : (min_sub_pos);
        max_sub_pos = (position > max_sub_pos) ? (position) : (max_sub_pos);
    }

    const int64_t rebuild_position =
        *image->rcv_pos_position.value_addr > max_sub_pos ? *image->rcv_pos_position.value_addr : max_sub_pos;
    int64_t hwm_position = aeron_counter_get_volatile(image->rcv_hwm_position.value_addr);

    bool loss_found = false;
    /* TODO: loss detector scan */
    const int32_t rebuild_term_offset = (int32_t)(rebuild_position & image->term_length_mask);
    const int64_t new_rebuild_position = (rebuild_position - rebuild_term_offset) + 0; /* TODO: use outcome of scan */

    aeron_counter_propose_max_ordered(image->rcv_pos_position.value_addr, new_rebuild_position);

    bool should_force_send_sm = false;
    const int32_t window_length = image->congestion_control->on_track_rebuild(
        image->congestion_control->state,
        &should_force_send_sm,
        now_ns,
        min_sub_pos,
        image->next_sm_position,
        hwm_position,
        rebuild_position,
        new_rebuild_position,
        loss_found);

    const int32_t threshold = window_length / 4;

    if (should_force_send_sm ||
        (now_ns > (image->last_status_mesage_timestamp + status_message_timeout)) ||
        (min_sub_pos > (image->next_sm_position + threshold)))
    {
        aeron_publication_image_schedule_status_message(image, now_ns, min_sub_pos, window_length);
        aeron_publication_image_clean_buffer_to(image, min_sub_pos - (image->term_length_mask + 1));
    }
}

int aeron_publication_image_insert_packet(
    aeron_publication_image_t *image, int32_t term_id, int32_t term_offset, const uint8_t *buffer, size_t length)
{
    const bool is_heartbeat = aeron_publication_image_is_heartbeat(buffer, length);
    const int64_t packet_position =
        aeron_logbuffer_compute_position(term_id, term_offset, image->position_bits_to_shift, image->initial_term_id);
    const int64_t proposed_position = is_heartbeat ? packet_position : packet_position + (int64_t)length;
    const int64_t window_position = image->next_sm_position;

    if (!aeron_publication_image_is_flow_control_under_run(image, window_position, packet_position) &&
        !aeron_publication_image_is_flow_control_over_run(image, window_position, proposed_position))
    {
        if (aeron_publication_image_is_heartbeat(buffer, length))
        {
            if (aeron_publication_image_is_end_of_stream(buffer, length))
            {
                AERON_PUT_ORDERED(image->log_meta_data->end_of_stream_position, packet_position);
            }

            aeron_counter_ordered_increment(image->heartbeats_received_counter, 1);
        }
        else
        {
            const size_t index = aeron_logbuffer_index_by_position(packet_position, image->position_bits_to_shift);
            uint8_t *term_buffer = image->mapped_raw_log.term_buffers[index].addr;

            aeron_term_rebuilder_insert(term_buffer + term_offset, buffer, length);
        }

        aeron_publication_image_hwm_candidate(image, proposed_position);
    }

    return (int)length;
}

int aeron_publication_image_on_rttm(
    aeron_publication_image_t *image, aeron_rttm_header_t *header, struct sockaddr_storage *addr)
{
    const int64_t now_ns = image->nano_clock();
    const int64_t rtt_in_ns = now_ns - header->echo_timestamp - header->reception_delta;

    image->congestion_control->on_rttm(image->congestion_control->state, now_ns, rtt_in_ns, addr);
    return 1;
}

int aeron_publicaion_image_send_pending_status_message(aeron_publication_image_t *image)
{
    int work_count = 0;

    if (AERON_PUBLICATION_IMAGE_STATUS_ACTIVE == image->conductor_fields.status)
    {
        int64_t change_number;
        AERON_GET_VOLATILE(change_number, image->end_sm_change);

        if (change_number != image->last_sm_change_number)
        {
            const int64_t sm_position = image->next_sm_position;
            const int32_t receiver_window_length = image->next_sm_receiver_window_length;

            aeron_acquire(); /* loadFence */

            if (change_number == image->begin_sm_change)
            {
                const int32_t term_id =
                    aeron_logbuffer_compute_term_id_from_position(
                        sm_position, image->position_bits_to_shift, image->initial_term_id);
                const int32_t term_offset = (int32_t)(sm_position & image->term_length_mask);

                int send_sm_result = send_sm_result = aeron_receive_channel_endpoint_send_sm(
                    image->endpoint,
                    &image->control_address,
                    image->stream_id,
                    image->session_id,
                    term_id,
                    term_offset,
                    receiver_window_length,
                    0);

                aeron_counter_ordered_increment(image->status_messages_sent_counter, 1);

                image->last_sm_change_number = change_number;
                work_count = send_sm_result < 0 ? send_sm_result : 1;
            }
        }
    }

    return work_count;
}

extern bool aeron_publication_image_is_heartbeat(const uint8_t *buffer, size_t length);
extern bool aeron_publication_image_is_end_of_stream(const uint8_t *buffer, size_t length);
extern bool aeron_publication_image_is_flow_control_under_run(
    aeron_publication_image_t *image, int64_t window_position, int64_t packet_position);
extern bool aeron_publication_image_is_flow_control_over_run(
    aeron_publication_image_t *image, int64_t window_position, int64_t proposed_position);
extern void aeron_publication_image_hwm_candidate(aeron_publication_image_t *image, int64_t proposed_position);
extern void aeron_publication_image_schedule_status_message(
    aeron_publication_image_t *image, int64_t now_ns, int64_t sm_position, int32_t window_length);