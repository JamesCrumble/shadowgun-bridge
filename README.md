# Shadowgun Bridge

**English:** run **SHADOWGUN 1.7.0** (Madfinger Games, 32-bit ARMv7, Unity 5.3) on 64-bit-only Android phones
that have no AArch32 support. The game runs under a patched `qemu-user`, and its OpenGL ES calls are forwarded
to the phone's real GPU. 60–90 fps on a OnePlus 13s (Snapdragon 8 Elite). No game files are included: you need
your own copy. Ready-made APK: [Releases](https://github.com/JamesCrumble/shadowgun-bridge/releases).
The rest of this page is in Russian.

Запуск 32-битной **SHADOWGUN 1.7.0** (Madfinger Games, Unity 5.3.8, IL2CPP, только `armeabi-v7a`) на 64-битных
Android-телефонах, в которых нет поддержки AArch32. Проверено на OnePlus 13s (Snapdragon 8 Elite, Android 15).

Игра исполняется в собственном пропатченном `qemu-arm` (linux-user), а её вызовы OpenGL ES пробрасываются
на настоящий GPU телефона. Звук, касания, геймпад, пауза и возврат в приложение работают.

**Игра не распространяется.** Нужны ваши собственные APK версии 1.7.0 и данные игры (cache-zip или `.obb`).

[Состояние](#состояние) · [Установка](#установка) · [Настройки](#настройки) · [Как это устроено](#как-это-устроено) ·
[Производительность](#производительность) · [Структура](#структура) · [Сборка](#сборка) · [Диагностика](#диагностика) ·
[Другие игры](#другие-игры) · [Лицензия и авторство](#лицензия-и-авторство)

## 💳 Поддержка проекта

Если вам нравится проект и вы хотите поддержать разработку:

👉 **[Отправить чаевые через CloudTips](https://pay.cloudtips.ru/p/cad77257)**

## Состояние

- Игра проходится: меню, загрузка уровней, бой, чекпойнты.
- Скорость на OnePlus 13s при рендере 720p: **60–90 fps** в зависимости от сцены.
- Не реализовано: вступительный ролик `introHD.mp4` (вызов пропускается).
- Проверено на одном устройстве. На других чипах возможны отличия в драйвере GLES.

Скорость ограничивает не GPU, а процессор: игра упирается в один поток, а Android держит частоту ядер ниже
максимума (см. [Производительность](#производительность)).

## Установка

Готовая сборка лежит на странице [Releases](https://github.com/JamesCrumble/shadowgun-bridge/releases):
`Shadowgun-Bridge-<версия>.apk` и `INSTALL.txt`.

1. Установите APK (пакет `com.jamescrumble.shadowgun`).
2. Откройте приложение и разрешите доступ ко всем файлам (оно предложит само): будет создана папка
   `/sdcard/BridgeGames/com.jamescrumble.shadowgun/`. Положите в неё два файла (подойдёт и старая
   `Android/data/com.jamescrumble.shadowgun/files/`, читаются обе): оригинальный APK игры 1.7.0 и её данные (cache-архив `.zip` или `main.*.com.madfingergames.shadowgun.obb`).
   Имена не важны, важны расширения.
3. Нажмите «Играть» (настройки при необходимости поменяйте).

Первый запуск дольше обычного: распаковываются sysroot (~100 МБ) и данные игры. Если что-то не так, причина
пишется на экране и в `shadowgun.log` в той же папке.

**Папка BridgeGames.** С доступом ко всем файлам она основная: из `Android/data/com.jamescrumble.shadowgun/files/` туда сами переезжают все файлы
(APK, кэш, настройки, лог). Читаются обе папки, без доступа всё работает с папкой в `Android/data`. Сохранения игра пишет в `BridgeGames/com.jamescrumble.shadowgun/save`, копия свежих файлов остаётся в приватной папке приложения. Как это устроено и как перенести в другой порт: [docs/STORAGE.md](docs/STORAGE.md).

**Обновление со старого имени пакета.** До версии 0.2 пакет назывался иначе, и для Android это другое приложение:
поверх оно не обновится. Прогресс переносится так: сборка `APP_ID=<старый пакет> tools/build_install.sh noinstall`
обновляет старое приложение и на его экране настроек выгружает сохранения в папку `migrate` рядом с файлами игры;
эту папку нужно переложить в `BridgeGames/com.jamescrumble.shadowgun/` (или в `Android/data/com.jamescrumble.shadowgun/files/`), новое приложение заберёт её при запуске игры.

## Настройки

Экран настроек открывается при старте приложения. Значения хранятся в `settings.txt` рядом с файлами игры.

| Настройка | Ключ | Что делает |
|---|---|---|
| Разрешение рендера (высота) | `RENDER_HEIGHT` | Высота кадра, по умолчанию 720 |
| Лимит кадров | `FPS_LIMIT` | 30 / 60 / 90 / 120; та же частота запрашивается у экрана |
| Счётчик FPS | `FPS_COUNTER` | Показывает fps поверх игры |
| Буфер звука | `AUDIO_LATENCY_MS` | Размер буфера AudioTrack |
| Только быстрые ядра (6–7) | `CPU_PRIME` | Привязывает qemu к двум самым быстрым ядрам; по умолчанию включено |
| Подсказки производительности (ADPF) | `PERF_HINT` | Сообщает системе длительность кадров против цели из лимита кадров |
| Кадров «в полёте» | `SHADOWGUN_GL_INFLIGHT` | Сколько кадров мост может держать в очереди к GPU |
| Многопоточный рендер Unity | `MT_RENDERING` | Отдельный поток отрисовки движка; по умолчанию включено |
| Выключить тени | `Q_SHADOWS_OFF` | Меньше проходов отрисовки |
| Динамические источники света | `Q_PIXEL_LIGHTS` | Число попиксельных источников на объект |
| Дальность деталей (LOD) | `Q_LOD_PERCENT` | Масштаб LOD |
| Профилирование в логе, TRACE | `PROFILING`, `TRACE` | Диагностика, см. ниже |

`MT_RENDERING` и три настройки качества меняют несколько байт в `globalgamemanagers` внутри копии APK игры,
которую приложение держит у себя; оригинальный файл не трогается, и выключение настройки возвращает исходные байты.

## Как это устроено

```
 APK-обёртка (Java, AArch64)                    дочерний процесс
 ┌──────────────────────────────┐   pipe   ┌─────────────────────────────────────────┐
 │ SettingsActivity             │ ◄──────► │ qemu-arm (наш патч, статический AArch64) │
 │ MainActivity + SurfaceView   │  команды │  └ shadowgun (порт-лоадер, ARM32, glibc) │
 │ GlBridge: исполняет GLES     │   GLES   │      ├ bionic ELF loader                 │
 │ AudioTrack, касания, геймпад │          │      ├ libunity.so / libil2cpp.so (игра) │
 │ PerfHint (ADPF, телеметрия)  │          │      ├ JNI и Android API на заглушках    │
 └──────────────────────────────┘          │      └ GL-мост: сериализует вызовы GLES  │
                                           └─────────────────────────────────────────┘
```

- **Порт-лоадер** (`src/`, `loader/`, `thunks/`) загружает библиотеки игры, собранные под bionic, в процесс
  с glibc, подменяет libc и системные вызовы Android и играет роль Java-класса `UnityPlayer`.
- **Динамический JNI** (`jni/`): классы, методы и поля, которые запрашивает движок, создаются на лету.
- **GL-мост** (`src/gl_bridge.cpp` ↔ `apk/src/.../GlBridge.java`): вызовы GLES 2.0 пишутся в pipe и исполняются
  на стороне Java в контексте GLES 3. Там же обходятся особенности драйвера, например теневые сэмплеры
  `EXT_shadow_samplers`, которых Adreno не даёт шейдерам ES2.
- **qemu** (`qemu/`): QEMU 11.1.2 с нашим патчем, см. ниже.

Подробный журнал разработки, выводы и тупиковые ветки — в [PROJECT_STATE.md](PROJECT_STATE.md).
Пошаговый разбор — что было видно, как нашли причину, что исправили — в
[docs/PORTING_GUIDE.md](docs/PORTING_GUIDE.md) ([English](docs/PORTING_GUIDE.en.md)).

## Производительность

Штатный qemu исполняет игру слишком медленно, поэтому используется QEMU 11.1.2 с набором правок под
AArch64-хост (подробно в [qemu/README.md](qemu/README.md)):

- **Плавающая точка на процессоре телефона.** Скалярные VFP-операции add/sub/mul/div вставляются прямо
  в сгенерированный код одной командой, без вызова функции (новые TCG-операции). Остальные операции VFP
  и преобразования float↔int считаются одной командой процессора внутри хелпера вместо softfloat.
- **NEON f32 целым регистром.** Сложение, вычитание, умножение, умножение с накоплением, сравнения и min/max
  считаются сразу для всех четырёх элементов на NEON телефона, без переключения режима FPCR.
- **Флаги и сравнения в коде.** Сравнения VCMP и запись флагов NZCV(Q) делаются в сгенерированном коде.
- **Косвенные переходы.** Быстрый поиск блока для возвратов и вызовов по указателю: флаги собираются
  на месте, кэш переходов увеличен с 4096 до 65536 записей и хранит всё нужное для попадания.
- **Сцепление блоков** напрямую через границу страницы.
- Обходы ограничений Android: системные вызовы, которые фильтр seccomp приложений запрещает, не используются.

Точность плавающей точки проверяется тестами `qemu/fptest.c`, `cmptest.c`, `neontest.c`, `neoncmptest.c`:
результат должен совпадать с `QEMU_STRICT_FP=1` (чистый softfloat).

Главный ограничитель на OnePlus 13s — политика энергосбережения системы: она опускает потолок частоты
быстрых ядер до 1,2–2,6 ГГц из 4,3 при холодном телефоне, и fps идёт строго за этим потолком. Из приложения
снять это ограничение не удалось (ADPF, game mode, состояние «загрузка», перенос потоков — всё проверено,
результаты в `PROJECT_STATE.md`).

## Структура

| Папка | Что |
|---|---|
| `src/` | порт: `main.cpp` (роль `UnityPlayer`), GL-мост, декодеры ATC/DXT, канал управления, профилировщики |
| `loader/`, `thunks/` | загрузчик bionic-ELF и переходники bionic → glibc |
| `jni/`, `android/` | JNI, классы Java на заглушках, Android API (EGL, ассеты, OpenSL, FMOD-звук) |
| `apk/` | APK-обёртка: Java-код, манифест, сборка APK и sysroot |
| `qemu/` | патчи qemu 11.1.2, Dockerfile для кросс-сборки, тесты точности, бенчмарк |
| `tools/` | сборка и установка, логи, скриншоты, профилирование, разбор форматов Unity |
| `third_party/` | заимствованный код, лицензии в [NOTICE.md](NOTICE.md) |

В репозитории нет файлов игры, тулчейнов и сборочных каталогов (см. `.gitignore`).

## Сборка

Сборка идёт в WSL (Ubuntu).

Нужны, кладутся в корень проекта и в git не входят:

- `jdk/` — JDK 17;
- `android-sdk/` — build-tools 34.0.0, platform 34, platform-tools;
- `sdl-arm/` — SDL2 под armhf (исходники в `third_party/SDL2-2.0.22.tar.gz`);
- системный armhf-тулчейн (`gcc-arm-linux-gnueabihf`, `g++-arm-linux-gnueabihf`) и Docker для qemu;
- `gamebin/` — ваши файлы игры: APK версии 1.7.0 и её данные (cache-архив `.zip` или `main.*.obb`).
  Из APK при сборке берётся значок приложения; оба файла затем кладутся на телефон (см. [Установка](#установка));
- `sgdata/` — только для запуска на ПК: те же файлы в распакованном виде (`lib/armeabi-v7a/*.so`, `base.apk`,
  `Android/obb/com.madfingergames.shadowgun/main.170200014.com.madfingergames.shadowgun.obb`).

```sh
source env.sh
./build.sh                         # порт: build/shadowgun (ARM32)
tools/build_install.sh             # порт + APK + установка на телефон по adb
tools/build_install.sh noinstall   # то же без установки
tools/make_release.sh              # релизный APK, архив исходников и INSTALL.txt в release/<версия>/
tools/run_local.sh 60              # запуск на ПК (qemu-arm хоста + программный GL, медленно), лог logs/local.log
```

Телефон из WSL виден через виндовый `adb.exe`: используйте обёртку `tools/adb`.

### qemu

Исходники QEMU 11.1.2 распаковываются в `qemu-src/qemu-11.1.2`, на них по порядку накладываются
`qemu/patches/nova3.patch`, `neon_nofpcr.patch` и `shadowgun_fast_vfp.patch`.

```sh
docker build -t shadowgun-qemu-build qemu
docker run --rm -u 1000:1000 -e HOME=/tmp -v "$PWD":/work shadowgun-qemu-build \
  bash /work/qemu/build_variant.sh E \
  '-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP -DNOVA3_NEON_NOFPCR -O3 -march=armv8.2-a+lse+rcpc+dotprod -mtune=neoverse-v1'
```

Результат — `qemu/out/qemu-arm-E.stripped`, его забирает сборка APK. Не добавляйте SVE в `-march`: у ядер Oryon
его нет. Имена файлов патчей и макросов `NOVA3_*` исторические, менять их не нужно. Наложение трёх патчей
на чистые исходники с нуля не перепроверялось: рабочее дерево `qemu-src` велось правками по ходу разработки.

## Диагностика

- Лог игры: `tools/pull_log.sh` → `logs/phone.log`. Строка `[gl] frames` раз в несколько секунд показывает fps,
  число GL-команд, загрузку потоков, текущую частоту и потолок частоты ядра, `uclamp`.
- Профиль кода игры пишется в лог, пока включена настройка «Профилирование в логе»; поток выбирается ключом
  `SHADOWGUN_PC_PROF_THREAD=<имя потока>` в `settings.txt`, разбор — `tools/pcprof.py`.
- Профиль самого qemu: `simpleperf record --app com.jamescrumble.shadowgun -p <pid qemu> -e cpu-clock`
  (приложение `profileable`), адреса сопоставляются с `nm qemu/out/qemu-arm-E`.
- Вставку VFP в код можно выключить ключом `SHADOWGUN_INLINE_FP=0` в `settings.txt` (в окружение qemu
  из него попадают только ключи `SHADOWGUN_*`). `QEMU_STRICT_FP=1` (чистый softfloat) задаётся переменной
  окружения при запуске qemu вручную, например для тестов из `/data/local/tmp`.

## Другие игры

Тот же способ запуска — qemu-user и мост GLES на GPU телефона — для других 32-битных игр:

- [NOVA3 Bridge](https://github.com/JamesCrumble/NOVA3-Bridge) — N.O.V.A. 3 1.0.7
- [Mass Effect Infiltrator Bridge](https://github.com/JamesCrumble/mass-effect-infiltrator-bridge) — Mass Effect Infiltrator 1.0.58

## Лицензия и авторство

GPL-3.0, см. [LICENSE](LICENSE). Происхождение заимствованных частей и их лицензии — в [NOTICE.md](NOTICE.md).
Патч qemu — GPL-2.0-or-later, как сам QEMU.

Порт Shadowgun, APK-обёртка, GL-мост и правки qemu написаны в этом проекте. Загрузчик библиотек взят
из gmloader-next (JohnnyonFlame), а исходная основа JNI, переходников libc и заглушек Android API — из порта
Mass Effect Infiltrator для PortMaster (автор EapRules). К порту Shadowgun EapRules отношения не имеет.

SHADOWGUN © Madfinger Games. Проект не связан с правообладателем и не содержит файлов игры.
