// SPDX-License-Identifier: GPL-2.0-or-later
//
// The variable CIC's combs, decimation control and output rounding for all
// the receivers (rx_cic_int.sv has their integrators): the Hermes-Lite 2
// core's varcic.v in each receiver, with its registers pruned (Hogenauer;
// TODO/2026-11-17-core-takeover.md stage 2b).
//
// varcic.v, per receiver and for I and Q: on every input sample a counter
// counts to the decimation; on the last it raises the output strobe for
// one clock, at whose end each of the five combs takes, from the stage
// before it (the first from the last integrator), the value from before
// that clock: out = in - prev, prev = in. The output register takes the
// last comb's value, rounded to 16 bits, on every clock. rx_cic_int.sv
// shifts the input so that the output is bits 42..27 at every decimation;
// the combs here keep only their top bits: 23, 22, 21, 20 and 20 of 43,
// each taking the stage before's value with its lowest bits dropped
// (truncated: a comb's output has no lasting bias from it, the next comb
// takes the difference).
//
// The combs' state, {prev, out} of each stage for I and Q, is in one
// memory, {receiver, stage}, as 23-bit words from bit 20 (bits 42..20),
// the bits a stage does not keep held at zero; one engine on the 153.6 MHz
// clock runs the combs of every receiver in turn after each output strobe,
// from a copy of the last integrators taken during the strobe: receiver by
// receiver, the stages in order, each stage's old out carried to the next.
// A receiver's last comb value is then held for its output register. Each
// comb takes three pipeline stages after the memory: its input and prev
// registered, the difference registered, then written back. The engine
// takes 5 cycles a receiver (30 for six) plus five, against 50
// between strobes at 384 ksps. The core's filter reads the outputs at the
// strobe, when the original's output register still holds the previous
// strobe's result, as these do.
module rx_varcomb #(
    parameter int NR = 6            // receivers, 1 to 10
) (
    input  logic               clock,       // 76.8 MHz
    input  logic               clock_2x,    // 153.6 MHz, edges aligned
    input  logic               rst_all,
    input  logic [5:0]         decimation,  // 40, 20, 10 or 5 (48 to 384 ksps)
    input  logic               in_strobe,   // the integrators have added a sample
    input  logic [25*NR-1:0]   int_i,       // receiver k's last integrator (bits 42..18)
    input  logic [25*NR-1:0]   int_q,       //   in 25k + 24..25k
    output logic               out_strobe = 1'b0,
    output logic [16*NR-1:0]   out_i,       // receiver k in bits 16k + 15..16k
    output logic [16*NR-1:0]   out_q
);
    localparam int WI = 25;     // the last integrator: bits 42..18
    localparam int WC = 23;     // the comb words: bits 42..20
    localparam int KB = (NR > 8) ? 4 : (NR > 4) ? 3 : (NR > 2) ? 2 : 1;
    localparam int D = NR * 5;
    localparam int AW = $clog2(D);

    // ------------------------------------------------------------------
    // Decimation control, as varcic.v (76.8 MHz).
    logic [15:0] sample_no = 16'd0;
    always_ff @(posedge clock) begin
        if (rst_all) begin
            sample_no  <= 16'd0;
            out_strobe <= 1'b0;
        end else if (in_strobe) begin
            if (sample_no == {10'b0, (decimation - 6'h1)}) begin
                sample_no  <= 16'd0;
                out_strobe <= 1'b1;
            end else begin
                sample_no  <= sample_no + 16'd1;
                out_strobe <= 1'b0;
            end
        end else begin
            out_strobe <= 1'b0;
        end
    end

    // ------------------------------------------------------------------
    // The engine (153.6 MHz). On its first edge in the strobe's clock (the
    // integrators are steady through it) the last integrators are copied,
    // to be shifted down by one receiver as each is started.
    logic            os_q = 1'b0;
    logic            start;
    logic [WI*NR-1:0] snap_i = '0, snap_q = '0;
    logic            busy = 1'b0;
    logic [KB-1:0]   s = '0;
    logic [2:0]      k = '0;
    assign start = out_strobe && !os_q && !busy;

    // Issue: one {receiver, stage} a cycle; the memory answers two cycles
    // later.
    logic [AW-1:0]   raddr;
    assign raddr = AW'(s * 5 + k);
    logic            v1 = 1'b0, v2 = 1'b0;
    logic [AW-1:0]   a1 = '0, a2 = '0;
    logic [KB-1:0]   s1 = '0, s2 = '0;
    logic [2:0]      k1 = '0, k2 = '0;

    logic [4*WC-1:0] q;             // {I prev, I out, Q prev, Q out}
    logic [WC-1:0]   prev_i, old_i, prev_q, old_q;
    assign {prev_i, old_i, prev_q, old_q} = q;

    // The bits stage k keeps: 23, 22, 21, 20, 20 from the top.
    function automatic logic [WC-1:0] keep(input logic [WC-1:0] x, input logic [2:0] st);
        case (st)
            3'd0:    return x;
            3'd1:    return {x[WC-1:1], 1'b0};
            3'd2:    return {x[WC-1:2], 2'b0};
            default: return {x[WC-1:3], 3'b0};
        endcase
    endfunction

    // Three stages: when the memory answers (v2), the stage's input (the
    // copy of the last integrator for the first stage, else the stage
    // before's old out), cut to the stage's bits, and its prev are
    // registered, and its old out kept for the next stage; a cycle later
    // (v3) the difference is registered; a cycle after that (v4) the comb
    // is written back and, for the last, held for the output.
    logic [WC-1:0]   carry_i = '0, carry_q = '0;
    logic [WC-1:0]   in_i = '0, in_q = '0, pv_i = '0, pv_q = '0;
    logic            v3 = 1'b0, v4 = 1'b0;
    logic [AW-1:0]   a3 = '0, a4 = '0;
    logic [KB-1:0]   s3 = '0, s4 = '0;
    logic [2:0]      k3 = '0, k4 = '0;
    logic [WC-1:0]   new_i = '0, new_q = '0, in4_i = '0, in4_q = '0;

    fir_sdpram #(.W(4 * WC), .D(D), .AW(AW)) comb_ram (
        .clk(clock_2x), .we(v4), .waddr(a4), .wdata({in4_i, new_i, in4_q, new_q}),
        .raddr(raddr), .q(q)
    );

    // The last comb of each receiver, bits 42..23 (all it keeps), held for
    // its output register.
    logic [19:0]     last_i [0:NR-1];
    logic [19:0]     last_q [0:NR-1];
    initial for (int r = 0; r < NR; r++) begin
        last_i[r] = '0;
        last_q[r] = '0;
    end

    always_ff @(posedge clock_2x) begin
        os_q <= out_strobe;
        v1 <= busy;
        a1 <= raddr;
        s1 <= s;
        k1 <= k;
        v2 <= v1;
        a2 <= a1;
        s2 <= s1;
        k2 <= k1;
        v3 <= v2;
        a3 <= a2;
        s3 <= s2;
        k3 <= k2;
        v4 <= v3;
        a4 <= a3;
        s4 <= s3;
        k4 <= k3;
        new_i <= in_i - pv_i;
        new_q <= in_q - pv_q;
        in4_i <= in_i;
        in4_q <= in_q;
        if (start) begin
            snap_i <= int_i;
            snap_q <= int_q;
            busy   <= 1'b1;
            s      <= '0;
            k      <= '0;
        end else if (busy) begin
            if (k == 3'd4) begin
                k <= '0;
                if (s == KB'(NR - 1)) busy <= 1'b0;
                else s <= s + 1'b1;
            end else begin
                k <= k + 3'd1;
            end
        end
        if (v2) begin
            in_i    <= keep((k2 == 3'd0) ? snap_i[WI-1:WI-WC] : carry_i, k2);
            in_q    <= keep((k2 == 3'd0) ? snap_q[WI-1:WI-WC] : carry_q, k2);
            pv_i    <= prev_i;
            pv_q    <= prev_q;
            carry_i <= old_i;
            carry_q <= old_q;
            if (k2 == 3'd0 && !start) begin
                snap_i <= snap_i >> WI;
                snap_q <= snap_q >> WI;
            end
        end
        if (v4 && k4 == 3'd4) begin
            last_i[s4] <= new_i[WC-1:WC-20];
            last_q[s4] <= new_q[WC-1:WC-20];
        end
    end

    // ------------------------------------------------------------------
    // Output rounding (76.8 MHz, every clock): bits 42..27, rounded by
    // bits 26..23, halves to even. The last combs keep only four bits
    // below the output, so an exact half comes one time in sixteen and
    // varcic.v's rounding of halves up would be biased by 1/32 LSB (0.031
    // LSB measured, -120 dBFS of DC on the board).
    function automatic logic [15:0] round_even(input logic [19:0] c);
        return c[19:4] + {15'd0, c[3] & (c[2] | c[1] | c[0] | c[4])};
    endfunction

    genvar r;
    generate
        for (r = 0; r < NR; r++) begin : g_out
            logic [15:0] oi = 16'd0, oq = 16'd0;
            always_ff @(posedge clock) begin
                oi <= round_even(last_i[r]);
                oq <= round_even(last_q[r]);
            end
            assign out_i[16*r +: 16] = oi;
            assign out_q[16*r +: 16] = oq;
        end
    endgenerate
endmodule
