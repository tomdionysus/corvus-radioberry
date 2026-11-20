// SPDX-License-Identifier: GPL-2.0-or-later
//
// The transmit up-converter, original against new: the Hermes-Lite 2
// core's cpl_cordic.v with the core's cut to the DAC (its 16-bit output
// shifted right by 2, the low 12 bits), and rtl/core/tx_upconv.sv without
// and with dither, on the same baseband input and frequency word, every
// clock. The three 12-bit DAC words of every clock of each case go to
// OUT_<case>.txt (one line: original, new, new with dither) for spectral
// analysis (outside this testbench). It also prints, per case, the RMS of
// each output and the largest magnitude, and passes when the new outputs'
// level is within 0.1% of the original's in every case inside the 12-bit
// range.
//
// Cases (N clocks each, after 64 clocks of settling):
//   0 carrier 14.074 MHz, constant I 18000 (a carrier, near the CIC's most)
//   1 carrier 14.074 MHz, constant I 2000 (-19 dB)
//   2 carrier 14.074 MHz, two tones +700 Hz and +1900 Hz, 9000 each
//   3 carrier 7.074 MHz, a tone +1000 Hz, 18000
//   4 carrier 28.074 MHz, a tone +1000 Hz, 18000
//   5 carrier 14.074 MHz, a tone +1000 Hz, 25000 (beyond the 12-bit range)
//
//   +N=<clocks>   per case (default 262144)`timescale 1ns / 1ps

module tx_upconv_tb;
    logic clk = 1'b0;
    always #6.51 clk = ~clk;  // 76.8 MHz

    logic [31:0]        freq = '0;
    logic signed [15:0] xi = '0, xq = '0;

    // The original: cpl_cordic and the core's cut.
    logic signed [15:0] co_i, co_q;
    cpl_cordic #(.OUT_WIDTH(16)) ref_c (
        .clock(clk), .frequency(freq), .in_data_I(xi), .in_data_Q(xq),
        .out_data_I(co_i), .out_data_Q(co_q));
    logic signed [15:0] txsum;
    assign txsum = co_i >>> 2;
    logic signed [11:0] dac_ref = '0;
    always @(posedge clk) dac_ref <= txsum[11:0];

    logic signed [11:0] dac_n, dac_d;
    tx_upconv up_n (.clk(clk), .dither(1'b0), .frequency(freq), .in_i(xi), .in_q(xq), .dac(dac_n));
    tx_upconv up_d (.clk(clk), .dither(1'b1), .frequency(freq), .in_i(xi), .in_q(xq), .dac(dac_d));

    localparam real FS = 76.8e6;
    localparam real TWO_PI = 6.283185307179586;

    function automatic logic [31:0] word(input real f);
        return 32'($rtoi(f / FS * 4294967296.0 + 0.5));
    endfunction

    int n;
    int fd;
    int errors = 0;
    real t;
    string name;
    initial begin
        if (!$value$plusargs("N=%d", n)) n = 262144;
        for (int c = 0; c < 6; c++) begin
            real fc, a1, f1, a2, f2;
            real s_r, s_n, s_d;
            int  m_r, m_n, m_d;
            case (c)
                0: begin fc = 14.074e6; a1 = 18000; f1 = 0;      a2 = 0;    f2 = 0;      end
                1: begin fc = 14.074e6; a1 = 2000;  f1 = 0;      a2 = 0;    f2 = 0;      end
                2: begin fc = 14.074e6; a1 = 9000;  f1 = 700;    a2 = 9000; f2 = 1900;   end
                3: begin fc = 7.074e6;  a1 = 18000; f1 = 1000;   a2 = 0;    f2 = 0;      end
                4: begin fc = 28.074e6; a1 = 18000; f1 = 1000;   a2 = 0;    f2 = 0;      end
                default: begin fc = 14.074e6; a1 = 25000; f1 = 1000; a2 = 0; f2 = 0;   end
            endcase
            freq = word(fc);
            $sformat(name, "OUT_%0d.txt", c);
            fd = $fopen(name, "w");
            s_r = 0; s_n = 0; s_d = 0; m_r = 0; m_n = 0; m_d = 0;
            for (int k = 0; k < n + 64; k++) begin
                @(posedge clk);
                #1;
                t = real'(k) / FS;
                xi = 16'($rtoi(a1 * $cos(TWO_PI * f1 * t) + a2 * $cos(TWO_PI * f2 * t)));
                xq = 16'($rtoi(a1 * $sin(TWO_PI * f1 * t) + a2 * $sin(TWO_PI * f2 * t)));
                if (k >= 64) begin
                    $fwrite(fd, "%0d %0d %0d\n", dac_ref, dac_n, dac_d);
                    s_r += real'(dac_ref) * real'(dac_ref);
                    s_n += real'(dac_n) * real'(dac_n);
                    s_d += real'(dac_d) * real'(dac_d);
                    if ((dac_ref < 0 ? -dac_ref : dac_ref) > m_r) m_r = (dac_ref < 0 ? -dac_ref : dac_ref);
                    if ((dac_n < 0 ? -dac_n : dac_n) > m_n) m_n = (dac_n < 0 ? -dac_n : dac_n);
                    if ((dac_d < 0 ? -dac_d : dac_d) > m_d) m_d = (dac_d < 0 ? -dac_d : dac_d);
                end
            end
            $fclose(fd);
            $display("case %0d: rms original %f new %f dithered %f; largest %0d %0d %0d",
                     c, $sqrt(s_r / n), $sqrt(s_n / n), $sqrt(s_d / n), m_r, m_n, m_d);
            // Within the 12-bit range the new level is the original's
            // (case 5 goes beyond it: the original wraps, the new saturates).
            if (c < 5) begin
                real rn, rd;
                rn = $sqrt(s_n / s_r) - 1.0;
                rd = $sqrt(s_d / s_r) - 1.0;
                if (rn < 0) rn = -rn;
                if (rd < 0) rd = -rd;
                if (rn > 1e-3 || rd > 1e-3) begin
                    $display("FAIL case %0d: level differs from the original's by more than 0.1%%", c);
                    errors++;
                end
            end
        end
        if (errors == 0) $display("PASS");
        else $display("FAILED");
        $finish;
    end
endmodule
