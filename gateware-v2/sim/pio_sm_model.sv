// SPDX-License-Identifier: GPL-2.0-or-later
//
// Simulation model of one RP1 PIO state machine running the driver's RX or
// TX program (driver/kernel/rb_pio.h, instruction words copied verbatim),
// with the state machine configuration the driver gives it: side-set of
// one pin with an enable bit, IN shifting left with autopush at 28 bits
// (RX), OUT of one pin shifting left with a blocking pull (TX), and the
// two-flop input synchronisers every PIO input passes through. Only the
// instructions those two programs use are decoded.
//
// en low stops the state machine where it is (rp1_pio_sm_set_enabled);
// restart puts the program counter back to the start (a new program load).
`timescale 1ns / 1ps
module pio_sm_model #(
    parameter bit IS_TX = 1'b0
) (
    input  logic        en,
    input  logic        restart,
    input  int          period_ps,    // PIO clock period (200 MHz / clkdiv)
    output logic        side_pin,     // the clock to the FPGA
    input  logic        jmp_pin,      // sample-ready (RX) or ready (TX)
    input  logic [3:0]  in_pins,      // RX data, GPIO 18..21
    output logic        out_pin       // TX data
);
    logic [15:0] prog [0:17];
    int          len, wrap_top;
    initial begin
        if (!IS_TX) begin
            prog[0]  = 16'h000f; prog[1]  = 16'hbb42; prog[2]  = 16'h5004;
            prog[3]  = 16'hbb42; prog[4]  = 16'h5004; prog[5]  = 16'hbb42;
            prog[6]  = 16'h5004; prog[7]  = 16'hbb42; prog[8]  = 16'h5004;
            prog[9]  = 16'hbb42; prog[10] = 16'h5004; prog[11] = 16'hbb42;
            prog[12] = 16'h5004; prog[13] = 16'hbb42; prog[14] = 16'h5004;
            prog[15] = 16'hba42; prog[16] = 16'hb242; prog[17] = 16'h00c1;
            len = 18; wrap_top = 17;
        end else begin
            prog[0] = 16'h1ac2; prog[1] = 16'h1200; prog[2] = 16'hb242;
            prog[3] = 16'he03f; prog[4] = 16'h80a0; prog[5] = 16'h6001;
            prog[6] = 16'hba42; prog[7] = 16'h1245; prog[8] = 16'h0000;
            len = 9; wrap_top = 8;
        end
    end

    logic        pclk = 1'b0;
    always begin
        #(period_ps / 2000.0) pclk = ~pclk;
    end

    // Words pushed (RX) and words to send (TX).
    logic [31:0] rxq [$];
    logic [31:0] txq [$];
    int          pushes = 0;

    logic [4:0]  pc = 5'd0;
    int          dly = 0;
    logic [31:0] isr = 32'd0, osr = 32'd0, x = 32'd0;
    int          isr_n = 0;
    logic        s1_j = 1'b0, s2_j = 1'b0;
    logic [3:0]  s1_i = 4'd0, s2_i = 4'd0;

    initial begin
        side_pin = 1'b0;
        out_pin  = 1'b0;
    end

    always @(posedge pclk) begin : step
        logic [15:0] ins;
        logic [4:0]  nxt;
        logic        stall, jv, jp;
        logic [3:0]  iv;
        jv = s2_j;
        iv = s2_i;
        s2_j = s1_j; s1_j = jmp_pin;
        s2_i = s1_i; s1_i = in_pins;
        if (restart) begin
            pc = 5'd0; dly = 0; isr = 32'd0; isr_n = 0;
        end else if (en) begin
            if (dly > 0) dly--;
            else begin
                ins   = prog[pc];
                stall = 1'b0;
                nxt   = (pc == wrap_top[4:0]) ? 5'd0 : pc + 5'd1;
                if (ins[12]) side_pin = ins[11];
                case (ins[15:13])
                    3'b000: begin   // JMP
                        case (ins[7:5])
                            3'b000: jp = 1'b1;
                            3'b010: begin jp = (x != 32'd0); x = x - 32'd1; end
                            3'b110: jp = jv;
                            default: $fatal(1, "pio model: jmp condition %b", ins[7:5]);
                        endcase
                        if (jp) nxt = ins[4:0];
                    end
                    3'b010: begin   // IN pins, n (shift left, autopush at 28)
                        isr   = (isr << ins[4:0]) | {28'd0, iv};
                        isr_n = isr_n + ins[4:0];
                        if (isr_n >= 28) begin
                            rxq.push_back(isr);
                            pushes++;
                            isr = 32'd0; isr_n = 0;
                        end
                    end
                    3'b011: begin   // OUT pins, 1 (shift left)
                        out_pin = osr[31];
                        osr = osr << 1;
                    end
                    3'b100: begin   // PULL block
                        if (txq.size() == 0) stall = 1'b1;
                        else osr = txq.pop_front();
                    end
                    3'b101: ;       // MOV y, y: nop
                    3'b111: x = {27'd0, ins[4:0]};   // SET x
                    default: $fatal(1, "pio model: opcode %b", ins[15:13]);
                endcase
                if (!stall) begin
                    pc  = nxt;
                    dly = ins[10:8];
                end
            end
        end
    end
endmodule
