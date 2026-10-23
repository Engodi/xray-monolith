# Проводка coop-ядра в движок (F0-04 / F0-05 / F0-07 / F0-12)

Движковый адаптер к host-authoritative ядру (`xrn::core`, репозиторий `xray-net`,
`src/core`). Ядро остаётся engine-free (`NFR-ARCH-9`): здесь только точка
соприкосновения — тик в игровом цикле, мост в Lua, артефакт наблюдаемости и
файловый канал ботов.

```
CLevel::OnFrame
   └─ coop::core_runtime().tick()          src/xrGame/coop/coop_core_runtime.*
        ├─ HostSession::service()/publish()      (хост)     — ядро, F0-06
        ├─ ClientSession::service()              (клиент)
        ├─ EnvironmentAuthority::advance/take_events()      — F1-05
        ├─ coop::bridge() <-> core BridgeLink               — Lua, F0-05
        ├─ bot::CBotChannel poll/ack/observe                — F0-12
        ├─ <run-dir>/<instance>.jsonl            obs::EventLog (F0-07)
        └─ <run-dir>/<instance>.metrics.json     obs::Metrics (F0-07)
```

## 1. Что включено

| Требование | Где | Как проверено |
|---|---|---|
| Тик ядра в игровом цикле | `CLevel::OnFrame` → `core_runtime().tick()`; фиксированный шаг 20 мс, ограничение догона 200 мс | `tests/cpp/coop/run_core.sh` (шаги/тик растут) |
| Публикация событий мира в Lua | `env::publish_environment_events` → `HostSession::bridge()` → `coop::bridge().pump()` → `coop.sync.on_inbound` | `tests/lua/coop/test_core.lua` (сырой канал `environment` → `coop.events`) |
| События мира в наблюдения ботов | `coop_bot_channel` из `ccop_core_runtime` (`weather`, `emission_start`, `time_sync`, `player_rejoin`, `window_expired`, `join_rejected`) | `tests/cpp/coop/run_core.sh` |
| Артефакт F0-07 | `EventLog::write_jsonl` → `<run-dir>/<instance>.jsonl`, `obs::Metrics` → `<instance>.metrics.json` | `obs::read_jsonl` в тесте + `xrn_core_runlog merge` |
| Интенты ботов в `HostSession` + ack | `coop_bot_channel` (inbox) → `ClientSession::bridge()` → `HostSession` → вердикт → `outbox` + `observations.jsonl` | `tests/cpp/coop/run_core.sh` (подбор `bread` получает вердикт хоста) |

## 2. Включение: только harness-профиль

Проводка вооружается **только** переменными, которые выставляет харнесс `F0-11`
(`tools/harness` в репозитории coop). Обычный игровой процесс их не имеет и
платит одну проверку на кадр (затем ничего).

| Переменная | Значение |
|---|---|
| `XRN_HARNESS_RUN_DIR` | каталог прогона (`<run-dir>`) |
| `XRN_HARNESS_INSTANCE` | имя инстанса (`host`, `client-1`, …) — оно же `src` в логе |
| `XRN_HARNESS_ROLE` | `host` / `client` |
| `XRN_HARNESS_SLOT` | слот клиента (идентичность игрока) |
| `XRN_HARNESS_LEVEL` | имя уровня для наблюдений (по умолчанию `l01_escape`) |

Таблица `coop_runtime` (Luabind) отдаёт `available/start/status/command`;
Lua-фасад — `gamedata/scripts/coop_core.script` (`coop_core.*`).

## 3. Артефакты прогона

```
<run-dir>/
  <instance>.jsonl          # F0-07: по одному JSON-событию на строку (obs::EventLog)
  <instance>.metrics.json   # F0-07: тик/латентность/размеры снапшотов
  <instance>/bot/inbox/NNNN.json         # runner -> бот
  <instance>/bot/outbox/NNNN.json        # бот -> runner: ack
  <instance>/bot/observations.jsonl      # state / event / ack
```

`<instance>.jsonl` переписывается целиком (tmp+rename) при каждом сбросе
(каждые 500 мс и на shutdown): файл всегда консистентен, даже если процесс
убили между сбросами. Формат — ровно `xrn::core::obs::EventLog::write_jsonl`,
поэтому его читают `xrn_core_runlog merge` и `testrun.sh diff` (F0-14).

