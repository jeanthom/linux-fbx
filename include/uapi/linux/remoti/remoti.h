#ifndef _UAPI_REMOTI_H
#define _UAPI_REMOTI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * subsystem definitions
 */
#define NPI_SYS_RES0		0
#define NPI_SYS_SYS		1
#define NPI_SYS_MAC		2
#define NPI_SYS_NWK		3
#define NPI_SYS_AF		4
#define NPI_SYS_ZDO		5
#define NPI_SYS_SAPI		6
#define NPI_SYS_UTIL		7
#define NPI_SYS_DBG		8
#define NPI_SYS_APP		9
#define NPI_SYS_RCAF		10
#define NPI_SYS_RCN		11
#define NPI_SYS_RCN_CLI		12
#define NPI_SYS_BOOT		13
#define NPI_SYS_MAX		14
#define NPI_SYS_MASK		0x1F

/*
 * type definitions
 */
#define NPI_POLL		0
#define NPI_SREQ		1
#define NPI_AREQ		2
#define NPI_SRSP		3
#define NPI_TYPE_MAX		4
#define NPI_TYPE_MASK		3
#define NPI_TYPE_SHIFT		5


/* common error codes (see RemoTI API) */
#define RTI_SUCCESS		0x00

/*
 * rti user message
 */
#define NPI_MAX_DATA_LEN	123

struct rti_msg {
	__u8	type;
	__u8	subsys;
	__u8	cmd;

	__u8	data_len;
	__u8	data[NPI_MAX_DATA_LEN];

	__u8	custom_reply_cmd;
	__u8	reply_cmd;
	__u8	reply_len;
	__u8	reply[NPI_MAX_DATA_LEN];
};

/*
 * socket addr family on "user" device
 */
#ifndef PF_REMOTI
#define PF_REMOTI			37
#define AF_REMOTI			PF_REMOTI
#endif

struct sockaddr_rti {
	__u32	device_id;
};

#define SOL_REMOTI			280
#define REMOTI_REGISTER_CB		0

struct rti_callback {
	__u8	subsys;
	__u8	cmd;
};

/*
 * ioctl on uart device
 */
enum rti_dev_state {
	RTI_DEV_S_STOPPED = 0,
	RTI_DEV_S_BOOTING,
	RTI_DEV_S_BOOT_FAILED,
	RTI_DEV_S_OPERATIONAL,
	RTI_DEV_S_STOPPING,
	RTI_DEV_S_DEAD,
};

struct rti_dev_status {
	__u32	dev_state;
	__u32	fw_version;
};

struct rti_dev_stats {
	__u64	tx_bytes;
	__u64	tx_packets;

	__u64	tx_boot_packets;
	__u64	tx_rcaf_packets;
	__u64	tx_util_packets;
	__u64	tx_other_packets;


	__u64	rx_bytes;
	__u64	rx_packets;
	__u64	rx_bad_sof;
	__u64	rx_len_errors;
	__u64	rx_fcs_errors;
	__u64	rx_tty_errors;
	__u64	rx_full_errors;
	__u64	rx_subsys_errors;
	__u64	rx_type_errors;
	__u64	rx_no_callback;

	__u64	rx_boot_packets;
	__u64	rx_rcaf_packets;
	__u64	rx_util_packets;
	__u64	rx_other_packets;
};

enum {
	RTI_BOOT_FLAGS_FORCE_UPDATE	= (1 << 0),
};

#define RTI_IOCTL_MAGIC		0xd4
#define RTI_ATTACH_DEVICE	_IOR(RTI_IOCTL_MAGIC, 1, __u32)
#define RTI_GET_STATUS		_IOW(RTI_IOCTL_MAGIC, 2, struct rti_dev_status)
#define RTI_GET_STATS		_IOW(RTI_IOCTL_MAGIC, 3, struct rti_dev_stats)

#define RTI_START_DEVICE	_IOR(RTI_IOCTL_MAGIC, 8, __u32)
#define RTI_STOP_DEVICE		_IO(RTI_IOCTL_MAGIC, 9)

#endif /* _UAPI_REMOTI_H */
