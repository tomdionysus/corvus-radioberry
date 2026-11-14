// SPDX-License-Identifier: GPL-2.0-or-later
//
// Raw capture of the AD9866 receive bus. On a start request it stores
// 2048 consecutive samples of {RX sync, RX data[5:0]} taken at the rising
// edge of the doubled sample clock and, beside each, the same pins taken
// at the falling edge before it. The host reads them back one by one and
// decides which edge and which pairing of halves is right.
module rbt_rxcap (
    input  logic        clk_2x,
    input  logic [5:0]  rx,
    input  logic        rxsync,

    input  logic        clk,
    input  logic        start_tgl,   // toggle in clk's domain
    output logic        done,        // in clk's domain
    input  logic [10:0] raddr,
    output logic [13:0] rdata        // {falling[6:0], rising[6:0]}
);

    logic [6:0] pos_q, neg_q, neg_r;
    always_ff @(posedge clk_2x) pos_q <= {rxsync, rx};
    always_ff @(negedge clk_2x) neg_q <= {rxsync, rx};
    always_ff @(posedge clk_2x) neg_r <= neg_q;

    logic [2:0]  st_s = 3'b000;
    logic        running = 1'b0, done_2x = 1'b0;
    logic [10:0] waddr = 11'd0;

    (* ramstyle = "M9K" *) logic [13:0] mem [0:2047];

    always_ff @(posedge clk_2x) begin
        st_s <= {st_s[1:0], start_tgl};
        if (st_s[2] ^ st_s[1]) begin
            running <= 1'b1;
            done_2x <= 1'b0;
            waddr   <= 11'd0;
        end else if (running) begin
            mem[waddr] <= {neg_r, pos_q};
            waddr      <= waddr + 11'd1;
            if (waddr == 11'd2047) begin
                running <= 1'b0;
                done_2x <= 1'b1;
            end
        end
    end

    logic [1:0] done_s = 2'b00;
    always_ff @(posedge clk) begin
        done_s <= {done_s[0], done_2x};
        rdata  <= mem[raddr];
    end
    assign done = done_s[1];

endmodule
