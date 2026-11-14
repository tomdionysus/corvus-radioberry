# SPDX-License-Identifier: GPL-2.0-or-later
# quartus_sta -t worst.tcl PROJECT: worst setup paths of every clock to
# worst.txt, worst hold paths to worst-hold.txt
project_open [lindex $argv 0]
create_timing_netlist
read_sdc
update_timing_netlist
foreach_in_collection clk [get_clocks *] {
  report_timing -to_clock [get_clock_info -name $clk] -setup -npaths 6 -detail summary -file worst.txt -append
  report_timing -to_clock [get_clock_info -name $clk] -hold -npaths 6 -detail summary -file worst-hold.txt -append
}
delete_timing_netlist
project_close
