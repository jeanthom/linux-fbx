#ifndef SMSC_CAP1066_H_
#define SMSC_CAP1066_H_

#define CAP1066_MAX_BTNS	6

struct smsc_cap1066_pdata {
	unsigned short	key_map[CAP1066_MAX_BTNS];
	unsigned int	irq_gpio;
	bool		has_irq_gpio;
};

#endif /* ! SMSC_CAP1066_H_ */
