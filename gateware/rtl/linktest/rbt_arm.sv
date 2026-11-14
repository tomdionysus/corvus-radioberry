// SPDX-License-Identifier: GPL-2.0-or-later
//
// Arming for the link test image's two transmit-side tests. Nothing on
// the transmit side moves unless the image is armed, and arming lapses on
// its own.
//
// Arming takes two writes to ARM in order, the second within 100 ms of
// the first: ARM_KEY1 then ARM_KEY2. The image then stays armed for
// ARM_TIME milliseconds (capped at 10 000) and disarms itself. Any other
// write to ARM disarms at once, and so does a loss of PLL lock.
//
// While armed:
//   - pa_req[3:0] reaches the PA lines (exttr, inttr, envpa, envbias);
//   - tx_req starts the tone on the AD9866 transmit bus, but only if
//     txgain_ok: the AD9866's transmit gain register has been written with
//     a value no higher than TXGAIN_MAX since the chip's reset was last
//     released. The chip comes out of reset at its highest gain.
// On disarming both requests are cleared.
module rbt_arm (
    input  logic        clk,
    input  logic        rst,
    input  logic        ms_tick,

    input  logic        wr_arm,
    input  logic [31:0] wdata,
    input  logic [13:0] arm_ms,

    output logic        armed,
    output logic        stage1,
    output logic [13:0] remaining_ms,
    output logic        disarm_stb
);

    localparam logic [31:0] ARM_KEY1 = 32'h41524D31;   // "ARM1"
    localparam logic [31:0] ARM_KEY2 = 32'h54583221;   // "TX2!"

    logic [6:0] s1_ms = 7'd0;

    initial begin
        armed        = 1'b0;
        stage1       = 1'b0;
        remaining_ms = 14'd0;
        disarm_stb   = 1'b0;
    end

    always_ff @(posedge clk) begin
        disarm_stb <= 1'b0;
        if (rst) begin
            armed        <= 1'b0;
            stage1       <= 1'b0;
            remaining_ms <= 14'd0;
            disarm_stb   <= 1'b1;
        end else begin
            if (stage1 && ms_tick) begin
                if (s1_ms == 7'd100) stage1 <= 1'b0;
                else s1_ms <= s1_ms + 7'd1;
            end
            if (armed && ms_tick) begin
                if (remaining_ms == 14'd0) begin
                    armed      <= 1'b0;
                    disarm_stb <= 1'b1;
                end else remaining_ms <= remaining_ms - 14'd1;
            end
            if (wr_arm) begin
                if (wdata == ARM_KEY1 && !armed) begin
                    stage1 <= 1'b1;
                    s1_ms  <= 7'd0;
                end else if (wdata == ARM_KEY2 && stage1 && !armed) begin
                    stage1       <= 1'b0;
                    armed        <= 1'b1;
                    remaining_ms <= (arm_ms > 14'd10000) ? 14'd10000 : arm_ms;
                end else begin
                    stage1     <= 1'b0;
                    armed      <= 1'b0;
                    disarm_stb <= 1'b1;
                end
            end
        end
    end

endmodule
