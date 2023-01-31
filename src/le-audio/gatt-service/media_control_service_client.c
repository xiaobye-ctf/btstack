/*
 * Copyright (C) 2021 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "media_control_service_client.c"

#include "btstack_config.h"

#ifdef ENABLE_TESTING_SUPPORT
#include <stdio.h>
#include <unistd.h>
#endif

#include <stdint.h>
#include <string.h>


#include "le-audio/gatt-service/media_control_service_client.h"

#include "btstack_memory.h"
#include "ble/core.h"
#include "ble/gatt_client.h"
#include "bluetooth_gatt.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "gap.h"

// static btstack_linked_list_t       le_audio_clients;
// active gatt client query
static le_audio_service_client_t * le_audio_active_client;

static void le_audio_service_client_handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// LE Audio Service Client helper functions
static void le_audio_service_client_finalize_connection(le_audio_service_client_t * client, le_audio_service_client_connection_t * connection){
    if (client == NULL){
        return;
    }
    btstack_linked_list_remove(&client->connections, (btstack_linked_item_t*) connection);
    le_audio_active_client = NULL;
}

static le_audio_service_client_connection_t * le_audio_service_client_get_connection_for_con_handle(le_audio_service_client_t * client, hci_con_handle_t con_handle){
    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &client->connections);
    while (btstack_linked_list_iterator_has_next(&it)){
        le_audio_service_client_connection_t * connection = (le_audio_service_client_connection_t *)btstack_linked_list_iterator_next(&it);
        if (connection->con_handle != con_handle) continue;
        return connection;
    }
    return NULL;
}

static le_audio_service_client_connection_t * le_audio_service_client_get_connection_for_cid(le_audio_service_client_t * client, uint16_t connection_cid){
    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &client->connections);
    while (btstack_linked_list_iterator_has_next(&it)){
        le_audio_service_client_connection_t * connection = (le_audio_service_client_connection_t *)btstack_linked_list_iterator_next(&it);
        if (connection->cid != connection_cid) continue;
        return connection;
    }
    return NULL;
}

static void le_audio_service_client_emit_connected(btstack_packet_handler_t event_callback, uint16_t cid, uint8_t subevent, uint8_t status){
    btstack_assert(event_callback != NULL);

    uint8_t event[6];
    int pos = 0;
    event[pos++] = HCI_EVENT_GATTSERVICE_META;
    event[pos++] = sizeof(event) - 2;
    event[pos++] = subevent;
    little_endian_store_16(event, pos, cid);
    pos += 2;
    event[pos++] = status;
    (*event_callback)(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static void le_audio_service_client_emit_disconnected(btstack_packet_handler_t event_callback, uint16_t cid, uint8_t subevent){
    btstack_assert(event_callback != NULL);

    uint8_t event[5];
    int pos = 0;
    event[pos++] = HCI_EVENT_GATTSERVICE_META;
    event[pos++] = sizeof(event) - 2;
    event[pos++] = subevent;
    little_endian_store_16(event, pos, cid);
    pos += 2;
    (*event_callback)(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static uint16_t le_audio_service_client_get_next_cid(le_audio_service_client_t * client){
    client->cid_counter = btstack_next_cid_ignoring_zero(client->cid_counter);
    return client->cid_counter;
}

void le_audio_service_client_hci_event_handler(le_audio_service_client_t * client, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    
    hci_con_handle_t con_handle;
    le_audio_service_client_connection_t * connection;
    
    switch (hci_event_packet_get_type(packet)){
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            connection = le_audio_service_client_get_connection_for_con_handle(client, con_handle);
            if (connection != NULL){
                le_audio_service_client_emit_disconnected(connection->event_callback, connection->cid, GATTSERVICE_SUBEVENT_LE_AUDIO_CLIENT_DISCONNECTED);
                le_audio_service_client_finalize_connection(client, connection);
            }
            break;
        default:
            break;
    }
}

static void le_audio_service_client_run_for_client(le_audio_service_client_t * client, le_audio_service_client_connection_t * connection){
    // gatt_client_characteristic_t characteristic;
    uint8_t status = ATT_ERROR_SUCCESS;
    
    if (le_audio_active_client != NULL){
        return;
    }

    switch (connection->state){
        case LE_AUDIO_SERVICE_CLIENT_STATE_W2_QUERY_SERVICE:
            le_audio_active_client = client;
            connection->state = LE_AUDIO_SERVICE_CLIENT_STATE_W4_SERVICE_RESULT;
            status = gatt_client_discover_primary_services_by_uuid16(&le_audio_service_client_handle_gatt_client_event, connection->con_handle, le_audio_active_client->service_uuid);
            break;

        case LE_AUDIO_SERVICE_CLIENT_STATE_CONNECTED:
            // TODO
            break;
        default:
            break;
    }

    if (status != ATT_ERROR_SUCCESS){
        le_audio_service_client_emit_connected(connection->event_callback, connection->cid, le_audio_active_client->connect_subevent, status);
        le_audio_service_client_finalize_connection(le_audio_active_client, connection);
    }
}

// @return true if client valid / run function should be called
static bool le_audio_service_client_handle_query_complete(le_audio_service_client_connection_t * connection, uint8_t status){
    btstack_assert(le_audio_active_client != NULL);

    switch (connection->state){
        case LE_AUDIO_SERVICE_CLIENT_STATE_W4_SERVICE_RESULT:
            if (status != ATT_ERROR_SUCCESS){
                le_audio_service_client_emit_connected(connection->event_callback, connection->cid, le_audio_active_client->connect_subevent, status);
                le_audio_service_client_finalize_connection(le_audio_active_client, connection);
                return false;
            }

            if (connection->num_instances == 0){
                le_audio_service_client_emit_connected(connection->event_callback, connection->cid, le_audio_active_client->connect_subevent, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                le_audio_service_client_finalize_connection(le_audio_active_client, connection);
                return false;
            }

            connection->state = LE_AUDIO_SERVICE_CLIENT_STATE_W2_QUERY_CHARACTERISTICS;
            break;

        default:
            break;

    }
    // TODO run_for_client
    return true;
}

static void le_audio_service_client_handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(packet_type); 
    UNUSED(channel);
    UNUSED(size);
    
    btstack_assert(le_audio_active_client != NULL);

    le_audio_service_client_connection_t * connection = NULL;
    gatt_client_service_t service;
    // gatt_client_characteristic_t characteristic;
    bool call_run = true;

    switch(hci_event_packet_get_type(packet)){
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            connection = le_audio_service_client_get_connection_for_con_handle(le_audio_active_client, gatt_event_service_query_result_get_handle(packet));
            btstack_assert(connection != NULL);

            if (connection->num_instances < 1){
                gatt_event_service_query_result_get_service(packet, &service);
                connection->start_handle = service.start_group_handle;
                connection->end_handle   = service.end_group_handle;

#ifdef ENABLE_TESTING_SUPPORT
                printf("Service: start handle 0x%04X, end handle 0x%04X\n", connection->start_handle, connection->end_handle);
#endif          
                connection->num_instances++;
            } else {
                log_info("Found more then one Service instance.");
            }
            break;
        
        case GATT_EVENT_QUERY_COMPLETE:
            connection = le_audio_service_client_get_connection_for_con_handle(le_audio_active_client, gatt_event_query_complete_get_handle(packet));
            btstack_assert(connection != NULL);
            call_run = le_audio_service_client_handle_query_complete(connection, gatt_event_query_complete_get_att_status(packet));
            le_audio_active_client = NULL;
            break;

        default:
            break;
    }

    if (call_run && (connection != NULL)){
        le_audio_service_client_run_for_client(le_audio_active_client, connection);
    }
}

static void le_audio_service_client_init(le_audio_service_client_t * service,
    void (*hci_event_handler_trampoline)(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)){
    
    service->hci_event_callback_registration.callback = hci_event_handler_trampoline;
    hci_add_event_handler(&service->hci_event_callback_registration);
}

static uint8_t le_audio_service_client_connect(
    le_audio_service_client_t * service, hci_con_handle_t con_handle, 
    btstack_packet_handler_t packet_handler, uint16_t * connection_cid){
    btstack_assert(packet_handler != NULL);
    
    le_audio_service_client_connection_t * connection = le_audio_service_client_get_connection_for_con_handle(service, con_handle);
    if (connection != NULL){
        return ERROR_CODE_COMMAND_DISALLOWED;
    }

    uint16_t cid = le_audio_service_client_get_next_cid(service);
    if (connection_cid != NULL) {
        *connection_cid = cid;
    }
    
    connection->state = LE_AUDIO_SERVICE_CLIENT_STATE_W2_QUERY_SERVICE;
    connection->cid = *connection_cid;
    connection->con_handle = con_handle;
    connection->event_callback = packet_handler; 
    
    le_audio_service_client_run_for_client(service, connection);

    return ERROR_CODE_SUCCESS;
}

static uint8_t le_audio_service_client_disconnect(le_audio_service_client_t * service, uint16_t connection_cid){
    le_audio_service_client_connection_t * connection = le_audio_service_client_get_connection_for_cid(service, connection_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    // finalize connections
    le_audio_service_client_emit_disconnected(connection->event_callback, connection->cid, GATTSERVICE_SUBEVENT_MCS_CLIENT_DISCONNECTED);
    le_audio_service_client_finalize_connection(service, connection);
    return ERROR_CODE_SUCCESS;
}

static void le_audio_service_client_deinit(le_audio_service_client_t * service){
    service->cid_counter = 0;
    
    btstack_linked_list_iterator_t it;    
    btstack_linked_list_iterator_init(&it, (btstack_linked_list_t *) &service->connections);
    while (btstack_linked_list_iterator_has_next(&it)){
        le_audio_service_client_connection_t * connection = (le_audio_service_client_connection_t *)btstack_linked_list_iterator_next(&it);
        le_audio_service_client_finalize_connection(service, connection);
    }
}


// MSC Client
static le_audio_service_client_t msc_service;

static void mcs_client_packet_handler_trampoline(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    le_audio_service_client_hci_event_handler(&msc_service, packet_type, channel, packet, size);
}

uint8_t media_control_service_client_connect(hci_con_handle_t con_handle, btstack_packet_handler_t packet_handler, uint16_t * mcs_cid){
    return le_audio_service_client_connect(&msc_service, con_handle, packet_handler, mcs_cid);
}

uint8_t media_control_service_client_disconnect(uint16_t mcs_cid){
    return le_audio_service_client_disconnect(&msc_service, mcs_cid);
}

void media_control_service_client_init(void){
    msc_service.disconnect_subevent = GATTSERVICE_SUBEVENT_MCS_CLIENT_DISCONNECTED;
    msc_service.connect_subevent    = GATTSERVICE_SUBEVENT_MCS_CLIENT_CONNECTED;
    msc_service.service_uuid        = ORG_BLUETOOTH_SERVICE_MEDIA_CONTROL_SERVICE;

    le_audio_service_client_init(&msc_service, &mcs_client_packet_handler_trampoline);
}

void media_control_service_client_deinit(void){
    le_audio_service_client_deinit(&msc_service);
}

