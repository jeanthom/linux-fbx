#ifndef __UAPI_HDMI_CEC_H
#define __UAPI_HDMI_CEC_H

#include <linux/kernel.h>
#include <linux/types.h>

/* Common defines for HDMI CEC */
#define CEC_BCAST_ADDR		(0x0f)
#define CEC_ADDR_MAX		CEC_BCAST_ADDR

#define CEC_MAX_MSG_LEN		(16)	/* 16 blocks */

enum cec_rx_msg_flags {
	/*
	 * an ACK was received for this message
	 */
	CEC_RX_F_ACKED			= (1 << 0),

	/*
	 * message was fully received
	 */
	CEC_RX_F_COMPLETE		= (1 << 1),
};

/**
 * struct cec_rx_msg - user-space exposed cec message cookie
 * @data:	cec message payload
 * @len:	cec message length
 * @valid:	0 for invalid message
 * @flags:	flag field (cec_rx_msg_flags)
 */
struct cec_rx_msg {
	__u8	data[CEC_MAX_MSG_LEN];
	__u8	len;
	__u8	valid;
	__u8	flags;

} __attribute__((packed));

enum cec_tx_status_flags {
	/*
	 * message was nacked at some point
	 */
	CEC_TX_F_NACK			= (1 << 0),

	/*
	 * abort sending because total time to send was elapsed
	 */
	CEC_TX_F_TIMEOUT		= (1 << 1),

	/*
	 * abort sending because maximum number of retry has passed
	 */
	CEC_TX_F_MAX_RETRIES		= (1 << 2),

	/*
	 * abort sending because of arbitration loss
	 */
	CEC_TX_F_ARBITRATION_LOST	= (1 << 3),

	/*
	 * message failed for other reason
	 */
	CEC_TX_F_UNKNOWN_ERROR		= (1 << 7),
};

/**
 * struct cec_tx_msg - user-space exposed cec message cookie
 * @expire_ms:	how long we try to send message (milliseconds)
 * @data:	cec message payload
 * @len:	cec message length
 * @success:	0 => message was sent, else => failed to send message
 * @flags:	flag field (cec_tx_msg_flags)
 * @tries:	number of try done to send message
 */
struct cec_tx_msg {
	__u16	expire_ms;
	__u8	data[CEC_MAX_MSG_LEN];
	__u8	len;
	__u8	success;
	__u8	flags;
	__u8	tries;
} __attribute__((packed));

struct cec_tx_status {
	__u8	sent;
	__u8	success;
	__u8	flags;
	__u8	tries;
} __attribute__((packed));

#define DETACH_CFG_F_WAKEUP		(1 << 0)

struct cec_detached_config {
	__u8	phys_addr_valid;
	__u8	phys_addr[2];
	__u8	flags;
} __attribute__((packed));

/* Counters */

/**
 * struct cec_rx_counters - cec adpater RX counters
 */
struct cec_rx_counters {
	__u8	pkts;
	__u8	filtered_pkts;
	__u8	valid_pkts;
	__u8	rx_queue_full;
	__u8	late_ack;
	__u8	error;
	__u8	rx_timeout_abort;
	__u8	rx_throttled;
};

/**
 * struct cec_tx_counters - cec adapter TX counters
 */
struct cec_tx_counters {
	__u8	done;
	__u8	fail;
	__u8	timeout;
	__u8	arb_loss;
	__u8	bad_ack_timings;
	__u8	tx_miss_early;
	__u8	tx_miss_late;
};

/**
 * struct cec_counters - tx and rx cec counters
 * @rx:	struct cec_rx_counters
 * @tx: struct cec_tx_counters
 */
struct cec_counters {
	struct cec_rx_counters	rx;
	struct cec_tx_counters	tx;
};

/**
 * enum cec_rx_mode - cec adapter rx mode
 * @CEC_RX_MODE_DISABLED:	RX path is disabled (default)
 * @CEC_RX_MODE_DEFAULT:	accept only unicast traffic
 * @CEC_RX_MODE_ACCEPT_ALL:	accept all incoming RX traffic (sniffing mode)
 * @CEC_RX_MODE_MAX:		sentinel
 */
enum cec_rx_mode {
	CEC_RX_MODE_DISABLED = 0,
	CEC_RX_MODE_DEFAULT,
	CEC_RX_MODE_ACCEPT_ALL,
	CEC_RX_MODE_MAX
};

#endif /* __UAPI_HDMI_CEC_H */
