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

/** 
 * @file  hal_bt.c
 ***************************************************************************/
#include <stdint.h>

#include <msp430.h>
#include "hal_compat.h"

#include "hal_uart_dma.h"

extern void hal_cpu_set_uart_needed_during_sleep(uint8_t enabled);

// debugging only
// #include <stdio.h>


// RXD 3.4
// TXD 3.3
#define BT_PORT_OUT      P3OUT
#define BT_PORT_SEL      P3SEL
#define BT_PORT_DIR      P3DIR
#define BT_PORT_REN      P3REN
#define BT_PIN_TXD       BIT3
#define BT_PIN_RXD       BIT4

// RTS P2.3
#define RTS_SEL P2SEL
#define RTS_OUT P2OUT
#define RTS_DIR P2DIR
#define RTS_PIN BIT3

// CTS P8.1 rewired to P2.6 - only P1 & P2 have interrupts
#define CTS_SEL P2SEL
#define CTS_OUT P2OUT
#define CTS_DIR P2DIR
#define CTS_PIN BIT6

// N_SHUTDOWN P4.1
#define N_SHUTDOWN_SEL P4SEL
#define N_SHUTDOWN_OUT P4OUT
#define N_SHUTDOWN_DIR P4DIR
#define N_SHUTDOWN_PIN BIT1

void dummy_handler(void){};

// rx state
static uint16_t  bytes_to_read = 0;
static uint8_t * rx_buffer_ptr = 0;

// tx state
static uint16_t  bytes_to_write = 0;
static uint8_t * tx_buffer_ptr = 0;

// handlers
static void (*rx_done_handler)(void) = dummy_handler;
static void (*tx_done_handler)(void) = dummy_handler;
static void (*cts_irq_handler)(void) = dummy_handler;

/**
 * @brief  Initializes the serial communications peripheral and GPIO ports 
 *         to communicate with the PAN BT .. assuming 16 Mhz CPU
 * 
 * @param  none
 * 
 * @return none
 */
void hal_uart_dma_init(void)
{
    BT_PORT_SEL |= BT_PIN_RXD + BT_PIN_TXD;
    BT_PORT_DIR |= BT_PIN_TXD;
    BT_PORT_DIR &= ~BT_PIN_RXD;

    // set BT RTS
    RTS_SEL &= ~RTS_PIN;  // = 0 - I/O
    RTS_DIR |=  RTS_PIN;  // = 1 - Output
    RTS_OUT |=  RTS_PIN;  // = 1 - RTS high -> stop

    // set BT CTS
    CTS_SEL &= ~CTS_PIN;  // = 0 - I/O
    CTS_DIR &= ~CTS_PIN;  // = 0 - Input
        
    // set BT SHUTDOWN to 1 (active low)
    N_SHUTDOWN_SEL &= ~N_SHUTDOWN_PIN;  // = 0 - I/O
    N_SHUTDOWN_DIR |=  N_SHUTDOWN_PIN;  // = 1 - Output
    N_SHUTDOWN_OUT |=  N_SHUTDOWN_PIN;  // = 1 - Active low -> ok

    // wait for Bluetooth to power up properly after providing 32khz clock
    waitAboutOneSecond();

    UCA0CTL1 |= UCSWRST;              //Reset State                      
    UCA0CTL0 = UCMODE_0;
    
    UCA0CTL0 &= ~UC7BIT;              // 8bit char
    UCA0CTL1 |= UCSSEL_2;
    
    UCA0CTL1 &= ~UCSWRST;             // continue

    hal_uart_dma_set_baud(115200);
}

/**

 UART used in low-frequency mode
 In this mode, the maximum USCI baud rate is one-third the UART source clock frequency BRCLK.
 
 16000000 /  576000 = 277.77
 16000000 /  115200 = 138.88
 16000000 /  921600 =  17.36
 16000000 / 1000000 =  16.00
 16000000 / 2000000 =   8.00
 16000000 / 2400000 =   6.66
 16000000 / 3000000 =   3.33
 16000000 / 4000000 =   2.00
 
 */
int hal_uart_dma_set_baud(uint32_t baud){

    int result = 0;
    
    UCA0CTL1 |= UCSWRST;              //Reset State                      

    switch (baud){

        case 4000000:
            UCA0BR0 = 2;
            UCA0BR1 = 0;
            UCA0MCTL= 0 << 1;  // + 0.000
            break;
            
        case 3000000:
            UCA0BR0 = 3;
            UCA0BR1 = 0;
            UCA0MCTL= 3 << 1;  // + 0.375
            break;
            
        case 2400000:
            UCA0BR0 = 6;
            UCA0BR1 = 0;
            UCA0MCTL= 5 << 1;  // + 0.625
            break;

        case 2000000:
            UCA0BR0 = 8;
            UCA0BR1 = 0;
            UCA0MCTL= 0 << 1;  // + 0.000
            break;

        case 1000000:
            UCA0BR0 = 16;
            UCA0BR1 = 0;
            UCA0MCTL= 0 << 1;  // + 0.000
            break;
            
        case 921600:
            UCA0BR0 = 17;
            UCA0BR1 = 0;
            UCA0MCTL= 7 << 1;  // 3 << 1;  // + 0.375
            break;
            
        case 115200:
            UCA0BR0 = 138;  // from family user guide
            UCA0BR1 = 0;
            UCA0MCTL= 7 << 1;  // + 0.875
            break;

        case 57600:
            UCA0BR0 = 21;
            UCA0BR1 = 1;
            UCA0MCTL= 7 << 1;  // + 0.875
            break;

        default:
            result = -1;
            break;
    }

    UCA0CTL1 &= ~UCSWRST;             // continue
    
    return result;
}

