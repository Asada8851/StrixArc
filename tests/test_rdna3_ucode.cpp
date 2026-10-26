//
//  test_rdna3_ucode.cpp — Unit tests and adversarial fuzzer for RDNA 3 firmware extraction.
//  Builds cleanly under Clang with -fsanitize=address,undefined.
//
#include "../src/amd/rdna3_ucode_extract.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <cstring>

using namespace rdna3;

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << path << std::endl;
        return {};
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char *>(buffer.data()), size)) {
        return {};
    }
    return buffer;
}

// Firmware fixtures are AMD's redistributable linux-firmware blobs for Phoenix
// (GC 11.0.1, SDMA 6.0.1, DCN 3.1.4). They are not committed: run
// `make fixtures` (tools/fetch_test_firmware.sh) to download them.
static const char *kFixtureDir = "tests/fixtures/phoenix/";

static std::vector<uint8_t> read_fixture(const std::string &name) {
    return read_file(std::string(kFixtureDir) + name);
}

static bool fixtures_present() {
    std::ifstream probe(std::string(kFixtureDir) + "gc_11_0_1_rlc.bin", std::ios::binary);
    return probe.is_open();
}

// Strix Point (GC 11.5.0, SDMA 6.1.0) blobs, fetched next to Phoenix's by `make fixtures`.
static const char *kStrixDir = "tests/fixtures/strix/";

static std::vector<uint8_t> read_strix(const std::string &name) {
    return read_file(std::string(kStrixDir) + name);
}

static bool strix_fixtures_present() {
    std::ifstream probe(std::string(kStrixDir) + "gc_11_5_0_rlc.bin", std::ios::binary);
    return probe.is_open();
}

static void test_null_and_zero_inputs() {
    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint8_t dummy[128] = {};

    assert(rdna3_ucode_extract(kHostFwFile_SDMA, nullptr, 128, out) == 0);
    assert(rdna3_ucode_extract(kHostFwFile_SDMA, dummy, 0, out) == 0);
    assert(rdna3_ucode_extract(kHostFwFile_SDMA, dummy, 128, nullptr) == 0);
    assert(rdna3_ucode_extract(0x9999, dummy, 128, out) == 0); // Unknown host fw type
    std::cout << "[PASS] test_null_and_zero_inputs" << std::endl;
}

static void test_phoenix_rlc() {
    auto bin = read_fixture("gc_11_0_1_rlc.bin");
    assert(!bin.empty());

    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint32_t count = rdna3_ucode_extract(kHostFwFile_RLC, bin.data(), bin.size(), out);
    assert(count > 0);

    for (uint32_t i = 0; i < count; ++i) {
        assert(out[i].size_bytes > 0);
        assert(static_cast<uint64_t>(out[i].offset_bytes) + out[i].size_bytes <= bin.size());
    }

    // Critical hardware requirement: RLC_G must be the LAST payload
    assert(out[count - 1].fw_type == static_cast<uint32_t>(PSPGfxFwType::RLC_G));
    std::cout << "[PASS] test_phoenix_rlc (extracted " << count << " payloads, RLC_G is last)" << std::endl;
}

// Phoenix (GC 11.0.1) ships F32 CP microcode with a v1.0 header: one payload each,
// loaded as CP_PFP / CP_ME / CP_MEC (matches tools/phxctl/bringup5.sh).
static void check_cp_file_in(const std::vector<uint8_t> &bin, uint64_t host_type, PSPGfxFwType legacy_type) {
    assert(!bin.empty());
    auto *common = reinterpret_cast<const common_firmware_header *>(bin.data());
    assert(common->header_version_major == 1);

    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint32_t count = rdna3_ucode_extract(host_type, bin.data(), bin.size(), out);
    assert(count == 1);
    assert(out[0].fw_type == static_cast<uint32_t>(legacy_type));
    assert(out[0].offset_bytes == common->ucode_array_offset_bytes);
    assert(out[0].size_bytes == common->ucode_size_bytes);
    assert(static_cast<uint64_t>(out[0].offset_bytes) + out[0].size_bytes <= bin.size());
}

static void check_cp_file(const char *name, uint64_t host_type, PSPGfxFwType legacy_type) {
    check_cp_file_in(read_fixture(name), host_type, legacy_type);
}

static void test_phoenix_cp() {
    check_cp_file("gc_11_0_1_pfp.bin", kHostFwFile_CP_PFP, PSPGfxFwType::CP_PFP);
    check_cp_file("gc_11_0_1_me.bin",  kHostFwFile_CP_ME,  PSPGfxFwType::CP_ME);
    check_cp_file("gc_11_0_1_mec.bin", kHostFwFile_CP_MEC, PSPGfxFwType::CP_MEC);
    std::cout << "[PASS] test_phoenix_cp (F32 v1.0: PFP, ME, MEC = 1 payload each)" << std::endl;
}

