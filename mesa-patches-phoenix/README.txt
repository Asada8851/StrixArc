Phoenix (Radeon 780M, gfx1103) patches on top of Cobalt64's RADV Darwin port.

Base: Mesa f5cb8ee032adabef599ae892ec4e42d796b84da9 + Cobalt64's mesa-patches/0001..0009 (the RADV Darwin port,
MIT: https://github.com/Almosst-DEV/Cobalt64, directory mesa-patches).
Apply these after them (git am, 0001..0011).  0001..0003 are what tools/mesa_phoenix_canned.py,
tools/mesa_phoenix_identity.py and tools/mesa_phoenix_service.py produce (0003: open the PhxBringup service).
0004 answers AMDGPU_INFO_TIMESTAMP from SMUIO GOLDEN_TSC (needs PhxBringup build 31): Metal GPUStartTime/GPUEndTime.
0005 opens PhxBringup through NMXAccel when sandboxed (needs the PhxAccel proxy): Metal in WebKit's GPU process.
0006 unrolls nested loops without a full optimization round per level: a new CoreAnimation compositor shader
     variant compiles in ~1 s instead of ~2 s (same code), so WindowServer stalls less on first use.

Build on the Mac (Tahoe 26.6.2, CLT 16.2, Python 3.14 venv with meson/ninja/mako/pyyaml/packaging,
glslangValidator built from source into ~/gpu/prefix):
  meson setup build --buildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
    -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
    -Dgles2=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
    -Dbuild-tests=false -Dvulkan-layers= -Dtools=
  ninja -C build

Check (no GPU, in-process mock kext):
  VK_ICD_FILENAMES=build/src/amd/vulkan/radeon_devenv_icd.x86_64.json RADV_DARWIN_MOCK=1 tools/vkprobe/vkprobe
  -> "AMD Radeon 780M Graphics (RADV PHOENIX) 1002:1900", device created, compute pipeline compiled (ACO gfx1103).
0007 is GC 11.5.0 and the build-time default canned device (strix1 for Strix Point); 0008 names strix1
     "AMD Radeon 880M/890M Graphics" (the RADV the 880M ran from 2026-10-25 had it, unrecorded until now).
0009 is VCN 4.0.5 behind PhxBringup 43+: the unified queue (HW_IP_INFO VCN_ENC), the firmware version and the
     video caps, so RADV offers Vulkan Video decode once `mesboot vcn adopt` has handed the ring to the kext.
0010 advertises one GFX and one compute ring, the kext's only ones: with the Linux masks RADV offered four compute
     queues and FFmpeg's frame downloads on queues 1..3 were rejected (EINVAL), losing the device (round 53).
0011 takes a freed BO out of the backend's table before the kext frees its handle: another thread's allocation
     could get the handle back in between and fail ("returned BO handle twice"), leaking the kext BO (round 56).

Build on Linux for macOS x86_64 (the toolchain of tools/cross, plus llvm-ar, meson >= 1.4, glslangValidator and Python
with mako/pyyaml/packaging):
  git clone https://gitlab.freedesktop.org/mesa/mesa.git && cd mesa && git checkout f5cb8ee032ad
  git am <Cobalt64>/mesa-patches/0*.patch <StrixArc>/mesa-patches-phoenix/0*.patch
  MESA_PY=<venv>/bin tools/cross/build-radv.sh <env.sh> <mesa tree>
It sets ACD_DEFAULT_CANNED="strix1"; MESA_VIDEO_CODECS=h264dec,h265dec adds the decoders Mesa leaves out by default.
Checked 2026-10-27 against the deployed 2026-10-25 dylib: the same 20 exports and libraries, the same messages.
