// SPDX-License-Identifier: GPL-2.0-or-later
//
// Self-test of the FPGA's block RAM and embedded multipliers.
//
// RAM: every word of a 1024 x 18 memory is written with a pattern derived
// from its address, read back and compared, twice with inverted patterns.
// Multipliers: an 18 x 18 signed product from an embedded multiplier is
// compared with the same product built from logic cells, over 65536
// pseudo-random operand pairs.
module rbt_bist (
    input  logic        clk,
    input  logic        rst,
    input  logic        start,
    output logic        busy,
    output logic        done,
    output logic [15:0] ram_errors,
    output logic [15:0] mul_errors
);

    // ---- RAM ----
    (* ramstyle = "M9K" *) logic [17:0] mem [0:1023];
    logic [9:0]  ram_addr;
    logic [17:0] ram_wdata;
    logic        ram_we;
    logic [17:0] ram_q;

    always_ff @(posedge clk) begin
        if (ram_we) mem[ram_addr] <= ram_wdata;
        ram_q <= mem[ram_addr];
    end

    function automatic logic [17:0] pattern(input logic [9:0] a, input logic inv);
        logic [17:0] p;
        p = {a[7:0], a} ^ {a[1:0], a[9:2], a[9:2]} ^ 18'h2A5A5;
        return inv ? ~p : p;
    endfunction

    // ---- multipliers ----
    logic signed [17:0] op_a, op_b;
    (* multstyle = "dsp" *)   logic signed [35:0] prod_dsp;
    (* multstyle = "logic" *) logic signed [35:0] prod_lut;
    logic [35:0] lfsr = 36'h1;

    always_ff @(posedge clk) begin
        op_a     <= lfsr[17:0];
        op_b     <= lfsr[35:18];
        prod_dsp <= op_a * op_b;
        prod_lut <= op_a * op_b;
    end

    // ---- sequencer ----
    typedef enum logic [2:0] {S_IDLE, S_WRITE, S_READ, S_CHECK, S_MUL, S_DONE} state_t;
    state_t      state = S_IDLE;
    logic        inv;
    logic [10:0] idx;
    logic [16:0] mul_n;
    logic [9:0]  addr_d;

    assign ram_addr  = idx[9:0];
    assign ram_wdata = pattern(idx[9:0], inv);
    assign ram_we    = (state == S_WRITE);

    always_ff @(posedge clk) begin
        if (rst) begin
            state <= S_IDLE;
            busy  <= 1'b0;
            done  <= 1'b0;
        end else begin
            case (state)
                S_IDLE: if (start) begin
                    busy       <= 1'b1;
                    done       <= 1'b0;
                    ram_errors <= 16'd0;
                    mul_errors <= 16'd0;
                    inv        <= 1'b0;
                    idx        <= 11'd0;
                    state      <= S_WRITE;
                end
                S_WRITE: begin
                    if (idx == 11'd1023) begin
                        idx   <= 11'd0;
                        state <= S_READ;
                    end else idx <= idx + 11'd1;
                end
                // Address idx is presented in S_READ; its data is compared
                // one cycle later in S_CHECK.
                S_READ: begin
                    addr_d <= idx[9:0];
                    state  <= S_CHECK;
                end
                S_CHECK: begin
                    if (ram_q != pattern(addr_d, inv) && ram_errors != 16'hFFFF)
                        ram_errors <= ram_errors + 16'd1;
                    if (idx == 11'd1023) begin
                        idx <= 11'd0;
                        if (inv) begin
                            mul_n <= 17'd0;
                            state <= S_MUL;
                        end else begin
                            inv   <= 1'b1;
                            state <= S_WRITE;
                        end
                    end else begin
                        idx   <= idx + 11'd1;
                        state <= S_READ;
                    end
                end
                S_MUL: begin
                    lfsr <= {lfsr[34:0], lfsr[35] ^ lfsr[24]};
                    if (mul_n > 17'd3 && prod_dsp != prod_lut && mul_errors != 16'hFFFF)
                        mul_errors <= mul_errors + 16'd1;
                    if (mul_n == 17'd65539) state <= S_DONE;
                    else mul_n <= mul_n + 17'd1;
                end
                S_DONE: begin
                    busy  <= 1'b0;
                    done  <= 1'b1;
                    state <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
