# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run by tools/build-gateware.sh as quartus_sh -t compile.tcl <seed>:
# compiles the project in the current directory with the given fitter
# seed. The seed is set on the build copy only; the source is unchanged.
load_package flow
project_open radioberry -revision radioberry
set_global_assignment -name SEED [lindex $argv 0]
execute_flow -compile
project_close
