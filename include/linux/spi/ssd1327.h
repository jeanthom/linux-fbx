#ifndef __LINUX_SPI_SSD1327_H
#define __LINUX_SPI_SSD1327_H

struct ssd1327_platform_data {
	/* attached screen info */
	unsigned int		width;
	unsigned int		height;

	int			rotate;

	/* gpio used to select command/data */
	int			data_select_gpio;

	/* watchdog (second), enabled if non zero, screen is blanked
	 * if nothing is written for this number of seconds */
	unsigned int		watchdog;
};

#endif /* ! __LINUX_SPI_SSD1327_H */

