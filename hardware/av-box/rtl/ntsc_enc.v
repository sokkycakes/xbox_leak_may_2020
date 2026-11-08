// NTSC encoder, genlocked to an incoming 15 kHz 480i HDMI stream.
// Runs at the HDMI pixel clock: 54 MHz for 2880x480i (VIC 10/11), so one
// line is exactly 3432 clocks and 227.5 colour subcarrier cycles.
// Outputs: composite, S-video Y and C (8-bit DAC codes), RGB (6-bit, full
// range, or YPbPr when ypbpr=1) and composite sync.
module ntsc_enc (
    input  wire       clk,
    input  wire       rst,
    input  wire       ypbpr,
    input  wire       de_in,
    input  wire       hs_in,
    input  wire       vs_in,
    input  wire [7:0] r_in, g_in, b_in,   // limited range 16-235 RGB
    output reg  [7:0] comp,
    output reg  [7:0] luma,
    output reg  [7:0] chroma,
    output reg  [5:0] ro, go, bo,
    output reg        csync_n,
    output reg        field2
);
    localparam LINE = 3432, HALF = 1716;
    localparam SYNC = 254, EQ = 124, BROAD = HALF - 254;
    localparam BURST_ON = 286, BURST_OFF = 286 + 136;
    localparam [7:0] BLANK = 8'd57;   // 0 IRE; code = (IRE + 40) * 255 / 180

    // ---- sync polarity: the idle level is whatever the line has during video
    reg hs_idle, vs_idle;
    always @(posedge clk) if (de_in) begin hs_idle <= hs_in; vs_idle <= vs_in; end
    wire hsp = hs_in ^ hs_idle;   // 1 during the pulse
    wire vsp = vs_in ^ vs_idle;
    reg hsp_d, vsp_d;

    // ---- line and half-line position, locked to the source ----
    reg [11:0] hcnt;
    reg [9:0]  line;      // 1..525
    always @(posedge clk) begin
        hsp_d <= hsp; vsp_d <= vsp;
        if (rst) begin
            hcnt <= 0; line <= 1; field2 <= 0;
        end else begin
            hcnt <= (hsp && !hsp_d) ? 12'd0 : (hcnt == LINE - 1 ? 12'd0 : hcnt + 12'd1);
            if (hsp && !hsp_d) line <= (line == 10'd525) ? 10'd1 : line + 10'd1;
            if (vsp && !vsp_d) begin
                if (hcnt > 12'd800 && hcnt < 12'd2600) begin
                    line <= 10'd266; field2 <= 1'b1;     // vsync starts mid-line 266
                end else begin
                    line <= 10'd4; field2 <= 1'b0;
                end
            end
        end
    end
    wire        second_half = hcnt >= HALF;
    wire [11:0] hh = second_half ? hcnt - HALF : hcnt;
    wire [10:0] H  = {line - 10'd1, 1'b0} + second_half;   // half-line 0..1049

    wire eq_zone    = (H < 6) || (H >= 12 && H < 18) || (H >= 525 && H < 531) || (H >= 537 && H < 543);
    wire broad_zone = (H >= 6 && H < 12) || (H >= 531 && H < 537);
    wire vblank_sync = eq_zone || broad_zone;
    wire sync_now = eq_zone    ? (hh < EQ) :
                    broad_zone ? (hh < BROAD) :
                                 (!second_half && hcnt < SYNC);
    wire no_burst = (line <= 10'd9) || (line >= 10'd263 && line <= 10'd272);
    wire burst_now = !no_burst && hcnt >= BURST_ON && hcnt < BURST_OFF;

    // ---- subcarrier: exactly 455/6864 cycles per clock (227.5 per line) ----
    reg [12:0] acc;
    always @(posedge clk) acc <= (acc + 13'd455 >= 13'd6864) ? acc + 13'd455 - 13'd6864 : acc + 13'd455;
    wire [7:0] ph = (acc * 27'd9777) >> 18;      // 0..255 per cycle
    reg signed [7:0] sin_lut [0:255];
    integer n;
    initial for (n = 0; n < 256; n = n + 1) sin_lut[n] = $rtoi(127.0 * $sin(6.283185307 * n / 256.0));
    wire signed [7:0] s = sin_lut[ph];
    wire signed [7:0] c = sin_lut[ph + 8'd64];

    // ---- colour: limited to full range, then Y, U, V ----
    function [7:0] full(input [7:0] x);
        reg [15:0] t;
        begin
            t = (x <= 8'd16) ? 16'd0 : (x - 8'd16) * 16'd298;
            full = (t[15:8] > 8'd255 || x >= 8'd235) ? 8'd255 : t[15:8];
        end
    endfunction
    wire [7:0] R = full(r_in), G = full(g_in), B = full(b_in);
    wire [15:0] ysum = 16'd77 * R + 16'd150 * G + 16'd29 * B;
    wire [7:0]  Y = ysum[15:8];
    wire signed [9:0] bmy = $signed({2'b0, B}) - $signed({2'b0, Y});
    wire signed [9:0] rmy = $signed({2'b0, R}) - $signed({2'b0, Y});
    wire signed [19:0] U20 = (bmy * 20'sd126) >>> 8;   // 0.492 (B - Y)
    wire signed [19:0] V20 = (rmy * 20'sd224) >>> 8;   // 0.877 (R - Y)
    wire signed [9:0]  U = U20[9:0];
    wire signed [9:0]  V = V20[9:0];

    // ---- band limiting: 4-tap box on Y, 16-tap box on U and V ----
    reg [7:0] yd [0:3];
    reg signed [9:0] ud [0:15], vd [0:15];
    reg [9:0] ysum4;
    reg signed [13:0] usum, vsum;
    reg [18:0] ded;   // DE delayed to match the filters
    integer k;
    always @(posedge clk) begin
        yd[0] <= de_in ? Y : 8'd0;
        ud[0] <= de_in ? U : 10'sd0;
        vd[0] <= de_in ? V : 10'sd0;
        for (k = 1; k < 4;  k = k + 1) yd[k] <= yd[k-1];
        for (k = 1; k < 16; k = k + 1) begin ud[k] <= ud[k-1]; vd[k] <= vd[k-1]; end
        ysum4 <= ysum4 + (de_in ? Y : 8'd0) - yd[3];
        usum  <= usum + (de_in ? U : 10'sd0) - ud[15];
        vsum  <= vsum + (de_in ? V : 10'sd0) - vd[15];
        ded   <= {ded[17:0], de_in};
        if (rst) begin ysum4 <= 0; usum <= 0; vsum <= 0; end
    end
    wire [7:0]        Yf = ysum4[9:2];
    wire signed [9:0] Uf = usum >>> 4;
    wire signed [9:0] Vf = vsum >>> 4;
    wire video = ded[2] | ded[9];   // between the luma and chroma filter delays

    // chroma in 1/256 of full scale: U sin + V cos (burst sits at -U)
    wire signed [17:0] cm = (Uf * s + Vf * c) >>> 7;
    wire signed [10:0] cmp = $signed({3'b0, Yf}) + cm[10:0];
    // composite code = 67 + 0.5139 * value  (7.5 IRE setup, 100 IRE white)
    wire signed [19:0] code_v = 20'sd67 + ((cmp * 20'sd131) >>> 8);
    wire signed [19:0] code_y = 20'sd67 + ((Yf * 20'sd131) >>> 8);
    wire signed [19:0] code_c = 20'sd128 + ((cm * 20'sd131) >>> 8);
    wire signed [19:0] burst  = (-s * 20'sd28) >>> 7;   // 40 IRE peak to peak

    function [7:0] clip(input signed [19:0] v);
        clip = (v < 0) ? 8'd0 : (v > 255) ? 8'd255 : v[7:0];
    endfunction

    always @(posedge clk) begin
        csync_n <= !sync_now;
        if (sync_now) begin
            comp <= 8'd0; luma <= 8'd0; chroma <= 8'd128;
        end else if (video && !vblank_sync) begin
            comp <= clip(code_v); luma <= clip(code_y); chroma <= clip(code_c);
        end else if (burst_now) begin
            comp <= clip(20'sd57 + burst); luma <= BLANK; chroma <= clip(20'sd128 + burst);
        end else begin
            comp <= BLANK; luma <= BLANK; chroma <= 8'd128;
        end
        // RGB / YPbPr outputs follow the unfiltered pixels
        if (de_in && !ypbpr) begin
            ro <= R[7:2]; go <= G[7:2]; bo <= B[7:2];
        end else if (de_in) begin
            go <= Y[7:2];
            bo <= clip(20'sd128 + ((bmy * 20'sd145) >>> 8)) >> 2;  // Pb = 0.564 (B - Y)
            ro <= clip(20'sd128 + ((rmy * 20'sd183) >>> 8)) >> 2;  // Pr = 0.713 (R - Y)
        end else begin
            ro <= ypbpr ? 6'd32 : 6'd0; go <= 6'd0; bo <= ypbpr ? 6'd32 : 6'd0;
        end
    end
endmodule
