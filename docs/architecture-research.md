# Архитектурное исследование: Navi48 (Cobalt64) и адаптация под AMD RDNA 3

**Дата:** 11 октября 2026 г.  
**Исследуемый репозиторий:** `Almosst-DEV/Cobalt64` (ранее `Navi48-MacOS`)  
**Официальные источники:** Linux Kernel (`drivers/gpu/drm/amd/amdgpu`), Mesa 3D (`src/amd/vulkan`), AMD ISA Documentation (RDNA 3 / RDNA 4).

---

## 1. Исследование оригинального проекта Navi48 (Cobalt64)

### 1.1. Статус, репозиторий и лицензия
- **Репозиторий:** [Almosst-DEV/Cobalt64](https://github.com/Almosst-DEV/Cobalt64) (ветка `main`, коммит `d6c9aedf...`).
- **Лицензия:** 
  - Основной код (`src/navi48-bringup`, `src/xlat12`, `src/dcn41`, `tools`): **MIT License**.
  - Транслятор шейдеров (`third-party/metal2vulkan-n48`): **LGPL-3.0-or-later**.
  - Патчи к Mesa (`mesa-patches/`): **MIT License** (соответствует лицензии Mesa).
  - Микрокод AMD (firmware): Не включен в репозиторий; загружается из `linux-firmware` (собственная бинарная лицензия AMD).
- **Текущий статус:** Public Preview / Proof-of-Concept. Драйвер обеспечивает:
  - Композитинг рабочего стола на GPU через WindowServer на трех мониторах (DP + 2x HDMI).
  - Поддержку Resizable BAR (до 1 GiB отображения BAR0).
  - Нативную работу приложений macOS через стек Metal-on-Vulkan.
  - Ограничение: Протестировано только на одной машине с RX 9070 XT (Navi 48) на macOS Tahoe 26.6.2 под OpenCore.

### 1.2. Архитектура программного стека Navi48 (Cobalt64)
Проект решает фундаментальную проблему: **Apple прекратила поддержку видеокарт AMD на поколении RDNA 2 (Navi 21/23)** в драйвере `AMDRadeonX6000.kext`. Для запуска более новых архитектур Apple не предоставляет ни драйверов, ни исходников.

Стек Cobalt64 состоит из 4 ключевых эшелонов:

```
+-------------------------------------------------------------------+
|               macOS Applications / WindowServer                   |
+-------------------------------------------------------------------+
|     Metal API calls      |     Legacy AMDRadeonX6000 Calls       |
|            v             |                  v                     |
|   Metal-on-Vulkan        |       xlat12 Command Stream            |
| (navi48metal bundle)     |      Translation (GFX10.3 -> GFX12)    |
|   AIR -> SPIR-V          |                  |                     |
+--------------------------+                  |                     |
|   Mesa RADV (Vulkan)     |                  |                     |
| (Darwin winsys backend)  |                  |                     |
+--------------------------+------------------+---------------------+
|                  IOKit UserClient "N48N"                          |
| (BO alloc, GEM_VA map, CS submit, Fences, Scanout, Display mode)   |
+-------------------------------------------------------------------+
|               Navi48Bringup.kext (IOKit Kernel Extension)         |
|  - PCIe BAR & ReBAR      - PSP 14.0 (Security / FW load)          |
|  - GMC 12.0 (Memory/VM)  - SMU 14.0 (Power & Clocks)              |
|  - GFX 12.0 / CP / MES   - SDMA 7.0 (DMA copy)                    |
|  - DCN 4.1 (Display Controller Engine)                            |
+-------------------------------------------------------------------+
|                       Hardware: AMD Navi 48                       |
+-------------------------------------------------------------------+
```

1. **`Navi48Bringup.kext` (Kernel Space):**
   - Реализован как IOKit драйвер расширения ядра (C++).
   - Управляет аппаратной инициализацией GPU без использования стандартного Linux ядра.
   - Экспортирует интерфейс пользователя `IOAccelNavi48NativeClient` (`N48N`), фактически реализующий семантику Linux DRM UAPI (GEM allocation domains CPU/GTT/VRAM, GEM_VA mapping, Command Submission с буферами команд PM4, аппаратные заборы fences).
2. **`xlat12` (Command Translation):**
   - Библиотека трансляции командных пакетов PM4 времени исполнения.
   - Перехватывает командный поток, сформированный проприетарным кодом Apple `AMDRadeonX6000` (рассчитанным на GFX10.3 / RDNA 2), и налету преобразует его в форматы пакетов и регистры GFX12 (RDNA 4).
3. **Mesa RADV Darwin Port (`mesa-patches`):**
   - Официальный Vulkan-драйвер RADV из Mesa, портированный на Darwin (macOS).
   - Вместо файловых дескрипторов `/dev/dri/cardX` и DRM ioctl обращается к интерфейсу `N48N` через `IOConnectCallMethod`.
4. **Metal-on-Vulkan (`tools/native/navi48metal`):**
   - Библиотека-обертка, реализующая интерфейсы Apple Metal поверх Vulkan API.
   - Преобразует промежуточное представление Apple AIR (LLVM bitcode) в SPIR-V через `metal2vulkan` / `libn48xlate`.
5. **Дисплейный движок (`src/dcn41`):**
   - Прямое программирование регистров DCN 4.1 для настройки режимов OTG (Output Timing Generator), HUBP, DPP и физических выходов HDMI/DP.

---

## 2. Сравнение архитектур: Navi 48 (RDNA 4) vs RDNA 3

| Подсистема GPU | Navi 48 (RDNA 4) в Cobalt64 | Архитектура RDNA 3 (Целевая) | Совместимость и возможность переиспользования |
|---|---|---|---|
| **GFX Generation** | GFX12 (`gfx1200`, `gfx1201`) | GFX11 (`gfx1100`, `gfx1101`, `gfx1102`, `gfx1103`) | **Разная ISA.** Форматы регистров и дескрипторов шейдеров отличаются. Требуется инициализатор GFX11. |
| **GMC (Память и VM)** | GMC 12.0 | GMC 11.0 (VM Hub v2.x) | **Разные смещения.** Логика аллокации VRAM/GTT идентична, но регистры MMU и PTB отличаются. |
| **Display Core (DCN)** | DCN 4.1 (`dcn41`) | DCN 3.2 (Navi 31/32) / DCN 3.2.1 (Navi 33) / DCN 3.1.4 (Phoenix) | **Несовместимо.** Смещения регистров HUBP, DPP, OTG и структура управления тактовыми частотами DCN отличаются. |
| **PSP (Безопасность/FW)** | PSP v14.0 (`psp_14_0_3`) | PSP v13.0 (`psp_13_0_x`) | **Разные версии протокола.** Форматы команд TMR и загрузки FW отличаются версией ABI. |
| **SMU (Управление питанием)** | SMU v14.0 (`smu_14_0_3`) | SMU v13.0 (`smu_13_0_x`) | **Разные структуры сообщений.** Таблицы PPTable и сообщения SMU v13 требуют собственной таблицы ID. |
| **SDMA (DMA Engine)** | SDMA v7.0 (`sdma_7_0_1`) | SDMA v6.0 (`sdma_6_0_x`) | **Близкая архитектура.** Форматы кольцевых буферов SDMA6 и SDMA7 очень похожи, но опкоды заголовков пакетов отличаются. |
| **MES (Micro-Engine Scheduler)** | MES v12.1 | MES v11.0 | **Разный API.** Форматы очередей планировщика MES требуют адаптации структуры `mes_v11_0`. |
| **UserClient ABI (`N48N`)** | ABI 1.12 | Применимо на 100% (переименование в `R3N`) | **Полная совместимость.** Абстракция BO, GEM_VA, Submit, WaitSeq не зависит от поколения GPU. |
| **ReBAR Logic (`rebar_pure.h`)** | Чистая логика ReBAR | Применимо на 100% | **Полная совместимость.** Логика PCIe BAR0 одинакова для всех карт AMD с поддержкой ReBAR. |
| **Mesa RADV Winsys** | Патчи к RADV winsys | Применимо на 95% | **Высокая степень переиспользования.** Более того: GFX11 в Mesa имеет статус полностью стабильного кода (в отличие от GFX12). |
| **Metal-on-Vulkan** | Шейдерный транслятор | Применимо на 100% | **Полная совместимость.** Трансляция Metal -> Vulkan/SPIR-V не зависит от микроархитектуры AMD. |
| **Command Translator (`xlat`)** | `xlat12` (GFX10.3 -> GFX12) | Требуется `xlat11` (GFX10.3 -> GFX11) | **Требуется отдельная реализация**, но GFX10.3 ближе к GFX11, чем к GFX12. |

---

## 3. Анализ стека операционных систем: macOS vs Linux

> [!IMPORTANT]
> **Критическое различие системного контекста:**
> - Исходный проект **Cobalt64** спроектирован **исключительно для macOS (XNU kernel / IOKit)**.
> - На **Linux** официальный открытый драйвер ядра `amdgpu` и пользователи Mesa уже имеют полную производственную поддержку RDNA 3 начиная с ядра 6.0+.
> - Если цель проекта — **создание драйвера RDNA 3 для macOS (Hackintosh)**, проект Cobalt64 является готовым архитектурным шаблоном, где требуется замена блоков IP (v12/v14/v7 -> v11/v13/v6).
> - Если цель проекта — **исследовательский/учебный драйвер для Linux**, прямой перенос кода IOKit C++ в Linux невозможен; требуется адаптация под подсистему Linux DRM/KMS.

---

## 4. Конкретные выводы для проекта RDNA 3

1. **Что переиспользуется без изменений:**
   - Модель `IOAccelUserClient` (`N48N` -> `R3N`), принципы управления памятью, синхронизация.
   - Стек трансляции Metal-on-Vulkan (`navi48metal` / `metal2vulkan`).
   - Патчи к Mesa RADV (Winsys интерфейс).
   - Инфраструктура автономного тестирования на хосте (pure functions, тесты `rebar_pure`, фаззеры).

2. **Что требует отдельной реализации для RDNA 3:**
   - Портирование инициализации аппаратных IP блоков из `drivers/gpu/drm/amd/amdgpu/` ядра Linux:
     - `gfx_v11_0.c` -> C++ kext
     - `gmc_v11_0.c` -> C++ kext
     - `psp_v13_0.c` -> C++ kext
     - `smu_v13_0.c` -> C++ kext
     - `sdma_v6_0.c` -> C++ kext
     - `dcn32` / `dcn314` -> C++ kext
   - Интеграция микрокодов RDNA 3 (`gc_11_0_*`, `psp_13_0_*`, `smu_13_0_*`, `sdma_6_0_*`).
   - Разработка `xlat11` (транслятора GFX10.3 -> GFX11) для пути акселератора Apple.
