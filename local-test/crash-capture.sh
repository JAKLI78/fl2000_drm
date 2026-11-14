#!/bin/sh
# Until next reboot: turn a lockup or oops into a panic, save the kernel log to EFI pstore,
# and reboot automatically after 10 s. Run as root. Saved logs appear after reboot in
# /var/lib/systemd/pstore/
sysctl -w kernel.softlockup_panic=1 kernel.hardlockup_panic=1 kernel.panic_on_oops=1 \
	kernel.hung_task_panic=0 kernel.panic=10
