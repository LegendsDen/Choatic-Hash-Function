#include "HybridParallelHash.hpp"

#include<bits/stdc++.h>
using namespace std;


HybridParallelHash::HybridParallelHash(double r1, double r2,
                                     uint32_t transient_iters, uint32_t warmup_for_T1)
: base_r1(r1), base_r2(r2),
  transient_iterations(transient_iters), warmup_for_T1(warmup_for_T1)
{
    if (r1 <= 0 || r1 > 4.0 || r2 <= 0 || r2 > 4.0) {
        // Broad valid ranges for chaotic behavior
        throw invalid_argument("r1 and r2 must be in (0, 4.0]");
    }
}

string HybridParallelHash::digest(const vector<uint8_t>& message, size_t L) {
    if (L < 128 || L % 32 != 0) {
        throw invalid_argument("Hash size L must be >= 128 and a multiple of 32.");
    }
    
    // 1) *** CHANGED ***: Pad message and convert to 32-bit blocks
    auto blocks = pad_and_make_blocks(message);

    // 2) derive initial state (Li & Dai style) [cite: 1315]
    State s0 = derive_initial_state(blocks);

    // 3) *** CHANGED ***: Get T1 and T2 start states (Akhavan style) [cite: 82]
    // We only need two states, derived from the *same* s0
    State t1_start = transient_warmup(s0, warmup_for_T1); // default 1000
    State t2_start = transient_warmup(s0, transient_iterations); // default 1500

    // 4) prepare output matrices
    size_t out_bytes = L / 8;
    size_t k = (out_bytes + 3) / 4; // Number of uint32_t elements
    vector<uint32_t> D1(k, 0), D2(k, 0);
    atomic<bool> errFlag(false);

    // 5) *** CHANGED ***: Launch threads to get FINAL states
    State final_state_1, final_state_2;

    thread th1([&] {
        final_state_1 = thread_worker_forward(blocks, t1_start, errFlag);
    });
    thread th2([&] {
        final_state_2 = thread_worker_backward(blocks, t2_start, errFlag);
    });

    th1.join();
    th2.join();
    
    if(errFlag.load()) {
        // Handle error, e.g., throw exception or return empty string
        return "HASH_ERROR";
    }

    // 6) *** NEW ***: Post-processing (Finalization / Squeezing)
    // Per Akhavan, iterate the *final state* L/8 times (k=L/32 rounds) 
    // We use the base_r parameters for this finalization
    finalize_and_fill_matrix(final_state_1, D1, base_r1, base_r2);
    finalize_and_fill_matrix(final_state_2, D2, base_r1, base_r2);
    
    // 7) Combine and format
    auto bytes = combine_matrices(D1, D2, out_bytes);
    return to_hex_digest(bytes);
}

string HybridParallelHash::digest(const string &msg, size_t L) {
    vector<uint8_t> v(msg.begin(), msg.end());
    return digest(v, L);
}

// ----------------- chaotic map (Li & Dai style) -----------------
HybridParallelHash::State HybridParallelHash::li_dai_step(const State &s, double r1_local, double r2_local) const {
    State out;
    double xi = s.x, yi = s.y, zi = s.z;

    // Equations from Li & Dai paper 
    double xnp = r1_local * xi * (1.0 - xi) + r2_local * sin(M_PI * yi);
    double ynp = (r1_local - yi * yi)       + r2_local * sin(M_PI * zi);
    double znp = r1_local * zi * (1.0 - zi) + r2_local * sin(M_PI * xi);
    
    // fmod(x, 1.0) is numerically unstable for large x.
    // x - floor(x) is the correct way to get fractional part.
    xnp = xnp - floor(xnp);
    ynp = ynp - floor(ynp);
    znp = znp - floor(znp);
    
    // Ensure state is always in [0, 1)
    out.x = (xnp < 0) ? xnp + 1.0 : xnp;
    out.y = (ynp < 0) ? ynp + 1.0 : ynp;
    out.z = (znp < 0) ? znp + 1.0 : znp;
    
    return out;
}

