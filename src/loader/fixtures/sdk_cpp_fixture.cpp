/* Self-authored only. Runtime ABI calls are forwarded to the locked game SDK;
 * this object is linked by C without Vita's C++ runtime or unwinder. */
struct Guard {
    volatile unsigned *count;
    ~Guard() { ++*count; }
};
extern "C" __attribute__((noinline)) int vp_sdk_cpp_fixture(int value,volatile unsigned *count) {
    try {
        Guard guard={count};
        if (value) throw value;
        return 11;
    } catch (int caught) {
        return caught+100;
    }
}
extern "C" __attribute__((noinline)) void vp_sdk_cpp_fixture_end(void) {}
