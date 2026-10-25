// Sion SD game card case: floppy-sized sleeve for one full-size SD card.
// Print flat, back face down, no supports. Units: mm.
// Open in OpenSCAD, change the values below, then F6 + File > Export > STL.

/* [Case] */
case_w      = 90;    // width (3.5" floppy: 90)
case_h      = 94;    // height (3.5" floppy: 94)
case_t      = 3.3;   // thickness (3.5" floppy: 3.3). 3.5 gives stiffer skins.
corner_r    = 2.5;   // rounded corners
key_chamfer = 5;     // floppy-style cut corner (top left), 0 = off

/* [SD card] */
sd_w        = 24.0;  // SD spec 24.0 x 32.0 x 2.1 (thickness up to 2.25)
sd_l        = 32.0;
clear_xy    = 0.25;  // per side
pocket_t    = 2.3;   // card cavity height
floor_t     = 0.5;   // back skin under the card (roof = case_t - floor_t - pocket_t)
slot_x      = 0;     // slot offset from centre
sd_key      = 3.0;   // fills the SD's cut corner so the card only goes in one way, 0 = off
recess_in   = 2.0;   // card sits this far inside the mouth (detent catches its edge)

/* [Grip and retention] */
pinch_r     = 6;     // half-round notch at the mouth on both faces to pinch the card
window      = true;  // window in the front over the card label (also a push-out spot)
window_w    = 15;
window_h    = 17;
detent      = true;  // springy tongue in the back skin with a bump that clicks behind the card
tongue_w    = 3.5;
tongue_l    = 13;
tongue_gap  = 0.6;
bump_h      = 0.4;

/* [Label] */
label       = true;  // shallow label recess on the front, below the card
label_depth = 0.2;
label_margin= 6;

$fn = 48;

roof_t   = case_t - floor_t - pocket_t;
pw       = sd_w + 2*clear_xy;
pd       = sd_l + clear_xy + recess_in;
cx       = case_w/2 + slot_x;
pocket_y = case_h - pd;           // inner end of the card cavity

module rounded_plate(w, h, t, r) {
    hull() for (x = [r, w-r], y = [r, h-r]) translate([x, y, 0]) cylinder(r=r, h=t);
}

module body() {
    difference() {
        rounded_plate(case_w, case_h, case_t, corner_r);
        if (key_chamfer > 0)
            translate([0, case_h, -1]) rotate(45) cube([key_chamfer*1.4142, key_chamfer*1.4142, case_t+2], center=true);
    }
}

module pocket() {
    difference() {
        translate([cx - pw/2, pocket_y, floor_t]) cube([pw, pd + 1, pocket_t]);
        // key: SD goes in contacts first, label up, so its cut corner lands at the inner left
        if (sd_key > 0)
            translate([cx - pw/2, pocket_y, floor_t - 0.01])
                linear_extrude(pocket_t + 0.02) polygon([[0,0],[sd_key,0],[0,sd_key]]);
    }
}

module pinch() {
    translate([cx, case_h, -1]) cylinder(r=pinch_r, h=case_t+2);
}

module window_cut() {
    translate([cx - window_w/2, pocket_y + (sd_l - window_h)/2 + 2, floor_t + pocket_t - 0.01])
        cube([window_w, window_h, roof_t + 1]);
}

// Tongue sits to one side of the pinch notch; its free end points at the mouth.
tx  = cx + pw/2 - 0.6 - tongue_gap - tongue_w;   // must stay under the card cavity
assert(tx - tongue_gap > cx + pinch_r + 0.5, "detent tongue overlaps the pinch notch: shrink pinch_r or tongue_w");
ty0 = case_h - recess_in - tongue_l;  // root end
module tongue_slot() {
    translate([0, 0, -1]) linear_extrude(floor_t + 1.01) difference() {
        translate([tx - tongue_gap, ty0]) square([tongue_w + 2*tongue_gap, tongue_l + recess_in + 1]);
        translate([tx, ty0]) square([tongue_w, tongue_l + recess_in - 0.6]);
    }
}
module bump() {
    // half-round ridge across the tongue tip, just outside the card's trailing edge
    card_end = case_h - recess_in - clear_xy;
    translate([tx, card_end + bump_h*1.6*0.95, floor_t])
        intersection() {
            rotate([0, 90, 0]) translate([0,0,0]) cylinder(r=bump_h*1.6, h=tongue_w, $fn=32);
            translate([0, -5, 0]) cube([tongue_w, 10, bump_h]);
        }
}

module label_cut() {
    translate([label_margin, label_margin, case_t - label_depth])
        rounded_plate(case_w - 2*label_margin, pocket_y - 2*label_margin, 1, 1.5);
}

union() {
    difference() {
        body();
        pocket();
        pinch();
        if (window) window_cut();
        if (detent) tongue_slot();
        if (label)  label_cut();
    }
    if (detent) bump();
}
