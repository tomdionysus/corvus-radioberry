// SPDX-License-Identifier: GPL-2.0-or-later
//
// Dual-clock FIFO with Gray-coded pointers, block RAM storage and a
// one-word prefetch on the read side (rd_valid/rd_data show the next word;
// rd_take consumes it). A clear request from the write side empties it
// from both sides, each side resetting itself in its own clock domain.
//
// wfree is registered and so one write behind: a write is taken only while
// it shows 2 or more, which keeps one word in hand.
module rbv_afifo #(
    parameter int W  = 28,
    parameter int AW = 10
) (
    input  logic          wclk,
    input  logic          wclr,          // write domain: empty the FIFO
    input  logic          wen,
    input  logic [W-1:0]  wdata,
    output logic [AW:0]   wfree,         // free words, write domain

    input  logic          rclk,
    input  logic          rtake,
    output logic          rvalid,
    output logic [W-1:0]  rdata
);

    function automatic logic [AW:0] b2g(input logic [AW:0] b);
        return b ^ (b >> 1);
    endfunction
    function automatic logic [AW:0] g2b(input logic [AW:0] g);
        logic [AW:0] b;
        b[AW] = g[AW];
        for (int i = AW - 1; i >= 0; i--) b[i] = b[i + 1] ^ g[i];
        return b;
    endfunction

    (* ramstyle = "M9K" *) logic [W-1:0] mem [0:(1<<AW)-1];

    // ---- write side ----
    logic [AW:0] wptr = '0, wgray = '0;
    logic [AW:0] rgray_w1 = '0, rgray_w2 = '0, rbin_w = '0;
    logic [AW:0] wfree_q = '0;
    logic        wclr_tgl = 1'b0;

    always_ff @(posedge wclk) begin
        rgray_w1 <= rgray;
        rgray_w2 <= rgray_w1;
        rbin_w   <= g2b(rgray_w2);
        wfree_q  <= (AW+1)'(1 << AW) - (wptr - rbin_w);
        if (wclr) begin
            wptr     <= '0;
            wgray    <= '0;
            wfree_q  <= '0;
            wclr_tgl <= ~wclr_tgl;
        end else if (wen && wfree_q > (AW+1)'(1)) begin
            mem[wptr[AW-1:0]] <= wdata;
            wptr  <= wptr + 1'b1;
            wgray <= b2g(wptr + 1'b1);
        end
    end
    assign wfree = wfree_q;

    // ---- read side ----
    logic [AW:0]  rptr = '0, rgray = '0;
    logic [AW:0]  wgray_r1 = '0, wgray_r2 = '0;
    logic [2:0]   clr_s = 3'b000;
    logic         fetch = 1'b0, have = 1'b0;
    logic [W-1:0] q;
    logic         empty;

    assign empty = (rgray == wgray_r2);

    always_ff @(posedge rclk) begin
        wgray_r1 <= wgray;
        wgray_r2 <= wgray_r1;
        clr_s    <= {clr_s[1:0], wclr_tgl};
        q        <= mem[rptr[AW-1:0]];
        fetch    <= 1'b0;
        if (clr_s[2] ^ clr_s[1]) begin
            rptr  <= '0;
            rgray <= '0;
            have  <= 1'b0;
        end else begin
            if (rtake) have <= 1'b0;
            if ((!have || rtake) && !fetch && !empty) begin
                fetch <= 1'b1;
                rptr  <= rptr + 1'b1;
                rgray <= b2g(rptr + 1'b1);
            end
            if (fetch) begin
                rdata <= q;
                have  <= 1'b1;
            end
        end
    end
    assign rvalid = have;

endmodule
