// SPDX-License-Identifier: GPL-2.0-or-later
//
// Decimate-by-8 FIR filter, symmetric (folded): a drop-in for the
// Hermes-Lite 2 core's firX8R8 with the same ports and the same 976
// coefficients (the core's coefL4*.mif files, by James Ahlstrom N2ADR and
// Phil Harman VK6APH). Design A of docs/core-rework.md, section 9. Not in
// the images: it is the reference the images' filter (firsharedfold.sv,
// the same sums for all receivers at once) is simulated against.
//
// For each group of eight input samples g, with t0 = 8g + 3 the index of
// the group's fourth input (as the original), it computes
//
//   y[g] = sum over n = 0..487 of round(h[n] * (x[t0 - n] + x[t0 - 975 + n]))
//
// summed in 24 bits, where round(p) is p[35:12] + p[11], as the original
// rounds each of its 976 products. The coefficients are symmetric
// (h[n] = h[975 - n]), so the sum is the original's filter with half the
// multiplies; it differs from the original's output in the lowest bits,
// because each product rounded here is the sum of two the original
// rounds separately.
//
// Two multiply units for I and two for Q: unit 0 takes the even n, unit 1
// the odd. The last 1024 samples are kept in two memories, even and odd
// sample indices, each with two read ports, so the four samples of a
// cycle come from the two memories two at a time. A new sample is written
// through port A, and the computation stalls for that cycle.
//
// Pipeline (153.6 MHz): addresses; memory reads and the coefficient pair;
// the memory outputs registered again (the memory blocks' output
// registers: a memory block's output with no register takes about half of
// the clock period alone); the samples chosen for each unit by t0's parity;
// their sums; the sums and coefficients in the multipliers' input
// registers (an adder feeding a multiplier block directly leaves too
// little of the cycle for the route into it); the products in their output registers;
// rounded; accumulated (the rounding and the accumulation are each a
// 24-bit carry chain, too much for one cycle together). The registers after the reads run freely, with
// no enable and no reset, so they can sit in the memory and multiplier
// blocks.
//
// The output strobe comes when the sum is done: about 1.7 us after the
// group's fourth input, which is up to 0.35 us later than the original's
// strobe (at 384 ksps; less at lower rates).
module firX8R8 (
    input  logic                    rst_all,
    input  logic                    clock,      // 76.8 MHz
    input  logic                    clock_2x,   // 153.6 MHz, edges aligned
    input  logic                    x_avail,
    input  logic signed [MBITS-1:0] x_real,
    input  logic signed [MBITS-1:0] x_imag,
    output logic                    y_avail = 1'b0,
    output logic signed [OBITS-1:0] y_real = '0,
    output logic signed [OBITS-1:0] y_imag = '0
);
    localparam int MBITS = 18;
    parameter int TAPS = NTAPS / 8, ABITS = 24, OBITS = 24, NTAPS = 976;  // as the original's
    localparam int PAIRS = 244;   // coefficient pairs: (h[2k], h[2k+1]), k = 0..243

    // ------------------------------------------------------------------
    // Input: one write per input sample, on the first 153.6 MHz edge of
    // x_avail (as the original's engines take it).
    logic        xa_q = 1'b0;
    logic        wr;
    logic [9:0]  t = '0;          // index of the next sample, from reset
    assign wr = x_avail && !xa_q && !rst_all;

    // Sample memories, 512 x 36 each: {I, Q}. mem_e holds even indices,
    // mem_o odd; index i is at address i[9:1].
    logic [35:0] mem_e [0:511];
    logic [35:0] mem_o [0:511];
