// One TMDS lane: bit alignment (bitslip over a 20-bit window) and 8b/10b decode.
// raw10 comes from the I/O deserializer (Gowin IDES10) at the pixel clock,
// with an unknown bit offset. TMDS sends bit 0 first.
module tmds_lane (
    input  wire       clk,
    input  wire       rst,
    input  wire [9:0] raw10,
    output reg  [9:0] sym,      // aligned 10-bit symbol
    output reg        is_ctrl,  // sym is one of the 4 control tokens
    output reg  [1:0] ctrl,     // {c1,c0} when is_ctrl
    output reg  [7:0] data,     // 8b/10b decoded value
    output reg        locked
);
    reg [9:0]  prev;
    reg [3:0]  slip;
    reg [12:0] timer;
    reg [4:0]  run;
    reg        seen_run;

    wire [19:0] both = {raw10, prev};
    wire [9:0]  win  = both[slip +: 10];

    function [2:0] ctok(input [9:0] s); // {valid, c1, c0}
        case (s)
            10'b1101010100: ctok = 3'b100;
            10'b0010101011: ctok = 3'b101;
            10'b0101010100: ctok = 3'b110;
            10'b1010101011: ctok = 3'b111;
            default:        ctok = 3'b000;
        endcase
    endfunction

    wire [2:0] ct = ctok(win);
    wire [7:0] d  = win[9] ? ~win[7:0] : win[7:0];
    wire [7:0] dec;
    assign dec[0] = d[0];
    genvar i;
    generate for (i = 1; i < 8; i = i + 1) begin : g
        assign dec[i] = win[8] ? (d[i] ^ d[i-1]) : ~(d[i] ^ d[i-1]);
    end endgenerate

    always @(posedge clk) begin
        prev    <= raw10;
        sym     <= win;
        is_ctrl <= ct[2];
        ctrl    <= ct[1:0];
        data    <= dec;
        if (rst) begin
            slip <= 0; timer <= 0; run <= 0; seen_run <= 0; locked <= 0;
        end else begin
            // A blanking period gives long runs of control tokens. Look for a
            // run of 16 in every 8192-clock window, else try the next bit offset.
            run <= ct[2] ? (run == 5'd31 ? run : run + 5'd1) : 5'd0;
            if (run >= 5'd16) seen_run <= 1'b1;
            timer <= timer + 13'd1;
            if (timer == 13'h1fff) begin
                locked   <= seen_run;
                seen_run <= 1'b0;
                if (!seen_run) slip <= (slip == 4'd9) ? 4'd0 : slip + 4'd1;
            end
        end
    end
endmodule
