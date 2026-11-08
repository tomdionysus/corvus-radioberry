# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run by tools/build-gateware.sh with quartus_sta -t after a compile:
# writes the worst setup and hold paths of every clock to timing.txt.
project_open radioberry -revision radioberry
create_timing_netlist
read_sdc
update_timing_netlist
foreach_in_collection clk [get_clocks *] {
    set name [get_clock_info -name $clk]
    report_timing -to_clock $name -setup -npaths 8 -detail summary -file timing.txt -append
    report_timing -to_clock $name -hold -npaths 3 -detail summary -file timing.txt -append
}
delete_timing_netlist
project_close
