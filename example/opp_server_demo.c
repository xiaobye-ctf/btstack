/*
 * Copyright (C) 2022 BlueKitchen GmbH
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

#define BTSTACK_FILE__ "opp_server_demo.c"

// *****************************************************************************
/* EXAMPLE_START(pbap_server_demo): OPP Server - Demo OPP Server
 */
// *****************************************************************************


#define OPP_SERVER_L2CAP_PSM         0x1001
#define OPP_SERVER_RFCOMM_CHANNEL_NR 1

#include "btstack_config.h"

#include <stdint.h>
#include <stdio.h>

#include "btstack.h"

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration;

static uint16_t opp_cid;

#ifdef HAVE_POSIX_FILE_IO
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
static int outfile_fd = -1;
static uint32_t expected_bytes = 0;
#endif

static uint8_t service_buffer[150];

// static uint32_t sdp_service_record_handle;

static const uint8_t supported_formats[] = { 1, 2, 3, 4, 5, 6};
static uint8_t handle_pull_default_object = 1;
static uint8_t handle_push_object_response = OBEX_RESP_SUCCESS;

// from https://www.w3.org/2002/12/cal/vcard-examples/
static const char *default_object_vcards[] = {
    "BEGIN:VCARD\n"
    "VERSION:3.0\n"
    "N:Doe;John;;;\n"
    "FN:John Doe\n"
    "ORG:Example.com Inc.;\n"
    "TITLE:Imaginary test person\n"
    "EMAIL;type=INTERNET;type=WORK;type=pref:johnDoe@example.org\n"
    "TEL;type=WORK;type=pref:+1 617 555 1212\n"
    "TEL;type=WORK:+1 (617) 555-1234\n"
    "TEL;type=CELL:+1 781 555 1212\n"
    "TEL;type=HOME:+1 202 555 1212\n"
    "item1.ADR;type=WORK:;;2 Enterprise Avenue;Worktown;NY;01111;USA\n"
    "item1.X-ABADR:us\n"
    "item2.ADR;type=HOME;type=pref:;;3 Acacia Avenue;Hoemtown;MA;02222;USA\n"
    "item2.X-ABADR:us\n"
    "NOTE:John Doe has a long and varied history\\, being documented on more police files that anyone else. Reports of his death are alas numerous.\n"
    "item3.URL;type=pref:http\\://www.example/com/doe\n"
    "item3.X-ABLabel:_$!<HomePage>!$_\n"
    "item4.URL:http\\://www.example.com/Joe/foaf.df\n"
    "item4.X-ABLabel:FOAF\n"
    "item5.X-ABRELATEDNAMES;type=pref:Jane Doe\n"
    "item5.X-ABLabel:_$!<Friend>!$_\n"
    "CATEGORIES:Work,Test group\n"
    "X-ABUID:5AD380FD-B2DE-4261-BA99-DE1D1DB52FBE\\:ABPerson\n"
    "END:VCARD"
};

#ifdef HAVE_BTSTACK_STDIN

// Testing User Interface
static void show_usage(void){
    bd_addr_t iut_address;
    gap_local_bd_addr(iut_address);

    printf("\n--- Bluetooth OPP Server Test Console %s ---\n", bd_addr_to_str(iut_address));
    printf("d - toggle availability of the default object (current: %savailable)\n", handle_pull_default_object ? "" : "un");
    printf("p - toggle acceptance of push requests (current: %02x)\n", handle_push_object_response);

    printf("\n");
}

static void stdin_process(char c) {
    log_info("stdin: %c", c);
    switch (c) {
        case '\n':
        case '\r':
            break;
        case 'd':
            handle_pull_default_object = !handle_pull_default_object;
            printf ("[+] Default object (text/vcard) is now %savailable\n",
                    handle_pull_default_object ? "" : "un");
            break;
        case 'p':
            switch (handle_push_object_response) {
                case OBEX_RESP_SUCCESS:
                    handle_push_object_response = OBEX_RESP_UNSUPPORTED_MEDIA_TYPE;
                    break;
                case OBEX_RESP_UNSUPPORTED_MEDIA_TYPE:
                    handle_push_object_response = OBEX_RESP_ENTITY_TOO_LARGE;
                    break;
                case OBEX_RESP_ENTITY_TOO_LARGE:
                default:
                    handle_push_object_response = OBEX_RESP_SUCCESS;
                    break;
            }
            printf ("[+] pushing objects is now %s\n",
                    handle_push_object_response == OBEX_RESP_ENTITY_TOO_LARGE ? "refused due to size" :
                    handle_push_object_response == OBEX_RESP_UNSUPPORTED_MEDIA_TYPE ? "refused due to media type" :
                    "allowed");
            break;
        default:
            show_usage();
            break;
    }
}
#endif