## 4. Debug-поверхность (`PLAN §8.11`)

Команды бота (`action = send`, `command = "<name> <argument>"`) идут в
`CCoreRuntime::command`:

| Команда | Что делает | Событие наблюдений |
|---|---|---|
| `debug.weather [type=fog]` | прокручивает цикл погоды хоста на один шаг (≥ одного dwell) | `weather` |
| `debug.emission start` | `EnvironmentAuthority::trigger_emission()` | `emission_start` |
| `debug.clock sync` | `set_world_time(+1 игровая минута)` | `time_sync` |
| `debug.reconnect window=ok` | отбрасывает loopback-пира и возвращает его внутри окна | `player_rejoin` |
| `debug.reconnect window=expired` | то же, но возврат после закрытия окна (600 мс в debug-профиле) | `window_expired` |

`debug.cfg_probe` и `debug.level_change` пока **не** реализованы: адаптер
отвечает `done`, но публикует `debug_unsupported` с причиной, чтобы сценарий
e2e падал на ассерте события, а не «зеленел» молча.

## 5. Ограничения (честно)

* **Транспорт F0-02 не подключён.** Процессы работают на
  `xrn::core::InProcessNetwork`; у хоста есть loopback-пир (реальная
  `ClientSession`), поэтому интент бота проходит настоящий путь
  `ClientSession → HostSession → вердикт`. Межпроцессный обмен появится, когда
  `transport_link.cpp` будет собран в движке (файл намеренно исключён из сборки:
  он единственный привязан к C ABI транспорта). Ничего в адаптере для этого
  менять не нужно — `HostSession`/`ClientSession` уже принимают `IMessageLink`.
* **Компиляция движка — только CI (MSVC/VS2022, F0-01).** Локально проверены:
  адаптер против реального ядра (`tests/cpp/coop/run_core.sh`, g++),
  файловый канал/JSON (`tests/cpp/coop/run.sh`) и Lua-слой
  (`tests/lua/coop/run.lua`, 56 тестов).
* **`tools/diff/merge.py` (xray-net) отстал от формата.** Python-фолбэк
  `testrun.sh diff` знает только F0-07 виды (`session.start`, `world.mutation`,
  `state.hash`, …) и падает `неизвестный вид события 'weather'` на событиях
  F1-05/F1-02. Канонический C++ `xrn_core_runlog merge` тот же файл читает
  (448 событий, 29 хеш-выборок, 0 расхождений). Фикс — расширить `KINDS` в
  `tools/diff/merge.py` до списка `xrn::core::obs::to_string()`; правка в
  репозитории coop, не в движке.
* **Ядро компилируется из sibling-чекаута.** CI раскладывает `engine/` и
  `coop/` рядом, поэтому путь по умолчанию — `$(SolutionDir)..\..\coop`
  (то есть `<workspace>/coop`). Другая раскладка — `/p:CoopSourceRoot=...`.
  Без чекаута coop проект xrGame не соберётся (это цена того, что ядро не
  копируется в движок).

## 6. Файлы

| Файл | Роль |
|---|---|
| `src/xrGame/coop/coop_core_runtime.{h,cpp}` | тик, сессии, события среды, артефакты, debug-команды |
| `src/xrGame/coop/coop_core_script.{h,cpp}` | Lua-таблица `coop_runtime` |
| `src/xrGame/coop/coop_bot_json.{h,cpp}` | минимальный JSON протокола ботов (без движка) |
| `src/xrGame/coop/coop_bot_channel.{h,cpp}` | inbox/outbox/observations протокола ботов (без движка) |
| `gamedata/scripts/coop_core.script` | bootstrap Lua-слоя + события среды → `coop.events` |
| `gamedata/scripts/coop_sync.script` | `on_raw`/`off_raw` — канал ядра без кодека `coop_events` |
| `src/xrGame/Level.cpp` | одна строка в `CLevel::OnFrame` |
| `src/xrServerEntities/script_engine_export.cpp` | регистрация `coop_runtime` |
| `tests/cpp/coop/**`, `tests/lua/coop/test_core.lua` | локальные проверки |
