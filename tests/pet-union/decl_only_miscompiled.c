void f(float *in, unsigned *out, int n) {
    /* ppcg generated CPU code */
    
    union (unnamed at u.c:4:9) c;
    if (n >= 1) {
      for (int c0 = 0; c0 < n; c0 += 1)
        out[c0] = ((c.u & 2147483648) ? (~c.u) : (c.u | 2147483648));
      c.f = in[n - 1];
    }
}