// packet handler
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t status;
    bd_addr_t event_addr;
    uint32_t object_size;
    char filename[32];
    char filetype[16];
    uint16_t filename_len;
    uint16_t filetype_len;

    switch (packet_type){
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    // BTstack activated, get started_
                    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING){
#ifdef HAVE_BTSTACK_STDIN
                        show_usage();
#endif
                    }
                    break;
                case HCI_EVENT_PIN_CODE_REQUEST:
                    // inform about pin code request
                    printf("Pin code request - using '0000'\n");
                    hci_event_pin_code_request_get_bd_addr(packet, event_addr);
                    gap_pin_code_response(event_addr, "0000");
                    break;
                case HCI_EVENT_OPP_META:
                    switch (hci_event_opp_meta_get_subevent_code(packet)){
                        case OPP_SUBEVENT_CONNECTION_OPENED:
                            status = opp_subevent_connection_opened_get_status(packet);
                            if (status){
                                printf("[!] Connection failed, status 0x%02x\n", status);
                            } else {
                                opp_cid = opp_subevent_connection_opened_get_opp_cid(packet);
                                printf("[+] Connected opp_cid 0x%04x\n", opp_cid);
                            }
                            break;
                        case OPP_SUBEVENT_CONNECTION_CLOSED:
                            printf("[+] Connection closed\n");
                            break;
                        case OPP_SUBEVENT_PUSH_OBJECT:
                            object_size = opp_subevent_push_object_get_object_size(packet);
                            filename_len = btstack_min(sizeof(filename) - 1, opp_subevent_push_object_get_name_len(packet));
                            filetype_len = btstack_min(sizeof(filetype) - 1, opp_subevent_push_object_get_type_len(packet));
                            memcpy(filename, opp_subevent_push_object_get_name(packet), filename_len);
                            memcpy(filetype, opp_subevent_push_object_get_type(packet), filetype_len);
                            filename[filename_len] = 0;
                            filetype[filetype_len] = 0;
                            printf("PUSH: \"%s\" (type '%s', %d bytes)\n", filename, filetype, object_size);

                            if (handle_push_object_response != OBEX_RESP_SUCCESS){
                                printf("PUSH: Rejected with reason 0x%02x\n", handle_push_object_response);
                                opp_server_abort_request (opp_cid,handle_push_object_response);
                            }

#ifdef HAVE_POSIX_FILE_IO
                            if (handle_push_object_response == OBEX_RESP_SUCCESS &&
                                outfile_fd < 0) {
                                outfile_fd = open (filename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
                                if (outfile_fd < 0)
                                    perror ("failed to open output file");
                                else
                                    expected_bytes = object_size;
                            }
#endif

                            break;
                        case OPP_SUBEVENT_PULL_DEFAULT_OBJECT:
                            if (handle_pull_default_object) {
                                uint32_t position;
                                uint16_t max_size;
                                uint16_t data_size;
                                uint8_t  resp = OBEX_RESP_SUCCESS;

                                position = opp_subevent_pull_default_object_get_position (packet);
                                max_size = opp_subevent_pull_default_object_get_buffer_size (packet);
                                data_size = strlen (default_object_vcards[0] + position);
                                if (data_size > max_size) {
                                    data_size = max_size;
                                    resp = OBEX_RESP_CONTINUE;
                                }

                                printf("PULL Default Object(position %u, max size %u): send %u bytes\n", position, max_size, data_size);

                                status = opp_server_send_pull_response (opp_cid, resp,
                                                               data_size,
                                                               (uint8_t *) (default_object_vcards[0] + position));

                            } else {
                                printf("PULL Default Object: reject with OBEX_RESP_NOT_FOUND\n");
                                status = opp_server_send_pull_response (opp_cid, OBEX_RESP_NOT_FOUND, 0, NULL);
                            }
                            btstack_assert(status == ERROR_CODE_SUCCESS);
                            break;
                        case OPP_SUBEVENT_OPERATION_COMPLETED:
                            printf("[+] Operation complete, status 0x%02x\n", opp_subevent_operation_completed_get_status(packet));
                            break;

                        default:
                            log_info("[+] OPP event packet of type %d\n", hci_event_opp_meta_get_subevent_code(packet));
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        case OPP_DATA_PACKET:
#ifdef HAVE_POSIX_FILE_IO
            if (outfile_fd >= 0) {
                if (write (outfile_fd, packet, size) < size) {
                    perror ("write did not complete");
                }
                expected_bytes -= size;

                if (expected_bytes <= 0) {
                    close (outfile_fd);
                    outfile_fd = -1;
                    expected_bytes = 0;
                }
            }
#endif
#if 0
            {
                uint16_t i;
                for (i=0;i<size;i++){
                    printf("%c", packet[i]);
                }
                printf ("\n");
            }
#else
            printf("OPP Data: %u bytes, need %u more\n", size, expected_bytes);
#endif
            break;
        default:
            log_info ("[-] packet of type %d\n", packet_type);
            break;
    }
}



int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void)argc;
    (void)argv;

    gap_discoverable_control(1);
    gap_set_class_of_device(0x200408);
    gap_set_local_name("OPP Server Demo 00:00:00:00:00:00");

    // init L2CAP
    l2cap_init();

#ifdef ENABLE_BLE
    // Initialize LE Security Manager. Needed for cross-transport key derivation
    sm_init();
#endif

    // init RFCOM
    rfcomm_init();

    // init GOEP Server
    goep_server_init();

    // init OPP Server
    opp_server_init(&packet_handler, OPP_SERVER_RFCOMM_CHANNEL_NR, OPP_SERVER_L2CAP_PSM, LEVEL_2);

    // setup SDP Record
    sdp_init();
    opp_server_create_sdp_record(service_buffer, sdp_create_service_record_handle(), OPP_SERVER_RFCOMM_CHANNEL_NR,
                                  OPP_SERVER_L2CAP_PSM, "OPP Server", sizeof(supported_formats), supported_formats);
    sdp_register_service(service_buffer);

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // turn on!
    hci_power_control(HCI_POWER_ON);

#ifdef HAVE_BTSTACK_STDIN
    btstack_stdin_setup(stdin_process);
#endif

    return 0;
}
/* EXAMPLE_END */
