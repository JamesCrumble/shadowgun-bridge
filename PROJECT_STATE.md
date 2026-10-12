# Shadowgun Bridge (SHADOWGUN 1.7.0 -> Android APK для OnePlus 13s): состояние проекта

Цель: запустить 32-битный Shadowgun на OnePlus 13s (только AArch64) тем же способом, что `../nova3probe` и
`../mass-effect-infiltrator-probe`: игра исполняется в нашем qemu-arm, её GLES-вызовы идут через мост на GPU телефона.
Каркас скопирован из `../mass-effect-infiltrator-probe` (лоадер, thunks, мост, APK-обёртка, инструменты).

Обновлено: 2026-10-11.

## Почему не «пересборка под arm64»
- Игра собрана на **Unity 5.3.8f1** (Android arm64 в Unity появился только в 2017.4/2018.1) — 64-битной `libunity.so` нет.
- Код игры — **IL2CPP** (`libil2cpp.so`, ARM32, `global-metadata.dat` v21), управляемых DLL в APK нет.
- Библиотеки только `armeabi-v7a` (в Anniversary Edition ещё `x86`).

## Исходные файлы (`gamebin/`, не в git)
- `Shadowgun-Anniversary-Edition-v1-7-0.apk` + `...-cache.zip` (OBB) — **основа**: dex открыт, без упаковщика.
- `SHADOWGUN_1.7.0-Mod.Max_Gfx.apk` — тот же билд (Build ID `libunity.so` совпадает, OBB побайтно тот же), запакован
  Qihoo Jiagu, с `libappcloner`/`libsandhook`; отличается 49 байтами в `globalgamemanagers` (настройки качества).
- Текстуры — ATC (манифест: `GL_AMD_compressed_ATC_texture`). Декодеры ATC/DXT в порте есть (`src/atc_decompress.cpp`).

## Архитектура порта (отличия от Mass Effect)
- `src/main.cpp` играет роль Java-класса `UnityPlayer`: грузит `libunity.so` (+`libmain.so` по DT_NEEDED), зовёт
  `JNI_OnLoad`, затем зарегистрированные через `RegisterNatives` методы: `initJni`, `nativeFile(apk)`, `nativeFile(obb)`,
  `nativeInitWWW`, `nativeRecreateGfxState`, `nativeResume`, `nativeFocusChanged`, далее `nativeRender` каждый кадр.
  Порядок взят из `classes.dex` игры (`tools/dexclass.py`, дамп `scratch/dex.txt` делается `dexdump -d`).
