// Sion SD game card case: MiniDisc-sized shell for one full-size SD card,
// laid out after sokky's mockup (spine label upper left, cover-art label,
// card slot on the left edge, lower half).
// Print flat, back face down, no supports. Units: mm.
// Open in OpenSCAD, change the values below, then F6 + File > Export > STL.

/* [Case] */
case_w      = 72;    // MiniDisc: 72 x 68 x 5
case_h      = 68;
case_t      = 5.0;
corner_r    = 3.5;

/* [SD card] */
sd_w        = 24.0;  // SD spec 24.0 x 32.0 x 2.1 (thickness up to 2.25)
sd_l        = 32.0;
clear_xy    = 0.25;  // per side
pocket_t    = 2.4;   // card cavity height
floor_t     = 1.2;   // back skin under the card
slot_y      = 15.5;  // centre of the slot, measured up from the bottom edge
sd_key      = 3.0;   // fills the SD's cut corner so the card only goes in one way, 0 = off
recess_in   = 2.0;   // card sits this far inside the mouth (detent catches its edge)

/* [Grip and retention] */
grip_l      = 11;    // pill-shaped thumb scoop at the mouth, both faces
grip_d      = 5.5;   // how far it reaches in from the edge
detent      = true;  // springy tongue in the back skin with a bump that clicks behind the card
tongue_w    = 3.5;
tongue_l    = 14;
tongue_t    = 0.8;   // thinned so it flexes
tongue_gap  = 0.6;
bump_h      = 0.45;

/* [Labels] */
labels      = true;
label_depth = 0.3;
margin      = 2.2;   // frame around the labels
spine_w     = 8;     // "COTTON CRIMSON" spine label, upper left
spine_h     = 31;
label_gap   = 0.9;
label_r     = 2.5;

$fn = 48;

pw     = sd_w + 2*clear_xy;
pd     = sd_l + clear_xy + recess_in;
py0    = slot_y - pw/2;          // pocket bottom edge
roof_t = case_t - floor_t - pocket_t;
assert(roof_t - label_depth >= 0.8, "roof too thin under the label");

module rplate(w, h, t, r) {
    hull() for (x = [r, w-r], y = [r, h-r]) translate([x, y, 0]) cylinder(r=r, h=t);
}

module pocket() {
    difference() {
        translate([-1, py0, floor_t]) cube([pd + 1, pw, pocket_t]);
        // SD goes in contacts first, label up: its cut corner lands at the inner top
        if (sd_key > 0)
            translate([pd, py0 + pw, floor_t - 0.01]) linear_extrude(pocket_t + 0.02)
                polygon([[0,0],[-sd_key,0],[0,-sd_key]]);
    }
}

module grip() {
    // pill notch through both skins at the mouth, exposes the card's trailing edge
    hull() for (dy = [-(grip_l/2 - grip_d/2), grip_l/2 - grip_d/2])
        translate([0, slot_y + dy, -1]) cylinder(d=grip_d*2, h=case_t + 2);
}

// Tongue runs along x below the grip, inside the card cavity; free end at the mouth.
ty  = py0 + 0.6 + tongue_gap;
assert(ty + tongue_w + tongue_gap < slot_y - grip_l/2 - 0.5, "detent tongue overlaps the grip");
tx_tip = 0.5;                                // tongue tip, near the mouth
module tongue_cut() {
    // U-slot through the back skin, plus thinning the tongue from the cavity side
    translate([0, 0, -1]) linear_extrude(floor_t + 1.01) difference() {
        translate([tx_tip - tongue_gap - 1, ty - tongue_gap]) square([tongue_l + tongue_gap + 1, tongue_w + 2*tongue_gap]);
        translate([tx_tip, ty]) square([tongue_l, tongue_w]);
    }
    translate([tx_tip - 0.01, ty - 0.01, tongue_t]) cube([tongue_l - 1.5, tongue_w + 0.02, floor_t]);
}
module bump() {
    // rounded ridge on the tongue tip, stops just short of the inserted card's trailing edge
    edge = recess_in + clear_xy - 0.1;
    top  = floor_t + bump_h;
    translate([0, ty + tongue_w, 0]) rotate([90, 0, 0]) linear_extrude(tongue_w)
        polygon([[tx_tip + 0.1, tongue_t], [tx_tip + 0.1, floor_t], [edge - 1.4, top],
                 [edge - 0.6, top], [edge, floor_t], [edge, tongue_t]]);
}

module label_cuts() {
    z = case_t - label_depth;
    translate([margin, case_h - margin - spine_h, z]) rplate(spine_w, spine_h, 1, label_r);
    ax = margin + spine_w + label_gap;
    translate([ax, margin + 0.8, z]) rplate(case_w - ax - margin - 0.6, case_h - 2*margin - 0.8, 1, label_r + 1);
}

union() {
    difference() {
        rplate(case_w, case_h, case_t, corner_r);
        pocket();
        grip();
        if (detent) tongue_cut();
        if (labels) label_cuts();
    }
    if (detent) bump();
}
