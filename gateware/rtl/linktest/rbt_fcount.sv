// SPDX-License-Identifier: GPL-2.0-or-later
//
// Counts the rising edges of an asynchronous signal over a window of the
// system clock. The signal clocks a divide-by-16 prescaler, whose top bit
// is slow enough to synchronise; the result is in units of 16 edges.
module rbt_fcount (
    input  logic        sig,
    input  logic        clk,
    input  logic        window_stb,
    output logic [31:0] count
);

    logic [3:0] pre = 4'd0;
    always_ff @(posedge sig) pre <= pre + 4'd1;

    logic [2:0]  sync = 3'b000;
    logic [31:0] acc  = 32'd0;

    always_ff @(posedge clk) begin
        sync <= {sync[1:0], pre[3]};
        if (window_stb) begin
            count <= acc;
            acc   <= 32'd0;
        end else if (sync[1] & ~sync[2]) begin
            acc <= acc + 32'd1;
        end
    end

endmodule