// RS64 (header v2.0, Navi 3x) layout, checked on a synthetic header since no
// Phoenix firmware uses it: ucode + 2 stacks (PFP/ME) or 4 stacks (MEC).
static void test_cp_rs64_synthetic() {
    std::vector<uint8_t> bin(4096, 0);
    auto *hdr = reinterpret_cast<gfx_firmware_header_v2_0 *>(bin.data());
    hdr->header.size_bytes = static_cast<uint32_t>(bin.size());
    hdr->header.header_size_bytes = sizeof(gfx_firmware_header_v2_0);
    hdr->header.header_version_major = 2;
    hdr->header.ucode_array_offset_bytes = 256;
    hdr->ucode_size_bytes = 1024;
    hdr->data_offset_bytes = 2048;
    hdr->data_size_bytes = 512;

    UcodePayload out[kMaxUcodePayloadsPerFile];
    assert(rdna3_ucode_extract(kHostFwFile_CP_PFP, bin.data(), bin.size(), out) == 3);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::RS64_PFP));
    assert(out[1].fw_type == static_cast<uint32_t>(PSPGfxFwType::RS64_PFP_P0_STACK));
    assert(out[2].fw_type == static_cast<uint32_t>(PSPGfxFwType::RS64_PFP_P1_STACK));
    assert(rdna3_ucode_extract(kHostFwFile_CP_ME, bin.data(), bin.size(), out) == 3);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::RS64_ME));
    assert(rdna3_ucode_extract(kHostFwFile_CP_MEC, bin.data(), bin.size(), out) == 5);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::RS64_MEC));
    assert(out[4].fw_type == static_cast<uint32_t>(PSPGfxFwType::RS64_MEC_P3_STACK));
    std::cout << "[PASS] test_cp_rs64_synthetic (PFP=3, ME=3, MEC=5 payloads)" << std::endl;
}

static void test_phoenix_sdma() {
    auto bin = read_fixture("sdma_6_0_1.bin");
    assert(!bin.empty());

    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint32_t count = rdna3_ucode_extract(kHostFwFile_SDMA, bin.data(), bin.size(), out);
    assert(count == 2);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::SDMA_UCODE_TH0));
    assert(out[0].size_bytes == 17408);
    assert(out[1].fw_type == static_cast<uint32_t>(PSPGfxFwType::SDMA_UCODE_TH1));
    assert(out[1].size_bytes == 16896);

    for (uint32_t i = 0; i < count; ++i) {
        assert(static_cast<uint64_t>(out[i].offset_bytes) + out[i].size_bytes <= bin.size());
    }
    std::cout << "[PASS] test_phoenix_sdma (TH0=17408, TH1=16896)" << std::endl;
}

static void test_phoenix_imu() {
    auto bin = read_fixture("gc_11_0_1_imu.bin");
    assert(!bin.empty());

    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint32_t count = rdna3_ucode_extract(kHostFwFile_IMU, bin.data(), bin.size(), out);
    assert(count == 2);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::IMU_I));
    assert(out[1].fw_type == static_cast<uint32_t>(PSPGfxFwType::IMU_D));
    assert(out[1].offset_bytes == out[0].offset_bytes + out[0].size_bytes);

    for (uint32_t i = 0; i < count; ++i) {
        assert(static_cast<uint64_t>(out[i].offset_bytes) + out[i].size_bytes <= bin.size());
    }
    std::cout << "[PASS] test_phoenix_imu (IMU_I + IMU_D contiguous)" << std::endl;
}

static void test_phoenix_dmcub() {
    auto bin = read_fixture("dcn_3_1_4_dmcub.bin");
    assert(!bin.empty());

    UcodePayload out[kMaxUcodePayloadsPerFile];
    uint32_t count = rdna3_ucode_extract(kHostFwFile_DMCUB, bin.data(), bin.size(), out);
    assert(count == 1);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::DMUB));
    assert(static_cast<uint64_t>(out[0].offset_bytes) + out[0].size_bytes <= bin.size());
    std::cout << "[PASS] test_phoenix_dmcub (DMUB=51 extracted)" << std::endl;
}

static void test_adversarial_truncation() {
    auto bin = read_fixture("sdma_6_0_1.bin");
    assert(!bin.empty());

    UcodePayload out[kMaxUcodePayloadsPerFile];
    // Truncate to every size from 0 up to full size
    for (size_t sz = 0; sz < 128; ++sz) {
        uint32_t n = rdna3_ucode_extract(kHostFwFile_SDMA, bin.data(), sz, out);
        if (sz < sizeof(sdma_firmware_header_v2_0)) {
            assert(n == 0);
        }
    }
    std::cout << "[PASS] test_adversarial_truncation" << std::endl;
}