HybridParallelHash::State HybridParallelHash::recombine(const State &s, double r1_local, double r2_local) const {
    // Optional extra mixing from Li&Dai [cite: 1341-1343]
    State r;
    double x = s.x, y = s.y, z = s.z;
    
    double x2 = ( (x + y*z) * r1_local );
    double y2 = ( (y + z*x) * r2_local );
    double z2 = ( (z + x*y) * (r1_local * r2_local) );
    
    x2 -= floor(x2); 
    y2 -= floor(y2); 
    z2 -= floor(z2);

    r.x = (x2 < 0) ? x2 + 1.0 : x2;
    r.y = (y2 < 0) ? y2 + 1.0 : y2;
    r.z = (z2 < 0) ? z2 + 1.0 : z2;

    return r;
}

// -------------- initialization & message handling ---------------

// *** NEW ***: Merkle–Damgård padding
vector<uint32_t> HybridParallelHash::pad_and_make_blocks(const vector<uint8_t>& message) {
    vector<uint8_t> padded = message;
    uint64_t bit_len = (uint64_t)message.size() * 8;

    // 1. Append '1' bit (0x80)
    padded.push_back(0x80);

    // 2. Append '0' bits until length % 64 == 56
    // (We work in 32-bit/4-byte blocks, so pad to % 4 == 0 first)
    while (padded.size() % 4 != 0) {
        padded.push_back(0x00);
    }
    // Now pad with 32-bit(4-byte) zero blocks
    // We need 8 bytes (2 blocks) at the end for the length.
    // So pad until (padded.size() / 4) % (16) == 14
    // (16 blocks = 64 bytes, 14 blocks = 56 bytes)
    while ((padded.size() / 4) % 16 != 14) {
        padded.push_back(0x00);
        padded.push_back(0x00);
        padded.push_back(0x00);
        padded.push_back(0x00);
    }

    // 3. Append 64-bit length (big-endian)
    padded.push_back((uint8_t)((bit_len >> 56) & 0xFF));
    padded.push_back((uint8_t)((bit_len >> 48) & 0xFF));
    padded.push_back((uint8_t)((bit_len >> 40) & 0xFF));
    padded.push_back((uint8_t)((bit_len >> 32) & 0xFF));
    padded.push_back((uint8_t)((bit_len >> 24) & 0xFF));
    padded.push_back((uint8_t)((bit_len >> 16) & 0xFF));
    padded.push_back((uint8_t)((bit_len >> 8)  & 0xFF));
    padded.push_back((uint8_t)(bit_len & 0xFF));

    // Now convert the fully padded byte vector to uint32_t blocks
    vector<uint32_t> blocks;
    blocks.reserve(padded.size() / 4);
    for(size_t i = 0; i < padded.size(); i += 4) {
        uint32_t v = (uint32_t(padded[i])   << 24) |
                     (uint32_t(padded[i+1]) << 16) |
                     (uint32_t(padded[i+2]) << 8)  |
                     (uint32_t(padded[i+3]));
        blocks.push_back(v);
    }
    return blocks;
}

HybridParallelHash::State HybridParallelHash::derive_initial_state(const vector<uint32_t>& blocks) const {
    // Based on Li&Dai initialization [cite: 1315, 1318, 1319, 1332]
    // We use the user's variant (XOR, PROD, MUL) for diversity.
    uint64_t sumx = 0;
    uint64_t sumz = 0;
    uint64_t prod_mod = 1;
    size_t N = blocks.size();
    
    if (N == 0) return State{0.5, 0.5, 0.5}; // Handle empty padded msg

    for(size_t i = 0; i < N; i++){
        uint32_t Mi = blocks[i];
        uint32_t r1 = rotate_left32(Mi, (unsigned)((i+1) % 32));
        uint32_t r2 = rotate_left32(Mi, (unsigned)((N - i) % 32));
        
        sumx += uint64_t(Mi ^ r1);
        sumz += (uint64_t(Mi) * uint64_t(r1));
        
        // Use | 1 to prevent multiplication by zero
        uint64_t add = (uint64_t(Mi) + uint64_t(r2)) | 1; 
        
        // Keep 32-bit-ish product
        prod_mod = (prod_mod * add) & 0xFFFFFFFFULL; 
    }
    
    // Scale to [0, 1)
    double x0 = double(sumx & 0xFFFFFFFFULL) / 4294967296.0;
    double y0 = double(prod_mod & 0xFFFFFFFFULL) / 4294967296.0;
    double z0 = double(sumz & 0xFFFFFFFFULL) / 4294967296.0;
    
    return State{x0, y0, z0};
}

