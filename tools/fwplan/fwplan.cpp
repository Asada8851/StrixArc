// fwplan: print the PSP LOAD_IP_FW plan (file, byte offset, size, GFX_FW_TYPE) for one GC generation's
// linux-firmware blobs, from their own headers (src/amd/rdna3_ucode_extract). Replaces the offsets and sizes that
// bringup5.sh hardcodes for gc_11_0_1 so the same script can load another chip's files (gc_11_5_0 on Strix Point).
//   fwplan <firmware dir> <gc prefix> <sdma file>     e.g.  fwplan ~/gpu/fw gc_11_5_0 sdma_6_1_0.bin
#include "rdna3_ucode_extract.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace rdna3;

static bool slurp(const std::string &p, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: fwplan <dir> <gc prefix> <sdma file>\n"); return 2; }
    const std::string dir = argv[1], gc = argv[2], sdma = argv[3];
    // Same files and order as bringup5.sh; mes_2 is the scheduler pipe, mes1 the KIQ pipe.
    const struct { std::string file; uint64_t kind; } files[] = {
        {gc + "_pfp.bin", kHostFwFile_CP_PFP}, {gc + "_me.bin", kHostFwFile_CP_ME},
        {gc + "_mec.bin", kHostFwFile_CP_MEC}, {sdma, kHostFwFile_SDMA},
        {gc + "_mes_2.bin", kHostFwFile_MES},  {gc + "_mes1.bin", kHostFwFile_MES_KIQ},
        {gc + "_imu.bin", kHostFwFile_IMU},    {gc + "_rlc.bin", kHostFwFile_RLC},
    };
    int rc = 0;
    for (const auto &f : files) {
        std::vector<uint8_t> bin;
        if (!slurp(dir + "/" + f.file, bin)) { std::fprintf(stderr, "fwplan: cannot read %s\n", f.file.c_str()); rc = 1; continue; }
        UcodePayload p[kMaxUcodePayloadsPerFile];
        uint32_t n = rdna3_ucode_extract(f.kind, bin.data(), bin.size(), p);
        if (!n) { std::fprintf(stderr, "fwplan: no payloads in %s\n", f.file.c_str()); rc = 1; }
        for (uint32_t i = 0; i < n; i++)
            std::printf("%s %u %u %u\n", f.file.c_str(), p[i].offset_bytes, p[i].size_bytes, p[i].fw_type);
        // mesboot programs CP_MES_PRGRM_CNTR_START from the header's uc start address (0xF0005000 on Phoenix)
        if ((f.kind == kHostFwFile_MES || f.kind == kHostFwFile_MES_KIQ) && bin.size() >= sizeof(mes_firmware_header_v1_0)) {
            auto *h = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin.data());
            std::printf("# %s uc_start 0x%08x%08x data_start 0x%08x%08x\n", f.file.c_str(), h->mes_uc_start_addr_hi,
                        h->mes_uc_start_addr_lo, h->mes_data_start_addr_hi, h->mes_data_start_addr_lo);
        }
    }
    return rc;
}
