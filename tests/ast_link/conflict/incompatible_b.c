struct Point { double x; double y; double z; };
int shape(double n);
int use_b(void) { struct Point p = {1, 2, 3}; return shape(p.x); }