HybridParallelHash::State HybridParallelHash::transient_warmup(const State &init, uint32_t iters) const {
    State s = init;
    double r1_local = base_r1, r2_local = base_r2;
    for(uint32_t i = 0; i < iters; i++){
        s = li_dai_step(s, r1_local, r2_local);
        // Recombine occasionally
        if((i & 31) == 0) s = recombine(s, r1_local, r2_local);
    }
    return s;
}

// rotate left
uint32_t HybridParallelHash::rotate_left32(uint32_t v, unsigned k) {
    k &= 31; // k = k % 32
    return (v << k) | (v >> (32 - k));
}

// useful converter (Akhavan style [cite: 106])
uint32_t HybridParallelHash::float_to_uint31(double v) {
    // Ensure v is in [0, 1)
    double frac = v - floor(v);
    if (frac < 0) frac += 1.0;
    // Scale to [0, 2^31)
    return (uint32_t)(frac * 2147483648.0) & 0x7FFFFFFFu;
}

// *** CHANGED ***: Stronger parameter regeneration
double HybridParallelHash::regen_r_from_uint(double r_prev, uint32_t Ci) const {
    // Non-linear mixing: use all 32 bits of Ci
    // Treat r_prev as a 32-bit integer payload
    uint32_t r_bits;
    double r_norm = (r_prev / 4.0); // map [0, 4] -> [0, 1]
    memcpy(&r_norm, &r_bits, sizeof(uint32_t)); // Not portable, but common trick
    
    // Mix
    uint32_t mixed = (Ci ^ K1) + (r_bits ^ K2);
    mixed = (mixed >> 16) ^ (mixed & 0xFFFF); // Fold
    
    // Scale mixed bits to a perturbation in [-0.5, 0.5]
    double perturb = (double(mixed) / 65536.0) - 0.5;
    
    double r_next = r_prev + perturb;

    // Clamp to chaotic range [0.1, 4.0]
    // Use reflection to "bounce" back into the range
    if (r_next > 4.0) {
        r_next = 4.0 - (r_next - 4.0);
    }
    if (r_next < 0.1) {
        r_next = 0.1 + (0.1 - r_next);
    }
    // Final clamp
    return min(4.0, max(0.1, r_next));
}

// *** CHANGED ***: More robust 32-bit combination
vector<uint8_t> HybridParallelHash::combine_matrices(const vector<uint32_t>& A, const vector<uint32_t>& B, size_t out_bytes) {
    size_t k = min(A.size(), B.size());
    vector<uint8_t> out;
    out.reserve(k * 4);

    for(size_t i = 0; i < k; i++){
        uint32_t a = A[i];
        uint32_t b = B[i];
        uint32_t c;

        // Use the conditional combine from Akhavan [cite: 131]
        // but apply it in a full-width 32-bit way
        if((a & 1) == 0) { // Even
            c = (a ^ K1) + b; // [cite: 132]
        } else { // Odd
            // Paper's (a & 0xFF) ^ b is weird[cite: 134].
            // We'll use a more robust mix.
            c = (a + K2) ^ b;
        }
        
        // append c as big-endian bytes
        out.push_back(uint8_t((c >> 24) & 0xFF));
        out.push_back(uint8_t((c >> 16) & 0xFF));
        out.push_back(uint8_t((c >> 8)  & 0xFF));
        out.push_back(uint8_t(c & 0xFF));
    }
    
    // Ensure final length is exactly out_bytes
    if(out.size() > out_bytes) {
        out.resize(out_bytes);
    }
    return out;
}

string HybridParallelHash::to_hex_digest(const vector<uint8_t>& bytes) {
    ostringstream os;
    os << hex << setfill('0');
    for(uint8_t b : bytes) {
        os << setw(2) << (int)b;
    }
    return os.str();
}

