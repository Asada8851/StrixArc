# Этап 3: аппаратное ускорение Radeon 780M (Phoenix / Hawk Point, gfx1103) в macOS

Дата: 2026-10-18. Цель: Metal на 780M, рабочий стол композитится через GPU.

## Исходная точка

- Ноутбук: Firebat U6C, Ryzen 7 H 255, iGPU `1002:1900` rev `0xBA`, VBIOS `113-PHXGENERIC-001`
  (`AMD_PHOENIX_GENERIC`). UMA, BAR0 256 MiB, BAR2 doorbells 2 MiB, BAR5 MMIO.
- macOS Sonoma 14.8.9, графика на кадровом буфере прошивки (IONDRVSupport), без Metal.
- `src/amd/*` прошлого агента проверен только на мок-регистрах и использует версии блоков Navi 33
  (GC 11.0.3, SDMA 6.0.3) — для этого GPU неверно.
- **Подтверждено с железа 2026-10-18 (шаг A, `PhxProbe.kext`):** IP discovery v3, 39 блоков;
  GC 11.0.1, SDMA0/LSDMA 6.0.1, MP0(PSP)/MP1(SMU) 13.0.4, DMU(DCN) 3.1.4, MMHUB/ATHUB 3.0.1, HDP 5.2.1,
  OSSSYS 6.0.1, NBIF 7.7.0, VCN 4.0.2, UMC 11.5.0, SMUIO 13.0.7. UMA carve-out 6144 MiB
  (`RCC_CONFIG_MEMSIZE`), таблица на 6 GiB − 64 KiB, `MP0_C2PMSG_33` = 0x80000000.
  Дамп: `hardware/dumps/firebat_u6c/ip_discovery.bin` (+ `.txt`).

## Эталонные проекты

| Проект | Что взять | Лицензия |
|---|---|---|
| Cobalt64 (`reference/Cobalt64`) | kext инициализации AMD GPU под IOKit (`src/navi48-bringup`), Darwin-бэкенд RADV (`mesa-patches`), Metal-бандл поверх Vulkan (`tools/native/navi48metal`) | MIT (+LGPL транслятор) |
| NullMoth (`github.com/nullmoth/nvidia-macos-driver`) | связка с macOS: IOFramebuffer (NVRMFB), IOAccelerator (NVAccel), AGDC-шим, Metal-плагин в `/Library/GPUBundles`, установка kext'ов через AuxKC (`kmutil`), безопасный откат | PolyForm Noncommercial (личное использование) + LGPL транслятор |

Обе схемы одинаковы: Metal-плагин → транслятор AIR→SPIR-V → Mesa Vulkan → kext ядра → GPU.

## Ограничения, известные заранее

- Ни один из проектов не поддерживает Sonoma 14: NullMoth — Sequoia 15 (+Tahoe 26), Cobalt64 — Tahoe 26.6.2.
  Перед шагом E понадобится обновление macOS (вероятно до Sequoia 15.8.x, как у NullMoth).
- Kext'ы дисплея и ускорителя линкуются с IOGraphics/IOAccelerator из системной KC — их нельзя
  внедрять через OpenCore, только `/Library/Extensions` + `kmutil` (AuxKC).
