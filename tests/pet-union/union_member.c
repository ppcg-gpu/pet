union ggml_f32bits { float f; unsigned u; };

void f(float *in, unsigned *out, int n) {
#pragma scop
    for (int i = 0; i < n; i++) {
        union ggml_f32bits c;
        c.f = in[i];
        out[i] = (c.u & 0x80000000u) ? ~c.u : (c.u | 0x80000000u);
    }
#pragma endscop
}
