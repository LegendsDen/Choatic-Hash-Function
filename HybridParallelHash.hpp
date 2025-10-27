#pragma once

#include<bits/stdc++.h>
using namespace std;

// Define M_PI if not available (e.g., Windows)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class HybridParallelHash {
public:
    // Constructor: supply optional key parameters
    HybridParallelHash(double r1 = 3.7, double r2 = 2.9,
                       uint32_t transient_iters = 1500, uint32_t warmup_for_T1 = 1000);

    // compute digest for input message (bytes). L is digest bits (128/256/512)
    string digest(const vector<uint8_t>& message, size_t L = 256);

    // convenience: digest from string (UTF-8 bytes)
    string digest(const string &msg, size_t L = 256);

    // Public hooks for analysis:
    uint64_t detect_cycle_fixed_input(const vector<uint8_t>& message, uint64_t maxIterations);

private:
    // --- chaotic map state and params ---
    struct State { double x, y, z; };
    
    // Li & Dai coupled map iteration (one step) 
    State li_dai_step(const State &s, double r1_local, double r2_local) const;
    // Li & Dai optional nonlinear recombination [cite: 1341-1343]
    State recombine(const State &s, double r1_local, double r2_local) const;

    // --- initialization functions ---
    // *** CHANGED ***: Added proper Merkle-Damgård padding
    static vector<uint32_t> pad_and_make_blocks(const vector<uint8_t>& message);
    
    // derive initial x0,y0,z0 from blocks (per Li & Dai style [cite: 1315])
    State derive_initial_state(const vector<uint32_t>& blocks) const;

    // warm up transient to remove initial non-chaotic behavior
    State transient_warmup(const State &init, uint32_t iters) const;

    // --- thread worker ---
    // *** CHANGED ***: Worker now just processes blocks and returns the final state
    State thread_worker_forward(const vector<uint32_t>& blocks,
                                State start_state,
                                atomic<bool>& errorFlag);
    State thread_worker_backward(const vector<uint32_t>& blocks,
                                 State start_state,
                                 atomic<bool>& errorFlag);

    // *** NEW ***: Finalization (squeezing) loop per Akhavan 
    void finalize_and_fill_matrix(State final_state,
                                  vector<uint32_t>& out_matrix,
                                  double r1_start, double r2_start);

    // *** CHANGED ***: Stronger parameter regeneration function
    double regen_r_from_uint(double r_prev, uint32_t Ci) const;

    // --- combine & extract ---
    // *** CHANGED ***: More robust 32-bit combination
    static vector<uint8_t> combine_matrices(const vector<uint32_t>& A, const vector<uint32_t>& B, size_t out_bytes);
    static string to_hex_digest(const vector<uint8_t>& bytes);

    // --- utility ---
    static uint32_t float_to_uint31(double v); // [cite: 106]
    static uint32_t rotate_left32(uint32_t v, unsigned k);

private:
    double base_r1;
    double base_r2;
    uint32_t transient_iterations;
    uint32_t warmup_for_T1;
    uint32_t per_block_iterations = 1; // how many map iter per block
    
    // Magic constants for mixing (from SHA-2)
    static constexpr uint32_t K1 = 0x9E3779B9u;
    static constexpr uint32_t K2 = 0x6A09E667u;
};