/* Host test for the pure GPIO decoders in wlanprobe.h. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -I. -o out/wlanprobe_host_test wlanprobe_host_test.c */
#include <assert.h>
#include <stdio.h>
#include "wlanprobe.h"

int try_read32(uintptr_t address, uint32_t *value)
{
	(void)address; (void)value;
	return 0;
}

int main(void)
{
	/* gpp14-2 as output (1) with the other pins inputs; WLAN_EN high. */
	assert(wlan_gpio_func(0x00000100u, 2) == 1);
	assert(wlan_gpio_func(0x00000100u, 1) == 0);
	assert(wlan_gpio_func(0x00000100u, 3) == 0);
	assert(wlan_gpio_level(0x4u, 2) == 1);
	assert(wlan_gpio_level(0x4u, 0) == 0);
	/* gph2: PERST out (1), CLKREQ func 2, dev_wake pin 5 = func 0xF in the top nibble used. */
	assert(wlan_gpio_func(0x00F00021u, 0) == 1);
	assert(wlan_gpio_func(0x00F00021u, 1) == 2);
	assert(wlan_gpio_func(0x00F00021u, 5) == 0xF);
	assert(wlan_gpio_func(0xF0000000u, 7) == 0xF);
	assert(wlan_gpio_level(0x80000000u, 31) == 1);
	puts("PASS wlanprobe decoders");
	return 0;
}
