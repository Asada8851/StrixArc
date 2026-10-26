# Рабочий Linux amdgpu (Arch 2026.10.01, ядро 7.2.7) на Firebat U6C, 2026-10-19
regtrace_init.txt.xz — ftrace amdgpu_device_wreg/rreg при unbind/bind драйвера (формат: время, r/w, reg(dword), value).
linux_regdump_gfxon.txt — регистры работающего GPU (GFXOFF отключён через debugfs).
Ключевое: IC_BASE (PFP/ME/CPC) в Linux тоже =0; MEC_RS64_CNTL=0x40000000 (halt) — так и должно быть для legacy fw.
Порядок: PSP fw (25 команд) -> SMU: 0x3,0x2, 0x16(EnableGfxImu ПАРАМЕТР=1), 0x7,0x22,0xd(0x80),0xe(fbd000),0x12.. ->
gfx init: GRBM_GFX_INDEX=0x40000000/0x40000100/0xe0000000, CSIB(0xa987=lo,0xa988=0x80,0xa989=0x3c0), SRM(0xcc80=3),
CP_MEC_CNTL=0, CP_ME_CNTL=0x01000000 (CE_HALT ОСТАЁТСЯ 1!), CP_MES_CNTL=0x40030000 -> PC start -> 0x0c000000,
RLC_CP_SCHEDULERS=0x30e8, MES KIQ HQD (me3 pipe1), CP_MAX_CONTEXT=7, CP_DEVICE_ID=1, doorbell range 0x305a=0x458/0x305b=0x7f8.
CP_RB0_* в Linux НЕ программируются (gfx очередь мапится MES ADD_QUEUE).
