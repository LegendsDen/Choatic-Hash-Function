// analysis.cpp
// Compile: g++ analysis.cpp HybridParallelHash.cpp -o analysis -std=c++17 -O2 -pthread -lm

#include "HybridParallelHash.hpp"
#include <bits/stdc++.h>
using namespace std;

static std::random_device rd;
static std::mt19937_64 rng(rd());

// helper: convert hex string (lowercase) to vector<uint8_t>
static vector<uint8_t> hex_to_bytes(const string &hex)
{
    vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        unsigned int byte;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        ss >> byte;
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}

// helper: hex digest (string) -> bitset vector<bool> of length bits
static vector<int> hex_to_bits(const string &hex, size_t bits)
{
    vector<uint8_t> bytes = hex_to_bytes(hex);
    vector<int> bits_out(bits);
    for (size_t i = 0; i < bits && i / 8 < bytes.size(); ++i)
    {
        uint8_t b = bytes[i / 8];
        int bit = (b >> (7 - (i % 8))) & 1;
        bits_out[i] = bit;
    }
    return bits_out;
}

// hamming distance between two bit vectors
static int hamming_distance(const vector<int> &a, const vector<int> &b)
{
    int n = min(a.size(), b.size());
    int dist = 0;
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i])
            ++dist;
    return dist;
}

// random message generator: random length between minlen..maxlen
static vector<uint8_t> random_message(size_t minlen, size_t maxlen)
{
    uniform_int_distribution<size_t> len_dist(minlen, maxlen);
    size_t len = len_dist(rng);
    vector<uint8_t> v(len);
    for (size_t i = 0; i < len; ++i)
        v[i] = static_cast<uint8_t>(rng() & 0xFF);
    return v;
}

// flip a single bit in the message at random position
static vector<uint8_t> flip_one_bit(const vector<uint8_t> &msg)
{
    if (msg.empty())
    {
        vector<uint8_t> v = {0x00};
        v[0] ^= 0x80; // flip MSB
        return v;
    }
    vector<uint8_t> out = msg;
    uniform_int_distribution<size_t> posd(0, out.size() * 8 - 1);
    size_t bitpos = posd(rng);
    size_t bytepos = bitpos / 8;
    int bit = bitpos % 8;
    // flip bit (we treat MSB-first ordering consistent with digest byte order)
    out[bytepos] ^= (1u << (7 - bit));
    return out;
}

int main(int argc, char **argv)
{
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // Parameters (tuneable)
    const size_t DIGEST_BITS = 256;          // test digest length
    const size_t AVALANCHE_SAMPLES = 5000;   // number of random messages for avalanche test
    const size_t BIAS_SAMPLES = 20000;       // number of messages for bit-bias test
    const size_t COLLISION_SAMPLES = 200000; // optional, for truncated-collision test
    const size_t COLLISION_TRUNC_BITS = 128; // truncated size for collision test

    cout << "Starting analysis tests\n";

    HybridParallelHash hasher(3.91, 3.99, 1500, 1000);

    // ---------- Avalanche test ----------
    cout << "Avalanche: generating " << AVALANCHE_SAMPLES << " samples...\n";
    std::ofstream aval("avalanche.csv");
    aval << "hamming\n";
    for (size_t i = 0; i < AVALANCHE_SAMPLES; ++i)
    {
        vector<uint8_t> m = random_message(1, 128); // messages 1..128 bytes
        string hex1 = hasher.digest(m, DIGEST_BITS);
        vector<uint8_t> m2 = flip_one_bit(m);
        string hex2 = hasher.digest(m2, DIGEST_BITS);
        vector<int> bits1 = hex_to_bits(hex1, DIGEST_BITS);
        vector<int> bits2 = hex_to_bits(hex2, DIGEST_BITS);
        int dist = hamming_distance(bits1, bits2);
        aval << dist << "\n";
        if ((i + 1) % 500 == 0)
            cerr << "."; // progress
    }
    aval.close();
    cerr << "\nAvalanche test done. CSV: avalanche.csv\n";

    // ---------- Bit-bias test ----------
    cout << "Bit-bias: generating " << BIAS_SAMPLES << " samples...\n";
    vector<uint64_t> ones_count(DIGEST_BITS, 0);
    for (size_t i = 0; i < BIAS_SAMPLES; ++i)
    {
        // generate deterministic messages (counter + random salt)
        std::ostringstream ss;
        ss << "bias-" << i << "-" << (rng() & 0xffffffffULL);
        string s = ss.str();
        vector<uint8_t> m(s.begin(), s.end());
        string hex = hasher.digest(m, DIGEST_BITS);
        vector<int> bits = hex_to_bits(hex, DIGEST_BITS);
        for (size_t b = 0; b < DIGEST_BITS; ++b)
            if (bits[b])
                ones_count[b]++;
        if ((i + 1) % 2000 == 0)
            cerr << ".";
    }
    cerr << "\nBit-bias test done. CSV: bias.csv\n";
    ofstream biasf("bias.csv");
    biasf << "bit_index,ones,total,sample_fraction\n";
    for (size_t b = 0; b < DIGEST_BITS; ++b)
    {
        biasf << b << "," << ones_count[b] << "," << BIAS_SAMPLES << "," << (double)ones_count[b] / (double)BIAS_SAMPLES << "\n";
    }
    biasf.close();

    // ---------- Collision test (logical truncation to 32 bits safely) ----------
    cout << "Collision test (logical truncation to 32 bits). Samples: " << COLLISION_SAMPLES << "\n";
    unordered_map<uint32_t, size_t> seen;
    set<string> input;
    set<string> full_hashes;
    size_t collisions = 0;
    size_t report_every = 50000;

    for (size_t i = 0; i < COLLISION_SAMPLES; ++i)
    {
        // Build pseudo-random short messages
        std::ostringstream ss;
        ss << "coll-" << i << "-" << (rng() & 0xffffffffULL);
        string msg = ss.str();
        input.insert(msg);
        vector<uint8_t> m(msg.begin(), msg.end());

        // Always hash with L = 128 (valid), then truncate manually to 32 bits
        string hex_full = hasher.digest(m, 128);
        full_hashes.insert(hex_full);
        vector<uint8_t> bytes_full = hex_to_bytes(hex_full);

        if (bytes_full.empty())
            continue; // safety check

        // Combine first 4 bytes into 32-bit integer
        uint32_t val = 0;
        for (size_t k = 0; k < bytes_full.size() && k < 4; ++k)
            val = (val << 8) | bytes_full[k];

        // Record collisions
        auto it = seen.find(val);
        if (it != seen.end())
            collisions++;
        else
            seen[val] = i;

        if ((i + 1) % report_every == 0)
            cerr << ".";
    }
    cout << input.size() << " unique input seen out of " << COLLISION_SAMPLES << " samples.\n";
    cout << full_hashes.size() << " unique full hashes seen out of " << COLLISION_SAMPLES << " samples.\n";

    cerr << "\nCollision test done. collisions=" << collisions << " (out of " << COLLISION_SAMPLES << ")\n";
    ofstream colf("collisions.csv");
    colf << "collisions,total_samples\n"
         << collisions << "," << COLLISION_SAMPLES << "\n";
    colf.close();

    cout << "All tests finished. Files generated: avalanche.csv, bias.csv, collisions.csv\n";
    return 0;
}
