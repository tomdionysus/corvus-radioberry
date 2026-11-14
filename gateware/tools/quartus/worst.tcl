# SPDX-License-Identifier: GPL-2.0-or-later
# quartus_sta -t worst.tcl PROJECT: worst setup paths of every clock to
# worst.txt, worst hold paths to worst-hold.txt, and the AD9866 TX bus
# under its strict output constraint to worst-txbus.txt
set p [lindex $argv 0]
project_open $p
create_timing_netlist
read_sdc
update_timing_netlist
foreach_in_collection clk [get_clocks *] {
  report_timing -to_clock [get_clock_info -name $clk] -setup -npaths 6 -detail summary -file worst.txt -append
  report_timing -to_clock [get_clock_info -name $clk] -hold -npaths 6 -detail summary -file worst-hold.txt -append
}

# The project's constraints relax ad_tx[4] (pin 105) with set_max_delay,
# which hides that bit's margin. Analyse the TX bus again with every
# set_max_delay removed, so all six bits face the AD9866's setup window.
if {[llength [get_ports -nowarn {ad_tx[*]}]] > 0} {
  set in [open $p.sdc r]
  set out [open strict-txbus.sdc w]
  foreach line [split [read $in] "\n"] {
    if {![string match "set_max_delay*" [string trim $line]]} { puts $out $line }
  }
  close $in
  close $out
  reset_design
  read_sdc strict-txbus.sdc
  update_timing_netlist
  report_timing -to [get_ports {ad_tx[*] ad_txsync}] -setup -npaths 7 -detail summary -file worst-txbus.txt
}
delete_timing_netlist
project_close
