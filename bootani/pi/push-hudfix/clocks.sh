#!/bin/sh
# Sample the Pi's clocks while a game runs: CPU (cpufreq), and the
# firmware clocks for ARM, core (VPU/V3D bus), V3D and SDRAM from
# debugfs, plus the undervoltage alarm. One line per second.
#   sh clocks.sh [seconds]     (default 30)
n=${1:-30}
mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>/dev/null
clk=/sys/kernel/debug/clk
rate() { [ -r "$clk/$1/clk_rate" ] && echo $(( $(cat "$clk/$1/clk_rate") / 1000000 )) || echo -; }
lcrit=$(ls /sys/class/hwmon/hwmon*/in0_lcrit_alarm 2>/dev/null | head -n 1)
echo "gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
echo "time  cpu0 cpu1 cpu2 cpu3  arm core v3d sdram  uv  temp"
i=0
while [ $i -lt "$n" ]; do
	f=""
	for c in 0 1 2 3; do
		v=$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null)
		f="$f $(( ${v:-0} / 1000 ))"
	done
	t=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
	printf '%4d %s  %s %s %s %s  %s  %s\n' $i "$f" \
		"$(rate fw-clk-arm)" "$(rate fw-clk-core)" "$(rate fw-clk-v3d)" \
		"$(rate fw-clk-sdram)" "$(cat "$lcrit" 2>/dev/null || echo -)" \
		"$(( ${t:-0} / 1000 ))C"
	i=$((i + 1))
	sleep 1
done
