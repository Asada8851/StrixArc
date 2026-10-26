# Установка nullarc

[English](INSTALL.md) · **Русский**

nullarc — альфа-версия, она работает в ядре и внутри WindowServer. Прежде чем начать, оставьте себе путь назад:
вторую загружаемую macOS или установочную флешку и копию папки EFI.

## Что нужно

| | |
|---|---|
| Видеокарта | AMD Radeon 780M или 760M (Phoenix, Hawk Point; PCI 1002:15bf, 1900). Проверено только на Firebat U6C (Ryzen 7 H 255). |
| macOS | Tahoe 26.6 (проверено на 26.6.2, 25G83), установлена и загружается через OpenCore. |
| OpenCore | Рабочая сборка для вашей машины: Lilu, VirtualSMC и остальное из [`hardware/REQUIRED_KEXTS.md`](../hardware/REQUIRED_KEXTS.md). [`hardware/Sample.plist`](../hardware/Sample.plist) — конфиг эталонной машины с пустыми серийниками. |
| Релиз | `nullarc-<версия>.zip` со [страницы релизов](https://github.com/luuumi-dev/nullarc/releases), распакованный. |

## 1. OpenCore

1. Скопируйте `EFI/OC/Kexts/PhxBringup.kext` из релиза в `EFI/OC/Kexts/` вашего EFI.
2. В `config.plist`:
   - **Kernel → Add**: запись с `BundlePath` `PhxBringup.kext`, `ExecutablePath` `Contents/MacOS/PhxBringup`,
     `PlistPath` `Contents/Info.plist`, `MinKernel` `25.0.0`, `Enabled` true. Поставьте её после Lilu и VirtualSMC.
   - **NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82**:
     - в `boot-args` должны быть `amfi=0x80 amfi_get_out_of_my_way=0x1 -amfipassbeta phxdebug=1` (и AMFIPass.kext в Kernel → Add);
     - `csr-active-config` = `7F080000`.
     Оба ключа должны быть и в NVRAM → Delete, иначе новое значение не применится.
   - **Misc → Security → SecureBootModel** = `Disabled`.
3. Перезагрузитесь. В Терминале `ioreg -l | grep phx-build` должен вывести строку — значит, PhxBringup загружен.

`amfi=0x80` позволяет WindowServer загрузить драйвер с ad-hoc подписью. Он отключает проверку библиотек во всей
системе: перестаёт работать видео с FairPlay (Apple TV+, Netflix в Safari), а некоторые приложения, проверяющие
подписи, могут не запуститься.

## 2. Запуск установщика

В Терминале, в папке распакованного релиза:

```sh
sudo ./install.sh --check          # только проверки, ничего не меняет
sudo ./install.sh --windowserver   # установка; рабочий стол рисует GPU со второй загрузки
```

Без `--windowserver` Metal на GPU получают только приложения, а рабочий стол WindowServer рисует процессором.

Установщик кладёт:

| Что | Куда |
|---|---|
| Запуск GPU при загрузке (`phxboot.sh`, скрипты, `phxctl`, `mesboot`) и прошивки GPU | `/Users/Shared/gpu` |
| LaunchDaemon, который запускает его при каждой загрузке | `/Library/LaunchDaemons/local.phxboot.plist` |
| `PhxAccel.kext`, собранный во вспомогательную коллекцию кекстов (Aux KC) | `/Library/Extensions` |
| Плагин Metal `NVMTLDriver.bundle` и Mesa RADV `nvmtl/` | `/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU`, ссылки из `/Library/GPUBundles` |

Плагин лежит в `/System/Library/CoreServices`, потому что песочные GPU-процессы (Safari, Chromium) могут загружать
код только из системных путей. Эта папка — firmlink на раздел Data: запечатанный системный раздел не меняется.

## 3. Перезагрузка

- **Первая загрузка:** GPU поднимается в течение пары минут после включения. С `--windowserver`
  до перезагрузки включите автоматический вход (Системные настройки → Пользователи и группы): окно входа на GPU
  может оказаться чёрным.
- **С `--windowserver` перезагрузитесь ещё раз:** с этой загрузки WindowServer работает на GPU.

Проверка:

- `/var/log/phxboot.log` заканчивается строкой `done`;
- Информация о системе → Графика/Мониторы показывает **AMD Radeon 780M** и **Metal Support: Metal 3**;
- `sysctl debug.nvaccel_iop_ok` растёт, пока на экране что-то меняется (кадры WindowServer рисует GPU).

Когда новый вид визуального эффекта появляется впервые, его шейдер переводится и компилируется: один раз будет пауза
1–3 секунды. Дальше он берётся из кэша на диске.

## Если что-то пошло не так

phxboot страхует каждую загрузку:

- загрузку, на которой запуск GPU не дошёл до конца (зависание, отключение питания), он замечает на следующей: та
  загрузка проходит без GPU и создаёт `/Users/Shared/gpu/phxboot-off`;
- если WindowServer на GPU падает или зависает 3 раза за 90 секунд, phxboot возвращает фреймбуфер прошивки и
  перезагружает Мак;
- запуск GPU ставит таймер перезагрузки, который срабатывает, если тот завис на 3–4 минуты;
- позже, во время работы, зависание GPU выглядит как WindowServer, который виснет снова и снова (сессия закрывается,
  экран остаётся чёрным): после двух отчётов о зависании WindowServer за 5 минут phxboot перезагружает Мак, а после трёх
  таких перезагрузок за час возвращает WindowServer на фреймбуфер прошивки.

Фоновые нейросети (анализ Фото `mediaanalysisd`, `photoanalysisd`, `Spotlight`) на GPU не пускаются: их свёрточные
ядра в тестах вешали GPU. Они работают на процессоре, как и без драйвера.

Выключить GPU вручную: `sudo touch /Users/Shared/gpu/phxboot-off` и перезагрузка. Включить обратно:
`sudo rm /Users/Shared/gpu/phxboot-off` и перезагрузка. Если macOS перестала загружаться, загрузитесь без PhxBringup:
выключите его в OpenCore (или верните копию EFI), загрузитесь и запустите удаление.

## Обновление

Распакуйте новый релиз и снова запустите его `install.sh` (с теми же опциями), затем перезагрузитесь. Если в описании
релиза сказано, замените и `PhxBringup.kext` в EFI.

## Удаление

```sh
sudo ./uninstall.sh
```

Затем уберите `PhxBringup.kext` из OpenCore (папку и запись в Kernel → Add) и, если они больше ничему не нужны,
boot-args `amfi=0x80 amfi_get_out_of_my_way=0x1 phxdebug=1`. Перезагрузитесь.

## Что не входит в nullarc

- **Звук:** в macOS Tahoe больше нет `AppleHDA.kext`. Одного AppleALC мало, AppleHDA нужно вернуть (это умеет
  OpenCore Legacy Patcher). nullarc не распространяет программы Apple.
- **Батарея, Wi-Fi, Bluetooth:** обычные кексты хакинтоша, см. [`hardware/REQUIRED_KEXTS.md`](../hardware/REQUIRED_KEXTS.md).
  В `extras/firebat-u6c/` релиза лежит SSDT адаптера питания эталонного ноутбука.
