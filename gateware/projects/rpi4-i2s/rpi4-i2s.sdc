# SPDX-License-Identifier: GPL-2.0-or-later
#
# Timing constraints for the rpi4-i2s receive build.

set_time_format -unit ns -decimal_places 3

create_clock -name clk53 -period 13.020 [get_ports clk53]
create_clock -name spi_sck -period 50.000 [get_ports spi_sck]
derive_pll_clocks
derive_clock_uncertainty

# Domains meet only through synchronisers and toggle handshakes whose data
# is held still while they cross.
set_clock_groups -asynchronous \
    -group {clk53} \
    -group {spi_sck} \
    -group {pll_i|pll|auto_generated|pll1|clk[0] pll_i|pll|auto_generated|pll1|clk[1]} \
    -group {pll_i|pll|auto_generated|pll1|clk[2]}

# The radio core's FIR filters accumulate at 153.6 MHz and hand the sum to
# 76.8 MHz logic every second fast clock: a two-cycle path, as the radio
# gateware's own constraints declare it.
set_multicycle_path -from [get_clocks {pll_i|pll|auto_generated|pll1|clk[1]}] -to [get_clocks {pll_i|pll|auto_generated|pll1|clk[0]}] -setup -start 2
set_multicycle_path -from [get_clocks {pll_i|pll|auto_generated|pll1|clk[1]}] -to [get_clocks {pll_i|pll|auto_generated|pll1|clk[0]}] -hold -start 2

# AD9866 receive and transmit buses against the doubled sample clock, as
# the radio gateware constrains them (its timing.sdc).
create_clock -name virt_adc -period 6.510
set_input_delay -clock virt_adc -max 5.0 [get_ports {ad_rx[*] ad_rxsync}]
set_input_delay -clock virt_adc -min 0.0 [get_ports {ad_rx[*] ad_rxsync}]
set_output_delay -clock virt_adc -max 2.5 [get_ports {ad_tx[*] ad_txsync}]
set_output_delay -clock virt_adc -min 0.0 [get_ports {ad_tx[*] ad_txsync}]
# Pin 105 (ad_tx[4]) has a 5.4 ns output buffer in the slow model, the
# other TX pins about 2.2 ns; the radio gateware relaxes this one bit the
# same way (set_max_delay 10) and transmits on this board. Fragile: the
# strict margin is printed by tools/worst.sh; see hardware_discovery.md
# section 4.
set_max_delay -from [get_registers {ad_tx_q[4]}] -to [get_ports {ad_tx[4]}] 10.0

# Slow or asynchronous pins.
set_false_path -from [get_ports {spi_ce0_n spi_mosi i2s_from_pi ad_sdio}]
set_false_path -to   [get_ports {spi_miso i2s_bclk i2s_fs i2s_to_pi}]
set_false_path -to   [get_ports {pa_out[*] ad_rst_n ad_mode ad_sen_n ad_sclk ad_sdio}]
set_false_path -to   [get_ports {ad_txquiet_n}]