// ------------ thread workers -------------
// *** CHANGED ***: Now returns final State
HybridParallelHash::State HybridParallelHash::thread_worker_forward(
    const vector<uint32_t>& blocks,
    State start_state,
    atomic<bool>& errorFlag) {
    
    State s = start_state;
    try {
        double r1_local = base_r1, r2_local = base_r2;
        size_t n = blocks.size();
        
        for(size_t i = 0; i < n; i++){
            // iterate map per-block
            for(uint32_t it = 0; it < per_block_iterations; ++it) {
                s = li_dai_step(s, r1_local, r2_local);
                if((i & 7) == 0) s = recombine(s, r1_local, r2_local);
            }
            
            // Akhavan-style C_i generation [cite: 97, 106]
            uint32_t Ci = float_to_uint31(s.z) ^ blocks[i];
            
            // Akhavan-style parameter regeneration [cite: 111, 112]
            r1_local = regen_r_from_uint(r1_local, Ci);
            r2_local = regen_r_from_uint(r2_local, Ci ^ K1);
        }
    } catch(...) {
        errorFlag.store(true);
    }
    return s; // Return the final state
}

// *** CHANGED ***: Now returns final State
HybridParallelHash::State HybridParallelHash::thread_worker_backward(
    const vector<uint32_t>& blocks,
    State start_state,
    atomic<bool>& errorFlag) {

    State s = start_state;
    try {
        double r1_local = base_r1, r2_local = base_r2;
        size_t n = blocks.size();
        
        for(size_t ii = 0; ii < n; ++ii){
            size_t i = n - 1 - ii; // Process from n-1 down to 0
            
            for(uint32_t it = 0; it < per_block_iterations; ++it) {
                s = li_dai_step(s, r1_local, r2_local);
                if((ii & 7) == 0) s = recombine(s, r1_local, r2_local);
            }
            
            // Akhavan-style C_i generation [cite: 97, 106]
            uint32_t Ci = float_to_uint31(s.z) ^ blocks[i];
            
            // Akhavan-style parameter regeneration [cite: 111, 112]
            r1_local = regen_r_from_uint(r1_local, Ci ^ K2);
            r2_local = regen_r_from_uint(r2_local, Ci);
        }
    } catch(...) {
        errorFlag.store(true);
    }
    return s; // Return the final state
}

// *** NEW ***: Finalization (squeezing) loop
void HybridParallelHash::finalize_and_fill_matrix(
    State final_state,
    vector<uint32_t>& out_matrix,
    double r1_start, double r2_start) {
    
    State s = final_state;
    double r1 = r1_start;
    double r2 = r2_start;
    size_t k = out_matrix.size();

    for(size_t i = 0; i < k; ++i) {
        s = li_dai_step(s, r1, r2);
        s = recombine(s, r1, r2); // Extra mixing

        // Extract 32 bits from the state
        uint32_t x_bits = float_to_uint31(s.x);
        uint32_t y_bits = float_to_uint31(s.y);
        uint32_t z_bits = float_to_uint31(s.z);
        
        out_matrix[i] = (x_bits << 1) ^ y_bits ^ (z_bits << 16);
        
        // Perturb parameters for next round
        r1 = regen_r_from_uint(r1, x_bits ^ y_bits);
        r2 = regen_r_from_uint(r2, y_bits ^ z_bits);
    }
}


// ---------------- cycle detection function (simple hash table approach) -----------
uint64_t HybridParallelHash::detect_cycle_fixed_input(const vector<uint8_t>& message, uint64_t maxIterations) {
    // This is for analysis only, not part of the hash
    auto blocks = pad_and_make_blocks(message);
    State s0 = derive_initial_state(blocks);
    State s = transient_warmup(s0, transient_iterations);
    
    unordered_set<uint64_t> seen;
    // Estimate 1 million entries (approx 8MB)
    seen.reserve(1000000); 

    for(uint64_t iter = 0; iter < maxIterations; ++iter) {
        s = li_dai_step(s, base_r1, base_r2);
        
        // quantize state into 63-bit key
        uint64_t kx = (uint64_t)(s.x * 2097152.0); // 21 bits
        uint64_t ky = (uint64_t)(s.y * 2097152.0); // 21 bits
        uint64_t kz = (uint64_t)(s.z * 2097152.0); // 21 bits
        uint64_t key = (kx << 42) | (ky << 21) | kz;

        if(!seen.insert(key).second) {
            // insertion failed, key was already present
            return iter; // Cycle detected
        }
        
        if (seen.size() > (1ULL << 24)) {
             // Safety break to avoid OOM, ~128MB
             return 0ULL;
        }
    }
    return 0ULL; // not found
}