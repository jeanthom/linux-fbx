#ifndef __HDMI_CEC_DEV_H
#define __HDMI_CEC_DEV_H

#include <linux/ioctl.h>
#include <linux/hdmi-cec/hdmi-cec.h>

#define CEC_IOCTL_BASE	'C'

#define CEC_SET_LOGICAL_ADDRESS	_IOW(CEC_IOCTL_BASE, 0, int)
#define CEC_RESET_DEVICE	_IOW(CEC_IOCTL_BASE, 3, int)
#define CEC_GET_COUNTERS	_IOR(CEC_IOCTL_BASE, 4, struct cec_counters)
#define CEC_SET_RX_MODE		_IOW(CEC_IOCTL_BASE, 5, enum cec_rx_mode)
#define CEC_GET_TX_STATUS	_IOW(CEC_IOCTL_BASE, 6, struct cec_tx_status)
#define CEC_SET_DETACHED_CONFIG	_IOW(CEC_IOCTL_BASE, 7, struct cec_detached_config)

#define CEC_MAX_DEVS	(10)

#ifdef __KERNEL__

struct cec_adapter;

int __init cec_cdev_init(void);
void __exit cec_cdev_exit(void);

int cec_create_adapter_node(struct cec_adapter *);
void cec_remove_adapter_node(struct cec_adapter *);

#endif /* __KERNEL__ */

#endif /* __HDMI_CEC_DEV_H */
