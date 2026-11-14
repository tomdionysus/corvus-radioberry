# SPDX-License-Identifier: GPL-2.0-or-later
#
# Timing constraints for the link test image.

set_time_format -unit ns -decimal_places 3

create_clock -name clk53 -period 13.020 [get_ports clk53]
derive_pll_clocks
derive_clock_uncertainty

# Clocks the image only measures or is driven by from outside.
create_clock -name spi_sck      -period 50.000 [get_ports spi_sck]
create_clock -name ad_clkout1b  -period 6.510  [get_ports ad_clkout1b]
create_clock -name ad_rxclk     -period 6.510  [get_ports ad_rxclk]
create_clock -name ad_rxsync_ck -period 6.510  [get_ports ad_rxsync]

# The I2S tester's clock crosses to the rest by synchronisers only.
set_clock_groups -asynchronous -group {pll_i|pll|auto_generated|pll1|clk[2]}
set_clock_groups -asynchronous -group {spi_sck}
set_clock_groups -asynchronous -group {ad_clkout1b}
set_clock_groups -asynchronous -group {ad_rxclk}
set_clock_groups -asynchronous -group {ad_rxsync_ck}

# AD9866 receive bus against the doubled sample clock, as the radio
# gateware constrains it.
create_clock -name virt_adc -period 6.510
set_input_delay -clock virt_adc -max 5.0 [get_ports {ad_rx[*] ad_rxsync}]
set_input_delay -clock virt_adc -min 0.0 [get_ports {ad_rx[*] ad_rxsync}]

# The raw capture's falling-edge registers sample the bus half a clock
# away from the intended point on purpose; they are a diagnostic, not a
# path that has to meet the bus timing.
set_false_path -to [get_registers {rbt_rxcap:rxcap_i|neg_q[*]}]

# Slow or asynchronous pins.
set_false_path -from [get_ports {spi_ce0_n spi_mosi}]
set_false_path -to   [get_ports {spi_miso}]
set_false_path -from [get_ports {pin_io[*] pin_in[*] cn301_in[*] ad_sdo ad_sdio}]
set_false_path -to   [get_ports {pin_io[*] pa_out[*] ad_sdio ad_sen_n ad_sclk}]
set_false_path -to   [get_ports {ad_rst_n ad_mode ad_tx[*] ad_txsync ad_txquiet_n}]
