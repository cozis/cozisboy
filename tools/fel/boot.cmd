# USB (FEL) development boot. Green LED = PI12. Watchdog 0: cfg 0x030090b4, mode 0x030090b8.
mw.l 0x030090b8 0
# 2 slow blinks: U-Boot is alive
for i in 1 2; do gpio clear PI12; sleep 0.6; gpio set PI12; sleep 0.6; done
sleep 1
setenv kernel_comp_addr_r 0x46000000
setenv kernel_comp_size 0x1000000
setenv bootargs console=ttyS0,115200 console=tty0 panic=5 g_ether.dev_addr=02:a8:b0:00:00:02 g_ether.host_addr=02:a8:b0:00:00:01
# 3 quick blinks: about to start Linux
for i in 1 2 3; do gpio clear PI12; sleep 0.2; gpio set PI12; sleep 0.2; done
sleep 1
gpio clear PI12
# Re-arm the watchdog (16 s); Linux's sunxi_wdt keeps it fed once it probes
mw.l 0x030090b4 1
mw.l 0x030090b8 0xb1
booti 0x40080000 0x4ff00000:${initrd_size} 0x4fa00000
# booti failed: fast blink until the watchdog resets us into FEL
while true; do gpio toggle PI12; sleep 0.1; done