void hal_uart_dma_set_block_received( void (*the_block_handler)(void)){
    rx_done_handler = the_block_handler;
}

void hal_uart_dma_set_block_sent( void (*the_block_handler)(void)){
    tx_done_handler = the_block_handler;
}

void hal_uart_dma_set_csr_irq_handler( void (*the_irq_handler)(void)){
#ifdef HAVE_CTS_IRQ
    if (the_irq_handler){
        P2IFG  =  0;     // no IRQ pending
        P2IV   =  0;     // no IRQ pending
        P2IES &= ~ CTS_PIN;  // IRQ on 0->1 transition
        P2IE  |=   CTS_PIN;  // enable IRQ for P8.1
        cts_irq_handler = the_irq_handler;
        return;
    }
    P2IE  &= ~CTS_PIN;
    cts_irq_handler = dummy_handler;
#endif
}

/**********************************************************************/
/**
 * @brief  Disables the serial communications peripheral and clears the GPIO
 *         settings used to communicate with the BT.
 * 
 * @param  none
 * 
 * @return none
 **************************************************************************/
void hal_uart_dma_shutdown(void) {
    
    UCA0IE &= ~(UCRXIE | UCTXIE);
    UCA0CTL1 = UCSWRST;                          //Reset State                         
    BT_PORT_SEL &= ~( BT_PIN_RXD + BT_PIN_TXD );
    BT_PORT_DIR |= BT_PIN_TXD;
    BT_PORT_DIR |= BT_PIN_RXD;
    BT_PORT_OUT &= ~(BT_PIN_TXD + BT_PIN_RXD);
}

void hal_uart_dma_send_block(const uint8_t * data, uint16_t len){
    
    // printf("hal_uart_dma_send_block, size %u\n\r", len);
    
    UCA0IE &= ~UCTXIE ;  // disable TX interrupts

    tx_buffer_ptr = (uint8_t *) data;
    bytes_to_write = len;

    UCA0IE |= UCTXIE;    // enable TX interrupts
}

static inline void hal_uart_dma_enable_rx(void){
    RTS_OUT &= ~ RTS_PIN;  // = 0 - RTS low -> ok
}

static inline void hal_uart_dma_disable_rx(void){
    RTS_OUT |= RTS_PIN;  // = 1 - RTS high -> stop
}

void hal_uart_dma_receive_block(uint8_t *buffer, uint16_t len){
    // disable RX interrupts
    UCA0IE &= ~UCRXIE;

    rx_buffer_ptr = buffer;
    bytes_to_read = len;
    
    // check if byte already received
    int pending = UCAIFG & UCRXIFG;

    // enable RX interrupts - will trigger ISR below if byte pending
    UCA0IE |= UCRXIE;    // enable RX interrupts

    // if byte was pending, ISR controls RTS
    if (!pending) {
        hal_uart_dma_enable_rx();
    }
}

void hal_uart_dma_set_sleep(uint8_t sleep){
    hal_cpu_set_uart_needed_during_sleep(!sleep);    
}

// block-wise "DMA" RX/TX UART driver
#ifdef __GNUC__
__attribute__((interrupt(USCI_A0_VECTOR)))
#endif
#ifdef __IAR_SYSTEMS_ICC__
#pragma vector=USCI_A0_VECTOR
__interrupt
#endif
void usbRxTxISR(void){ 

    // find reason
    switch (UCA0IV){
    
        case 2: // RXIFG
            if (bytes_to_read == 0) {
                hal_uart_dma_disable_rx();
                UCA0IE &= ~UCRXIE ;  // disable RX interrupts
                return;
            }
            *rx_buffer_ptr = UCA0RXBUF;
            ++rx_buffer_ptr;
            --bytes_to_read;
            if (bytes_to_read > 0) {
                hal_uart_dma_enable_rx();
                return;
            }
            RTS_OUT |= RTS_PIN;      // = 1 - RTS high -> stop
            UCA0IE &= ~UCRXIE ; // disable RX interrupts
        
            (*rx_done_handler)();
            
            // force exit low power mode
            __bic_SR_register_on_exit(LPM0_bits);   // Exit active CPU
            
            break;

        case 4: // TXIFG
            if (bytes_to_write == 0){
                UCA0IE &= ~UCTXIE ;  // disable TX interrupts
                return;
            }
            UCA0TXBUF = *tx_buffer_ptr;
            ++tx_buffer_ptr;
            --bytes_to_write;
            
            if (bytes_to_write > 0) {
                return;
            }
            
            UCA0IE &= ~UCTXIE ;  // disable TX interrupts

            (*tx_done_handler)();

            // force exit low power mode
            __bic_SR_register_on_exit(LPM0_bits);   // Exit active CPU

            break;

        default:
            break;
    }
}


// CTS ISR 
#ifdef HAVE_CTS_IRQ
// TODO: there's no PORT8_VECTOR, but configuration seems possible

extern void ehcill_handle(uint8_t action);
#define EHCILL_CTS_SIGNAL      0x034

#ifdef __GNUC__
__attribute__((interrupt(PORT2_VECTOR)))
#elif defined( __IAR_SYSTEMS_ICC__)
#pragma vector=PORT2_VECTOR
__interrupt
#endif
void ctsISR(void){ 
    P2IV = 0;
    (*cts_irq_handler)();
}
#endif