- **Динамический JNI** (`jni/dyn.h`, `jni/dyn.cpp`): неизвестные классы/методы/поля создаются на лету, вызов уходит в
  обработчик по ключу `класс.имя+сигнатура` (или `*`), без обработчика — строка `JNI unhandled ...` в логе и 0/null.
  Поиск нестрогий по типу результата (так C# ищет методы через `ReflectionHelper`).
- `jni/unity_java.cpp` — обработчики Android/Unity (Context, File, SharedPreferences=PlayerPrefs в
  `files/playerprefs.txt`, Build, Display, PackageManager, ReflectionHelper, плагин разрешений Madfinger и т.д.).
- `jni/fmod_audio.cpp` — звук: цикл `fmodProcess` -> очередь SDL (в APK дальше SDL disk -> FIFO -> AudioTrack).
- `jni/unity_www.cpp` — `UnityEngine.WWW`: `jar:file://` и `file://` читаются по-настоящему, сеть сразу даёт ошибку.
- `src/control.cpp` — канал управления: `ctl/commands` (`touch a x y id`, `key code down|up`, `pause`, `resume`, `quit`)
  -> `nativeInjectEvent` с поддельными `MotionEvent`/`KeyEvent`; `ctl/status.json`.
- `android/egl_shim.cpp` — EGL: один конфиг RGBA8888+D24S8, **только ES2** (ES3-контекст отклоняется), pbuffer.
- `dlopen`/`dlsym` (`thunks/libc/misc.cpp`) грузят настоящие модули (Unity делает `dlopen("il2cpp")`).
- Новые таблицы импортов: `src/symtab_unity.cpp` (остаток libandroid, SHA1, divmod из libgcc), `src/symtab_signal.cpp`
  (bionic `sigset_t`/`sigaction` — нужны сборщику мусора il2cpp), дополнения в `src/symtab_pthread.cpp`.

## Раскладка данных игры (`sgdata/` локально, `files/game/` в APK)
`lib/armeabi-v7a/{libmain,libunity,libil2cpp}.so`, `base.apk` (движок читает ассеты прямо из zip),
`Android/obb/com.madfingergames.shadowgun/main.170200014.com.madfingergames.shadowgun.obb`, `files/`, `cache/`.

## Сборка и запуск
- `./build.sh` -> `build/shadowgun` (armhf). `jdk/ android-sdk/ sdl-arm/` — симлинки на соседний проект.
- Локально на ПК (qemu-arm хоста + Mesa llvmpipe, медленно): `tools/run_local.sh [сек]`, лог `logs/local.log`,
  скриншоты `ctl/shot_*.bmp` каждые `SHADOWGUN_SHOT_EVERY` кадров (`tools/bmp2png.py`), дайджест — `tools/iter_local.sh`.
- APK: `tools/build_install.sh [noinstall]` -> `Shadowgun-Bridge-debug.apk` (пакет `com.jamescrumble.shadowgun`).
  На телефоне в `Android/data/com.jamescrumble.shadowgun/files/` кладутся APK игры и cache-zip (или `.obb`).

## Правила работы (как в соседних проектах)
- По-русски, кратко. Установка APK + перезапуск — можно; любые другие команды на телефоне — после явного «да».
- Без sudo и крупных загрузок без спроса.

## Состояние на 2026-10-11
- **На ПК** (qemu-arm хоста + Mesa llvmpipe) игра проходит весь старт: проверка разрешений -> LOADING -> **главное меню**
  (логотип, PLAY/HELP/SETUP/EXTRAS/ABOUT, 3D-фон). Скриншоты: `scratch/shot_*.png`.
- Что пришлось найти по дороге:
  - `__aeabi_ldivmod` и др. из сгенерированной таблицы возвращали структуру через скрытый указатель — привязаны к libgcc.
  - `pthread_getattr_np`/`pthread_attr_getstack` и сигналы шли напрямую в glibc с bionic-раскладкой — сделаны мосты.
  - `PopLocalFrame` возвращал NULL (Unity возвращает через него все объекты).
  - Игра ждёт ответа плагина Google Play Games через `UnitySendMessage` (`_OnResolvePlayServicesAvailability`) —
    без него вечный LOADING. Ответ «false» шлёт `jni/unity_java.cpp`.
  - C# зовёт `GetObjectClass` на `jclass` — `dyn_class_of()`.
- Все 103 функции GL, которые Unity запрашивает через `dlsym`, есть в мосте (`scratch/dlsym_names.txt`).
- `Shadowgun-Bridge-debug.apk` собран и **установлен** на телефон. **На телефоне запускается и играбелен** (см. раздел «Производительность»).

## Не проверено / дальше
- (устарело, на телефоне работает) Запуск на телефоне: мост GL с Unity (порядок вызовов, ATC-текстуры, FBO), звук (FMOD -> SDL disk -> FIFO -> AudioTrack,
  44100 Гц), касания (`touch` -> `nativeInjectEvent`), пауза/возврат в приложение.
- Вступительный ролик `introHD.mp4` (`showVideoPlayer`) не реализован — вызов игнорируется.
- Настройки обёртки (`SettingsActivity`) остались от Mass Effect (NEON-математика, DPI) — почистить.
- Отладка: `tools/thread_dump.sh` (SIGUSR2 -> позиции потоков в лог), `SHADOWGUN_JNI_TRACE=1`, `SHADOWGUN_DLSYM_TRACE=all`.
- «Max Gfx»: 49 байт в `globalgamemanagers` мода — можно сделать переключателем (подмена файла в `base.apk`).

## Производительность (2026-10-12)
Тестовая сцена: старт игры, статуя + текст туториала (341 draw, ~300 тыс. GL-команд/с). Дисплей 120 Гц, `eglSwapInterval(1)`,
поэтому fps квантуется 120/60/40/30.

| Сборка | fps | UnityMain busy |
|---|---|---|
| Исходная (E0, 30 fps lock снят в `src/fps_unlock.cpp`, env `SHADOWGUN_TARGET_FPS`) | 17-22 при зажиме ОС, 33.6 без | 72% |
| E1: inline NZCV в `gen_set_cpsr`, `vfp_*` helpers `NO_RWG`, быстрый `vfp_cmps` | 38.5 | 68% |
| E2: inline VCMP (TCG), убраны hardening-флаги qemu | 40.0 | 70% |
| E3: cross-page goto_tb chaining (`translator_use_goto_tb`) | **41.4-42.2** | 73% |

- Патч qemu: `qemu/patches/shadowgun_fast_vfp.patch`, описание и тесты (`cmptest` d6137e57, `fptest` 94e3b209) в `qemu/README.md`.
  Сборка: `qemu/build_variant.sh E` (Docker `nova3-qemu-build`, исходники `qemu-src/`, не в git).
- Env-переключатели: `QEMU_STRICT_FP=1` (отключает быстрый FP и inline vcmp), `SHADOWGUN_INLINE_LOOKUP=1` (inline-проба jump cache,
  по умолчанию выкл: прироста не дала, работа переехала в JIT-код).
- Диагностика: `SHADOWGUN_PC_PROF=1` (`src/pc_prof.cpp`, разбор `tools/pcprof.py`), `src/syscall_prof.cpp`, телеметрия потоков
  (`PerfHint.java`) в строке `[gl] frames`. Профиль хоста: `simpleperf record --app com.jamescrumble.shadowgun -p <pid qemu> -e cpu-clock`
  (в манифесте `profileable shell=true`; без `--app` и с `cpu-cycles` не работает).
- Профиль после E3 (UnityMain): `helper_arm_lookup_tb_ptr` 16% (было 23%), `float32_add/mul` ~8% (уже быстрый хост-FPU путь из nova3),
  libunity 77% гостевого времени (культинг/рендер, ~30% скалярный VFP).
- Узкое место - однопоточный UnityMain (~17 мс CPU/кадр, 70-75% busy). GL-мост не узкий (busy 5-8%).
- Зажим ОС (uclamp.max 240 на самом тяжёлом потоке + средние ядра 1363 МГц) даёт 17 fps; приходит и уходит сам, не из нашего ADPF.
  `cmd game mode performance com.jamescrumble.shadowgun` включён: заметного эффекта нет (42-43.5 fps, при 2649 МГц на cpu7 всего 43.5 -
  значит, прирост от частоты малый, упор не только в частоту).
- Не помогло/не применимо: NEON-патч Matrix4 (из mass-effect; в Shadowgun уже NEON), kuser-CAS патч из nova3 (в libil2cpp 1 вхождение,
  в libunity 0), inline-проба jump cache.
- Идеи дальше: быстрый путь `vfp_adds/muls/subs` (~1-2%), меньше косвенных переходов, проверка без ADPF (`PerfHint`).
- TODO: `tools/cpu_diag.sh` использует пакет nova3; регулярки `timeline.py`/`log_digest.py` под новый формат glprof; решить, включать ли
  pc_prof/sysprof в релизе; убрать мёртвую настройку NEON_MATH.

### Настройки и потоки (2026-10-12)
- Экран настроек (`SettingsActivity`) - лаунчер приложения, внизу кнопки «Вернуть как было / Сохранить / Играть». Ключи: `CPU_PRIME`
  (по умолчанию ВКЛ: qemu под `taskset c0`, оба рабочих потока на ядрах 6-7, прирост ~10-15% fps), `PERF_HINT` (ADPF, по умолчанию вкл),
  `MT_RENDERING` (эксперимент, выкл), `SHADOWGUN_GL_INFLIGHT`, `RENDER_HEIGHT`, `AUDIO_LATENCY_MS`, `FPS_COUNTER`, `PROFILING`, `TRACE`.
- Потоки: GL-вызовы делает главный `UnityMain` (трасса `GL owner thread tid=`), из 7 воркеров Unity занят один (`Worker Thread`, 40-50%).
- `MT_RENDERING=1`: `GameProcess.applyMtRendering` ставит `m_MobileMTRendering` (байт +0x79 PlayerSettings, offset 24768+0x78 в
  `assets/bin/Data/globalgamemanagers`, формат v15 без type tree, разбор `tools/unity_objs.py`) в копии `game/base.apk`. Появляется поток
  `UnityGfxDeviceWorker` (~11%), картинка корректна, но выигрыша нет: главный поток остаётся ~73% загружен (GL-часть мала), мост берёт
  мьютекс. Замеры (динамическая сцена, маска c0): MT выкл - среднее 59.0 / медиана 59.9 (n=23), MT вкл - 56.4 / 57.8 (n=34); упор в 60 vsync.
- Правила: ничего не ставить глобально (pip/apt), только локально в проекте.
- Повторный замер MT на тестовой сцене со статуей (та же сцена, маска c0): MT выкл 50.5 fps (UnityMain 91% busy), MT вкл 56-57.5 fps
  (UnityMain 81%, 410k cmds/с). В сценах, упирающихся в vsync 60, разницы нет. Поэтому `MT_RENDERING` теперь включён по умолчанию.

### Зажим частоты ОС - главный ограничитель fps (2026-10-12)
- В одной и той же сцене (красная труба, 1-й уровень) fps идёт строго за `scaling_max_freq` ядер 6-7: 1209 МГц - 46 fps,
  1690-1960 МГц - 64-66 fps, 4320 МГц (первые секунды после сворачивания/разворачивания) - 80-90 fps. Телефон холодный
  (корпус 34-35 C, thermal status 0). Прежний вывод «прирост от частоты малый» неверен.
- `PerfHint`: цель ADPF была зашита 16.67 мс при любом лимите кадров; теперь `1e9 / FPS_LIMIT`. Зажим это не снимает:
  uclamp 1024 держится ~15 с после старта, потом снова 240-376.
- Проверено на контрольной точке (арена с роботами, ~8.2 тыс. GL-команд/кадр) и не помогло: `PERF_HINT=0` (в сцене с трубой
  52-53 fps, потолок 1209-1401), `CPU_PRIME=0` (41-42 fps против 65-68 - хуже), отчёт ADPF «кадр вдвое дольше цели» (71-76, в
  пределах разброса потолка), он же вместе с `GameManager.setGameState(isLoading=true)` (66-68). Приложение «Игры» OnePlus
  пользователь пробовал сам - без эффекта. Код двух последних опытов удалён.
- Графика: 16 встроенных шейдеров с тенями не компилируются (`sampler2DShadow` при `#version 100`: Unity считает Adreno ES3
  и включает `GL_EXT_shadow_samplers`, драйвер Adreno 830 его не объявляет). На трубе тень от ограждения сплошная чёрная с
  рваными краями (`logs/phone_shadow.png`); связь с шейдерами не доказана.
- Тени: исправлено в `GlBridge.shadowSamplers` - `sampler2DShadow` заменяется обычной выборкой глубины со сравнением в
  шейдере, `TEXTURE_COMPARE_MODE` на хосте не ставится. Ошибок компиляции шейдеров в логе 0 (было 16). Чёрная тень на трубе
  после этого пользователем ещё не проверена.

### Профиль главного потока и qemu (2026-10-12)
- `pcprof` по UnityMain в игре: 3 функции libunity дают ~36% потока (`sub_50f9c8` 19%, `sub_50c754` 10%, `sub_512310` 6.5%).
  Это уже NEON-код анимации (кватернионы/матрицы, вперемешку скалярный VFP и q-регистры) - переписывать вручную нечего.
- `simpleperf` по тому же потоку: ~65% JIT-код, ~35% хелперы qemu. Из них плавающая точка ~23% потока (`float32_add` 8%,
  `float32_mul` 3.8%, `gvec_fadd_s` 2.4%, `gvec_fmul_s` 2%, ...), `helper_arm_lookup_tb_ptr` + поиск ~9%.
  Разбор адресов: `simpleperf report --sort dso,symbol` + `nm -n qemu/out/qemu-arm-E` (на телефоне libqemu без символов);
  запись только через `-p <pid qemu>` и при игре на переднем плане (с `-t <tid>` 0 выборок).
- Быстрый путь `float32_add` уже ~19 инструкций; убрано чтение TLS в его начале (см. `qemu/README.md`), на armbench vfp -4%.
  Дальше микрооптимизацией хелперов много не взять: стоит сам вызов хелпера на каждую FP-операцию.
- Сделано: скалярные VFP add/sub/mul/div инлайном в JIT (свои TCG-опы `hf*` в aarch64-бэкенде, флаг TB `FASTFP`;
  описание в `qemu/README.md`). В nova3/mass-effect этого нет - там только быстрый хелпер. armbench vfp 200 -> 160 мс, хэши
  тестов те же. В игре UnityMain 78% -> ~66% busy; на контрольной точке 65-76 fps уже при потолке 1209 МГц (до правки при
  1209 МГц было ~46 в сцене с трубой - сцены разные, прямое сравнение неточное). Теперь заметнее ждёт Worker Thread (~50%).
- Сделано: NEON f32 целым регистром на хосте (add/sub/mul/mla/mls, сравнения, min/max; `qemu/README.md`). armbench:
  vfp 200 -> 160 мс, neon 48 -> 28 мс, всего 450 -> 395 мс; все тесты (`fptest`, `cmptest`, `neontest`, новый `neoncmptest`)
  совпадают со строгим режимом. В игре, локация «комната с ящиками» (2-я половина 1-го уровня, потолок 1209-1690 МГц):
  47-54 fps -> 64-70 fps (подтверждено пользователем: «65»). Доля хелперов qemu: UnityMain 29% -> 25%, Worker 30% -> 25%.
- Осталось в профиле: `helper_arm_lookup_tb_ptr` 13% UnityMain / 12% Worker (+ `interval_tree_iter_first` 1.3%, `qht_lookup` 0.5% -
  промахи jump cache). В самой функции ~70 инструкций, горячая точка - ожидание адреса перехода (гость сам его вычисляет).
  Дальше: кэш возвратов (bl/bx lr) или сокращение пути; `helper_atomic_cmpxchgq_le` 1.4% на Worker; vsqrt/vcvt/VFMA скаляром.
- Сокращён путь попадания в `helper_arm_lookup_tb_ptr` (запись jump cache хранит ключ флагов и указатель на код, TB не
  читается; `qemu/README.md`). armbench: calls без изменений (62-72 мс, шум), ints 71 -> 66 мс. В игре доля функции на UnityMain
  13.2% -> 11.9%, но замер снят при погашенном экране телефона (fps 36-39, GL-поток busy 67%) - **перемерить с включённым экраном**.
  Установлено на телефон, за минуту работы сбоев нет. Если выигрыша не будет - откатить (лишний 1 МБ на поток).
- Перемерено с включённым экраном (пауза в игре, статичная сцена, потолок 1209 МГц, 69.5-70 fps): доля
  `helper_arm_lookup_tb_ptr` на UnityMain 12.5% (было 13.2%) - в пределах шума; правка безвредна, оставлена.
- **Не сработало: кэш цели на каждое место косвенного перехода** (ячейка «последний pc -> код TB», проверка инлайном,
  промах идёт в хелпер и перезаписывает ячейку). armbench в ARM-режиме: calls 66-69 -> 78-82 мс (+18%), ints +9%.
  Места переходов в движковом коде полиморфны, промах дороже обычного поиска. Код удалён, дерево qemu = патчу.
  Это вторая неудача с инлайном поиска (первая - `SHADOWGUN_INLINE_LOOKUP`); дальше в эту сторону только настоящий
  стек возвратов (bl/bx lr как host call/ret), не кэши.
- `qemu/armbench.c` собирать с `-marm`: Debian-gcc по умолчанию даёт Thumb-2, а игра в ARM-режиме (пути для ARM и Thumb
  в трансляторе разные).
- Кандидат дальше: Worker Thread - `gvec_fmla_nf_idx_s` 7.3% + `gvec_fmul_idx_s` 3.8% + `fcge` 1.4% + `fadd` 1.2% (вызов
  хелпера на каждую NEON-команду скиннинга); инлайн NEON f32 в JIT по образцу `hf*`-опов. `helper_atomic_cmpxchgq_le` 2%.
- Замеры fps/профиля имеют смысл только при `mWakefulness=Awake` и игре на переднем плане (`dumpsys power`, `dumpsys activity`).
- `CPU_SPREAD=1` (settings.txt, без пункта в меню, по умолчанию выкл): все потоки qemu кроме UnityMain/Worker Thread
  переносятся на ядра 0-5 (`taskset -p 3f`). С ним uclamp остаётся 1024, но gfx-поток на средних ядрах занят 28% вместо 15%,
  и по пропускной способности (GL-команд/с) выигрыша не видно: 335-440k против 375-520k. Не доказано, оставлено опцией.
  `run-as` для приложения недоступен (не debuggable), поэтому менять affinity можно только из обёртки.
- Worker Thread в простое крутит `nanosleep(0)` ~60 раз за кадр (35% ядра) - возможный вклад в зажим ОС, не проверено.

### Имя пакета и перенос сохранений (2026-10-12)
- Пакет приложения теперь `com.jamescrumble.shadowgun` (был `com.eaprules.shadowgun`); для Android это другое приложение.
- `SaveMigration.java`: сборка `APP_ID=com.eaprules.shadowgun tools/build_install.sh noinstall` (aapt2
  `--rename-manifest-package`, та же отладочная подпись) обновляет старое приложение и на экране настроек выгружает
  `home/`, `save/`, `game/saves/`, `game/files/` в `Android/data/<старый пакет>/files/migrate`. Папку переложить в
  `Android/data/com.jamescrumble.shadowgun/files/` (права 666/777, если копировать через adb), новое приложение забирает
  её перед запуском игры и переименовывает в `migrate.done`. Прогресс Shadowgun - `game/files/playerprefs.txt`.
- Файлы, скопированные через `adb shell cp`, получают владельца shell и права 660: приложение их не откроет, нужен chmod.

