# StrixArc

**Metal на AMD Radeon 880M / 890M (Strix Point, RDNA 3.5, GC 11.5.0) для macOS Sequoia 15.**
Запуск GPU, вывод изображения, на котором WindowServer композитит силами GPU, и Metal поверх Vulkan (Mesa RADV).

StrixArc — порт [nullarc](https://github.com/luuumi-dev/nullarc) (Radeon 780M, Phoenix) на Strix Point. Имена
компонентов (PhxBringup, PhxAccel, NVMTLDriver) и лицензии взяты из nullarc, см. [LICENSE](LICENSE). Проект не связан
с nullarc и не одобрен его авторами.

## Состояние (октябрь 2026, ноутбук Lenovo с Radeon 880M, панель 3072x1920 30–120 Гц, macOS 15.8.1)

| | |
|---|---|
| **Запуск GPU** | PSP (с TMR от драйвера), SMU, RLC, MES + KIQ, очереди compute и gfx — при каждой загрузке (`phxboot`, со страховками). |
| **Metal** | работает в приложениях; WindowServer композитит на GPU. |
| **Экран** | родные 3072x1920 при 60 Гц. Кадр копирует **SDMA** в один из двух буферов видеопамяти (0,84 мс на кадр 3K), экран переключается на него по vsync: 61,7 fps, без разрывов, процессор в копировании не участвует. |
| **Пока нет** | 120 Гц (нужна перетренировка линка на HBR3 через DMCUB), видео, внешние мониторы, подсветка. |

Подробности со всеми замерами — в [docs/strix-point.md](docs/strix-point.md) (англ.).

## Сборка и проверка

Сборка в Linux: `tools/cross/build-macos.sh <env.sh тулчейна>`. Раунд проверки — папка `nullarc/` на флешке с
OpenCore; на Маке `bash go.sh` ставит изменившееся, просит перезагрузку, когда она нужна, иначе запускает тесты раунда,
логи сами ложатся на флешку (`nullarc_logs/`).

## Лицензия

Смешанная, как в nullarc: MIT или GPL-2.0-only для запуска GPU и утилит, PolyForm Noncommercial 1.0.0 для частей из
драйвера NullMoth (только некоммерческое использование), MIT для патчей Mesa и файлов из Linux. Название и знак nullarc
в репозиторий не входят.
