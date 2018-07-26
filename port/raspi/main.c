/*
 * Copyright (C) 2014 BlueKitchen GmbH
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
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
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

#define __BTSTACK_FILE__ "main.c"

// *****************************************************************************
//
// minimal setup for HCI code
//
// *****************************************************************************

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "btstack_config.h"

#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_link_key_db_fs.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_posix.h"
#include "bluetooth_company_id.h"
#include "hci.h"
#include "hci_dump.h"
#include "btstack_stdin.h"

#include "btstack_chipset_bcm.h"
#include "btstack_chipset_bcm_download_firmware.h"
#include "btstack_control_raspi.h"

int btstack_main(int argc, const char * argv[]);

typedef enum  {
    UART_INVALID,
    UART_SOFTWARE_NO_FLOW,
    UART_HARDWARE_NO_FLOW,
    UART_HARDWARE_FLOW
} uart_type_t;

// default config, updated depending on RasperryPi UART configuration
static hci_transport_config_uart_t transport_config = {
    HCI_TRANSPORT_CONFIG_UART,
    115200,
    0,       // main baudrate
    0,       // flow control
    NULL,
};
static btstack_uart_config_t uart_config;

static int main_argc;
static const char ** main_argv;

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void sigint_handler(int param){
    UNUSED(param);

    printf("CTRL-C - SIGINT received, shutting down..\n");   
    log_info("sigint_handler: shutting down");

    // reset anyway
    btstack_stdin_reset();

    // power down
    hci_power_control(HCI_POWER_OFF);
    hci_close();
    log_info("Good bye, see you.\n");    
    exit(0);
}

static int led_state = 0;
void hal_led_toggle(void){
    led_state = 1 - led_state;
    printf("LED State %u\n", led_state);
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    bd_addr_t addr;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            gap_local_bd_addr(addr);
            printf("BTstack up and running at %s\n",  bd_addr_to_str(addr));
            break;
        default:
            break;
    }
}

// see https://github.com/RPi-Distro/pi-bluetooth/blob/master/usr/bin/btuart
static int raspi_get_bd_addr(bd_addr_t addr){

    FILE *fd = fopen( "/proc/device-tree/serial-number", "r" );
    if( fd == NULL ){
        fprintf(stderr, "can't read serial number, %s\n", strerror( errno ) );
        return -1;
    }
    fscanf( fd, "%*08u" "%*02u" "%02" SCNx8 "%02" SCNx8 "%02" SCNx8, &addr[3], &addr[4], &addr[5] );
    fclose( fd );

    addr[0] =  0xb8; addr[1]  = 0x27; addr[2] =  0xeb;
    addr[3] ^= 0xaa; addr[4] ^= 0xaa; addr[5] ^= 0xaa;

    return 0;
}

// see https://github.com/RPi-Distro/pi-bluetooth/blob/master/usr/bin/btuart
// on UART_INVALID errno is set
static uart_type_t raspi_get_bluetooth_uart_type(void){

    uint8_t deviceUart0[21] = { 0 };
    FILE *fd = fopen( "/proc/device-tree/aliases/uart0", "r" );
    if( fd == NULL ) return UART_INVALID;
    fscanf( fd, "%20s", deviceUart0 );
    fclose( fd );
    
    uint8_t deviceSerial1[21] = { 0 };
    fd = fopen( "/proc/device-tree/aliases/serial1", "r" );
    if( fd == NULL ) return UART_INVALID;
    fscanf( fd, "%20s", deviceSerial1 );
    fclose( fd );
  
    // test if uart0 is an alias for serial1
    if( strncmp( (const char *) deviceUart0, (const char *) deviceSerial1, 21 ) == 0 ){
        // HW uart
        size_t count = 0;
        uint8_t buf[16];
        fd = fopen( "/proc/device-tree/soc/gpio@7e200000/uart0_pins/brcm,pins", "r" );
        if( fd == NULL ) return UART_INVALID;
        count = fread( buf, 1, 16, fd );
        fclose( fd );

        // contains assigned pins
        int pins = count / 4;
        if( pins == 4 ){
            return UART_HARDWARE_FLOW;
        } else {
            return UART_HARDWARE_NO_FLOW;
        }
    } else {
        return UART_SOFTWARE_NO_FLOW;
    }    
}

static void phase2(int status);
int main(int argc, const char * argv[]){

    /// GET STARTED with BTstack ///
    btstack_memory_init();

    // use logger: format HCI_DUMP_PACKETLOGGER, HCI_DUMP_BLUEZ or HCI_DUMP_STDOUT
    const char * pklg_path = "/tmp/hci_dump.pklg";
    hci_dump_open(pklg_path, HCI_DUMP_PACKETLOGGER);
    printf("Packet Log: %s\n", pklg_path);

    // setup run loop
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
        
    // pick serial port and configure uart block driver
    transport_config.device_name = "/dev/serial1";

    // derive bd_addr from serial number
    bd_addr_t addr = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    raspi_get_bd_addr(addr);

    // set UART config based on raspi Bluetooth UART type
    switch (raspi_get_bluetooth_uart_type()){
        case UART_INVALID:
            fprintf(stderr, "can't verify HW uart, %s\n", strerror( errno ) );
            return -1;
        case UART_SOFTWARE_NO_FLOW:
            printf("Software UART without flowcontrol\n");
            transport_config.baudrate_main = 460800;
            transport_config.flowcontrol = 0;
            break;
        case UART_HARDWARE_NO_FLOW:
            printf("Hardware UART without flowcontrol\n");
            transport_config.baudrate_main = 921600;
            transport_config.flowcontrol = 0;
            break;
        case UART_HARDWARE_FLOW:
            printf("Hardware UART with flowcontrol\n");
            transport_config.baudrate_main = 3000000;
            transport_config.flowcontrol = 1;
            break;
    }

    // get BCM chipset driver
    const btstack_chipset_t * chipset = btstack_chipset_bcm_instance();
    chipset->init(&transport_config);

    // set path to firmware files
    btstack_chipset_bcm_set_hcd_folder_path("/lib/firmware");

    // set chipset name
    btstack_chipset_bcm_set_device_name("BCM43430A1");

    // setup UART driver
    const btstack_uart_block_t * uart_driver = btstack_uart_block_posix_instance();

    // extract UART config from transport config
    uart_config.baudrate    = transport_config.baudrate_init;
    uart_config.flowcontrol = transport_config.flowcontrol;
    uart_config.device_name = transport_config.device_name;
    uart_driver->init(&uart_config);

    // setup HCI (to be able to use bcm chipset driver)
    const hci_transport_t * transport = hci_transport_h5_instance(uart_driver);
    const btstack_link_key_db_t * link_key_db = btstack_link_key_db_fs_instance();
    hci_init(transport, (void*) &transport_config);
    hci_set_bd_addr( addr );

    // btstack_control_t *control = btstack_control_raspi_get_instance();
    // hci_set_control( control );

    hci_set_link_key_db(link_key_db);
    hci_set_chipset(btstack_chipset_bcm_instance());

    // inform about BTstack state
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // handle CTRL-c
    signal(SIGINT, sigint_handler);

    main_argc = argc;
    main_argv = argv;

    // phase #1 download firmware
    printf("Phase 1: Download firmware\n");

    // control->off();
    // sleep( 1 );
    // control->on();

    // phase #2 start main app
    btstack_chipset_bcm_download_firmware(uart_driver, transport_config.baudrate_main, &phase2);

    // go
    btstack_run_loop_execute();    
    return 0;
}

static void phase2(int status){

    if (status){
        printf("Download firmware failed\n");
        return;
    }

    printf("Phase 2: Main app\n");

    // setup app
    btstack_main(main_argc, main_argv);
}