`ifdef FIR_FOLD_SIM
    // The FPGA's memory blocks power up at zero; a simulator needs telling.
    initial for (int i = 0; i < 512; i++) begin
        mem_e[i] = '0;
        mem_o[i] = '0;
    end
`endif
    logic [8:0]  ea_addr, eb_addr, oa_addr, ob_addr;
    logic [35:0] ea_q = '0, eb_q = '0, oa_q = '0, ob_q = '0;
    logic        e_we, o_we;
    assign e_we = wr && !t[0];
    assign o_we = wr && t[0];
    // Port A writes or reads (its read on a write cycle is never used);
    // port B reads.
    always_ff @(posedge clock_2x) begin
        if (e_we) mem_e[ea_addr] <= {x_real, x_imag};
        else ea_q <= mem_e[ea_addr];
    end
    always_ff @(posedge clock_2x) eb_q <= mem_e[eb_addr];
    always_ff @(posedge clock_2x) begin
        if (o_we) mem_o[oa_addr] <= {x_real, x_imag};
        else oa_q <= mem_o[oa_addr];
    end
    always_ff @(posedge clock_2x) ob_q <= mem_o[ob_addr];
    // The memory blocks' output registers.
    logic [35:0] ea_q2 = '0, eb_q2 = '0, oa_q2 = '0, ob_q2 = '0;
    always_ff @(posedge clock_2x) begin
        ea_q2 <= ea_q;
        eb_q2 <= eb_q;
        oa_q2 <= oa_q;
        ob_q2 <= ob_q;
    end

    // Coefficient pairs: {h[2k], h[2k+1]}, read with one clock latency.
    logic [7:0]  k = '0;
    logic [35:0] coef_q = '0, coef_q2 = '0;
    always_ff @(posedge clock_2x) begin
        coef_q  <= coef_rom[k];
        coef_q2 <= coef_q;
    end

    // ------------------------------------------------------------------
    // The computation for one t0.
    logic        busy = 1'b0, par = 1'b0;     // par: t0's parity
    logic [9:0]  n0 = '0, o0 = '0;            // x[t0 - 2k], x[t0 - 975 + 2k]
    logic        issue, last_issue;
    assign issue      = busy && !wr;
    assign last_issue = issue && k == 8'(PAIRS - 1);

    // Unit 0 reads x[n0] and x[o0], unit 1 x[n0 - 1] and x[o0 + 1]. n0 and
    // o0 + 1 have t0's parity, o0 and n0 - 1 the other.
    logic [9:0]  n1, o1;
    assign n1 = n0 - 10'd1;
    assign o1 = o0 + 10'd1;
    always_comb begin
        if (wr) begin
            ea_addr = t[9:1];
            oa_addr = t[9:1];
        end else if (!par) begin
            ea_addr = n0[9:1];
            oa_addr = n1[9:1];
        end else begin
            ea_addr = n1[9:1];
            oa_addr = n0[9:1];
        end
        if (!par) begin
            eb_addr = o1[9:1];
            ob_addr = o0[9:1];
        end else begin
            eb_addr = o0[9:1];
            ob_addr = o1[9:1];
        end
    end

    always_ff @(posedge clock_2x) begin
        xa_q <= x_avail;
        if (rst_all) begin
            t    <= '0;
            busy <= 1'b0;
            k    <= '0;
        end else begin
            if (wr) begin
                t <= t + 10'd1;
                // The group's fourth input: start the sum ending with it.
                if (t[2:0] == 3'd3) begin
                    busy <= 1'b1;
                    par  <= t[0];
                    n0   <= t;
                    o0   <= t - 10'd975;
                    k    <= '0;
                end
            end else if (issue) begin
                n0 <= n0 - 10'd2;
                o0 <= o0 + 10'd2;
                k  <= k + 8'd1;
                if (last_issue) busy <= 1'b0;
            end
        end
    end

    // Valid and last flags beside the data: read, output register,
    // operands, products.
    logic [6:0] v = '0, l = '0;
    logic       first = 1'b0;   // the next accumulation is a sum's first
    always_ff @(posedge clock_2x) begin
        v <= {v[5:0], issue};
        l <= {l[5:0], last_issue};
    end

    // Memory outputs, routed to the units by t0's parity, then added.
    logic [35:0] new0 = '0, old0 = '0, new1 = '0, old1 = '0;
    always_ff @(posedge clock_2x) begin
        if (!par) begin
            new0 <= ea_q2; old1 <= eb_q2; new1 <= oa_q2; old0 <= ob_q2;
        end else begin
            new0 <= oa_q2; old1 <= ob_q2; new1 <= ea_q2; old0 <= eb_q2;
        end
    end
    logic signed [17:0] s0r = '0, s0i = '0, s1r = '0, s1i = '0;
    logic [35:0] coef_q3 = '0, coef_q4 = '0;
    always_ff @(posedge clock_2x) begin
        s0r <= $signed(new0[35:18]) + $signed(old0[35:18]);
        s0i <= $signed(new0[17:0])  + $signed(old0[17:0]);
        s1r <= $signed(new1[35:18]) + $signed(old1[35:18]);
        s1i <= $signed(new1[17:0])  + $signed(old1[17:0]);
        coef_q3 <= coef_q2;
        coef_q4 <= coef_q3;
    end

    // The multipliers' operand registers: pre-added samples (17 bits of
    // 16-bit samples, carried in 18) and the coefficient, once for each
    // multiplier.
    logic signed [17:0] a0r = '0, a0i = '0, a1r = '0, a1i = '0;
    logic signed [17:0] b0r = '0, b0i = '0, b1r = '0, b1i = '0;
    logic signed [35:0] p0r = '0, p0i = '0, p1r = '0, p1i = '0;
    always_ff @(posedge clock_2x) begin
        a0r <= s0r;
        a0i <= s0i;
        a1r <= s1r;
        a1i <= s1i;
        b0r <= coef_q4[35:18];
        b0i <= coef_q4[35:18];
        b1r <= coef_q4[17:0];
        b1i <= coef_q4[17:0];
        p0r <= a0r * b0r;
        p0i <= a0i * b0i;
        p1r <= a1r * b1r;
        p1i <= a1i * b1i;
    end

    // Rounded and accumulated in 24 bits, as the original.
    function automatic logic [23:0] rnd(input logic signed [35:0] p);
        return p[35:12] + {23'd0, p[11]};
    endfunction
    logic [23:0] r0r = '0, r0i = '0, r1r = '0, r1i = '0;   // rounded products
    always_ff @(posedge clock_2x) begin
        r0r <= rnd(p0r);
        r0i <= rnd(p0i);
        r1r <= rnd(p1r);
        r1i <= rnd(p1i);
    end
    logic [23:0] acc0r = '0, acc0i = '0, acc1r = '0, acc1i = '0;
    logic [23:0] res_r = '0, res_i = '0;
    logic        res_t = 1'b0;   // toggles when res_r, res_i are new
    logic        done_q = 1'b0;
    always_ff @(posedge clock_2x) begin
        if (rst_all) begin
            first  <= 1'b1;
            done_q <= 1'b0;
        end else begin
            done_q <= 1'b0;
            if (v[6]) begin
                acc0r <= (first ? 24'd0 : acc0r) + r0r;
                acc0i <= (first ? 24'd0 : acc0i) + r0i;
                acc1r <= (first ? 24'd0 : acc1r) + r1r;
                acc1i <= (first ? 24'd0 : acc1i) + r1i;
                first <= l[6];
                done_q <= l[6];
            end
            if (done_q) begin
                res_r <= acc0r + acc1r;
                res_i <= acc0i + acc1i;
                res_t <= ~res_t;
            end
        end
    end

    // Output in the 76.8 MHz domain: one strobe for each new sum.
    logic res_t_s = 1'b0;
    always_ff @(posedge clock) begin
        res_t_s <= res_t;
        y_avail <= res_t != res_t_s;
        if (res_t != res_t_s) begin
            y_real <= res_r;
            y_imag <= res_i;
        end
    end

    // The 976 coefficients as 244 pairs {h[2k], h[2k+1]} (18-bit two's
    // complement each), from the core's coefL4AE/BF/CG/DH.mif files: AE
    // holds the taps n = 7 and 3 (mod 8), BF 6 and 2, CG 5 and 1, DH 4 and 0.
    // An initialised array read whole into a register, so that a tool
    // makes it one ROM; 256 words, as k is 8 bits.
    logic [35:0] coef_rom [0:255];
    initial begin
        coef_rom[0] = {18'h00004, 18'h00003};  // h[0] = 4, h[1] = 3
        coef_rom[1] = {18'h00003, 18'h00001};  // h[2] = 3, h[3] = 1
        coef_rom[2] = {18'h3fffe, 18'h3fff9};  // h[4] = -2, h[5] = -7
        coef_rom[3] = {18'h3fff0, 18'h3ffe5};  // h[6] = -16, h[7] = -27
        coef_rom[4] = {18'h3ffd7, 18'h3ffc5};  // h[8] = -41, h[9] = -59
        coef_rom[5] = {18'h3ffb1, 18'h3ff9c};  // h[10] = -79, h[11] = -100
        coef_rom[6] = {18'h3ff86, 18'h3ff72};  // h[12] = -122, h[13] = -142
        coef_rom[7] = {18'h3ff61, 18'h3ff54};  // h[14] = -159, h[15] = -172
        coef_rom[8] = {18'h3ff4e, 18'h3ff4f};  // h[16] = -178, h[17] = -177
        coef_rom[9] = {18'h3ff59, 18'h3ff6a};  // h[18] = -167, h[19] = -150
        coef_rom[10] = {18'h3ff83, 18'h3ffa2};  // h[20] = -125, h[21] = -94
        coef_rom[11] = {18'h3ffc5, 18'h3ffe9};  // h[22] = -59, h[23] = -23
        coef_rom[12] = {18'h0000b, 18'h0002a};  // h[24] = 11, h[25] = 42
        coef_rom[13] = {18'h00041, 18'h00050};  // h[26] = 65, h[27] = 80
        coef_rom[14] = {18'h00056, 18'h00051};  // h[28] = 86, h[29] = 81
        coef_rom[15] = {18'h00044, 18'h00030};  // h[30] = 68, h[31] = 48
        coef_rom[16] = {18'h00017, 18'h3fffd};  // h[32] = 23, h[33] = -3
        coef_rom[17] = {18'h3ffe4, 18'h3ffd0};  // h[34] = -28, h[35] = -48
        coef_rom[18] = {18'h3ffc3, 18'h3ffbd};  // h[36] = -61, h[37] = -67
        coef_rom[19] = {18'h3ffc1, 18'h3ffcc};  // h[38] = -63, h[39] = -52
        coef_rom[20] = {18'h3ffde, 18'h3fff4};  // h[40] = -34, h[41] = -12
        coef_rom[21] = {18'h0000b, 18'h00020};  // h[42] = 11, h[43] = 32
        coef_rom[22] = {18'h00030, 18'h0003a};  // h[44] = 48, h[45] = 58
        coef_rom[23] = {18'h0003b, 18'h00034};  // h[46] = 59, h[47] = 52
        coef_rom[24] = {18'h00026, 18'h00013};  // h[48] = 38, h[49] = 19
        coef_rom[25] = {18'h3fffd, 18'h3ffe8};  // h[50] = -3, h[51] = -24
        coef_rom[26] = {18'h3ffd6, 18'h3ffca};  // h[52] = -42, h[53] = -54
        coef_rom[27] = {18'h3ffc6, 18'h3ffca};  // h[54] = -58, h[55] = -54
        coef_rom[28] = {18'h3ffd6, 18'h3ffe8};  // h[56] = -42, h[57] = -24
        coef_rom[29] = {18'h3fffd, 18'h00013};  // h[58] = -3, h[59] = 19
        coef_rom[30] = {18'h00027, 18'h00035};  // h[60] = 39, h[61] = 53
        coef_rom[31] = {18'h0003b, 18'h00039};  // h[62] = 59, h[63] = 57
        coef_rom[32] = {18'h0002f, 18'h0001d};  // h[64] = 47, h[65] = 29
        coef_rom[33] = {18'h00007, 18'h3fff0};  // h[66] = 7, h[67] = -16
        coef_rom[34] = {18'h3ffdb, 18'h3ffca};  // h[68] = -37, h[69] = -54
        coef_rom[35] = {18'h3ffc2, 18'h3ffc2};  // h[70] = -62, h[71] = -62
        coef_rom[36] = {18'h3ffcb, 18'h3ffdd};  // h[72] = -53, h[73] = -35
        coef_rom[37] = {18'h3fff4, 18'h0000d};  // h[74] = -12, h[75] = 13
        coef_rom[38] = {18'h00025, 18'h00037};  // h[76] = 37, h[77] = 55
        coef_rom[39] = {18'h00042, 18'h00044};  // h[78] = 66, h[79] = 68
        coef_rom[40] = {18'h0003b, 18'h0002a};  // h[80] = 59, h[81] = 42
        coef_rom[41] = {18'h00012, 18'h3fff6};  // h[82] = 18, h[83] = -10
        coef_rom[42] = {18'h3ffdc, 18'h3ffc6};  // h[84] = -36, h[85] = -58
        coef_rom[43] = {18'h3ffb9, 18'h3ffb5};  // h[86] = -71, h[87] = -75
        coef_rom[44] = {18'h3ffbd, 18'h3ffcf};  // h[88] = -67, h[89] = -49
        coef_rom[45] = {18'h3ffe8, 18'h00006};  // h[90] = -24, h[91] = 6
        coef_rom[46] = {18'h00023, 18'h0003c};  // h[92] = 35, h[93] = 60
        coef_rom[47] = {18'h0004d, 18'h00053};  // h[94] = 77, h[95] = 83
        coef_rom[48] = {18'h0004c, 18'h0003a};  // h[96] = 76, h[97] = 58
        coef_rom[49] = {18'h0001f, 18'h3ffff};  // h[98] = 31, h[99] = -1
        coef_rom[50] = {18'h3ffde, 18'h3ffc1};  // h[100] = -34, h[101] = -63
        coef_rom[51] = {18'h3ffad, 18'h3ffa5};  // h[102] = -83, h[103] = -91
        coef_rom[52] = {18'h3ffaa, 18'h3ffbc};  // h[104] = -86, h[105] = -68
        coef_rom[53] = {18'h3ffd9, 18'h3fffc};  // h[106] = -39, h[107] = -4
        coef_rom[54] = {18'h00021, 18'h00041};  // h[108] = 33, h[109] = 65
        coef_rom[55] = {18'h00059, 18'h00064};  // h[110] = 89, h[111] = 100
        coef_rom[56] = {18'h00061, 18'h0004f};  // h[112] = 97, h[113] = 79
        coef_rom[57] = {18'h00031, 18'h0000a};  // h[114] = 49, h[115] = 10
        coef_rom[58] = {18'h3ffe2, 18'h3ffbc};  // h[116] = -30, h[117] = -68
        coef_rom[59] = {18'h3ffa0, 18'h3ff92};  // h[118] = -96, h[119] = -110
        coef_rom[60] = {18'h3ff93, 18'h3ffa5};  // h[120] = -109, h[121] = -91
        coef_rom[61] = {18'h3ffc5, 18'h3ffee};  // h[122] = -59, h[123] = -18
        coef_rom[62] = {18'h0001b, 18'h00045};  // h[124] = 27, h[125] = 69
        coef_rom[63] = {18'h00066, 18'h00079};  // h[126] = 102, h[127] = 121
        coef_rom[64] = {18'h00079, 18'h00068};  // h[128] = 121, h[129] = 104
        coef_rom[65] = {18'h00047, 18'h0001a};  // h[130] = 71, h[131] = 26
        coef_rom[66] = {18'h3ffe9, 18'h3ffb9};  // h[132] = -23, h[133] = -71
        coef_rom[67] = {18'h3ff93, 18'h3ff7d};  // h[134] = -109, h[135] = -131
        coef_rom[68] = {18'h3ff79, 18'h3ff89};  // h[136] = -135, h[137] = -119
        coef_rom[69] = {18'h3ffac, 18'h3ffdc};  // h[138] = -84, h[139] = -36
        coef_rom[70] = {18'h00012, 18'h00047};  // h[140] = 18, h[141] = 71
        coef_rom[71] = {18'h00073, 18'h0008f};  // h[142] = 115, h[143] = 143
        coef_rom[72] = {18'h00096, 18'h00086};  // h[144] = 150, h[145] = 134
        coef_rom[73] = {18'h00063, 18'h0002f};  // h[146] = 99, h[147] = 47
        coef_rom[74] = {18'h3fff4, 18'h3ffb8};  // h[148] = -12, h[149] = -72
        coef_rom[75] = {18'h3ff86, 18'h3ff66};  // h[150] = -122, h[151] = -154
        coef_rom[76] = {18'h3ff5b, 18'h3ff69};  // h[152] = -165, h[153] = -151
        coef_rom[77] = {18'h3ff8e, 18'h3ffc4};  // h[154] = -114, h[155] = -60
        coef_rom[78] = {18'h00005, 18'h00047};  // h[156] = 5, h[157] = 71
        coef_rom[79] = {18'h00080, 18'h000a6};  // h[158] = 128, h[159] = 166
        coef_rom[80] = {18'h000b5, 18'h000a9};  // h[160] = 181, h[161] = 169
        coef_rom[81] = {18'h00084, 18'h0004a};  // h[162] = 132, h[163] = 74
        coef_rom[82] = {18'h00003, 18'h3ffba};  // h[164] = 3, h[165] = -70
        coef_rom[83] = {18'h3ff7b, 18'h3ff4d};  // h[166] = -133, h[167] = -179
        coef_rom[84] = {18'h3ff39, 18'h3ff43};  // h[168] = -199, h[169] = -189
        coef_rom[85] = {18'h3ff69, 18'h3ffa7};  // h[170] = -151, h[171] = -89
        coef_rom[86] = {18'h3fff3, 18'h00043};  // h[172] = -13, h[173] = 67
        coef_rom[87] = {18'h0008b, 18'h000bf};  // h[174] = 139, h[175] = 191
        coef_rom[88] = {18'h000d9, 18'h000d2};  // h[176] = 217, h[177] = 210
        coef_rom[89] = {18'h000ab, 18'h0006b};  // h[178] = 171, h[179] = 107
        coef_rom[90] = {18'h00018, 18'h3ffc0};  // h[180] = 24, h[181] = -64
        coef_rom[91] = {18'h3ff70, 18'h3ff34};  // h[182] = -144, h[183] = -204
        coef_rom[92] = {18'h3ff15, 18'h3ff18};  // h[184] = -235, h[185] = -232
        coef_rom[93] = {18'h3ff3e, 18'h3ff82};  // h[186] = -194, h[187] = -126
        coef_rom[94] = {18'h3ffdb, 18'h0003b};  // h[188] = -37, h[189] = 59
        coef_rom[95] = {18'h00094, 18'h000d9};  // h[190] = 148, h[191] = 217
        coef_rom[96] = {18'h000ff, 18'h00100};  // h[192] = 255, h[193] = 256
        coef_rom[97] = {18'h000da, 18'h00093};  // h[194] = 218, h[195] = 147
        coef_rom[98] = {18'h00034, 18'h3ffcb};  // h[196] = 52, h[197] = -53
        coef_rom[99] = {18'h3ff68, 18'h3ff1a};  // h[198] = -152, h[199] = -230
        coef_rom[100] = {18'h3feec, 18'h3fee7};  // h[200] = -276, h[201] = -281
        coef_rom[101] = {18'h3ff0c, 18'h3ff56};  // h[202] = -244, h[203] = -170
        coef_rom[102] = {18'h3ffbc, 18'h0002e};  // h[204] = -68, h[205] = 46
        coef_rom[103] = {18'h0009b, 18'h000f3};  // h[206] = 155, h[207] = 243
        coef_rom[104] = {18'h00129, 18'h00134};  // h[208] = 297, h[209] = 308
        coef_rom[105] = {18'h00110, 18'h000c3};  // h[210] = 272, h[211] = 195
        coef_rom[106] = {18'h00057, 18'h3ffdb};  // h[212] = 87, h[213] = -37
        coef_rom[107] = {18'h3ff63, 18'h3feff};  // h[214] = -157, h[215] = -257
        coef_rom[108] = {18'h3fec0, 18'h3feb0};  // h[216] = -320, h[217] = -336
        coef_rom[109] = {18'h3fed2, 18'h3ff22};  // h[218] = -302, h[219] = -222
        coef_rom[110] = {18'h3ff95, 18'h0001a};  // h[220] = -107, h[221] = 26
        coef_rom[111] = {18'h0009e, 18'h0010e};  // h[222] = 158, h[223] = 270
        coef_rom[112] = {18'h00157, 18'h0016e};  // h[224] = 343, h[225] = 366
        coef_rom[113] = {18'h0014e, 18'h000fc};  // h[226] = 334, h[227] = 252
        coef_rom[114] = {18'h00082, 18'h3fff2};  // h[228] = 130, h[229] = -14
        coef_rom[115] = {18'h3ff61, 18'h3fee6};  // h[230] = -159, h[231] = -282
        coef_rom[116] = {18'h3fe92, 18'h3fe73};  // h[232] = -366, h[233] = -397
        coef_rom[117] = {18'h3fe8f, 18'h3fee4};  // h[234] = -369, h[235] = -284
        coef_rom[118] = {18'h3ff64, 18'h3ffff};  // h[236] = -156, h[237] = -1
        coef_rom[119] = {18'h0009d, 18'h00127};  // h[238] = 157, h[239] = 295
        coef_rom[120] = {18'h00187, 18'h001ae};  // h[240] = 391, h[241] = 430
        coef_rom[121] = {18'h00196, 18'h00140};  // h[242] = 406, h[243] = 320
        coef_rom[122] = {18'h000b8, 18'h00011};  // h[244] = 184, h[245] = 17
        coef_rom[123] = {18'h3ff65, 18'h3fecd};  // h[246] = -155, h[247] = -307
        coef_rom[124] = {18'h3fe5f, 18'h3fe2f};  // h[248] = -417, h[249] = -465
        coef_rom[125] = {18'h3fe43, 18'h3fe9a};  // h[250] = -445, h[251] = -358
        coef_rom[126] = {18'h3ff29, 18'h3ffdc};  // h[252] = -215, h[253] = -36
        coef_rom[127] = {18'h00097, 18'h0013f};  // h[254] = 151, h[255] = 319
        coef_rom[128] = {18'h001bb, 18'h001f6};  // h[256] = 443, h[257] = 502
        coef_rom[129] = {18'h001e7, 18'h0018f};  // h[258] = 487, h[259] = 399
        coef_rom[130] = {18'h000f9, 18'h0003a};  // h[260] = 249, h[261] = 58
        coef_rom[131] = {18'h3ff6f, 18'h3feb6};  // h[262] = -145, h[263] = -330
        coef_rom[132] = {18'h3fe2a, 18'h3fde3};  // h[264] = -470, h[265] = -541
        coef_rom[133] = {18'h3fdec, 18'h3fe44};  // h[266] = -532, h[267] = -444
        coef_rom[134] = {18'h3fee2, 18'h3ffad};  // h[268] = -286, h[269] = -83
        coef_rom[135] = {18'h00089, 18'h00155};  // h[270] = 137, h[271] = 341
        coef_rom[136] = {18'h001f1, 18'h00246};  // h[272] = 497, h[273] = 582
        coef_rom[137] = {18'h00245, 18'h001ec};  // h[274] = 581, h[275] = 492
        coef_rom[138] = {18'h00147, 18'h0006e};  // h[276] = 327, h[277] = 110
        coef_rom[139] = {18'h3ff80, 18'h3fea1};  // h[278] = -128, h[279] = -351
        coef_rom[140] = {18'h3fdf2, 18'h3fd8f};  // h[280] = -526, h[281] = -625
        coef_rom[141] = {18'h3fd88, 18'h3fde0};  // h[282] = -632, h[283] = -544
        coef_rom[142] = {18'h3fe8c, 18'h3ff73};  // h[284] = -372, h[285] = -141
        coef_rom[143] = {18'h00073, 18'h00168};  // h[286] = 115, h[287] = 360
        coef_rom[144] = {18'h0022b, 18'h0029e};  // h[288] = 555, h[289] = 670
        coef_rom[145] = {18'h002af, 18'h00258};  // h[290] = 687, h[291] = 600
        coef_rom[146] = {18'h001a6, 18'h000b0};  // h[292] = 422, h[293] = 176
        coef_rom[147] = {18'h3ff9b, 18'h3fe90};  // h[294] = -101, h[295] = -368
        coef_rom[148] = {18'h3fdb7, 18'h3fd32};  // h[296] = -585, h[297] = -718
        coef_rom[149] = {18'h3fd17, 18'h3fd6b};  // h[298] = -745, h[299] = -661
        coef_rom[150] = {18'h3fe24, 18'h3ff29};  // h[300] = -476, h[301] = -215
        coef_rom[151] = {18'h00053, 18'h00177};  // h[302] = 83, h[303] = 375
        coef_rom[152] = {18'h00268, 18'h00300};  // h[304] = 616, h[305] = 768
        coef_rom[153] = {18'h00328, 18'h002d7};  // h[306] = 808, h[307] = 727
        coef_rom[154] = {18'h00217, 18'h00102};  // h[308] = 535, h[309] = 258
        coef_rom[155] = {18'h3ffc1, 18'h3fe84};  // h[310] = -63, h[311] = -380
        coef_rom[156] = {18'h3fd79, 18'h3fcca};  // h[312] = -647, h[313] = -822
        coef_rom[157] = {18'h3fc94, 18'h3fce1};  // h[314] = -876, h[315] = -799
        coef_rom[158] = {18'h3fda8, 18'h3fecd};  // h[316] = -600, h[317] = -307
        coef_rom[159] = {18'h00026, 18'h00180};  // h[318] = 38, h[319] = 384
        coef_rom[160] = {18'h002a8, 18'h0036e};  // h[320] = 680, h[321] = 878
        coef_rom[161] = {18'h003b5, 18'h0036d};  // h[322] = 949, h[323] = 877
        coef_rom[162] = {18'h002a0, 18'h00169};  // h[324] = 672, h[325] = 361
        coef_rom[163] = {18'h3fff6, 18'h3fe7d};  // h[326] = -10, h[327] = -387
        coef_rom[164] = {18'h3fd37, 18'h3fc55};  // h[328] = -713, h[329] = -939
        coef_rom[165] = {18'h3fbfd, 18'h3fc3e};  // h[330] = -1027, h[331] = -962
        coef_rom[166] = {18'h3fd11, 18'h3fe5a};  // h[332] = -751, h[333] = -422
        coef_rom[167] = {18'h3ffe9, 18'h00183};  // h[334] = -23, h[335] = 387
        coef_rom[168] = {18'h002ec, 18'h003eb};  // h[336] = 748, h[337] = 1003
        coef_rom[169] = {18'h00458, 18'h0041f};  // h[338] = 1112, h[339] = 1055
        coef_rom[170] = {18'h00346, 18'h001ea};  // h[340] = 838, h[341] = 490
        coef_rom[171] = {18'h0003d, 18'h3fe7e};  // h[342] = 61, h[343] = -386
        coef_rom[172] = {18'h3fcf0, 18'h3fbd0};  // h[344] = -784, h[345] = -1072
        coef_rom[173] = {18'h3fb4c, 18'h3fb7b};  // h[346] = -1204, h[347] = -1157
        coef_rom[174] = {18'h3fc59, 18'h3fdc9};  // h[348] = -935, h[349] = -567
        coef_rom[175] = {18'h3ff96, 18'h0017d};  // h[350] = -106, h[351] = 381
        coef_rom[176] = {18'h00335, 18'h0047a};  // h[352] = 821, h[353] = 1146
        coef_rom[177] = {18'h00519, 18'h004f6};  // h[354] = 1305, h[355] = 1270
        coef_rom[178] = {18'h00413, 18'h0028d};  // h[356] = 1043, h[357] = 653
        coef_rom[179] = {18'h0009d, 18'h3fe8a};  // h[358] = 157, h[359] = -374
        coef_rom[180] = {18'h3fca4, 18'h3fb35};  // h[360] = -860, h[361] = -1227
        coef_rom[181] = {18'h3fa78, 18'h3fa8d};  // h[362] = -1416, h[363] = -1395
        coef_rom[182] = {18'h3fb74, 18'h3fd11};  // h[364] = -1164, h[365] = -751
        coef_rom[183] = {18'h3ff28, 18'h0016c};  // h[366] = -216, h[367] = 364
        coef_rom[184] = {18'h00385, 18'h00523};  // h[368] = 901, h[369] = 1315
        coef_rom[185] = {18'h00603, 18'h00600};  // h[370] = 1539, h[371] = 1536
        coef_rom[186] = {18'h00514, 18'h0035f};  // h[372] = 1300, h[373] = 863
        coef_rom[187] = {18'h0011d, 18'h3fea3};  // h[374] = 285, h[375] = -349
        coef_rom[188] = {18'h3fc4f, 18'h3fa7c};  // h[376] = -945, h[377] = -1412
        coef_rom[189] = {18'h3f974, 18'h3f963};  // h[378] = -1676, h[379] = -1693
        coef_rom[190] = {18'h3fa51, 18'h3fc22};  // h[380] = -1455, h[381] = -990
        coef_rom[191] = {18'h3fe93, 18'h0014a};  // h[382] = -365, h[383] = 330
        coef_rom[192] = {18'h003e0, 18'h005f0};  // h[384] = 992, h[385] = 1520
        coef_rom[193] = {18'h00727, 18'h00751};  // h[386] = 1831, h[387] = 1873
        coef_rom[194] = {18'h00660, 18'h00472};  // h[388] = 1632, h[389] = 1138
        coef_rom[195] = {18'h001cc, 18'h3fecf};  // h[390] = 460, h[391] = -305
        coef_rom[196] = {18'h3fbec, 18'h3f995};  // h[392] = -1044, h[393] = -1643
        coef_rom[197] = {18'h3f827, 18'h3f7e0};  // h[394] = -2009, h[395] = -2080
        coef_rom[198] = {18'h3f8d2, 18'h3fae1};  // h[396] = -1838, h[397] = -1311
        coef_rom[199] = {18'h3fdc4, 18'h00112};  // h[398] = -572, h[399] = 274
        coef_rom[200] = {18'h0044d, 18'h006f7};  // h[400] = 1101, h[401] = 1783
        coef_rom[201] = {18'h008a6, 18'h00911};  // h[402] = 2214, h[403] = 2321
        coef_rom[202] = {18'h00820, 18'h005ec};  // h[404] = 2080, h[405] = 1516
        coef_rom[203] = {18'h002c3, 18'h3ff17};  // h[406] = 707, h[407] = -233
        coef_rom[204] = {18'h3fb73, 18'h3f865};  // h[408] = -1165, h[409] = -1947
        coef_rom[205] = {18'h3f668, 18'h3f5d0};  // h[410] = -2456, h[411] = -2608
        coef_rom[206] = {18'h3f6bf, 18'h3f91d};  // h[412] = -2369, h[413] = -1763
        coef_rom[207] = {18'h3fc99, 18'h000b5};  // h[414] = -871, h[415] = 181
        coef_rom[208] = {18'h004d7, 18'h0085e};  // h[416] = 1239, h[417] = 2142
        coef_rom[209] = {18'h00abc, 18'h00b8c};  // h[418] = 2748, h[419] = 2956
        coef_rom[210] = {18'h00aa3, 18'h00813};  // h[420] = 2723, h[421] = 2067
        coef_rom[211] = {18'h00433, 18'h3ff8e};  // h[422] = 1075, h[423] = -114
        coef_rom[212] = {18'h3fad1, 18'h3f6b4};  // h[424] = -1327, h[425] = -2380
        coef_rom[213] = {18'h3f3dc, 18'h3f2c3};  // h[426] = -3108, h[427] = -3389
        coef_rom[214] = {18'h3f3a3, 18'h3f66e};  // h[428] = -3165, h[429] = -2450
        coef_rom[215] = {18'h3fac9, 18'h00019};  // h[430] = -1335, h[431] = 25
        coef_rom[216] = {18'h0059b, 18'h00a79};  // h[432] = 1435, h[433] = 2681
        coef_rom[217] = {18'h00def, 18'h00f6a};  // h[434] = 3567, h[435] = 3946
        coef_rom[218] = {18'h00e9a, 18'h00b86};  // h[436] = 3738, h[437] = 2950
        coef_rom[219] = {18'h0068e, 18'h0005e};  // h[438] = 1678, h[439] = 94
        coef_rom[220] = {18'h3f9da, 18'h3f3fb};  // h[440] = -1574, h[441] = -3077
        coef_rom[221] = {18'h3efad, 18'h3edac};  // h[442] = -4179, h[443] = -4692
        coef_rom[222] = {18'h3ee60, 18'h3f1d0};  // h[444] = -4512, h[445] = -3632
        coef_rom[223] = {18'h3f799, 18'h3fef9};  // h[446] = -2151, h[447] = -263
        coef_rom[224] = {18'h006e3, 18'h00e2c};  // h[448] = 1763, h[449] = 3628
        coef_rom[225] = {18'h013ae, 18'h01679};  // h[450] = 5038, h[451] = 5753
        coef_rom[226] = {18'h015f9, 18'h0120e};  // h[452] = 5625, h[453] = 4622
        coef_rom[227] = {18'h00b1d, 18'h00205};  // h[454] = 2845, h[455] = 517
        coef_rom[228] = {18'h3f806, 18'h3ee97};  // h[456] = -2042, h[457] = -4457
        coef_rom[229] = {18'h3e733, 18'h3e31e};  // h[458] = -6349, h[459] = -7394
        coef_rom[230] = {18'h3e337, 18'h3e7cb};  // h[460] = -7369, h[461] = -6197
        coef_rom[231] = {18'h3f07f, 18'h3fc57};  // h[462] = -3969, h[463] = -937
        coef_rom[232] = {18'h009c6, 18'h016e3};  // h[464] = 2502, h[465] = 5859
        coef_rom[233] = {18'h021a7, 18'h02833};  // h[466] = 8615, h[467] = 10291
        coef_rom[234] = {18'h0291a, 18'h0239e};  // h[468] = 10522, h[469] = 9118
        coef_rom[235] = {18'h017de, 18'h006e5};  // h[470] = 6110, h[471] = 1765
        coef_rom[236] = {18'h3f29f, 18'h3ddad};  // h[472] = -3425, h[473] = -8787
        coef_rom[237] = {18'h3cb20, 18'h3be24};  // h[474] = -13536, h[475] = -16860
        coef_rom[238] = {18'h3b99a, 18'h3bfba};  // h[476] = -18022, h[477] = -16454
        coef_rom[239] = {18'h3d1c6, 18'h3efcf};  // h[478] = -11834, h[479] = -4145
        coef_rom[240] = {18'h0189c, 18'h049bc};  // h[480] = 6300, h[481] = 18876
        coef_rom[241] = {18'h07fb5, 18'h0b656};  // h[482] = 32693, h[483] = 46678
        coef_rom[242] = {18'h0e922, 18'h113c2};  // h[484] = 59682, h[485] = 70594
        coef_rom[243] = {18'h1327a, 18'h1428f};  // h[486] = 78458, h[487] = 82575
        coef_rom[244] = 36'd0;
        coef_rom[245] = 36'd0;
        coef_rom[246] = 36'd0;
        coef_rom[247] = 36'd0;
        coef_rom[248] = 36'd0;
        coef_rom[249] = 36'd0;
        coef_rom[250] = 36'd0;
        coef_rom[251] = 36'd0;
        coef_rom[252] = 36'd0;
        coef_rom[253] = 36'd0;
        coef_rom[254] = 36'd0;
        coef_rom[255] = 36'd0;
    end
endmodule
