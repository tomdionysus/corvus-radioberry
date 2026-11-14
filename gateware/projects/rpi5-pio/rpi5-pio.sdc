# SPDX-License-Identifier: GPL-2.0-or-later
#
# Timing constraints for the rpi5-pio build.

set_time_format -unit ns -decimal_places 3

create_clock -name clk53 -period 13.020 [get_ports clk53]
create_clock -name spi_sck -period 50.000 [get_ports spi_sck]
# The Pi's RX clock: 50 ns a nibble at the driver's default (RP1 PIO at
# 200 MHz / 2: 40 ns high, 10 ns low), 80 ns while idle. Only rising edges
# are used. The TX clock: 280 ns a bit at the default divider (8); the
# period here is shorter than any the driver makes.
create_clock -name pi_rx_clk -period 50.000 [get_ports pi_rx_clk]
create_clock -name pi_tx_clk -period 80.000 [get_ports pi_tx_clk]
derive_pll_clocks
derive_clock_uncertainty

# Domains meet only through synchronisers, Gray-coded FIFO pointers and
# toggle handshakes whose data is held still while they cross; counters
# read across them are diagnostics.
set_clock_groups -asynchronous \
    -group {clk53} \
    -group {spi_sck} \
    -group {pll_i|pll|auto_generated|pll1|clk[0] pll_i|pll|auto_generated|pll1|clk[1]} \
    -group {pi_rx_clk} \
    -group {pi_tx_clk}

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
# Pin 105 (ad_tx[4]): see rpi4-i2s.sdc and hardware_discovery.md section 4.
set_max_delay -from [get_registers {ad_tx_q[4]}] -to [get_ports {ad_tx[4]}] 10.0

# RX wire: the nibble and sample-ready change on the Pi's rising edge and
# must be at the pins within 15 ns of the clock reaching its pin (the Pi
# samples about 20 ns after it drives the edge, less the wire both ways).
set_output_delay -clock pi_rx_clk -max 35.0 [get_ports {pi_rx_data[*] pi_rx_ready}]
set_output_delay -clock pi_rx_clk -min 0.0 [get_ports {pi_rx_data[*] pi_rx_ready}]
# TX wire: the Pi sets each bit 40 ns before its rising edge and holds it
# 120 ns after.
set_input_delay -clock pi_tx_clk -max 40.0 [get_ports {pi_tx_data}]
set_input_delay -clock pi_tx_clk -min 0.0 [get_ports {pi_tx_data}]
set_output_delay -clock pi_tx_clk -max 40.0 [get_ports {pi_tx_ready}]
set_output_delay -clock pi_tx_clk -min 0.0 [get_ports {pi_tx_ready}]

# Slow or asynchronous pins.
set_false_path -from [get_ports {spi_ce0_n spi_mosi ad_sdio}]
set_false_path -to   [get_ports {spi_miso}]
set_false_path -to   [get_ports {pa_out[*] ad_rst_n ad_mode ad_sen_n ad_sclk ad_sdio}]
set_false_path -to   [get_ports {ad_txquiet_n}]
