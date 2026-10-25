// Sion SD game card case: MiniDisc footprint, as thin as an SD card allows,
// laid out after sokky's mockup: one label that covers the front and wraps
// around the left edge as the spine, card slot on the left edge, lower half.
// Print flat, back face down, no supports, 0.1 mm layers. Units: mm.
// Open in OpenSCAD, change the values below, then F6 + File > Export > STL.

/* [Case] */
case_w      = 72;    // MiniDisc footprint: 72 x 68 (MiniDisc is 5 thick)
case_h      = 68;
case_t      = 3.2;   // SD 2.1 (max 2.25) + clearance + two 0.4-0.5 skins
corner_r    = 3.5;

/* [SD card] */
sd_w        = 24.0;  // SD spec 24.0 x 32.0 x 2.1 (thickness up to 2.25)
sd_l        = 32.0;
clear_xy    = 0.25;  // per side
pocket_t    = 2.3;   // card cavity height
floor_t     = 0.4;   // back skin under the card (roof = case_t - floor_t - pocket_t)
slot_y      = 15.5;  // centre of the slot, measured up from the bottom edge
sd_key      = 3.0;   // fills the SD's cut corner so the card only goes in one way, 0 = off
recess_in   = 0;     // 0 = card sits flush with the edge when fully in

/* [Grip and retention] */
grip_l      = 11;    // pill-shaped thumb scoop at the mouth, both faces
grip_d      = 5.5;   // how far it reaches in from the edge
detent      = true;  // springy tongue in the back skin, its bump presses on the card to hold it
tongue_w    = 3.5;
tongue_l    = 14;
tongue_t    = 0.4;   // = floor_t at this thickness, already flexible
tongue_gap  = 0.6;
bump_h      = 0.25;  // how far it pushes into the cavity

/* [Label] */
// One sticker: cover art on the front, folded over the left edge (above the
// slot) where the "COTTON CRIMSON" spine shows. Shallow recess = placement guide.
labels      = true;
label_depth = 0.1;
margin      = 2.2;   // frame around the front label
spine_h     = 31;    // spine length on the left edge, measured down from the top
spine_back  = 0;     // how far the label also wraps onto the back, 0 = stops at the edge
label_r     = 2.5;

/* [Print layout] */
// "one"  = single piece, cavity roof bridges across the card (no supports)
// "tray" = two-piece fallback, bottom half: back + walls, open on top
// "lid"  = two-piece fallback, top skin, printed label face down; glue onto the tray
part        = "one";

$fn = 48;

pw     = sd_w + 2*clear_xy;
pd     = sd_l + clear_xy + recess_in;
py0    = slot_y - pw/2;          // pocket bottom edge
roof_t = case_t - floor_t - pocket_t;
assert(roof_t - label_depth >= 0.35, "roof too thin under the label");

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
tx_tip = 1.5;                                 // tongue tip, near the mouth
module tongue_cut() {
    translate([0, 0, -1]) linear_extrude(floor_t + 1.01) difference() {
        translate([tx_tip - tongue_gap, ty - tongue_gap]) square([tongue_l + tongue_gap, tongue_w + 2*tongue_gap]);
        translate([tx_tip, ty]) square([tongue_l, tongue_w]);
    }
    if (tongue_t < floor_t)
        translate([tx_tip - 0.01, ty - 0.01, tongue_t]) cube([tongue_l - 1.5, tongue_w + 0.02, floor_t]);
}
module bump() {
    // rounded ridge across the tongue tip; inserting the card flexes the tongue out
    translate([0, ty + tongue_w, 0]) rotate([90, 0, 0]) linear_extrude(tongue_w)
        polygon([[tx_tip + 0.3, floor_t], [tx_tip + 1.0, floor_t + bump_h],
                 [tx_tip + 1.6, floor_t + bump_h], [tx_tip + 2.6, floor_t]]);
}

label_top = case_h - margin;                  // top edge of the whole label
spine_y0  = label_top - spine_h;
assert(spine_y0 > slot_y + grip_l/2 + 1, "spine runs into the card slot: shorten spine_h");
module outline() { hull() for (x = [corner_r, case_w-corner_r], y = [corner_r, case_h-corner_r]) translate([x, y]) circle(r=corner_r); }
module label_cuts() {
    z = case_t - label_depth;
    ax = margin + 8.9;                        // front art starts here, as in the mockup
    lr = label_r + 1;
    // front: art panel and the fold strip as one shape, square where they join
    translate([0, 0, z]) linear_extrude(1) intersection() {
        union() {
            translate([ax, margin + 0.8]) hull() {
                for (x = [lr, case_w - ax - margin - 0.6 - lr], y = [lr, label_top - margin - 0.8 - lr])
                    translate([x, y]) circle(r=lr);
                translate([0, spine_y0 - margin - 0.8]) square([1, label_top - spine_y0]);
            }
            translate([-1, spine_y0]) square([ax + lr + 1, spine_h]);
        }
        outline();
    }
    // spine: a skin of label_depth taken off the left edge (follows the rounded corner)
    translate([0, 0, -1]) linear_extrude(case_t + 2) intersection() {
        difference() { outline(); offset(delta=-label_depth) outline(); }
        translate([-1, spine_y0]) square([ax, spine_h]);
    }
    if (spine_back > 0)
        translate([-1, spine_y0, -1]) cube([1 + spine_back, spine_h, 1 + label_depth]);
}

module case() {
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
}

split_z = floor_t + pocket_t;   // top of the card cavity
if (part == "one") case();
if (part == "tray") intersection() { case(); translate([-5, -5, -1]) cube([case_w + 10, case_h + 10, split_z + 1]); }
if (part == "lid")  // flipped so the label face prints on the bed
    translate([0, case_h, case_t]) rotate([180, 0, 0])
        intersection() { case(); translate([-5, -5, split_z]) cube([case_w + 10, case_h + 10, case_t]); }
