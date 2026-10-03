#include <gmp.h>
#include <cmath>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <string>

// Extract top 53 bits as a double and return the power-of-two shift
inline double get_top53(const mpz_t x, int bits, int &shift) {
    if (bits <= 53) {
        shift = 0;
        return mpz_get_d(x);
    }
    shift = bits - 53;
    mpz_t top;
    mpz_init(top);
    mpz_tdiv_q_2exp(top, x, shift);
    double m = mpz_get_d(top);
    mpz_clear(top);
    return m;
}

int check_threshold_interval(const mpz_t p, const mpz_t Fn, const mpz_t Fd) {
    int L_p  = mpz_sizeinbase(p, 2);
    int L_fn = mpz_sizeinbase(Fn, 2);
    int L_fd = mpz_sizeinbase(Fd, 2);

    int shift_p, shift_fn, shift_fd;
    double m_p  = get_top53(p, L_p, shift_p);
    double m_fn = get_top53(Fn, L_fn, shift_fn);
    double m_fd = get_top53(Fd, L_fd, shift_fd);

    // If bits > 53, m_fd is truncated, so m_fd + 1 gives the exact upper bound.
    // If bits <= 53, m_fd is exact, so no upper bound addition is needed.
    double m_fn_hi = (L_fn > 53) ? (m_fn + 1.0) : m_fn;
    double m_fd_hi = (L_fd > 53) ? (m_fd + 1.0) : m_fd;
    double m_p_hi  = (L_p  > 53) ? (m_p  + 1.0) : m_p;

    // -------------------------------------------------------------
    // 1. COMPUTING T_lower (Strictly <= T_exact)
    // -------------------------------------------------------------
    std::fesetround(FE_DOWNWARD);
    double log2_4220_lo = std::log2(4220.0); 
    double log2_Fn_lo   = std::log2(m_fn) + shift_fn;
    
    std::fesetround(FE_UPWARD);
    double log2_Fd_hi   = std::log2(m_fd_hi) + shift_fd;
    
    std::fesetround(FE_DOWNWARD);
    double T_lower      = log2_4220_lo + 16.0 * (log2_Fn_lo - log2_Fd_hi);

    // -------------------------------------------------------------
    // 2. COMPUTING T_upper (Strictly >= T_exact)
    // -------------------------------------------------------------
    std::fesetround(FE_UPWARD);
    double log2_4220_hi = std::log2(4220.0);
    double log2_Fn_hi   = std::log2(m_fn_hi) + shift_fn;
    
    std::fesetround(FE_DOWNWARD);
    double log2_Fd_lo   = std::log2(m_fd) + shift_fd;
    
    std::fesetround(FE_UPWARD);
    double T_upper      = log2_4220_hi + 16.0 * (log2_Fn_hi - log2_Fd_lo);

    // -------------------------------------------------------------
    // 3. COMPUTING BOUNDS FOR log2(p)
    // -------------------------------------------------------------
    std::fesetround(FE_UPWARD);
    double log2_p_hi = std::log2(m_p_hi) + shift_p;
    
    std::fesetround(FE_DOWNWARD);
    double log2_p_lo = std::log2(m_p) + shift_p;

    std::fesetround(FE_TONEAREST);

    // -------------------------------------------------------------
    // 4. RIGOROUS DECISION RULE
    // -------------------------------------------------------------
    if (log2_p_hi < T_lower) {
        return -1; // PROVEN: p < T (Pass)
    }
    if (log2_p_lo >= T_upper) {
        return 1;  // PROVEN: p >= T (Fail)
    }

    return 0;      // AMBIGUOUS: Fall back to exact GMP
}

void run_test(const std::string& label, const char* str_p, const char* str_Fn, const char* str_Fd) {
    mpz_t p, Fn, Fd;
    mpz_inits(p, Fn, Fd, NULL);

    mpz_set_str(p, str_p, 10);
    mpz_set_str(Fn, str_Fn, 10);
    mpz_set_str(Fd, str_Fd, 10);

    int res = check_threshold_interval(p, Fn, Fd);

    std::cout << "[" << label << "]\n";
    std::cout << "  p  = " << str_p << "\n";
    std::cout << "  Fn = " << str_Fn << "\n";
    std::cout << "  Fd = " << str_Fd << "\n";
    std::cout << "  Result: ";
    if (res == -1) {
        std::cout << "-1 -> PROVEN PASS (p < T)\n";
    } else if (res == 1) {
        std::cout << " 1 -> PROVEN FAIL (p >= T)\n";
    } else {
        std::cout << " 0 -> AMBIGUOUS (Fall back to exact GMP exponentiation)\n";
    }
    std::cout << "---------------------------------------------------------\n";

    mpz_clears(p, Fn, Fd, NULL);
}

int main() {
    std::cout << "=== Testing Floating Point Interval Pre-Filter ===\n\n";

    run_test("Test 1: Small Exact Pass", "100000000", "2", "1");
    run_test("Test 2: Small Exact Fail", "500000000", "2", "1");
    run_test("Test 3: Large Multi-Word Pass", 
             "12345678901234567890123456789012345678901234567890", 
             "9876543210987654321098765432109876543210", 
             "123456789012345678901234567890");
    run_test("Test 4: Borderline Case (Triggers Fallback)", "276561920", "2", "1");

    return 0;
}
