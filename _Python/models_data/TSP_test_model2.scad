// colors
k = [0.312,0.316,0.367];
c = [0.1,0.5,0.814];
m = [0.85,0.306,0.347];
y = [0.865,0.678,0.11];
g = [0.392,0.941,0.372];

h = 2; // layer height 
gap = 2;

/*
// L0
color(k) translate([0,0,0]) cube([5,10,h],center=false);
color(c) translate([5,0,0]) cube([10,5,h],center=false);
color(m) translate([5,5,0]) cube([10,5,h],center=false);
color(y) translate([15,0,0]) cube([5,5,h],center=false);


// L1
color(k) translate([0,0,h+gap]) cube([5,10,h],center=false);
color(c) translate([5,0,h+gap]) cube([10,5,h],center=false);
color(m) translate([5,5,h+gap]) cube([10,5,h],center=false);
color(y) translate([15,0,h+gap]) cube([5,5,h],center=false);


// L2
color(k) translate([0,0,(h+gap)*2]) cube([3,10,h],center=false);
color(c) translate([3,0,(h+gap)*2]) cube([12,7,h],center=false);
color(m) translate([3,7,(h+gap)*2]) cube([12,3,h],center=false);
color(y) translate([15,0,(h+gap)*2]) cube([5,5,h],center=false);


// L3
color(k) translate([0,0,(h+gap)*3]) cube([3,10,h],center=false);
color(c) translate([3,0,(h+gap)*3]) cube([12,10,h],center=false);
color(y) translate([15,0,(h+gap)*3]) cube([5,5,h],center=false);
*/

// L4
color(k) translate([0,0,(h+gap)*4]) cube([10,10,h],center=false);
color(c) translate([10,0,(h+gap)*4]) cube([5,10,h],center=false);
color(y) translate([15,0,(h+gap)*4]) cube([5,5,h],center=false);


// L5
color(k) translate([0,0,(h+gap)*5]) cube([13,10,h],center=false);
color(g) translate([15,5,(h+gap)*5]) cube([5,5,h],center=false);
color(y) translate([15,0,(h+gap)*5]) cube([5,5,h],center=false);
color(m) translate([13,0,(h+gap)*5]) cube([2,10,h],center=false);


// L6
color(k) translate([0,0,(h+gap)*6]) cube([11,10,h],center=false);
color(g) translate([15,5,(h+gap)*6]) cube([5,5,h],center=false);
color(y) translate([15,0,(h+gap)*6]) cube([5,5,h],center=false);
color(m) translate([13,0,(h+gap)*6]) cube([2,10,h],center=false);
color(c) translate([11,0,(h+gap)*6]) cube([2,10,h],center=false);


// L7
color(k) translate([0,0,(h+gap)*7]) cube([11,10,h],center=false);
color(g) translate([15,5,(h+gap)*7]) cube([5,5,h],center=false);
color(g) translate([13,0,(h+gap)*7]) cube([2,10,h],center=false);
color(y) translate([15,0,(h+gap)*7]) cube([5,5,h],center=false);
color(c) translate([11,0,(h+gap)*7]) cube([2,10,h],center=false);


// L8
color(k) translate([0,0,(h+gap)*8]) cube([3,10,h],center=false);
color(g) translate([8,0,(h+gap)*8]) cube([12,10,h],center=false);
color(c) translate([3,0,(h+gap)*8]) cube([5,10,h],center=false);


// L9
color(k) translate([0,0,(h+gap)*9]) cube([5,10,h],center=false);
color(g) translate([5,0,(h+gap)*9]) cube([15,10,h],center=false);


// L10
color(g) translate([0,0,(h+gap)*10]) cube([15,10,h],center=false);


// L11
color(g) translate([0,0,(h+gap)*11]) cube([15,10,h],center=false);



