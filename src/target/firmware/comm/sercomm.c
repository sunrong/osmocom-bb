/* Serial communications layer, based on HDLC */

/* (C) 2010 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#ifdef HOST_BUILD
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#endif
#include <osmocore/msgb.h>
#include <sercomm.h>

#else
#include <debug.h>
#include <linuxlist.h>

#include <comm/msgb.h>
#include <comm/sercomm.h>
#include <calypso/uart.h>
#endif

#define SERCOMM_RX_MSG_SIZE	256

enum rx_state {
	RX_ST_WAIT_START,
	RX_ST_ADDR,
	RX_ST_CTRL,
	RX_ST_DATA,
	RX_ST_ESCAPE,
};

static struct {
	int initialized;

	/* transmit side */
	struct {
		struct llist_head dlci_queues[_SC_DLCI_MAX];
		struct msgb *msg;
		uint8_t *next_char;
	} tx;

	/* receive side */
	struct {
		dlci_cb_t dlci_handler[_SC_DLCI_MAX];
		struct msgb *msg;
		enum rx_state state;
		uint8_t dlci;
		uint8_t ctrl;
	} rx;
	
} sercomm;

void sercomm_init(void)
{
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(sercomm.tx.dlci_queues); i++)
		INIT_LLIST_HEAD(&sercomm.tx.dlci_queues[i]);

	sercomm.rx.msg = NULL;
	sercomm.initialized = 1;
}

int sercomm_initialized(void)
{
	return sercomm.initialized;
}

/* user interface for transmitting messages for a given DLCI */
void sercomm_sendmsg(uint8_t dlci, struct msgb *msg)
{
	uint8_t *hdr;

	/* prepend address + control octet */
	hdr = msgb_push(msg, 2);
	hdr[0] = dlci;
	hdr[1] = HDLC_C_UI;
	msgb_enqueue(&sercomm.tx.dlci_queues[dlci], msg);

#ifndef HOST_BUILD
	/* tell UART that we have something to send */
	uart_irq_enable(SERCOMM_UART_NR, UART_IRQ_TX_EMPTY, 1);
#endif
}

/* how deep is the Tx queue for a given DLCI */
unsigned int sercomm_tx_queue_depth(uint8_t dlci)
{
	struct llist_head *le;
	unsigned int num = 0;

	llist_for_each(le, &sercomm.tx.dlci_queues[dlci]) {
		num++;
	}

	return num;
}

/* fetch one octet of to-be-transmitted serial data */
int sercomm_drv_pull(uint8_t *ch)
{
	if (!sercomm.tx.msg) {
		unsigned int i;
		/* dequeue a new message from the queues */
		for (i = 0; i < ARRAY_SIZE(sercomm.tx.dlci_queues); i++) {
			sercomm.tx.msg = msgb_dequeue(&sercomm.tx.dlci_queues[i]);
			if (sercomm.tx.msg)
				break;
		}
		if (sercomm.tx.msg) {
			/* start of a new message, send start flag octet */
			*ch = HDLC_FLAG;
			sercomm.tx.next_char = sercomm.tx.msg->data;
			return 1;
		} else {
			/* no more data avilable */
			return 0;
		}
	}

	/* escaping for the two control octets */
	if (*sercomm.tx.next_char == HDLC_FLAG ||
	    *sercomm.tx.next_char == HDLC_ESCAPE) {
		/* send an escape octet */
		*ch = HDLC_ESCAPE;
		/* invert bit 5 of the next octet to be sent */
		*sercomm.tx.next_char ^= (1 << 5);
	} else if (sercomm.tx.next_char == sercomm.tx.msg->tail) {
		/* last character has already been transmitted,
		 * send end-of-message octet */
		*ch = HDLC_FLAG;
		/* we've reached the end of the message buffer */
		msgb_free(sercomm.tx.msg);
		sercomm.tx.msg = NULL;
		sercomm.tx.next_char = NULL;
	} else {
		/* standard case, simply send next octet */
		*ch = *sercomm.tx.next_char++;
	}
	return 1;
}

/* register a handler for a given DLCI */
int sercomm_register_rx_cb(uint8_t dlci, dlci_cb_t cb)
{
	if (dlci >= ARRAY_SIZE(sercomm.rx.dlci_handler))
		return -EINVAL;

	if (sercomm.rx.dlci_handler[dlci])
		return -EBUSY;

	sercomm.rx.dlci_handler[dlci] = cb;
	return 0;
}

/* dispatch an incomnig message once it is completely received */
static void dispatch_rx_msg(uint8_t dlci, struct msgb *msg)
{
	if (sercomm.rx.dlci_handler[dlci])
		sercomm.rx.dlci_handler[dlci](dlci, msg);
	else
		msgb_free(msg);
}

/* the driver has received one byte, pass it into sercomm layer */
int sercomm_drv_rx_char(uint8_t ch)
{
	uint8_t *ptr;

	if (!sercomm.rx.msg)
		sercomm.rx.msg = sercomm_alloc_msgb(SERCOMM_RX_MSG_SIZE);

	if (msgb_tailroom(sercomm.rx.msg) == 0) {
		msgb_free(sercomm.rx.msg);
		sercomm.rx.msg = sercomm_alloc_msgb(SERCOMM_RX_MSG_SIZE);
		sercomm.rx.state = RX_ST_WAIT_START;
		return 0;
	}

	switch (sercomm.rx.state) {
	case RX_ST_WAIT_START:
		if (ch != HDLC_FLAG)
			return 0;
		sercomm.rx.state = RX_ST_ADDR;
		break;
	case RX_ST_ADDR:
		sercomm.rx.dlci = ch;
		sercomm.rx.state = RX_ST_CTRL;
		break;
	case RX_ST_CTRL:
		sercomm.rx.ctrl = ch;
		sercomm.rx.state = RX_ST_DATA;
		break;
	case RX_ST_DATA:
		if (ch == HDLC_ESCAPE) {
			/* drop the escape octet, but change state */
			sercomm.rx.state = RX_ST_ESCAPE;
			break;
		} else if (ch == HDLC_FLAG) {
			/* message is finished */
			dispatch_rx_msg(sercomm.rx.dlci, sercomm.rx.msg);
			/* allocate new buffer */
			sercomm.rx.msg = NULL;
			/* start all over again */
			sercomm.rx.state = RX_ST_WAIT_START;

			/* do not add the control char */
			break;
		}
		/* default case: store the octet */
		ptr = msgb_put(sercomm.rx.msg, 1);
		*ptr = ch;
		break;
	case RX_ST_ESCAPE:
		/* store bif-5-inverted octet in buffer */
		ch ^= (1 << 5);
		ptr = msgb_put(sercomm.rx.msg, 1);
		*ptr = ch;
		/* transition back to nromal DATA state */
		sercomm.rx.state = RX_ST_DATA;
		break;
	}
	return 1;
}