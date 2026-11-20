// SPDX-License-Identifier: GPL-2.0-or-later
//
// Design A+B of docs/core-rework.md, section 9: one decimate-by-8 receive
// filter shared by all the receivers (as design B, firshared.sv, with the
// same ports and module name) that also uses the coefficients' symmetry
// (as design A, firfold.sv). With the core's 976 coefficients (its
// coefL4*.mif files, by James Ahlstrom N2ADR and Phil Harman VK6APH),
// h[n] = h[975 - n], and t0 = 8g + 3 the index of a group's fourth input,
//
//   y[g] = sum over n = 0..487 of round(h[n] * (x[t0 - n] + x[t0 - 975 + n]))
//
// in 24 bits, round(p) = p[35:12] + p[11]: the same terms as design A, so
// the same outputs bit for bit (a sum in modular arithmetic does not
// depend on the order of its terms), which differ from the original's in
// the lowest bits.
//
// Samples are kept as in design B: one memory for each index class (mod
// 8), one write port and one read port, with every receiver's last 128
// samples of that class. Four lanes, p = 0..3, each adding two samples
// and making one product for I and one for Q (8 multipliers), in two
// passes of 61 steps, m = 0..60:
//
//   pass 0: tap n = 8m + p; x[t0 - n] is of class 3 - p (index / 8 =
//           t0 / 8 - m), its partner x[t0 - 975 + n] of class 4 + p
//           (t0 / 8 - 122 + m);
//   pass 1: tap n = 8m + 7 - p; x[t0 - n] is of class 4 + p (t0 / 8 - 1
//           - m), its partner of class 3 - p (t0 / 8 - 121 + m).
//
// So every memory is read once a cycle, and lane p always adds classes
// 3 - p and 4 + p. A receiver takes 122 cycles of the 153.6 MHz clock: 3
// receivers at 384 ksps take about 386 of the 400 cycles between groups,
// 6 at 192 ksps about 752 of 800, the most either Raspberry Pi link
// carries.
//
// When a group's fourth input has been written for every receiver, the
// receivers 0 to last_chan are summed in turn (the core sends no more),
// and one output strobe then gives them all, as the core's sender expects
// (it reads every receiver on receiver 0's strobe). More receivers than
// that at a rate: a group that arrives while the last is still being
// summed is dropped, and its output with it.
module firshared #(
    parameter int NR = 6    // receivers, 1 to 10
) (
    input  logic               rst_all,
    input  logic               clock,       // 76.8 MHz
    input  logic               clock_2x,    // 153.6 MHz, edges aligned
    input  logic [3:0]         last_chan,   // receivers in use, less one
    input  logic               x_avail,     // every receiver's sample, together
    input  logic [16*NR-1:0]   x_real,      // receiver k in bits 16k + 15..16k
    input  logic [16*NR-1:0]   x_imag,
    output logic               y_avail = 1'b0,
    output logic [24*NR-1:0]   y_real = '0, // receiver k in bits 24k + 23..24k
    output logic [24*NR-1:0]   y_imag = '0
);
    localparam int KB = (NR > 8) ? 4 : (NR > 4) ? 3 : (NR > 2) ? 2 : 1;

    // ------------------------------------------------------------------
    // Input: on the first 153.6 MHz edge of x_avail, every receiver's
    // sample is held and then written, one receiver a cycle (the held
    // samples shifted down by one receiver each time), into the memory of
    // its index class, at {receiver, index / 8}.
    logic          xa_q = 1'b0;
    logic          wr;
    assign wr = x_avail && !xa_q && !rst_all;
    logic [16*NR-1:0] xr_h = '0, xi_h = '0;
    logic          writing = 1'b0;
    logic [KB-1:0] wk = '0;         // receiver being written
    logic [9:0]    t = '0;          // index of the sample being written
    logic          we;
    logic [2:0]    wclass;
    logic [KB+6:0] waddr;
    logic [31:0]   wdata;
    assign we     = writing;
    assign wclass = t[2:0];
    assign waddr  = {wk, t[9:3]};
    assign wdata  = {xr_h[15:0], xi_h[15:0]};   // receiver wk: shifted down

    // Group event: the fourth input of a group written for every receiver.
    logic          group;
    localparam logic [KB-1:0] KLAST = NR - 1;
    assign group = writing && wk == KLAST && t[2:0] == 3'd3;

    // ------------------------------------------------------------------
    // The sums, one receiver at a time: two passes of m = 0..60.
    logic          busy = 1'b0;
    logic [KB-1:0] jk = '0, jlast = '0;
    logic          ps = 1'b0;       // pass
    logic [5:0]    m = '0;
    logic [6:0]    base = '0;       // t0 / 8
    logic          issue, last_tap, last_job;
    assign issue    = busy;
    assign last_tap = ps && m == 6'd60;
    assign last_job = jk == jlast;

    // The control's next state, worked out here so that the read addresses
    // can be registered from it (below).
    logic          busy_n, ps_n;
    logic [KB-1:0] jk_n, jlast_n;
    logic [5:0]    m_n;
    logic [6:0]    base_n;
    always_comb begin
        busy_n  = busy;
        ps_n    = ps;
        jk_n    = jk;
        jlast_n = jlast;
        m_n     = m;
        base_n  = base;
        if (rst_all) begin
            busy_n = 1'b0;
        end else begin
            if (busy) begin
                if (m == 6'd60) begin
                    m_n  = '0;
                    ps_n = !ps;
                    if (ps) begin
                        if (last_job) busy_n = 1'b0;
                        else jk_n = jk + 1'b1;
                    end
                end else begin
                    m_n = m + 6'd1;
                end
            end
            if (group && !busy) begin
                busy_n  = 1'b1;
                jk_n    = '0;
                jlast_n = (last_chan >= NR - 1) ? KLAST : last_chan[KB-1:0];
                ps_n    = 1'b0;
                m_n     = '0;
                base_n  = t[9:3];
            end
        end
    end

    always_ff @(posedge clock_2x) begin
        xa_q  <= x_avail;
        busy  <= busy_n;
        ps    <= ps_n;
        jk    <= jk_n;
        jlast <= jlast_n;
        m     <= m_n;
        base  <= base_n;
        if (rst_all) begin
            writing <= 1'b0;
            wk      <= '0;
            t       <= '0;
        end else begin
            if (wr) begin
                xr_h    <= x_real;
                xi_h    <= x_imag;
                writing <= 1'b1;
                wk      <= '0;
            end else if (writing) begin
                xr_h <= xr_h >> 16;
                xi_h <= xi_h >> 16;
                if (wk == KLAST) begin
                    writing <= 1'b0;
                    t       <= t + 10'd1;
                end else begin
                    wk <= wk + 1'b1;
                end
            end
        end
    end

    // ------------------------------------------------------------------
    // Sample memories, one for each index class c, read at index / 8:
    // classes 0..3 at t0 / 8 - m (pass 0) or t0 / 8 - 121 + m (pass 1),
    // classes 4..7 at t0 / 8 - 122 + m or t0 / 8 - 1 - m. Each class keeps
    // its address in its own register, which steps by one each cycle (down
    // or up by class and pass) and is loaded from t0 / 8 at a group's start,
    // a pass's start or a receiver's: no arithmetic on m before the memory.
    logic start;
    assign start = group && !busy && !rst_all;
    logic [255:0] q;    // class c in bits 32c + 31..32c
    genvar c;
    generate for (c = 0; c < 8; c = c + 1) begin : CLASS
        logic [31:0] q_r, q_r2 = '0;
        logic [6:0]    ra = '0;
        logic [KB-1:0] rk = '0;
        always_ff @(posedge clock_2x) begin
            if (start) begin
                rk <= '0;
                ra <= (c < 4) ? t[9:3] : t[9:3] - 7'd122;
            end else if (busy) begin
                if (m == 6'd60) begin
                    if (!ps) begin
                        ra <= (c < 4) ? base - 7'd121 : base - 7'd1;
                    end else begin
                        rk <= rk + 1'b1;
                        ra <= (c < 4) ? base : base - 7'd122;
                    end
                end else if ((c < 4) != ps) begin
                    ra <= ra - 7'd1;
                end else begin
                    ra <= ra + 7'd1;
                end
            end
        end
        fir_sdpram #(.W(32), .D(NR * 128), .AW(KB + 7)) ram (
            .clk(clock_2x), .we(we && wclass == c), .waddr(waddr), .wdata(wdata),
            .raddr({rk, ra}), .q(q_r)
        );
        // fir_sdpram's output (q_r) is two cycles after the address: the
        // memory blocks' own output registers. One more register after the
        // select between the blocks of a memory deeper than one block.
        always_ff @(posedge clock_2x) q_r2 <= q_r;
        assign q[32*c +: 32] = q_r2;
    end endgenerate

    // Coefficients: word {pass, m} holds lane p's coefficient in bits
    // 18p + 17..18p: h[8m + p] in pass 0, h[8m + 7 - p] in pass 1.
    logic [71:0] coef_rom [0:127];
    logic [71:0] coef_q = '0;
    // coef_q2..coef_q4 in registers: Quartus would otherwise make them a
    // shift register in two memory blocks.
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [71:0] coef_q2 = '0;
    always_ff @(posedge clock_2x) coef_q <= coef_rom[{ps, m}];

    always_ff @(posedge clock_2x) coef_q2 <= coef_q;

    // Flags beside the data: read, the two output registers, pre-added
    // sums, operands, products, rounded products.
    logic [6:0]    v = '0, l = '0, g = '0;
    logic [KB-1:0] kp0 = '0, kp1 = '0, kp2 = '0, kp3 = '0, kp4 = '0, kp5 = '0, kp6 = '0;
    always_ff @(posedge clock_2x) begin
        v   <= {v[5:0], issue};
        l   <= {l[5:0], issue && last_tap};
        g   <= {g[5:0], issue && last_tap && last_job};
        kp6 <= kp5;
        kp0 <= jk;
        kp1 <= kp0;
        kp2 <= kp1;
        kp3 <= kp2;
        kp4 <= kp3;
        kp5 <= kp4;
    end
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [71:0] coef_q3 = '0;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)
    logic [71:0] coef_q4 = '0;
    always_ff @(posedge clock_2x) begin
        coef_q3 <= coef_q2;
        coef_q4 <= coef_q3;
    end

    // The lanes: pre-added operands, products and accumulators. The operand
    // and product registers run freely, with no enable and no reset, so
    // that they can sit in the multiplier blocks.
    function automatic logic [23:0] rnd(input logic signed [35:0] p);
        return p[35:12] + {23'd0, p[11]};
    endfunction
    logic first = 1'b1;
    logic [95:0] acc_r, acc_i;    // lane p in bits 24p + 23..24p
    genvar p;
    generate for (p = 0; p < 4; p = p + 1) begin : LANE
        localparam int CN = 3 - p;      // the two classes lane p adds
        localparam int CO = 4 + p;
        logic signed [15:0] nr, ni, orr, oi;
        assign nr  = q[32*CN+16 +: 16];
        assign ni  = q[32*CN +: 16];
        assign orr = q[32*CO+16 +: 16];
        assign oi  = q[32*CO +: 16];
        // The sums in logic registers, then the multipliers' own operand
        // registers: an adder feeding a multiplier block's input directly
        // leaves too little of the cycle for the route into the block.
        logic signed [17:0] s_r = '0, s_i = '0;
        logic signed [17:0] a_r = '0, a_i = '0, b_r = '0, b_i = '0;
        logic signed [35:0] p_r = '0, p_i = '0;
        logic [23:0] rr_ = '0, ri_ = '0;  // rounded products: the rounding and
                                          // the accumulation are each a 24-bit
                                          // carry chain, too much for one cycle
        logic [23:0] ar = '0, ai = '0;
        always_ff @(posedge clock_2x) begin
            s_r <= 18'(nr) + 18'(orr);
            s_i <= 18'(ni) + 18'(oi);
            a_r <= s_r;
            a_i <= s_i;
            b_r <= coef_q4[18*p +: 18];
            b_i <= coef_q4[18*p +: 18];
            p_r <= a_r * b_r;
            p_i <= a_i * b_i;
            rr_ <= rnd(p_r);
            ri_ <= rnd(p_i);
            if (v[6]) begin
                ar <= (first ? 24'd0 : ar) + rr_;
                ai <= (first ? 24'd0 : ai) + ri_;
            end
        end
        assign acc_r[24*p +: 24] = ar;
        assign acc_i[24*p +: 24] = ai;
    end endgenerate

    // A receiver's sum done: the four lanes added in two steps, then its
    // output; after the last receiver, the strobe.
    logic          d1 = 1'b0, d2 = 1'b0;
    logic          g1 = 1'b0, g2 = 1'b0;
    logic [KB-1:0] k1 = '0, k2 = '0;
    logic [23:0]   s2r0 = '0, s2r1 = '0, s2i0 = '0, s2i1 = '0;
    logic          res_t = 1'b0;   // toggles when every receiver's result is new
    always_ff @(posedge clock_2x) begin
        if (rst_all) first <= 1'b1;
        else if (v[6]) first <= l[6];
        d1 <= v[6] && l[6] && !rst_all;
        g1 <= g[6];
        k1 <= kp6;
        if (d1) begin
            s2r0 <= acc_r[0 +: 24]  + acc_r[24 +: 24];
            s2r1 <= acc_r[48 +: 24] + acc_r[72 +: 24];
            s2i0 <= acc_i[0 +: 24]  + acc_i[24 +: 24];
            s2i1 <= acc_i[48 +: 24] + acc_i[72 +: 24];
        end
        d2 <= d1;
        g2 <= g1;
        k2 <= k1;
        if (d2 && g2) res_t <= ~res_t;
    end

    // Each receiver's result.
    logic [24*NR-1:0] res_r, res_i;
    genvar k;
    generate for (k = 0; k < NR; k = k + 1) begin : RES
        logic [23:0] rr = '0, ri = '0;
        always_ff @(posedge clock_2x)
            if (d2 && k2 == k) begin
                rr <= s2r0 + s2r1;
                ri <= s2i0 + s2i1;
            end
        assign res_r[24*k +: 24] = rr;
        assign res_i[24*k +: 24] = ri;
    end endgenerate

    // Output in the 76.8 MHz domain: one strobe for each group.
    logic res_t_s = 1'b0;
    always_ff @(posedge clock) begin
        res_t_s <= res_t;
        y_avail <= res_t != res_t_s;
        if (res_t != res_t_s) begin
            y_real <= res_r;
            y_imag <= res_i;
        end
    end

    // The 488 distinct coefficients (h[n] = h[975 - n]), word {pass, m}
    // (18-bit two's complement each), from the core's coefL4AE/BF/CG/DH.mif
    // files.
    initial begin
        coef_rom[0] = {18'h00001, 18'h00003, 18'h00003, 18'h00004};
        coef_rom[1] = {18'h3ff9c, 18'h3ffb1, 18'h3ffc5, 18'h3ffd7};
        coef_rom[2] = {18'h3ff6a, 18'h3ff59, 18'h3ff4f, 18'h3ff4e};
        coef_rom[3] = {18'h00050, 18'h00041, 18'h0002a, 18'h0000b};
        coef_rom[4] = {18'h3ffd0, 18'h3ffe4, 18'h3fffd, 18'h00017};
        coef_rom[5] = {18'h00020, 18'h0000b, 18'h3fff4, 18'h3ffde};
        coef_rom[6] = {18'h3ffe8, 18'h3fffd, 18'h00013, 18'h00026};
        coef_rom[7] = {18'h00013, 18'h3fffd, 18'h3ffe8, 18'h3ffd6};
        coef_rom[8] = {18'h3fff0, 18'h00007, 18'h0001d, 18'h0002f};
        coef_rom[9] = {18'h0000d, 18'h3fff4, 18'h3ffdd, 18'h3ffcb};
        coef_rom[10] = {18'h3fff6, 18'h00012, 18'h0002a, 18'h0003b};
        coef_rom[11] = {18'h00006, 18'h3ffe8, 18'h3ffcf, 18'h3ffbd};
        coef_rom[12] = {18'h3ffff, 18'h0001f, 18'h0003a, 18'h0004c};
        coef_rom[13] = {18'h3fffc, 18'h3ffd9, 18'h3ffbc, 18'h3ffaa};
        coef_rom[14] = {18'h0000a, 18'h00031, 18'h0004f, 18'h00061};
        coef_rom[15] = {18'h3ffee, 18'h3ffc5, 18'h3ffa5, 18'h3ff93};
        coef_rom[16] = {18'h0001a, 18'h00047, 18'h00068, 18'h00079};
        coef_rom[17] = {18'h3ffdc, 18'h3ffac, 18'h3ff89, 18'h3ff79};
        coef_rom[18] = {18'h0002f, 18'h00063, 18'h00086, 18'h00096};
        coef_rom[19] = {18'h3ffc4, 18'h3ff8e, 18'h3ff69, 18'h3ff5b};
        coef_rom[20] = {18'h0004a, 18'h00084, 18'h000a9, 18'h000b5};
        coef_rom[21] = {18'h3ffa7, 18'h3ff69, 18'h3ff43, 18'h3ff39};
        coef_rom[22] = {18'h0006b, 18'h000ab, 18'h000d2, 18'h000d9};
        coef_rom[23] = {18'h3ff82, 18'h3ff3e, 18'h3ff18, 18'h3ff15};
        coef_rom[24] = {18'h00093, 18'h000da, 18'h00100, 18'h000ff};
        coef_rom[25] = {18'h3ff56, 18'h3ff0c, 18'h3fee7, 18'h3feec};
        coef_rom[26] = {18'h000c3, 18'h00110, 18'h00134, 18'h00129};
        coef_rom[27] = {18'h3ff22, 18'h3fed2, 18'h3feb0, 18'h3fec0};
        coef_rom[28] = {18'h000fc, 18'h0014e, 18'h0016e, 18'h00157};
        coef_rom[29] = {18'h3fee4, 18'h3fe8f, 18'h3fe73, 18'h3fe92};
        coef_rom[30] = {18'h00140, 18'h00196, 18'h001ae, 18'h00187};
        coef_rom[31] = {18'h3fe9a, 18'h3fe43, 18'h3fe2f, 18'h3fe5f};
        coef_rom[32] = {18'h0018f, 18'h001e7, 18'h001f6, 18'h001bb};
        coef_rom[33] = {18'h3fe44, 18'h3fdec, 18'h3fde3, 18'h3fe2a};
        coef_rom[34] = {18'h001ec, 18'h00245, 18'h00246, 18'h001f1};
        coef_rom[35] = {18'h3fde0, 18'h3fd88, 18'h3fd8f, 18'h3fdf2};
        coef_rom[36] = {18'h00258, 18'h002af, 18'h0029e, 18'h0022b};
        coef_rom[37] = {18'h3fd6b, 18'h3fd17, 18'h3fd32, 18'h3fdb7};
        coef_rom[38] = {18'h002d7, 18'h00328, 18'h00300, 18'h00268};
        coef_rom[39] = {18'h3fce1, 18'h3fc94, 18'h3fcca, 18'h3fd79};
        coef_rom[40] = {18'h0036d, 18'h003b5, 18'h0036e, 18'h002a8};
        coef_rom[41] = {18'h3fc3e, 18'h3fbfd, 18'h3fc55, 18'h3fd37};
        coef_rom[42] = {18'h0041f, 18'h00458, 18'h003eb, 18'h002ec};
        coef_rom[43] = {18'h3fb7b, 18'h3fb4c, 18'h3fbd0, 18'h3fcf0};
        coef_rom[44] = {18'h004f6, 18'h00519, 18'h0047a, 18'h00335};
        coef_rom[45] = {18'h3fa8d, 18'h3fa78, 18'h3fb35, 18'h3fca4};
        coef_rom[46] = {18'h00600, 18'h00603, 18'h00523, 18'h00385};
        coef_rom[47] = {18'h3f963, 18'h3f974, 18'h3fa7c, 18'h3fc4f};
        coef_rom[48] = {18'h00751, 18'h00727, 18'h005f0, 18'h003e0};
        coef_rom[49] = {18'h3f7e0, 18'h3f827, 18'h3f995, 18'h3fbec};
        coef_rom[50] = {18'h00911, 18'h008a6, 18'h006f7, 18'h0044d};
        coef_rom[51] = {18'h3f5d0, 18'h3f668, 18'h3f865, 18'h3fb73};
        coef_rom[52] = {18'h00b8c, 18'h00abc, 18'h0085e, 18'h004d7};
        coef_rom[53] = {18'h3f2c3, 18'h3f3dc, 18'h3f6b4, 18'h3fad1};
        coef_rom[54] = {18'h00f6a, 18'h00def, 18'h00a79, 18'h0059b};
        coef_rom[55] = {18'h3edac, 18'h3efad, 18'h3f3fb, 18'h3f9da};
        coef_rom[56] = {18'h01679, 18'h013ae, 18'h00e2c, 18'h006e3};
        coef_rom[57] = {18'h3e31e, 18'h3e733, 18'h3ee97, 18'h3f806};
        coef_rom[58] = {18'h02833, 18'h021a7, 18'h016e3, 18'h009c6};
        coef_rom[59] = {18'h3be24, 18'h3cb20, 18'h3ddad, 18'h3f29f};
        coef_rom[60] = {18'h0b656, 18'h07fb5, 18'h049bc, 18'h0189c};
        coef_rom[61] = 72'd0;
        coef_rom[62] = 72'd0;
        coef_rom[63] = 72'd0;
        coef_rom[64] = {18'h3fffe, 18'h3fff9, 18'h3fff0, 18'h3ffe5};
        coef_rom[65] = {18'h3ff86, 18'h3ff72, 18'h3ff61, 18'h3ff54};
        coef_rom[66] = {18'h3ff83, 18'h3ffa2, 18'h3ffc5, 18'h3ffe9};
        coef_rom[67] = {18'h00056, 18'h00051, 18'h00044, 18'h00030};
        coef_rom[68] = {18'h3ffc3, 18'h3ffbd, 18'h3ffc1, 18'h3ffcc};
        coef_rom[69] = {18'h00030, 18'h0003a, 18'h0003b, 18'h00034};
        coef_rom[70] = {18'h3ffd6, 18'h3ffca, 18'h3ffc6, 18'h3ffca};
        coef_rom[71] = {18'h00027, 18'h00035, 18'h0003b, 18'h00039};
        coef_rom[72] = {18'h3ffdb, 18'h3ffca, 18'h3ffc2, 18'h3ffc2};
        coef_rom[73] = {18'h00025, 18'h00037, 18'h00042, 18'h00044};
        coef_rom[74] = {18'h3ffdc, 18'h3ffc6, 18'h3ffb9, 18'h3ffb5};
        coef_rom[75] = {18'h00023, 18'h0003c, 18'h0004d, 18'h00053};
        coef_rom[76] = {18'h3ffde, 18'h3ffc1, 18'h3ffad, 18'h3ffa5};
        coef_rom[77] = {18'h00021, 18'h00041, 18'h00059, 18'h00064};
        coef_rom[78] = {18'h3ffe2, 18'h3ffbc, 18'h3ffa0, 18'h3ff92};
        coef_rom[79] = {18'h0001b, 18'h00045, 18'h00066, 18'h00079};
        coef_rom[80] = {18'h3ffe9, 18'h3ffb9, 18'h3ff93, 18'h3ff7d};
        coef_rom[81] = {18'h00012, 18'h00047, 18'h00073, 18'h0008f};
        coef_rom[82] = {18'h3fff4, 18'h3ffb8, 18'h3ff86, 18'h3ff66};
        coef_rom[83] = {18'h00005, 18'h00047, 18'h00080, 18'h000a6};
        coef_rom[84] = {18'h00003, 18'h3ffba, 18'h3ff7b, 18'h3ff4d};
        coef_rom[85] = {18'h3fff3, 18'h00043, 18'h0008b, 18'h000bf};
        coef_rom[86] = {18'h00018, 18'h3ffc0, 18'h3ff70, 18'h3ff34};
        coef_rom[87] = {18'h3ffdb, 18'h0003b, 18'h00094, 18'h000d9};
        coef_rom[88] = {18'h00034, 18'h3ffcb, 18'h3ff68, 18'h3ff1a};
        coef_rom[89] = {18'h3ffbc, 18'h0002e, 18'h0009b, 18'h000f3};
        coef_rom[90] = {18'h00057, 18'h3ffdb, 18'h3ff63, 18'h3feff};
        coef_rom[91] = {18'h3ff95, 18'h0001a, 18'h0009e, 18'h0010e};
        coef_rom[92] = {18'h00082, 18'h3fff2, 18'h3ff61, 18'h3fee6};
        coef_rom[93] = {18'h3ff64, 18'h3ffff, 18'h0009d, 18'h00127};
        coef_rom[94] = {18'h000b8, 18'h00011, 18'h3ff65, 18'h3fecd};
        coef_rom[95] = {18'h3ff29, 18'h3ffdc, 18'h00097, 18'h0013f};
        coef_rom[96] = {18'h000f9, 18'h0003a, 18'h3ff6f, 18'h3feb6};
        coef_rom[97] = {18'h3fee2, 18'h3ffad, 18'h00089, 18'h00155};
        coef_rom[98] = {18'h00147, 18'h0006e, 18'h3ff80, 18'h3fea1};
        coef_rom[99] = {18'h3fe8c, 18'h3ff73, 18'h00073, 18'h00168};
        coef_rom[100] = {18'h001a6, 18'h000b0, 18'h3ff9b, 18'h3fe90};
        coef_rom[101] = {18'h3fe24, 18'h3ff29, 18'h00053, 18'h00177};
        coef_rom[102] = {18'h00217, 18'h00102, 18'h3ffc1, 18'h3fe84};
        coef_rom[103] = {18'h3fda8, 18'h3fecd, 18'h00026, 18'h00180};
        coef_rom[104] = {18'h002a0, 18'h00169, 18'h3fff6, 18'h3fe7d};
        coef_rom[105] = {18'h3fd11, 18'h3fe5a, 18'h3ffe9, 18'h00183};
        coef_rom[106] = {18'h00346, 18'h001ea, 18'h0003d, 18'h3fe7e};
        coef_rom[107] = {18'h3fc59, 18'h3fdc9, 18'h3ff96, 18'h0017d};
        coef_rom[108] = {18'h00413, 18'h0028d, 18'h0009d, 18'h3fe8a};
        coef_rom[109] = {18'h3fb74, 18'h3fd11, 18'h3ff28, 18'h0016c};
        coef_rom[110] = {18'h00514, 18'h0035f, 18'h0011d, 18'h3fea3};
        coef_rom[111] = {18'h3fa51, 18'h3fc22, 18'h3fe93, 18'h0014a};
        coef_rom[112] = {18'h00660, 18'h00472, 18'h001cc, 18'h3fecf};
        coef_rom[113] = {18'h3f8d2, 18'h3fae1, 18'h3fdc4, 18'h00112};
        coef_rom[114] = {18'h00820, 18'h005ec, 18'h002c3, 18'h3ff17};
        coef_rom[115] = {18'h3f6bf, 18'h3f91d, 18'h3fc99, 18'h000b5};
        coef_rom[116] = {18'h00aa3, 18'h00813, 18'h00433, 18'h3ff8e};
        coef_rom[117] = {18'h3f3a3, 18'h3f66e, 18'h3fac9, 18'h00019};
        coef_rom[118] = {18'h00e9a, 18'h00b86, 18'h0068e, 18'h0005e};
        coef_rom[119] = {18'h3ee60, 18'h3f1d0, 18'h3f799, 18'h3fef9};
        coef_rom[120] = {18'h015f9, 18'h0120e, 18'h00b1d, 18'h00205};
        coef_rom[121] = {18'h3e337, 18'h3e7cb, 18'h3f07f, 18'h3fc57};
        coef_rom[122] = {18'h0291a, 18'h0239e, 18'h017de, 18'h006e5};
        coef_rom[123] = {18'h3b99a, 18'h3bfba, 18'h3d1c6, 18'h3efcf};
        coef_rom[124] = {18'h0e922, 18'h113c2, 18'h1327a, 18'h1428f};
        coef_rom[125] = 72'd0;
        coef_rom[126] = 72'd0;
        coef_rom[127] = 72'd0;
    end
endmodule