- Ошибки инициализации GPU вешают машину. Каждый kext с записью в регистры — с boot-arg отключения.
- Сборка kext'ов — нативно на маке (Command Line Tools), не самодельным упаковщиком `tools/build_kext.py`
  (OpenCore отвергал его C++-kext'ы: "Vtable patching failed").

## Шаги

- **A. Паспорт GPU с железа** (только чтение): `src/phxprobe` — kext читает `RCC_CONFIG_MEMSIZE`, статус PSP
  и таблицу IP Discovery через `MM_INDEX/MM_DATA`; разбор — `tools/parse_discovery.py`.
  Результат: точные версии и базовые адреса всех IP-блоков → список прошивок linux-firmware и
  заголовков регистров.
- **B. Инструменты:** CLT 16.2 на маке установлены (2026-10-18); kext'ы собираются нативно
  (`src/phxprobe/build.sh`, проверка `kmutil print-diagnostics` → Dependencies OK). Далее Homebrew для Mesa.
- **C. Обновление macOS:** выбрано Tahoe 26.6.2 (25G83) — ровно сборка Cobalt64. EFI до обновления:
  `hardware/efi_backups/2026-10-18-pre-tahoe`.
- **D. RADV для gfx1103 на macOS — ГОТОВО 2026-10-18 (на моке):** Mesa `f5cb8ee0` + Cobalt64 0001..0009 +
  `mesa-patches-phoenix/` (таблица `phoenix` из `amdgpu_devices.c`, GB_ADDR_CONFIG 0x242, имя/PCI ID 780M).
  Сборка на Tahoe без Homebrew (на Intel он больше не поддерживается): Python 3.14 + venv, glslang и
  Vulkan-Loader из исходников в `~/gpu/prefix`. `tools/vkprobe` с `RADV_DARWIN_MOCK=1`:
  «AMD Radeon 780M Graphics (RADV PHOENIX) 1002:1900», устройство создаётся, ACO компилирует compute-шейдер.
- **E1 — ГОТОВО 2026-10-18:** `src/phxbringup` (`PhxBringup.kext`, сервис `PhxBringup`, user client
  `IOAccelPhxNativeClient` типа 'N48N', селекторы 0..8 по контракту мока Mesa, BO = kernel/user shared память,
  Submit завершается сразу без GPU). Внедряется через OpenCore, `-phxbringupoff` отключает. `tools/vkprobe`
  через настоящий kext: устройство, компиляция, dispatch + fence проходят; `results: 0/256` (GPU ещё не исполняет)
  — в E4 должно стать 256/256.
- **E2 — ГОТОВО 2026-10-18:** `PhxBringup` build 2 + `tools/phxctl` (ReadRegs root-only; селектор 100 =
  сообщения SMU 13.0.4 из белого списка, только с `phxdebug=1`). SMU: TestMessage 0x1234→0x1235, PMFW 76.101.0
  (0x004c6500), driver-if 8 (= Linux SMU13_0_4_DRIVER_IF_VERSION), features 0x07a5f3f3_f3fff7fd (GFX_DPM,
  DS_GFXCLK, ATHUB_PG, S0I3 вкл.). После DisallowGfxOff все регистры GC (GRBM/CP/RLC/MES/SDMA/GB_ADDR_CONFIG)
  читаются 0 без зависания → графическое ядро не запитано: на Phoenix его включает IMU по сообщению SMU
  `EnableGfxImu` (0x16, Linux `set_gfx_power_up_by_imu`) после загрузки прошивок через PSP. AllowGfxOff возвращён.
- **E3a — ГОТОВО 2026-10-19:** PSP 13.0.4 KM (GPCOM) кольцо создаётся и уничтожается с железа.
  `PhxBringup` build 4 отображает **BAR0** (апертура кадрового буфера, phys `0x8_2040_0000`, 256 МиБ —
  это MMIO *выше* 32 ГиБ ОЗУ macOS, запись туда не трогает системную память). Кольцо инициализируется
  через BAR0 так же, как Cobalt64 `psp_v14_0.cpp`: обнуляется 0x4000 Б VRAM, MC = `fbMcBase + offset`
  пишется в `C2PMSG_69/70`, `C2PMSG_71 = 0x1000`, пинок `C2PMSG_64 = 2<<16`, ожидание флага `0x80000000`.
  Прошлый build писал кольцо по *системному физ. адресу* `fbPhys+off ≈ 26.5 ГиБ` — это ЖИВАЯ ОЗУ macOS
  (проверка «region non-zero» спасла от порчи). Новый read-only селектор 102 (`phxctl vram <off>`) читает
  VRAM через BAR0: подтверждено, что BAR0 = реальная видеопамять (консольный кадровый буфер 2560×1600×4 =
  `[0, 0xFA0000)`, дальше стабильная DRAM). Кольцо размещено на offset **0x1100000** (17 МиБ, сразу за FB).
  Результат (повторяемо): create `0x80c00000→0x80c20000`, destroy `0x80c30000`, recreate `0x80c20000`,
  status-биты 15:0 = 0, MC `0x8001100000`. macOS стабильна.
- **E3b — ГОТОВО 2026-10-19:** GPCOM-сабмит команд к PSP работает с железа. `PhxBringup` build 5 добавляет:
  примитив `pspSubmit` (psp_gfx_rb_frame 64 Б в кольце → cmd_buf 1 КиБ → fence, оба в VRAM сразу над кольцом:
  cmd = ring+0x4000, fence = ring+0x8000), HDP-flush (NBIO remap рег `0xE4D` = окно BAR5 `0x385c`, запись 0),
  и селектор 103 (`phxctl submit <cmdid>`, белый список: FB_FW_RESERV_ADDR/EXT). Тест: submit 0x50/0x51 →
  `resp.status 0x100` (SOS не знает команду — норма для APU), **fence записан за 1 мс**, буфер fence = 2,
  wptr = 0x20 (2 кадра). Весь путь CPU→BAR0→HDP-flush→C2PMSG_67→PSP→fence→ответ подтверждён.
- **E3c — ЗАГРУЗКА ПРОШИВОК РАБОТАЕТ 2026-10-19:** весь путь LOAD_IP_FW проверен на железе, все прошивки
  грузятся со `status 0`. `PhxBringup` build 9: селекторы 104 `fwStage` (стейдж байтов в fw_buf через BAR0,
  чанки ≤4 КиБ), 105 `loadIpFw(fwType,off,size)`, 106 `pspCtl(op)` (1=SETUP_TMR, 2=AUTOLOAD_RLC), 107 `pspRaw`
  (произвольная GPCOM-команда — для экспериментов). phxctl: `stage/loadfw/raw/tmr/autoload`. Критично:
  **13.0.4 = boot_time_tmr=false** (подтверждено из Linux amdgpu_psp.c) → драйвер сам делает SETUP_TMR.
  Последовательность: create → stage+**LOAD_TOC** (`psp_13_0_4_toc.bin`, payload 256/2304) → PSP вернул
  нужный размер TMR = **0x4000000 (64 МиБ)** в resp.tmr_size (+16) → **SETUP_TMR 64 МиБ** (natural-aligned,
  buf=MC, flags=0x2, system_phy=fbPhys+off; раньше слал 4 МиБ → BAD_PARAMETERS). После TMR грузятся:
  **IMU_I=68, IMU_D=69, CP_PFP=2, CP_ME=1, CP_MEC=4 (legacy, НЕ RS64 — pfp hdr v1.0), RLC_G=8, MES=33,
  SDMA=71** — все `status 0`. RS64-типы 87/88/89 → `0x8000030a` (не та прошивка). HDP-flush и fence работают.
- **E3d — ГРАФИЧЕСКОЕ ЯДРО ВКЛЮЧЕНО 2026-10-19 🎉:** build 10 + SMU `EnableGfxImu` (0x16) + `AUTOLOAD_RLC`.
  Последовательность (всё через phxctl, скрипт `~/gpu/bringup2.sh`): create → LOAD_TOC → SETUP_TMR 64 МиБ →
  LOAD_IP_FW {PFP 2, ME 1, MEC 4, RLC_G 8, SDMA 71, MES 33, IMU_I 68, IMU_D 69} → SMU 0x16 (отвечает
  timeout по мейлбоксу, но SMU всё равно поднимает питание GFX — async) → AUTOLOAD_RLC. После этого
  **регистры GC ожили и держатся**: GB_ADDR_CONFIG=0x242 (ровно как в canned-таблице, теперь читается с
  железа), GRBM_STATUS=0x302c, GRBM_STATUS2=0x0400000c, **RLC_STAT=0x25, RLC_GPM_STAT=0x00b40017 (RLC
  работает)**, **SDMA0_STATUS_REG=0x46dee557 (SDMA жив)**, CP_ME_CNTL=0x15000000, CP_MEC/MES_CNTL=0x40000000,
  CP_RB0_CNTL=0x00a00000. До этого всё было 0 (ядро обесточено). macOS стабильна. Это ворота к E4.
- **E4 — ДАЛЕЕ:** старт GFX/compute кольца (KIQ/MES + CP ring), реальный сабмит IB (NOP + fence),
  затем compute-шейдер; `vkprobe` должен дать 256/256. Нужно: заавтоматизировать bring-up в один вызов
  (build 11), поднять KIQ/compute-очередь, GART/VMID для BO.
- **E. Kext инициализации Phoenix** (по образцу `navi48-bringup`, но для APU): GMC 11 / GART в
  UMA-памяти, загрузка прошивок GFX/SDMA через PSP 13.0.4, кольца GFX и SDMA, прерывания IH;
  проверка — NOP + запись fence. Дисплей на этом шаге остаётся за прошивкой (GOP).
- **F. User client + Darwin winsys RADV** поверх kext из шага E; проверка — offscreen-треугольник Vulkan.
- **G. Metal-плагин** (navi48metal или плагин NullMoth) поверх RADV; проверка — `mtlprobe`, простые
  Metal-сэмплы.
- **H. IOFramebuffer (DCN 3.1.4, eDP 2560x1600@120) + IOAccelerator + AGDC** по образцу NullMoth;
  проверка — WindowServer композитит через GPU.

## E4a — SDMA исполняет команды (2026-10-19)
Первое исполнение на GPU: SDMA0 queue0 выполнил WRITE_LINEAR (0xDEADBEEF в VRAM 0x2002000).
Условия: прошивка SDMA 6.0.1 v2.0 грузится двумя потоками (TH0 fw_type 71 = file[256:+17408], TH1 fw_type 72 = file[17664:+16896]);
запись WPTR работает только внутри окна MINOR_PTR_UPDATE(0x1315)=1 … =0 (как в Linux sdma_v6_0_gfx_resume_instance);
F32_CNTL(0xF89A)=0x08084000 (unhalt), RB_CNTL=0x841815, IB_CNTL=0x101. Адрес кольца — MC (0x80_0000_0000+VRAM).

## E4b — GFX-кольцо (CP_RB0): пока НЕ исполняется (2026-10-19)
Скрипты: tools/phxctl/bringup3.sh (power-up + SDMA тест + RLC sub-bins), tools/phxctl/gfx_test.sh.
Сделано: CP unhalt (CP_ME_CNTL 0xA803=0), CP_RB0_* запрограммированы (CNTL=0x20000709, BASE=mc>>8, RPTR_ADDR, RB_ACTIVE), WPTR=5 защёлкивается, но RPTR=0, WRITE_DATA не исполняется.
Диагностика: CP_STAT=0x80008000 (PFP_BUSY навсегда), CP_STALLED_STAT2 = PFP_RCIU_READ_PENDING(0x20) пока GFXOFF разрешён; DisallowGfxOff(0x1A) сбрасывает stall, но кольцо не читается.
Загружены доп. RLC-прошивки (RESTORE_LIST_GPM 20, SRM 21, IRAM 26, DRAM_BOOT 48, RLC_P 25) — все status 0, эффекта нет. GC VM (gfxhub) совпадает с MMHUB по умолчанию (L2 off, TLB 0x501); попытки TLB=0x1859, L2_CNTL|=1, CP_DEBUG UTCL1_ERROR_HALT_DISABLE — без эффекта.
Гипотеза: нужна полная последовательность gfx_v11_0_hw_init: gfxhub_enable (GART/CONTEXT0 + page table), constants_init, gc_doorbell_init, rlc_resume, golden regs, затем cp_resume; либо кольцом должен управлять MES.

## E5 — вычислительная очередь исполняет PM4 (2026-10-19, после трассы рабочего Linux)
Причины прежних неудач (найдены сравнением с трассой amdgpu): (1) CP_ME_CNTL нужно писать 0x01000000 (CE_HALT остаётся!), иначе PFP виснет на RCIU;
(2) EnableGfxImu(0x16) с параметром 1, после автозагрузки, перед ним SMU 0x3 и 0x2; (3) все адреса колец/MQD/fence/wptr-shadow для MES — только MC-апертура VRAM (0x80_xxxxxxxx), GART-VA пока не работает;
(4) нужен wptr-shadow (WB+16) перед doorbell; (5) для map_legacy_kq нужен KIQ (RLC_CP_SCHEDULERS=0x30e8 + HQD me3/pipe1).
Результат: MES SET_HW_RESOURCES/QUERY OK, ADD_QUEUE(COMPUTE, map_legacy_kq) OK, на очереди выполнены SET_UCONFIG_REG (scratch=0xDEADBEEF) и WRITE_DATA в VRAM.
Скрипты: tools/phxctl/{bringup5.sh,full.sh,fresh.sh,mesboot.c}. Команда: `MC=1 CS=1 bash ~/gpu/full.sh kiqsched` после чистой перезагрузки.
Дальше: (a) GPUVM: настроить GCVM L2 как в Linux (L2_CNTL=0x80e01, CNTL3=0x80130009...), контексты VMID 8..15 (CNTL=0x01fffe07), понять почему GART-VA записи не доходят;
(b) перенести последовательность в kext (авто-bring-up), (c) IB из user-клиента через INDIRECT_BUFFER с VMID 8, (d) dispatch шейдера (vkprobe 256/256).

## E6 — GPUVM context0 работает (2026-10-20)
Ключ: полная копия gfxhub-регистров из трассы Linux + таблица страниц в СИСТЕМНОЙ памяти (корень phys|3) + clflush при записи PTE; цель — страницы VRAM (PA=0x6A0000000+off, флаги 0x71).
Запись CP в системные страницы через VM пока не доходит (SDMA через MMHUB AGP — доходит) -> BO держим в VRAM-пуле (carve-out 6 GiB), GTT откладываем.
Дальше: контексты VMID 8+ (4 уровня, depth 3), INDIRECT_BUFFER с VMID, перенос в kext, dispatch шейдера.

## E7 — контекст приложения VMID 8 (2026-10-20)
4-уровневая таблица (каталоги в системной памяти, PDE = phys|7 без BFS/TF), IB с VMID 8 исполняется, запись доходит после ACQUIRE_MEM (GL2 writeback).
Тесты гонять с FAULT_DEFAULT=1, чтобы ошибки трансляции не вешали CP.
Дальше: перенос bring-up и VM в kext, путь SUBMIT для RADV, dispatch шейдера.

## E8 — Mesa RADV Darwin (Vulkan API 1.4.363) на AMD Radeon 780M (2026-10-20)
* **Ядро (Build 18)**: UserClient `IOAccelPhxNativeClient` научился принимать вызовы `doDbgAdoptQueue` (селектор 112) и отдавать MES compute queue пользовательскому процессу.
* **RADV Darwin**: Mesa 26.3.0-devel (RADV) успешно подключилась к kext через UserClient:
  `[0] AMD Radeon 780M Graphics (RADV PHOENIX) vendor 0x1002 device 0x1900 api 1.4.363`
* **Создание устройства и шейдеры**: `VkDevice` создан, compute pipeline скомпилирован через ACO (`fill.comp` SPIR-V), командный буфер передан на видеокарту (`RPTR = 0x18, WPTR = 0x18`).
* **Окружение**: macOS Tahoe (Darwin 25.x).


## E8 — vkprobe 256/256 на реальном 780M (2026-10-20)
RADV (ACO) -> kext (build 21) -> MES-очередь MEC -> GPU: вычислительный шейдер выполнен, результат верный.
kext: приём очереди (селектор 112), таблица VMID 8 в системной памяти, SUBMIT = INDIRECT_BUFFER(VMID 8) + RELEASE_MEM.
Ловушки: старшее слово адреса IB — со знаковым расширением (0xffff8001), sfence перед doorbell (BAR0 = write-combining), clflush буферов.
Запуск сейчас вручную: чистая загрузка -> `MC=1 CS=1 ADOPT=1 bash ~/gpu/full.sh kiqsched` -> Vulkan-приложение.
Дальше: графическая очередь (MES ADD_QUEUE GFX), автозапуск при загрузке, затем Metal-плагин.

- **E9 — ГРАФИЧЕСКАЯ ОЧЕРЕДЬ, 2026-10-20:** GFX-очередь (me0 pipe0 q0) поставлена через MES ADD_QUEUE(GFX, map_legacy_kq);
  MQD по `gfx_v11_0_gfx_mqd_init`, doorbell-диапазон RB 0x458/0x7f8 (из трассы Linux), RLC CSIB + преамбула clear-state
  (`tools/phxctl/csb_gfx11.h`, 960 dword из `clearstate_gfx11.h`). Кекст build 22/23: две очереди (compute/gfx), общая
  нумерация fence, IB маршрутизируются по ip_type. vkprobe 256/256 на семействе очередей 0.
- **M1 — METAL РАБОТАЕТ (вычисления), 2026-10-20:** `src/phxaccel` = ускоритель NullMoth (NVAccel) в AMD-режиме
  (`NMX_ACCEL`, `NM_TAHOE`, классы NMX*) в Auxiliary KC; матчится только после `mesboot claim` (свойство
  `nmxgpu-claimed`), поэтому при загрузке ничего не стартует. Плагин NullMoth `NVMTLDriver.bundle` 2.4.1 (без изменений)
  в `/Library/GPUBundles` + наш RADV в `/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib`. `tools/mtlprobe`: MSL-шейдер
  скомпилирован и выполнен на 780M, 4096/4096. Порядок после загрузки: `bash ~/gpu/phxup.sh`.
  Ограничение: в AMD-режиме плагин не отдаёт устройство WindowServer (только приложениям) — для Liquid Glass нужен
  следующий этап (свой фреймбуфер поверх режима GOP + display pipe ускорителя + сборка плагина с WindowServer).
