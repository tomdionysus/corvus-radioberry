// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tone generator for the AD9866 transmit bus, used only by the armed
// transmit test. A 32-bit phase accumulator at the 76.8 MHz sample clock
// addresses a 1024-entry sine table; the 12-bit sample is scaled by a
// 12-bit amplitude and sent as the radio gateware sends it: each sample as
// two 6-bit halves at twice the sample rate, the high half with TX sync
// low, then the low half with TX sync high. TX quiet (active low) is
// released only while running and not asked to stay quiet (for the
// AD9866's digital loopback, which needs the bus but not the DAC);
// otherwise the bus and TX sync are held low.
module rbt_txtone (
    input  logic        clk,
    input  logic        clk_2x,
    input  logic        run,          // clk domain
    input  logic        quiet,        // keep TX quiet low (DAC and IAMP off) while running
    input  logic [31:0] phase_inc,
    input  logic [11:0] amplitude,

    output logic [5:0]  ad_tx,
    output logic        ad_txsync,
    output logic        ad_txquiet_n
);

    logic [11:0] sine [0:1023];
`ifndef SINE_HEX
`define SINE_HEX "../../rtl/linktest/data/sin1024.hex"
`endif
    initial $readmemh(`SINE_HEX, sine);

    logic [31:0]        phase = 32'd0;
    logic signed [11:0] s_raw;
    logic signed [23:0] s_scaled;
    logic [11:0]        sample = 12'd0;

    always_ff @(posedge clk) begin
        phase    <= run ? phase + phase_inc : 32'd0;
        s_raw    <= sine[phase[31:22]];
        s_scaled <= s_raw * $signed({1'b0, amplitude[11:1]});
        sample   <= run ? s_scaled[22:11] : 12'd0;
    end

    logic [2:0]  run_s = 3'b000;
    logic [2:0]  quiet_s = 3'b000;
    logic        tx_sync = 1'b0;
    logic [11:0] hold = 12'd0;

    initial begin
        ad_tx        = 6'd0;
        ad_txsync    = 1'b0;
        ad_txquiet_n = 1'b0;
    end

    always_ff @(posedge clk_2x) begin
        run_s   <= {run_s[1:0], run};
        quiet_s <= {quiet_s[1:0], quiet};
        tx_sync <= ~tx_sync;
        if (run_s[2]) begin
            if (tx_sync) begin
                hold  <= sample;
                ad_tx <= hold[5:0];
            end else begin
                ad_tx <= hold[11:6];
            end
            ad_txsync    <= tx_sync;
            ad_txquiet_n <= ~quiet_s[2];
        end else begin
            ad_tx        <= 6'd0;
            ad_txsync    <= 1'b0;
            ad_txquiet_n <= 1'b0;
        end
    end

endmodule
