// SPDX-License-Identifier: GPL-2.0-or-later
//
// The CW keying envelope: the Hermes-Lite 2 core's straight line (the level
// counter's top 15 bits) against rtl/core/cw_shape.sv's raised cosine, for
// one keyed element as the core's CW state machine makes it: the counter up
// by one a clock to LEVEL_MAX, held, down by one a clock to 0. Writes every
// clock's two amplitudes (straight, raised cosine) to OUT.txt for the
// click spectra (outside this testbench), and checks the raised cosine
// against the ideal curve: its largest error, its peak and that it starts
// and ends at 0.
//
//   +HOLD=<clocks>   the key held at the top (default 384000, 5 ms)
`timescale 1ns / 1ps

module cw_shape_tb;
    logic clk = 1'b0;
    always #6.51 clk = ~clk;  // 76.8 MHz

    localparam int LEVEL_MAX = 'h4d800;
    localparam int LAT = 10;
    localparam real PI = 3.141592653589793;

    logic [18:0] level = '0;
    logic [15:0] amp;
    cw_shape dut (.clk(clk), .level(level), .amp(amp));

    int hold, fd, errors = 0;
    int lv [$];
    real worst = 0.0;
    int  peak = 0;
    initial begin
        if (!$value$plusargs("HOLD=%d", hold)) hold = 384000;
        fd = $fopen("OUT.txt", "w");
        // Lead-in at 0, the element, lead-out at 0 (the pipeline drains).
        for (int k = 0; k < 2 * LEVEL_MAX + hold + 4000; k++) begin
            int l;
            if (k < 1000) l = 0;
            else if (k < 1000 + LEVEL_MAX) l = k - 1000;
            else if (k < 1000 + LEVEL_MAX + hold) l = LEVEL_MAX;
            else if (k < 1000 + 2 * LEVEL_MAX + hold) l = LEVEL_MAX - (k - 1000 - LEVEL_MAX - hold);
            else l = 0;
            @(posedge clk);
            #1;
            level = 19'(l);
            lv.push_back(l);
            if (lv.size() > LAT + 1) begin
                int lold;
                real ideal, err;
                lold = lv.pop_front();
                // amp now belongs to the level LAT clocks before this one.
                ideal = real'('h4d80) * (1.0 - $cos(PI * real'(lold) / real'(LEVEL_MAX))) / 2.0;
                err = real'(amp) - ideal;
                if (err < 0) err = -err;
                if (err > worst) worst = err;
                if (amp > peak) peak = amp;
                $fwrite(fd, "%0d %0d\n", lold >> 4, amp);
            end
        end
        $fclose(fd);
        $display("raised cosine: largest error %f LSB, peak %0d (straight line %0d), last %0d",
                 worst, peak, LEVEL_MAX >> 4, amp);
        if (worst > 2.0) begin $display("FAIL error"); errors++; end
        if (peak < ('h4d80 - 2) || peak > 'h4d80) begin $display("FAIL peak"); errors++; end
        if (amp != 0) begin $display("FAIL not back to 0"); errors++; end
        if (errors == 0) $display("PASS");
        else $display("FAILED");
        $finish;
    end
endmodule