static void test_fuzzer() {
    auto original = read_fixture("gc_11_0_1_rlc.bin");
    assert(!original.empty());

    UcodePayload out[kMaxUcodePayloadsPerFile];
    // Run 5000 mutation iterations with random bit flips
    uint32_t lcg = 0x12345678;
    auto rnd = [&]() -> uint32_t {
        lcg = lcg * 1664525u + 1013904223u;
        return lcg;
    };

    for (int iter = 0; iter < 5000; ++iter) {
        std::vector<uint8_t> corrupted = original;
        size_t mutate_count = 1 + (rnd() % 16);
        for (size_t m = 0; m < mutate_count; ++m) {
            size_t idx = rnd() % corrupted.size();
            corrupted[idx] ^= static_cast<uint8_t>(rnd() & 0xFF);
        }

        uint64_t fw_types[] = {
            kHostFwFile_SDMA, kHostFwFile_RLC, kHostFwFile_IMU,
            kHostFwFile_CP_PFP, kHostFwFile_CP_ME, kHostFwFile_CP_MEC,
            kHostFwFile_DMCUB, kHostFwFile_MES, kHostFwFile_MES_KIQ
        };
        uint64_t hostFw = fw_types[rnd() % 9];

        uint32_t count = rdna3_ucode_extract(hostFw, corrupted.data(), corrupted.size(), out);
        assert(count <= kMaxUcodePayloadsPerFile);
        for (uint32_t i = 0; i < count; ++i) {
            // Guarantee all returned payloads are strictly within bounds!
            assert(out[i].size_bytes > 0);
            assert(static_cast<uint64_t>(out[i].offset_bytes) + out[i].size_bytes <= corrupted.size());
        }
    }
    std::cout << "[PASS] test_fuzzer (5000 random mutations safely processed)" << std::endl;
}

// MES: ucode + data (stack), typed CP_MES/MES_STACK for the scheduler pipe and CP_MES_KIQ/MES_KIQ_STACK for the KIQ.
static void test_mes_synthetic() {
    std::vector<uint8_t> bin(4096, 0);
    auto *h = reinterpret_cast<mes_firmware_header_v1_0 *>(bin.data());
    h->header.size_bytes = 4096;
    h->header.header_size_bytes = sizeof(mes_firmware_header_v1_0);
    h->header.header_version_major = 1;
    h->mes_ucode_offset_bytes = 256;
    h->mes_ucode_size_bytes = 1024;
    h->mes_ucode_data_offset_bytes = 1280;
    h->mes_ucode_data_size_bytes = 2048;

    UcodePayload out[kMaxUcodePayloadsPerFile];
    assert(rdna3_ucode_extract(kHostFwFile_MES, bin.data(), bin.size(), out) == 2);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::CP_MES));
    assert(out[0].offset_bytes == 256 && out[0].size_bytes == 1024);
    assert(out[1].fw_type == static_cast<uint32_t>(PSPGfxFwType::MES_STACK));
    assert(out[1].offset_bytes == 1280 && out[1].size_bytes == 2048);

    assert(rdna3_ucode_extract(kHostFwFile_MES_KIQ, bin.data(), bin.size(), out) == 2);
    assert(out[0].fw_type == static_cast<uint32_t>(PSPGfxFwType::CP_MES_KIQ));
    assert(out[1].fw_type == static_cast<uint32_t>(PSPGfxFwType::MES_KIQ_STACK));

    // data running past the end of the file is dropped, the header alone is rejected
    h->mes_ucode_data_size_bytes = 4096;
    assert(rdna3_ucode_extract(kHostFwFile_MES, bin.data(), bin.size(), out) == 1);
    assert(rdna3_ucode_extract(kHostFwFile_MES, bin.data(), sizeof(mes_firmware_header_v1_0) - 1, out) == 0);
    h->header.header_version_major = 2;
    assert(rdna3_ucode_extract(kHostFwFile_MES, bin.data(), bin.size(), out) == 0);
    std::cout << "[PASS] test_mes_synthetic (scheduler and KIQ typing, bounds)" << std::endl;
}

