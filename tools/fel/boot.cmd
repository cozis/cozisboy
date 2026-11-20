# USB (FEL) development boot for the RG35XX SP.
# Signals use raw register writes (the `gpio` command refuses pins claimed by
# U-Boot's LED driver):
#   green LED  = PI12 -> PIO bank I data reg 0x0300b130, bit 12 (0x1000)
#   backlight  = PD28 -> PIO bank D cfg3 0x0300b078 bits[19:16], data 0x0300b07c bit 28
# Watchdog 0: cfg 0x030090b4, mode 0x030090b8 (0xb1 = 16 s, enabled).

# Disarm the FEL-armed watchdog: we are alive.
mw.l 0x030090b8 0

# PD28 as output
mw.l 0x0300b078 0x00010000

# Signal 1 "U-Boot alive": LED off + backlight on for 2 s, twice
for i in 1 2; do
  mw.l 0x0300b130 0x0; mw.l 0x0300b07c 0x10000000; sleep 1
  mw.l 0x0300b130 0x1000; mw.l 0x0300b07c 0x0; sleep 1
done

setenv kernel_comp_addr_r 0x46000000
setenv kernel_comp_size 0x1000000
setenv bootargs console=ttyS0,115200 console=tty0 loglevel=8 ignore_loglevel printk.time=1 panic=5 g_ether.dev_addr=02:a8:b0:00:00:02 g_ether.host_addr=02:a8:b0:00:00:01

# Signal 2 "starting Linux": 3 quick LED blinks
for i in 1 2 3; do
  mw.l 0x0300b130 0x0; sleep 0.2; mw.l 0x0300b130 0x1000; sleep 0.2
done

# Re-arm the watchdog; Linux's sunxi_wdt keeps it fed once it probes.
mw.l 0x030090b4 1
mw.l 0x030090b8 0xb1
booti 0x40080000 0x4ff00000:${initrd_size} 0x4fa00000

# booti failed: fast blink until the watchdog resets us into FEL
while true; do mw.l 0x0300b130 0x0; sleep 0.1; mw.l 0x0300b130 0x1000; sleep 0.1; done
