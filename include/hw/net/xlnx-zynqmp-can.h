/*
 * QEMU model of the Xilinx CAN device.
 *
 * Copyright (c) 2020 Xilinx Inc.
 *
 * Written-by: Vikram Garhwal<fnu.vikram@xilinx.com>
 *
 * Based on QEMU CAN Device emulation implemented by Jin Yang, Deniz Eren and
 * Pavel Pisa.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_CAN_XILINX_H
#define HW_CAN_XILINX_H

#include "hw/register.h"
#include "net/can_emu.h"
#include "net/can_host.h"
#include "qemu/fifo32.h"

#define TYPE_XLNX_ZYNQMP_CAN "xlnx.zynqmp-can"

#define XLNX_ZYNQMP_CAN(obj) \
     OBJECT_CHECK(XlnxZynqMPCANState, (obj), TYPE_XLNX_ZYNQMP_CAN)

#define MAX_CAN_CTRLS      2
#define XLNX_ZYNQMP_CAN_R_MAX     (0x84 / 4)
#define MAILBOX_CAPACITY   64

/* Each CAN_FRAME will have 4 * 32bit size. */
#define CAN_FRAME_SIZE     4
#define RXFIFO_SIZE        (MAILBOX_CAPACITY * CAN_FRAME_SIZE)


typedef struct XlnxZynqMPCANState {
    SysBusDevice        parent_obj;
    MemoryRegion        iomem;

    qemu_irq            irq;

    CanBusClientState   bus_client;
    CanBusState         *canbus[MAX_CAN_CTRLS];

    struct {
        uint8_t         ctrl_idx;
   } cfg;

    RegisterInfo        reg_info[XLNX_ZYNQMP_CAN_R_MAX];
    uint32_t            regs[XLNX_ZYNQMP_CAN_R_MAX];

    uint16_t            rx_time_stamp;

    Fifo32                rx_fifo;
    Fifo32                tx_fifo;
    Fifo32                txhpb_fifo;

} XlnxZynqMPCANState;

#endif