// Every payload of one MES file: ucode then stack, both inside the file, start addresses where mesboot expects them.
static void check_mes_file(const std::vector<uint8_t> &bin, bool kiq) {
    assert(!bin.empty());
    UcodePayload out[kMaxUcodePayloadsPerFile];
    assert(rdna3_ucode_extract(kiq ? kHostFwFile_MES_KIQ : kHostFwFile_MES, bin.data(), bin.size(), out) == 2);
    assert(out[0].fw_type == static_cast<uint32_t>(kiq ? PSPGfxFwType::CP_MES_KIQ : PSPGfxFwType::CP_MES));
    assert(out[1].fw_type == static_cast<uint32_t>(kiq ? PSPGfxFwType::MES_KIQ_STACK : PSPGfxFwType::MES_STACK));
    for (int i = 0; i < 2; i++)
        assert(static_cast<uint64_t>(out[i].offset_bytes) + out[i].size_bytes <= bin.size());
    auto *h = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin.data());
    // tools/phxctl/mesboot.c programs CP_MES_PRGRM_CNTR_START from 0xFFFFFFFFF0005000 >> 2
    assert(h->mes_uc_start_addr_lo == 0xF0005000u && h->mes_uc_start_addr_hi == 0xFFFFFFFFu);
}

static void test_phoenix_mes() {
    check_mes_file(read_fixture("gc_11_0_1_mes_2.bin"), false);
    check_mes_file(read_fixture("gc_11_0_1_mes1.bin"), true);
    std::cout << "[PASS] test_phoenix_mes (mes_2 = scheduler, mes1 = KIQ)" << std::endl;
}

// Strix Point's files go through the same extractors as Phoenix's, with the payload types bringup5.sh's slot map
// knows; the RLC differs in the size of its RLCP/RLCV section, which moves every later offset.
static void test_strix_files() {
    UcodePayload out[kMaxUcodePayloadsPerFile];
    auto rlc = read_strix("gc_11_5_0_rlc.bin");
    uint32_t n = rdna3_ucode_extract(kHostFwFile_RLC, rlc.data(), rlc.size(), out);
    assert(n == 6);
    assert(out[n - 1].fw_type == static_cast<uint32_t>(PSPGfxFwType::RLC_G));
    for (uint32_t i = 0; i < n; i++)
        assert(static_cast<uint64_t>(out[i].offset_bytes) + out[i].size_bytes <= rlc.size());
    check_cp_file_in(read_strix("gc_11_5_0_pfp.bin"), kHostFwFile_CP_PFP, PSPGfxFwType::CP_PFP);
    check_cp_file_in(read_strix("gc_11_5_0_me.bin"), kHostFwFile_CP_ME, PSPGfxFwType::CP_ME);
    check_cp_file_in(read_strix("gc_11_5_0_mec.bin"), kHostFwFile_CP_MEC, PSPGfxFwType::CP_MEC);
    auto sdma = read_strix("sdma_6_1_0.bin");
    assert(rdna3_ucode_extract(kHostFwFile_SDMA, sdma.data(), sdma.size(), out) == 2);
    auto imu = read_strix("gc_11_5_0_imu.bin");
    assert(rdna3_ucode_extract(kHostFwFile_IMU, imu.data(), imu.size(), out) == 2);
    check_mes_file(read_strix("gc_11_5_0_mes_2.bin"), false);
    check_mes_file(read_strix("gc_11_5_0_mes1.bin"), true);
    std::cout << "[PASS] test_strix_files (GC 11.5.0 / SDMA 6.1.0: RLC x6, CP x3, SDMA x2, IMU x2, MES x2+2)" << std::endl;
}

int main() {
    std::cout << "=== Running RDNA3 / GFX1103 Firmware Extractor Tests ===" << std::endl;
    test_null_and_zero_inputs();
    test_cp_rs64_synthetic();
    test_mes_synthetic();
    if (!fixtures_present()) {
        if (std::getenv("REQUIRE_FIXTURES")) {
            std::cerr << "[FAIL] firmware fixtures missing in " << kFixtureDir
                      << " (run `make fixtures`)" << std::endl;
            return 1;
        }
        std::cout << "[SKIP] firmware-based tests: no fixtures in " << kFixtureDir
                  << " (run `make fixtures` to enable)" << std::endl;
        return 0;
    }
    test_phoenix_rlc();
    test_phoenix_cp();
    test_phoenix_sdma();
    test_phoenix_imu();
    test_phoenix_dmcub();
    test_phoenix_mes();
    if (strix_fixtures_present()) {
        test_strix_files();
    } else if (std::getenv("REQUIRE_FIXTURES")) {
        std::cerr << "[FAIL] firmware fixtures missing in " << kStrixDir << " (run `make fixtures`)" << std::endl;
        return 1;
    } else {
        std::cout << "[SKIP] Strix Point firmware tests: no fixtures in " << kStrixDir << std::endl;
    }
    test_adversarial_truncation();
    test_fuzzer();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
