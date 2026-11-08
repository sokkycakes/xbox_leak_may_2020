// HDMI/DVI receiver: three aligned lanes -> deskew -> video timing + RGB.
// Handles video preambles/guard bands and data islands (sync bits are read
// from the TERC4 symbols on lane 0 so hsync/vsync stay valid inside islands).
// Audio packets are not decoded yet.
module hdmi_rx (
    input  wire        clk,     // pixel clock (54 MHz for 2880x480i)
    input  wire        rst,
    input  wire [9:0]  raw0, raw1, raw2,  // lane 0 = blue, 1 = green, 2 = red
    output reg         de,
    output reg         hs,      // raw sync levels as sent (polarity untouched)
    output reg         vs,
    output reg  [7:0]  r, g, b,
    output wire        locked
);
    wire [9:0] s0, s1, s2;
    wire c0, c1, c2;
    wire [1:0] k0, k1, k2;
    wire [7:0] d0, d1, d2;
    wire l0, l1, l2;
    tmds_lane u0 (clk, rst, raw0, s0, c0, k0, d0, l0);
    tmds_lane u1 (clk, rst, raw1, s1, c1, k1, d1, l1);
    tmds_lane u2 (clk, rst, raw2, s2, c2, k2, d2, l2);

    // ---- lane deskew: every lane leaves control at the same symbol ----
    localparam W = 21;  // {is_ctrl, ctrl[1:0], data[7:0], sym[9:0]}
    wire [W-1:0] in0 = {c0, k0, d0, s0};
    wire [W-1:0] in1 = {c1, k1, d1, s1};
    wire [W-1:0] in2 = {c2, k2, d2, s2};
    reg  [W-1:0] h0 [0:3], h1 [0:3], h2 [0:3];
    reg  pc0, pc1, pc2;
    reg  [1:0] del0, del1, del2;
    reg  [2:0] win_cnt;
    reg  [2:0] a0, a1, a2;
    reg  [2:0] seen;
    integer k;
    always @(posedge clk) begin
        h0[0] <= in0; h1[0] <= in1; h2[0] <= in2;
        for (k = 1; k < 4; k = k + 1) begin
            h0[k] <= h0[k-1]; h1[k] <= h1[k-1]; h2[k] <= h2[k-1];
        end
        pc0 <= c0; pc1 <= c1; pc2 <= c2;
        if (rst) begin
            seen <= 0; win_cnt <= 0; del0 <= 0; del1 <= 0; del2 <= 0;
        end else begin
            if (seen != 0 && win_cnt != 3'd7) win_cnt <= win_cnt + 3'd1;
            if (pc0 && !c0 && !seen[0]) begin seen[0] <= 1; a0 <= (seen == 0) ? 3'd0 : win_cnt + 3'd1; end
            if (pc1 && !c1 && !seen[1]) begin seen[1] <= 1; a1 <= (seen == 0) ? 3'd0 : win_cnt + 3'd1; end
            if (pc2 && !c2 && !seen[2]) begin seen[2] <= 1; a2 <= (seen == 0) ? 3'd0 : win_cnt + 3'd1; end
            if (win_cnt == 3'd6) begin
                if (seen == 3'b111 && a0 < 4 && a1 < 4 && a2 < 4) begin
                    // the latest lane gets no delay, earlier lanes wait for it
                    del0 <= mx(a0, a1, a2) - a0;
                    del1 <= mx(a0, a1, a2) - a1;
                    del2 <= mx(a0, a1, a2) - a2;
                end
                seen <= 0; win_cnt <= 0;
            end
        end
    end
    function [1:0] mx(input [2:0] x, input [2:0] y, input [2:0] z);
        mx = (x > y) ? ((x > z) ? x[1:0] : z[1:0]) : ((y > z) ? y[1:0] : z[1:0]);
    endfunction

    wire [W-1:0] q0 = h0[del0], q1 = h1[del1], q2 = h2[del2];
    wire       qc0 = q0[20], qc1 = q1[20], qc2 = q2[20];
    wire [1:0] qk0 = q0[19:18], qk1 = q1[19:18], qk2 = q2[19:18];
    wire [7:0] qd0 = q0[17:10], qd1 = q1[17:10], qd2 = q2[17:10];
    wire [9:0] qs0 = q0[9:0];

    // TERC4 decode of lane 0 (only the two sync bits are needed)
    function [4:0] terc4(input [9:0] s); // {valid, bits[3:0]}
        case (s)
            10'b1010011100: terc4 = 5'h10; 10'b1001100011: terc4 = 5'h11;
            10'b1011100100: terc4 = 5'h12; 10'b1011100010: terc4 = 5'h13;
            10'b0101110001: terc4 = 5'h14; 10'b0100011110: terc4 = 5'h15;
            10'b0110001110: terc4 = 5'h16; 10'b0100111100: terc4 = 5'h17;
            10'b1011001100: terc4 = 5'h18; 10'b0100111001: terc4 = 5'h19;
            10'b0110011100: terc4 = 5'h1a; 10'b1011000110: terc4 = 5'h1b;
            10'b1010001110: terc4 = 5'h1c; 10'b1001110001: terc4 = 5'h1d;
            10'b0101100011: terc4 = 5'h1e; 10'b1011000011: terc4 = 5'h1f;
            default:        terc4 = 5'h00;
        endcase
    endfunction
    wire [4:0] t0 = terc4(qs0);

    // ---- period tracking ----
    localparam M_CTRL = 2'd0, M_VIDEO = 2'd1, M_ISLAND = 2'd2;
    reg [1:0] mode;
    reg [3:0] pre_cnt;     // consecutive identical preamble tokens
    reg [1:0] pre_kind;    // 1 = video preamble, 2 = island preamble
    reg [1:0] skip;        // guard band symbols still to drop
    wire [3:0] ctl = {qk2, qk1};  // CTL3..CTL0
    always @(posedge clk) begin
        de <= 1'b0;
        if (rst) begin
            mode <= M_CTRL; pre_cnt <= 0; pre_kind <= 0; skip <= 0;
        end else if (qc1) begin
            // control period: lane 0 carries the syncs, lanes 1/2 the CTL bits
            mode <= M_CTRL;
            hs <= qk0[0];
            vs <= qk0[1];
            if (ctl == 4'b0001 || ctl == 4'b0101) begin
                if (pre_kind == (ctl == 4'b0001 ? 2'd1 : 2'd2)) begin
                    if (pre_cnt != 4'hf) pre_cnt <= pre_cnt + 4'd1;
                end else begin
                    pre_kind <= (ctl == 4'b0001) ? 2'd1 : 2'd2;
                    pre_cnt  <= 4'd1;
                end
            end else begin
                pre_kind <= 0; pre_cnt <= 0;
            end
        end else begin
            if (mode == M_CTRL) begin
                // first symbol after control: decide what this period is
                if (pre_kind == 2'd2 && pre_cnt >= 4'd8) begin
                    mode <= M_ISLAND;
                end else if (pre_kind == 2'd1 && pre_cnt >= 4'd8) begin
                    mode <= M_VIDEO; skip <= 2'd1;   // this symbol is guard band 1 of 2
                end else begin
                    mode <= M_VIDEO; skip <= 2'd0;   // DVI source: no preamble
                    de <= 1'b1; r <= qd2; g <= qd1; b <= qd0;
                end
                if (pre_kind == 2'd2 && pre_cnt >= 4'd8 && t0[4]) begin
                    hs <= t0[0]; vs <= t0[1];
                end
            end else if (mode == M_ISLAND) begin
                if (t0[4]) begin hs <= t0[0]; vs <= t0[1]; end
            end else begin
                if (skip != 0) begin
                    skip <= skip - 2'd1;
                end else begin
                    de <= 1'b1; r <= qd2; g <= qd1; b <= qd0;
                end
            end
        end
    end
    assign locked = l0 & l1 & l2;
endmodule
