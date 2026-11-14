// SPDX-License-Identifier: GPL-2.0-or-later
//
// PLL for the link test image: 76.8 MHz in; 76.8 MHz, 153.6 MHz and
// 122.88 MHz out.
module rbt_pll (
    input  logic inclk0,
    output logic c0,
    output logic c1,
    output logic c2,
    output logic locked
);

    logic [4:0] clk_out;

    altpll #(
        .bandwidth_type("AUTO"),
        .clk0_divide_by(1),
        .clk0_duty_cycle(50),
        .clk0_multiply_by(1),
        .clk0_phase_shift("0"),
        .clk1_divide_by(1),
        .clk1_duty_cycle(50),
        .clk1_multiply_by(2),
        .clk1_phase_shift("0"),
        .compensate_clock("CLK0"),
        .inclk0_input_frequency(13020),
        .intended_device_family("Cyclone 10 LP"),
        .lpm_type("altpll"),
        .operation_mode("NORMAL"),
        .pll_type("AUTO"),
        .port_inclk0("PORT_USED"),
        .port_locked("PORT_USED"),
        .port_clk0("PORT_USED"),
        .port_clk1("PORT_USED"),
        .clk2_divide_by(5),
        .clk2_duty_cycle(50),
        .clk2_multiply_by(8),
        .clk2_phase_shift("0"),
        .port_clk2("PORT_USED"),
        .port_clk3("PORT_UNUSED"),
        .port_clk4("PORT_UNUSED"),
        .self_reset_on_loss_lock("ON"),
        .width_clock(5)
    ) pll (
        .inclk({1'b0, inclk0}),
        .clk(clk_out),
        .locked(locked)
    );

    assign c0 = clk_out[0];
    assign c1 = clk_out[1];
    assign c2 = clk_out[2];

endmodule
