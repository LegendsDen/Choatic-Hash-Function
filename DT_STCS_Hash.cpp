/**
 * ============================================================
 *  DT-STCS: Dual-Threaded Spatio-Temporal Chaotic Sponge
 *  A Novel Cryptographic Hash Function
 * ============================================================
 *  Based on:
 *   - Alawida et al. (2020)  – DCFSA_FWP   (IEEE Access)
 *   - Dăscălescu et al. (2025) – Compounded Fn map (IEEE Access)
 *   - Bhatia et al. (2025)   – Sponge + DCFSA (JCIT)
 *   - BTP Report (2025)      – Parallel chaotic hash (IIT Guwahati)
 *   - Proposal doc           – DT-STCS architecture
 *
 *  Build:
 *    g++ -std=c++17 -O2 -pthread -o dt_stcs DT_STCS_Hash.cpp
 *
 *  Usage:
 *    ./dt_stcs
 * ============================================================
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <cassert>
#include <sstream>
#include <algorithm>
#include <array>
#include <bitset>
#include <functional>
#include <chrono>
#include <fstream>
#include <map>



// ============================================================
//  SECTION 1 – Mathematical primitives
// ============================================================

namespace chaos {

// ────────────────────────────────────────────────────────────
// 1-A  Fn compounded arcsin map  (Dăscălescu 2025)
//      f(x,r) = (2/π) * arcsin( sin(π * r * x) )
//      Fn = f_n ∘ f_{n-1} ∘ … ∘ f_1
//      Chaotic  iff  r1*r2*…*rn  > 1/2^n
// ────────────────────────────────────────────────────────────
inline double arcsin_map(double x, double r) {
    // f : [-1,1] -> [-1,1],  chaotic for r > 1
    return (2.0 / M_PI) * std::asin(std::sin(M_PI * r * x));
}

// Compound k applications with parameters rs[0..k-1]
inline double Fn(double x, const std::vector<double>& rs) {
    double v = x;
    for (double r : rs) v = arcsin_map(v, r);
    return v;
}

// ────────────────────────────────────────────────────────────
// 1-B  3-D coupled map  (Akhavan 2013 / BTP 2025)
//      Used as the "Base 3D Map TA" from the proposal
// ────────────────────────────────────────────────────────────
struct State3D {
    double x, y, z;

    void step(double r1, double r2, double r3,
              double alpha, double beta, double gamma_) {
        double nx = std::fmod(r1 * x * (1.0 - x) + beta  * y, 1.0);
        double ny = std::fmod(r2 * y * (1.0 - y) + gamma_* z, 1.0);
        double nz = std::fmod(r3 * z * (1.0 - z) + alpha * x, 1.0);
        // keep in [0,1)
        x = nx < 0.0 ? nx + 1.0 : nx;
        y = ny < 0.0 ? ny + 1.0 : ny;
        z = nz < 0.0 ? nz + 1.0 : nz;
    }
};

// ────────────────────────────────────────────────────────────
// 1-C  DCFSA_FWP  (Alawida 2020)
//      6 machine states  {q0…q5}
//      3 maps: logistic (q0,q3), tent (q1,q4), sine (q2,q5)
//      Perturbations: forward (G=1) and parameter (G=0)
// ────────────────────────────────────────────────────────────
struct DCFSA {
    // Six buffer values b0..b5 (initial conditions for each state)
    std::array<double,6> b;
    // Six control parameters r0..r5
    std::array<double,6> r;

    // G threshold
    static constexpr double T = 0.5;

    // ── chaotic maps ──
    static double logistic(double x, double r_) {
        return r_ * x * (1.0 - x);
    }
    static double tent(double x, double r_) {
        return (x < 0.5) ? (r_ / 2.0) * x : (r_ / 2.0) * (1.0 - x);
    }
    static double sine_map(double x, double r_) {
        return (r_ / 4.0) * std::sin(M_PI * x);
    }

    // map selector: state index -> map function
    static double apply_map(int state_idx, double x, double ri) {
        int m = state_idx % 3; // 0=logistic, 1=tent, 2=sine
        if (m == 0) return logistic(x, ri);
        if (m == 1) return tent(x, ri);
        return sine_map(x, ri);
    }

    // ── parameter scaling φ ──
    static double phi(double ri, double xn, double bmin, double bmax) {
        double zeta = bmax - bmin;
        return std::fmod((ri + (1.0 - xn) * zeta), zeta) + bmin;
    }

    // ── one full DCFSA round: visit states q0→q1→…→q5 ──
    // Returns the chaotic point after q5
    double iterate_round(double x_in) {
        double xn = x_in;
        for (int i = 0; i < 6; ++i) {
            double ci_val;
            if (xn > T) {
                // forward perturbation
                ci_val = std::fmod(apply_map(i, r[i], b[i]) + xn, 1.0);
            } else {
                // parameter perturbation
                double r_perturbed = phi(r[i], xn, 0.0, 4.0);
                ci_val = apply_map(i, b[i], r_perturbed);
            }
            // keep in [0,1)
            if (ci_val < 0.0) ci_val += 1.0;
            if (ci_val >= 1.0) ci_val -= 1.0;
            b[i] = ci_val;   // update buffer
            xn = ci_val;
        }
        return xn;
    }

    // ── perturb buffers with a 52-bit fixed-point sub-block ──
    void perturb_buffer(int j, double ch_val) {
        b[j] = std::fmod((ch_val + b[j]) * 7e14, 1.0);
        if (b[j] < 0.0) b[j] += 1.0;
    }
};

// ────────────────────────────────────────────────────────────
// 1-D  Compounded permutation  P  from the proposal
//      Snext = (2/π)*arcsin( sin( π * R * T(S) ) )
//      wraps the 3D map output through the Fn layer
// ────────────────────────────────────────────────────────────
inline double compounded_permutation(double state_val, double R) {
    // Two-layer compound: arcsin(sin(π*R*state))
    double inner = std::sin(M_PI * R * state_val);
    return (2.0 / M_PI) * std::asin(inner);
}

} // namespace chaos


// ============================================================
//  SECTION 2 – Thread state
// ============================================================

struct ThreadState {
    // Internal 512-bit state: 64 bytes represented as 8 doubles
    // (rate portion: 16 bytes = 2 doubles; capacity: 48 bytes = 6 doubles)
    static constexpr int RATE_DOUBLES     = 2;  // 128 bits
    static constexpr int CAPACITY_DOUBLES = 6;  // 384 bits  (total 512 bits)
    static constexpr int TOTAL_DOUBLES    = RATE_DOUBLES + CAPACITY_DOUBLES;

    std::array<double, TOTAL_DOUBLES> state{};  // S_A or S_B
    //state[0] to state[1] --->rate
    //state[2] to state[7] --->capacity

    // 3-D map parameters
    double r1, r2, r3;          // control params
    double alpha, beta, gamma_; // coupling params
    chaos::State3D map3d{};

    // DCFSA
    chaos::DCFSA dcfsa{};

    // Dynamic R for compounded permutation (updated per message byte)
    double R_dyn{3.5};

    // ── rate portion (first RATE_DOUBLES entries) ──
    double rate(int i) const { return state[i]; }
    double& rate(int i)      { return state[i]; }

    // ── capacity portion ──
    double cap(int i) const { return state[RATE_DOUBLES + i]; }
    double& cap(int i)      { return state[RATE_DOUBLES + i]; }

    // ── XOR rate with message block ──
    // Works cleanly in the [0,1) domain: additive modular mixing
    void xor_rate(double m0, double m1) {
        state[0] = std::fmod(state[0] + m0 + state[0] * m1 + 0.61803398875, 1.0);
        state[1] = std::fmod(state[1] + m1 + state[1] * m0 + 0.38196601125, 1.0);
        if (state[0] < 0.0) state[0] += 1.0;
        if (state[1] < 0.0) state[1] += 1.0;
    }
};


// ============================================================
//  SECTION 3 – DT-STCS Hash Function
// ============================================================

class DTSTCS {
public:
    // ── public parameters ──
    static constexpr int HASH_BITS  = 256;
    static constexpr int HASH_BYTES = HASH_BITS / 8;
    static constexpr int BLOCK_BITS = 256;  // bit-rate total (128 per thread)
    static constexpr int BLOCK_BYTES= BLOCK_BITS / 8;

    // Number of DCFSA rounds for squeezing
    static constexpr int SQUEEZE_ROUNDS = 50;

    // ── key ──
    struct Key {
        double r1, r2, r3;          // 3D map control params  (r ∈ (3.57, 4])
        double alpha, beta, gamma_; // coupling
        std::array<double,6> b0;    // initial DCFSA buffers  b0…b5
        std::array<double,6> r0;    // initial DCFSA control  r0…r5
        double R_init;              // initial dynamic R
    };

    // ── default key (deterministic, unkeyed mode) ──
    static Key default_key() {
        Key k;
        k.r1 = 3.99; k.r2 = 3.98; k.r3 = 3.97;
        k.alpha = 0.1; k.beta = 0.15; k.gamma_ = 0.12;
        for (int i = 0; i < 6; ++i) {
            k.b0[i] = 0.25 + i * 0.05;
            k.r0[i] = 3.99 - i * 0.01;
        }
        k.R_init = 3.5;
        return k;
    }

    // ── constructor ──
    explicit DTSTCS(Key key = default_key()) : key_(key) {}

    // ── main interface ──
    std::vector<uint8_t> hash(const std::vector<uint8_t>& message) {
        auto padded = pad(message);
        auto blocks = split_blocks(padded);

        ThreadState tA, tB;
        init_thread(tA, key_, false);
        init_thread(tB, key_, true);

        // ── absorption ──
        for (const auto& block : blocks) {
            absorb_block(tA, tB, block);
        }

        // ── squeezing: 50 blank rounds ──
        for (int i = 0; i < SQUEEZE_ROUNDS; ++i) {
            permute_thread(tA);
            permute_thread(tB);
            cross_couple(tA, tB);
        }

        // ── extract 256-bit digest ──
        return extract_digest(tA, tB);
    }

    // ── string convenience overload ──
    std::string hash_hex(const std::string& msg) {
        std::vector<uint8_t> m(msg.begin(), msg.end());
        auto digest = hash(m);
        std::ostringstream oss;
        for (auto b : digest)
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        return oss.str();
    }

private:
    Key key_;

    // ──────────────────────────────────────────────────────────
    // 3-A  Padding  (10*1 rule, block_size = 256 bits)
    // ──────────────────────────────────────────────────────────
    static std::vector<uint8_t> pad(const std::vector<uint8_t>& msg) {
        std::vector<uint8_t> out(msg);
        // append single 1 bit (0x80 byte)
        out.push_back(0x80);
        // zero-pad until length ≡ 28 (mod 32)  — last 4 bytes for length
        while ((out.size() % BLOCK_BYTES) != (BLOCK_BYTES - 4))
            out.push_back(0x00);
        // append original length as 32-bit big-endian
        uint32_t orig_bits = (uint32_t)(msg.size() * 8);
        out.push_back((orig_bits >> 24) & 0xFF);
        out.push_back((orig_bits >> 16) & 0xFF);
        out.push_back((orig_bits >>  8) & 0xFF);
        out.push_back( orig_bits        & 0xFF);
        return out;
    }

    // ──────────────────────────────────────────────────────────
    // 3-B  Split padded message into 256-bit blocks
    // ──────────────────────────────────────────────────────────
    static std::vector<std::vector<uint8_t>>
    split_blocks(const std::vector<uint8_t>& padded) {
        std::vector<std::vector<uint8_t>> blocks;
        for (size_t i = 0; i < padded.size(); i += BLOCK_BYTES) {
            blocks.emplace_back(padded.begin() + i,
                                padded.begin() + i + BLOCK_BYTES);
        }
        return blocks;
    }

    // ──────────────────────────────────────────────────────────
    // 3-C  Initialization
    // ──────────────────────────────────────────────────────────
    void init_thread(ThreadState& T, const Key& k, bool is_B) {
        T.r1 = k.r1; T.r2 = k.r2; T.r3 = k.r3;
        T.alpha = k.alpha; T.beta = k.beta; T.gamma_ = k.gamma_;

        // Seed the 3D map differently for thread A vs B
        double seed_offset = is_B ? 0.13 : 0.0;
        T.map3d = {0.3 + seed_offset,
                   0.5 + seed_offset * 0.5,
                   0.7 - seed_offset * 0.3};

        // DCFSA
        for (int i = 0; i < 6; ++i) {
            T.dcfsa.b[i] = k.b0[i] + (is_B ? 0.05 : 0.0);
            T.dcfsa.r[i] = k.r0[i];
        }

        T.R_dyn = k.R_init;

        // Warm-up: 200 blank iterations to leave transient
        for (int i = 0; i < 200; ++i) {
            T.map3d.step(T.r1, T.r2, T.r3, T.alpha, T.beta, T.gamma_);
            T.dcfsa.iterate_round(T.map3d.x);
        }

        // Initial sponge state from warm-up chaotic values
        for (int i = 0; i < ThreadState::TOTAL_DOUBLES; ++i) {
            T.map3d.step(T.r1, T.r2, T.r3, T.alpha, T.beta, T.gamma_);
            T.state[i] = T.map3d.x;
        }
    }

    // ──────────────────────────────────────────────────────────
    // 3-D  Single permutation (DCFSA + 3D + compounded P)
    // ──────────────────────────────────────────────────────────
    void permute_thread(ThreadState& T) {
        // Step 3D map
        T.map3d.step(T.r1, T.r2, T.r3, T.alpha, T.beta, T.gamma_);
        double x3d = T.map3d.x;

        // DCFSA state transition: choose perturbation type
        // (forward if x3d > 0.5, parameter otherwise — matching DCFSA_FWP)
        double dcfsa_out = T.dcfsa.iterate_round(x3d);

        // Compounded permutation on each state element
        for (int i = 0; i < ThreadState::TOTAL_DOUBLES; ++i) {
            double s = chaos::compounded_permutation(T.state[i], T.R_dyn);
            // mix with DCFSA output
            T.state[i] = std::fmod(std::fabs(s + dcfsa_out), 1.0);
        }

        // Update R_dyn using 3D map output (prevents fixed-orbit attacks)
        T.R_dyn = std::fmod(T.R_dyn + T.map3d.z * 0.5 + 1.0, 999.0) + 1.0;
    }

    // ──────────────────────────────────────────────────────────
    // 3-E  Cross-thread coupling (capacity portions interact)
    // ──────────────────────────────────────────────────────────
    void cross_couple(ThreadState& tA, ThreadState& tB) {
        // XOR capacity doubles between threads
        for (int i = 0; i < ThreadState::CAPACITY_DOUBLES; ++i) {
            double a = tA.cap(i);
            double b = tB.cap(i);
            // mix: each capacity entry becomes function of both
            double new_a = std::fmod(std::fabs(a * 3.99 * (1.0 - b)), 1.0);
            double new_b = std::fmod(std::fabs(b * 3.98 * (1.0 - a)), 1.0);
            tA.cap(i) = new_a;
            tB.cap(i) = new_b;
        }
    }

    // ──────────────────────────────────────────────────────────
    // 3-F  Absorb one 256-bit block
    // ──────────────────────────────────────────────────────────
    void absorb_block(ThreadState& tA, ThreadState& tB,
                      const std::vector<uint8_t>& block) {
        // Split 256-bit block into two 128-bit halves: MA, MB
        // Convert each 128-bit half to two 64-bit doubles in [0,1)
        auto bytes_to_double = [](const uint8_t* src) -> double {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v = (v << 8) | src[i];
            return (double)v / (double)std::numeric_limits<uint64_t>::max();
        };

        // MA: bytes [0..15] -> two doubles
        double ma0 = bytes_to_double(&block[0]);
        double ma1 = bytes_to_double(&block[8]);
        // MB: bytes [16..31] -> two doubles
        double mb0 = bytes_to_double(&block[16]);
        double mb1 = bytes_to_double(&block[24]);

        // Update R_dyn with message bytes (defeats fixed-orbit attacks)
        uint8_t first_byte = block[0];
        tA.R_dyn = std::fmod(tA.R_dyn + first_byte * 0.01 + 1.0, 999.0) + 1.0;
        tB.R_dyn = std::fmod(tB.R_dyn + first_byte * 0.01 + 1.5, 999.0) + 1.0;

        // Perturb DCFSA buffers with message sub-blocks (52-bit style)
        for (int j = 0; j < 6; ++j) {
            double ch = bytes_to_double(&block[(j * 4) % 28]);
            tA.dcfsa.perturb_buffer(j, ch);
            tB.dcfsa.perturb_buffer(j, ch);
        }

        // XOR rate portions with message halves
        tA.xor_rate(ma0, ma1);
        tB.xor_rate(mb0, mb1);

        // Apply permutation to each thread independently (parallelisable)
        permute_thread(tA);
        permute_thread(tB);

        // Cross-couple capacity (key novelty: thread interaction)
        cross_couple(tA, tB);
    }

    // ──────────────────────────────────────────────────────────
    // 3-G  Extract 256-bit digest
    //      H = S_A(rate)[128 bits] XOR S_B(rate)[128 bits]
    // ──────────────────────────────────────────────────────────
        std::vector<uint8_t> extract_digest(const ThreadState& tA,
                                        const ThreadState& tB) const {
 
        // SplitMix64 finaliser -- ONLY rate doubles are ever passed here.
        // The function has no visibility of capacity state whatsoever.
        auto rate_double_to_bytes = [](double v, uint8_t* dst) {
            uint64_t iv = (uint64_t)(std::fabs(v) *
                           (double)std::numeric_limits<uint64_t>::max());
            // Standard SplitMix64 finalisation (avalanche all 64 bits)
            iv += 0x9E3779B97F4A7C15ULL;
            iv  = (iv ^ (iv >> 30)) * 0xBF58476D1CE4E5B9ULL;
            iv  = (iv ^ (iv >> 27)) * 0x94D049BB133111EBULL;
            iv  =  iv ^ (iv >> 31);
            for (int k = 7; k >= 0; --k) {
                dst[k] = (uint8_t)(iv & 0xFF);
                iv >>= 8;
            }
        };
 
        // Compile-time guard: RATE_DOUBLES==2 => 16 bytes/thread => 32 bytes total
        static_assert(ThreadState::RATE_DOUBLES == 2,
            "Digest size must equal RATE_DOUBLES*8 bytes per thread * 2 threads");
 
        // --- Squeeze: rate bits only ---
        std::array<uint8_t, 16> rA{}, rB{};
 
        rate_double_to_bytes(tA.rate(0), &rA[0]);   // Thread-A rate[0] -> bytes 0-7
        rate_double_to_bytes(tA.rate(1), &rA[8]);   // Thread-A rate[1] -> bytes 8-15
 
        rate_double_to_bytes(tB.rate(0), &rB[0]);   // Thread-B rate[0] -> bytes 0-7
        rate_double_to_bytes(tB.rate(1), &rB[8]);   // Thread-B rate[1] -> bytes 8-15
        // tA.cap(*) and tB.cap(*) are NOT accessed beyond this point.
 
        // --- Combine: BTP conditional rule (no capacity involved) ---
        // K1 = 0x9E: high byte of golden-ratio constant 0x9E3779B9
        // K2 = 0x6A: high byte of SHA-2 round constant  0x6A09E667
        const uint8_t K1 = 0x9E;
        const uint8_t K2 = 0x6A;
 
        std::vector<uint8_t> digest(HASH_BYTES);  // 32 bytes = 256 bits
 
        // Bytes 0-15: Thread-A rate drives, Thread-B rate responds
        for (int i = 0; i < 16; ++i) {
            uint8_t Ai = rA[i], Bi = rB[i];
            digest[i] = (Ai % 2 == 0)
                ? (uint8_t)((Ai ^ K1) + Bi)   // even path: XOR-then-add
                : (uint8_t)((Ai + K2) ^ Bi);  // odd  path: add-then-XOR
        }
 
        // Bytes 16-31: Thread-B rate drives, Thread-A rate responds
        // Swapping roles mirrors the dual-direction absorption (fwd/bwd threads)
        for (int i = 0; i < 16; ++i) {
            uint8_t Ai = rB[i], Bi = rA[i];
            digest[16 + i] = (Ai % 2 == 0)
                ? (uint8_t)((Ai ^ K1) + Bi)
                : (uint8_t)((Ai + K2) ^ Bi);
        }
 
        return digest;
    }
};


// ============================================================
//  SECTION 4 – Security Analysis & Test Suite
// ============================================================

namespace test {

// ── Helper: hex string ──
std::string to_hex(const std::vector<uint8_t>& v) {
    std::ostringstream oss;
    for (auto b : v)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return oss.str();
}

// ── Helper: bit-level comparison ──
int hamming_distance(const std::vector<uint8_t>& a,
                     const std::vector<uint8_t>& b) {
    assert(a.size() == b.size());
    int dist = 0;
    for (size_t i = 0; i < a.size(); ++i)
        dist += __builtin_popcount(a[i] ^ b[i]);
    return dist;
}

// ── Helper: flip a single bit in message ──
std::vector<uint8_t> flip_bit(const std::vector<uint8_t>& msg, size_t bit_idx) {
    std::vector<uint8_t> m(msg);
    size_t byte_idx = bit_idx / 8;
    size_t bit_off  = 7 - (bit_idx % 8);
    m[byte_idx] ^= (1u << bit_off);
    return m;
}

// ────────────────────────────────────────────────────────────
// TEST 1: Distribution of hash values (hexadecimal histogram)
// ────────────────────────────────────────────────────────────
void test_distribution(DTSTCS& h, int N = 500) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 1: Hash Value Distribution (N=" << N << ")\n";
    std::cout << "========================================\n";

    std::array<int,16> freq{};
    freq.fill(0);

    for (int i = 0; i < N; ++i) {
        std::string msg = "TestMessage_" + std::to_string(i) + "_salt_abc";
        std::vector<uint8_t> m(msg.begin(), msg.end());
        auto digest = h.hash(m);
        for (auto byte : digest)
            freq[byte & 0xF]++, freq[(byte >> 4) & 0xF]++;
    }

    int total = 0; for (auto f : freq) total += f;
    std::cout << " Hex | Count | Bar\n";
    std::cout << " ----+-------+----\n";
    for (int i = 0; i < 16; ++i) {
        int bar_len = (int)(freq[i] * 40.0 / (*std::max_element(freq.begin(), freq.end())));
        std::cout << "  " << std::hex << i << std::dec
                  << "  | " << std::setw(5) << freq[i] << " | "
                  << std::string(bar_len, '#') << "\n";
    }
    // Chi-squared
    double expected = (double)total / 16.0;
    double chi2 = 0.0;
    for (int i = 0; i < 16; ++i)
        chi2 += ((freq[i] - expected) * (freq[i] - expected)) / expected;
    std::cout << " Chi-squared statistic: " << chi2
              << "  (df=15, ideal < 24.996 at p=0.05)\n";


              // ── CSV export ──
    { std::ofstream f("test1_distribution.csv");
      f << "hex_digit,count\n";
      for (int i = 0; i < 16; ++i) f << i << "," << freq[i] << "\n"; }
    std::cout << " [CSV] test1_distribution.csv written\n";
}

// ────────────────────────────────────────────────────────────
// TEST 2: Sensitivity / Avalanche (Bit Flip Test)
// ────────────────────────────────────────────────────────────
void test_sensitivity(DTSTCS& h) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 2: Sensitivity Test\n";
    std::cout << "========================================\n";

    std::string base_msg(1000, 'a');
    std::vector<uint8_t> M(base_msg.begin(), base_msg.end());
    auto H1 = h.hash(M);

    struct Case { std::string name; std::vector<uint8_t> msg; };
    std::vector<Case> cases = {
        {"Original message M",                     M},
        {"Flip first bit of M",                    flip_bit(M, 0)},
        {"Flip last bit of M",                     flip_bit(M, M.size()*8-1)},
        {"Flip middle bit of M",                   flip_bit(M, M.size()*4)},
        {"Change first char (a->b)",                [&]{ auto m=M; m[0]='b'; return m; }()},
        {"Change last char (a->b)",                 [&]{ auto m=M; m.back()='b'; return m; }()},
    };

    std::cout << " Case                          | Hash (first 32 hex)              | Changed bits | %\n";
    std::cout << " " << std::string(95,'-') << "\n";
    { std::ofstream f("test2_sensitivity.csv");
      f << "case,changed_bits,percent\n";
      for (auto& c : cases) {
          auto H = h.hash(c.msg);
          int d = hamming_distance(H, H1);
          double pct = 100.0 * d / (H.size() * 8);
          f << "\"" << c.name << "\"," << d << "," << pct << "\n";
          std::cout << " " << std::setw(30) << std::left << c.name
                    << " | " << to_hex(H).substr(0,32)
                    << " | " << std::setw(12) << std::right << d
                    << " | " << std::fixed << std::setprecision(2) << pct << "%\n";
      } }
    std::cout << " [CSV] test2_sensitivity.csv written\n";
}

// ────────────────────────────────────────────────────────────
// TEST 3: Diffusion & Confusion (Statistical)
// ────────────────────────────────────────────────────────────
void test_diffusion_confusion(DTSTCS& h, int N = 1000) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 3: Diffusion & Confusion (N=" << N << ")\n";
    std::cout << "========================================\n";

    std::vector<int> Bi;
    Bi.reserve(N);
    int hash_bits = DTSTCS::HASH_BITS;

    for (int i = 0; i < N; ++i) {
        // Random-ish message
        std::string msg = "diffusion_test_" + std::to_string(i * 31337 + 7);
        std::vector<uint8_t> M(msg.begin(), msg.end());
        auto H1 = h.hash(M);

        // Flip one bit at position i%msg_bits
        size_t bit_pos = i % (M.size() * 8);
        auto M2 = flip_bit(M, bit_pos);
        auto H2 = h.hash(M2);

        Bi.push_back(hamming_distance(H1, H2));
    }

    // Statistics
    double mean = 0.0;
    for (int b : Bi) mean += b;
    mean /= N;

    double var = 0.0;
    for (int b : Bi) var += (b - mean) * (b - mean);
    var /= (N - 1);
    double std_dev = std::sqrt(var);

    int bmin = *std::min_element(Bi.begin(), Bi.end());
    int bmax = *std::max_element(Bi.begin(), Bi.end());
    double P    = 100.0 * mean / hash_bits;
    double dP   = 100.0 * std_dev / hash_bits;

    std::cout << std::fixed << std::setprecision(4);
    std::cout << " Metric                  | Value        | Ideal\n";
    std::cout << " " << std::string(55,'-') << "\n";
    std::cout << " Mean changed bits  | " << std::setw(12) << mean
              << " | " << hash_bits/2 << "\n";
    std::cout << " Mean changed prob  (P%) | " << std::setw(12) << P
              << " | 50.0000\n";
    std::cout << " Std dev            | " << std::setw(12) << std_dev
              << " | ~5.6\n";
    std::cout << " Std dev prob      | " << std::setw(12) << dP
              << " | ~4.4\n";
    std::cout << " Min changed bits        | " << std::setw(12) << bmin << "\n";
    std::cout << " Max changed bits        | " << std::setw(12) << bmax << "\n";


    { std::ofstream f("test3_diffusion.csv");
      f << "trial,changed_bits\n";
      for (int i = 0; i < (int)Bi.size(); ++i) f << i << "," << Bi[i] << "\n"; }
    std::cout << " [CSV] test3_diffusion.csv written\n";
}

void test_collision(DTSTCS& h, int N = 2000) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 4: Collision Resistance (N=" << N << ")\n";
    std::cout << "========================================\n";

    // s = Total bits in the hash (256)
    constexpr int s = DTSTCS::HASH_BITS; 
    
    // We expect the mean to be s/2 = 128 bits matching.
    // We will track the distribution around the mean.
    std::map<int, int> WN; 

    for (int i = 0; i < N; ++i) {
        std::string msg1 = "collision_msg_" + std::to_string(i * 97);
        std::vector<uint8_t> M1(msg1.begin(), msg1.end());
        auto H1 = h.hash(M1);

        auto M2 = flip_bit(M1, i % (M1.size() * 8));
        auto H2 = h.hash(M2);

        // Count matching BITS (omega)
        int omega = 0;
        for (size_t j = 0; j < H1.size(); ++j) {
            // bits that are the same will have a 0 in the XOR result
            uint8_t same_bits = ~(H1[j] ^ H2[j]);
            omega += __builtin_popcount(same_bits);
        }
        WN[omega]++;
    }

    // Theoretical values (Binomial distribution: n=256, p=0.5)
    auto binom_pmf = [](int n, int k) -> double {
        if (k < 0 || k > n) return 0;
        return std::exp(std::lgamma(n + 1) - std::lgamma(k + 1) - std::lgamma(n - k + 1) 
               + k * std::log(0.5) + (n - k) * std::log(0.5));
    };

    std::ofstream f("test4_collision.csv");
    f << "w,experimental,theoretical\n";

    // We only care about the range around the mean (128) where data exists
    for (int w = 100; w <= 156; ++w) {
        double theo = N * binom_pmf(s, w);
        f << w << "," << WN[w] << "," << theo << "\n";
    }
    
    std::cout << " [CSV] test4_collision.csv written with bit-level analysis\n";
}

// ────────────────────────────────────────────────────────────
// TEST 5: Absolute Difference
// ────────────────────────────────────────────────────────────
void test_absolute_difference(DTSTCS& h, int N = 2000) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 5: Absolute Difference (N=" << N << ")\n";
    std::cout << "========================================\n";

    constexpr int s = 16;
    double sum_d = 0.0;
    double min_d = 1e9, max_d = -1.0;

    for (int i = 0; i < N; ++i) {
        std::string msg = "absdiff_" + std::to_string(i * 1337);
        std::vector<uint8_t> M1(msg.begin(), msg.end());
        auto H1 = h.hash(M1);

        auto M2 = flip_bit(M1, i % (M1.size() * 8));
        auto H2 = h.hash(M2);

        double d = 0.0;
        for (int j = 0; j < s; ++j) {
            int e1 = H1[j*2];
            int e2 = H2[j*2];
            d += std::abs(e1 - e2);
        }
        sum_d += d;
        min_d = std::min(min_d, d);
        max_d = std::max(max_d, d);
    }

    double mean_d = sum_d / N;
    // Theoretical mean absolute difference for 128-bit hash = 2/3 * 2040 = 1360
    std::cout << " Mean absolute difference : " << std::fixed
              << std::setprecision(3) << mean_d
              << "  (theoretical ~1360)\n";
    std::cout << " Min  absolute difference : " << min_d << "\n";
    std::cout << " Max  absolute difference : " << max_d << "\n";


    { std::ofstream f("test5_absdiff.csv");
      f << "metric,value\n";
      f << "mean," << mean_d << "\n";
      f << "min," << min_d << "\n";
      f << "max," << max_d << "\n";
      f << "theoretical,1360\n"; }
    std::cout << " [CSV] test5_absdiff.csv written\n";
}

// ────────────────────────────────────────────────────────────
// TEST 6: Strict Avalanche Criterion (SAC) matrix
// ────────────────────────────────────────────────────────────
void test_sac(DTSTCS& h, int T_runs = 200) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 6: Strict Avalanche Criterion (T=" << T_runs << ")\n";
    std::cout << "========================================\n";

    int n = DTSTCS::HASH_BITS;
    int m = 64; // input bit positions to test (first 64 bits of msg)

    // Dependence matrix D[n][m], each entry = sum of bit i in A_j / T
    std::vector<std::vector<double>> D(n, std::vector<double>(m, 0.0));

    std::string base = "SAC_BASE_MESSAGE_FOR_TESTING_XY";
    std::vector<uint8_t> P0(base.begin(), base.end());

    for (int t = 0; t < T_runs; ++t) {
        // Vary base message slightly per trial
        P0[0] = (uint8_t)((P0[0] + t * 7) & 0xFF);
        auto H0 = h.hash(P0);

        for (int j = 0; j < m && j < (int)(P0.size() * 8); ++j) {
            auto Pj = flip_bit(P0, j);
            auto Hj = h.hash(Pj);
            // Avalanche vector A_j = H0 XOR Hj
            for (int byte = 0; byte < (int)H0.size(); ++byte) {
                uint8_t av = H0[byte] ^ Hj[byte];
                for (int bit = 0; bit < 8; ++bit) {
                    int i = byte * 8 + bit;
                    if (i < n)
                        D[i][j] += ((av >> (7 - bit)) & 1);
                }
            }
        }
    }

    // Normalise
    double global_mean = 0.0, global_min = 1.0, global_max = 0.0;
    double global_var  = 0.0;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            double v = D[i][j] / T_runs;
            global_mean += v;
            global_min = std::min(global_min, v);
            global_max = std::max(global_max, v);
            ++count;
        }
    }
    global_mean /= count;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) {
            double v = D[i][j]/T_runs - global_mean;
            global_var += v * v;
        }
    global_var /= (count - 1);

    std::cout << " Dependence matrix statistics:\n";
    std::cout << "  Mean  : " << std::fixed << std::setprecision(6)
              << global_mean << "  (ideal = 0.5)\n";
    std::cout << "  Min   : " << global_min << "\n";
    std::cout << "  Max   : " << global_max << "\n";
    std::cout << "  StdDev: " << std::sqrt(global_var) << "  (ideal < 0.02)\n";



    { std::ofstream f("test6_sac.csv");
      f << "output_bit,input_bit,probability\n";
      for (int i = 0; i < n; ++i)
          for (int j = 0; j < m; ++j)
              f << i << "," << j << "," << D[i][j]/T_runs << "\n"; }
    std::cout << " [CSV] test6_sac.csv written\n";
}

// ────────────────────────────────────────────────────────────
// TEST 7: Performance benchmark
// ────────────────────────────────────────────────────────────
// Global to prevent the compiler from optimizing away the hash calculation
static std::vector<uint8_t> g_sink;

void test_performance(DTSTCS& h) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 7: Performance Benchmark\n";
    std::cout << "========================================\n";

    // Testing sizes from 64B to 1MB to see the sponge scaling
    std::vector<size_t> sizes = {64, 128, 512, 1024,2048,4096};
    
    std::ofstream perf_csv("test7_performance.csv");
    perf_csv << "msg_bytes,iters,time_ms,throughput_mbps\n";

    for (size_t sz : sizes) {
        // 1. Generate a pseudo-random actual message for this size
        std::vector<uint8_t> msg(sz);
        for (size_t i = 0; i < sz; ++i) {
            msg[i] = (uint8_t)((i * 16777619) ^ 0x55); // Simple LCG for varied data
        }

        // 2. Calibrate iterations to get a stable measurement (at least 100ms)
        int iters = (sz > 100000) ? 50 : 2000;
        if (sz < 1000) iters = 10000;

        // 3. Timing loop
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iters; ++i) {
            // Modifying the message slightly prevents the CPU from 
            // potentially caching the result (though chaos maps are too complex for that)
            msg[i % sz] ^= (uint8_t)i; 
            
            g_sink = h.hash(msg); // Force the result into a global sink
            
            // Revert modification for consistent sz-byte hashing
            msg[i % sz] ^= (uint8_t)i; 
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        // 4. Calculations
        double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double total_sec = total_ms / 1000.0;
        double total_mb = (double)(sz * iters) / (1024.0 * 1024.0);
        double throughput = total_mb / total_sec;

        perf_csv << sz << "," << iters << "," << total_ms/iters << "," << throughput << "\n";
        
        std::cout << "  msg=" << std::setw(8) << sz << " bytes | "
                  << std::setw(6) << iters << " iters | "
                  << std::fixed << std::setprecision(2) << std::setw(8) << total_ms/iters << " ms | "
                  << std::setw(8) << throughput << " MB/s\n";
    }
    
    perf_csv.close();
    std::cout << " [CSV] test7_performance.csv written\n";
}

// ────────────────────────────────────────────────────────────
// TEST 8: Cross-thread coupling effect
// ────────────────────────────────────────────────────────────
void test_cross_coupling(DTSTCS& h) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 8: Cross-Thread Coupling Verification\n";
    std::cout << "========================================\n";
    std::cout << " (Changing one thread's seed should affect the final hash\n";
    std::cout << "  due to capacity coupling.)\n\n";

    std::string msg = "hello world - cross coupling test";
    std::vector<uint8_t> M(msg.begin(), msg.end());

    // Hash with default key
    auto H1 = h.hash(M);
    std::cout << " Default key hash: " << to_hex(H1) << "\n";

    // Build modified key (perturb thread B seed slightly)
    auto k2 = DTSTCS::default_key();
    k2.b0[3] += 1e-10;  // tiny perturbation in buffer
    DTSTCS h2(k2);
    auto H2 = h2.hash(M);
    std::cout << " Modified key  :   " << to_hex(H2) << "\n";

    int d = hamming_distance(H1, H2);
    std::cout << " Hamming distance: " << d << " / " << DTSTCS::HASH_BITS
              << "  (" << 100.0*d/DTSTCS::HASH_BITS << "%)\n";



              { std::ofstream f("test8_coupling.csv");
      f << "key,hash_hex,hamming_to_default\n";
      f << "default," << to_hex(H1) << ",0\n";
      f << "modified," << to_hex(H2) << "," << d << "\n"; }
    std::cout << " [CSV] test8_coupling.csv written\n";
}

// ────────────────────────────────────────────────────────────
// TEST 9: Known-value sanity checks
// ────────────────────────────────────────────────────────────
void test_known_values(DTSTCS& h) {
    std::cout << "\n========================================\n";
    std::cout << " TEST 9: Known-value / Determinism\n";
    std::cout << "========================================\n";

    std::vector<std::string> msgs = {
        "",
        "Sushant",
        "sushant",
        "Sushent",
        std::string(1000, 'a'),
    };

    for (auto& msg : msgs) {
        std::vector<uint8_t> M(msg.begin(), msg.end());
        auto H1 = h.hash(M);
        auto H2 = h.hash(M); // second call must be identical
        bool det = (H1 == H2);
        std::cout << " msg=\"" << (msg.size() > 20 ? msg.substr(0,20)+"..." : msg)
                  << "\"  deterministic=" << (det ? "YES" : "NO!!!")
                  << "  hash=" << to_hex(H1).substr(0,32) << "\n";
    }
}



} // namespace test


// ============================================================
//  SECTION 5 – Main
// ============================================================

int main() {
    std::cout << "\n Architecture parameters:\n";
    std::cout << "  State size   : 1024 bits  (2*512-bit threads)\n";
    std::cout << "  Bit rate     : 256 bits   (128 bits per thread)\n";
    std::cout << "  Capacity     : 768 bits   (384 bits per thread)\n";
    std::cout << "  Output       : 256 bits\n";
    std::cout << "  Chaotic core : 3D Lorenz-type + DCFSA_FWP + Fn compound map\n";
    std::cout << "  Squeeze      : 50 blank rounds with cross-thread coupling\n\n";

    DTSTCS hasher(DTSTCS::default_key());

    // ── Quick demo ──
    std::cout << "---Quick Demo---\n";
    auto demo_hex = [&](const std::string& msg) {
        std::vector<uint8_t> m(msg.begin(), msg.end());
        auto d = hasher.hash(m);
        std::string hex;
        for (auto b : d) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        std::cout << " Hash(\"" << (msg.size()>30?msg.substr(0,30)+"...":msg)
                  << "\") =\n   " << hex << "\n";
    };

    demo_hex("");
    demo_hex("hello");
    demo_hex("The quick brown fox jumps over the lazy dog");
    demo_hex(std::string(64, 'a'));

    // ── Run full test suite ──
    test::test_distribution(hasher, 300);
    test::test_sensitivity(hasher);
    test::test_diffusion_confusion(hasher, 5000);
    test::test_collision(hasher, 1000);
    test::test_absolute_difference(hasher, 1000);
    test::test_sac(hasher, 100);
    test::test_cross_coupling(hasher);
    test::test_known_values(hasher);
    test::test_performance(hasher);



    std::cout << "\n--------------------------------------------\n";
    std::cout << " All tests complete.\n";
    std::cout << "-----------------------------------------------\n";
    return 0;
}
