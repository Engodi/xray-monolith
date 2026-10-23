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
| Транспорт F0-02 (кросс-процессный UDP) | `coop/coop_transport.*` → `xrn::core::HostTransportLink` / `ClientTransportLink`; `transport_link.cpp` собран в xrGame | `tests/cpp/coop/run_core.sh` (host + client в разных процессах по UDP loopback) |
| Тик ядра в игровом цикле | `CLevel::OnFrame` → `core_runtime().tick()`; фиксированный шаг 20 мс, ограничение догона 200 мс | `tests/cpp/coop/run_core.sh` (шаги/тик растут) |
| Публикация событий мира в Lua | `env::publish_environment_events` → `HostSession::bridge()` → `coop::bridge().pump()` → `coop.sync.on_inbound`; тот же поток уходит клиентам (`HostSession::broadcast_bridge`), клиент декодирует его в свои наблюдения и в свой Lua-слой | `tests/lua/coop/test_core.lua` (сырой канал `environment` → `coop.events`), `tests/cpp/coop/run_core.sh` (хост + клиент видят одинаковый набор событий) |
| События мира в наблюдения ботов | `coop_bot_channel` из `ccop_core_runtime` (`weather`, `emission_start`, `emission_end`, `time_sync`, `player_rejoin`, `window_expired`, `join_rejected`); хост рассылает их клиентам (`coop.obs.event`), поэтому `observer = peer` выполняется и по сети | `tests/cpp/coop/run_core.sh` |
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
| `XRN_HARNESS_NET` | `udp` (по умолчанию) — реальный транспорт; `inprocess` — очереди в одном процессе (только локальные проверки) |
| `XRN_HARNESS_HOST` | адрес (по умолчанию `127.0.0.1`: debug-профиль работает на loopback) |
| `XRN_HARNESS_PORT` | хост: порт для bind (`0` — эфемерный, публикуется в `<run-dir>/transport.port`) |
| `XRN_HARNESS_HOST_PORT` | клиент: порт хоста (`0` — взять из `<run-dir>/transport.port`) |
| `XRN_HARNESS_NET_TIMEOUT_MS` | таймаут молчания транспорта (по умолчанию 20000 мс: хост успевает загрузить уровень, прежде чем клиент сдастся) |
| `XRN_HARNESS_NET_KEEPALIVE_MS` | интервал keepalive (0 = значение транспорта) |

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

## 3.1. Транспорт F0-02 (`xnt`, UDP)

Harness-профиль по умолчанию идёт **по сети**, а не через `InProcessNetwork`:

```
CTransport (coop/coop_transport.*)          xnt C ABI (src/transport/include/xnt/transport.h)
  host: xnt_host_create(addr, port)   ->    xrn::core::HostTransportLink
  client: xnt_client_create(addr, port) ->  xrn::core::ClientTransportLink
```

* `transport_link.cpp` ядра (единственный файл, привязанный к C ABI) собран в
  xrGame; транспорт линкуется статически (`XNT_STATIC`), поэтому рядом с exe не
  нужен отдельный DLL; `ws2_32.lib` добавлен в зависимости проекта.
* При старте проверяется версия ABI (`xnt_abi_version()` против
  `XNT_ABI_VERSION`), а версия транспорта и отчёт о бэкендах
  (`xnt_backend_status()`: `builtin-udp`/`gns`/`steam`/`enet`) попадают в
  `SCoreStatus::transport` и в событие `session.start` лога F0-07 — прогон
  видно, каким путём он шёл.
* Сбой старта транспорта **не** откатывается молча на `InProcessNetwork`:
  процесс помечается `degraded` с ошибкой, сессия не создаётся, артефакт
  содержит `udp unavailable: ...`.
* `InProcessNetwork` остаётся только для локальных проверок
  (`XRN_HARNESS_NET=inprocess`): `tests/cpp/coop/run_core.sh` гоняет оба профиля.
* Клиентская сторона читает состояние только из снапшотов: `ClientMirror` →
  `inv::ItemDirectory`/`act::ActorDirectory` → наблюдения бота (`inventory`,
  `world_items`, `players`). Предмет бот адресует классом (`bread`), id
  разрешается в том, что он видит (у хоста — в авторитетном инвентаре).
* Команды debug-поверхности — механика хоста: клиентский процесс отправляет их
  интентом по мосту (`coop.debug.intent`), хост исполняет и отвечает
  (`coop.debug.ack`, широковещательно, по `seq`), бот получает ответ только
  после вердикта хоста. В in-process профиле команда исполняется локально, как
  раньше. Интент приходит с идентификатором отправителя (`BridgeMessage::from`),
  поэтому команда, меняющая *конкретного* пира (`debug.reconnect`), действует
  ровно на того, кто её послал.
* Поток событий мира идёт от хоста всем: каждое событие среды уходит и в свой
  Lua-слой, и каждому клиенту (`broadcast_bridge`, канал `environment`); клиент
  декодирует его тем же кодеком и наблюдает у себя, поэтому набор и порядок
  событий у клиента совпадают с хостом (`observer = peer` по сети). События
  сессии (`player_rejoin`, `window_expired`) хост тоже рассылает — отдельным
  каналом `coop.obs.event` (`event=<имя>`). Echo в in-process профиле (loopback-
  пир живёт в том же процессе) отбрасывается: хост уже наблюдал эти события.
* Восстановление клиента (F0-02): потеряв линк, клиент не завершает сессию, а
  поднимает новую транспортную сессию на том же сокете (`xnt_client_reconnect`,
  `CTransport::reconnect_client`) и входит снова с той же идентичностью; хост
  применяет политику окна реконнекта (F1-10) — тот же персонаж внутри окна,
  `window_expired` вне его. Хост закрывает линк конкретного пира через
  `HostSession::drop_peer(peer, PeerDrop::KeepSlot|CloseWindow)` поверх
  `xnt_host_disconnect`; `CloseWindow` освобождает слот и убирает персонажа
  сразу (следующий вход получает явный `window_expired`).
* Сетка слотов: хост + 3 клиента = 4 слота. Поэтому в сетевом профиле у хоста
  **нет** loopback-пира: его бот действует как собственный игрок хоста (слот 0),
  а слоты 1..3 занимают реальные клиентские процессы.

## 4. Debug-поверхность (`PLAN §8.11`)

Команды бота (`action = send`, `command = "<name> <argument>"`) идут в
`CCoreRuntime::command`:

| Команда | Что делает | Событие наблюдений |
|---|---|---|
| `debug.weather [type=fog]` | прокручивает цикл погоды хоста на один шаг (≥ одного dwell) | `weather` (виден всем) |
| `debug.emission start` | `EnvironmentAuthority::trigger_emission()` | `emission_start` (виден всем) |
| `debug.clock sync` | `set_world_time(+1 игровая минута)` | `time_sync` (виден всем) |
| `debug.reconnect window=ok` | рвёт линк пира, который послал команду, и держит его слот в окне реконнекта: возврат внутри окна — тот же персонаж (`HostSession::drop_peer(KeepSlot)`); в in-process профиле отбрасывается loopback-пир | `player_rejoin` |
| `debug.reconnect window=expired` | то же, но окно закрывается сразу (600 мс в debug-профиле): слот свободен, персонаж уходит, возврат — явный `window_expired` (`drop_peer(CloseWindow)`) | `window_expired` |

`debug.cfg_probe` и `debug.level_change` пока **не** реализованы: адаптер
отвечает `done`, но публикует `debug_unsupported` с причиной, чтобы сценарий
e2e падал на ассерте события, а не «зеленел» молча.

## 5. Ограничения (честно)

* **`debug.reconnect` работает и по сети.** Хост закрывает линк конкретного пира
  (`xnt_host_disconnect` → `HostSession::drop_peer`), клиент сам поднимает новую
  транспортную сессию и входит снова (`xnt_client_reconnect`), окно реконнекта
  решает исход (`player_rejoin` / `window_expired`). Ограничения этого пути:
  - адресация только по «пир, пославший интент»: дропнуть пира, который сам
    ничего не присылал, командой бота нельзя (нужен административный вызов
    `HostSession::drop_peer` из Lua/UI — не подключён);
  - клиент восстанавливается, пока жив процесс и пока идёт grace-окно
    (2×600 мс в debug-профиле); после него сессия завершается как EC-4, и
    переподключение потребует перезапуска клиента;
  - транспортная сессия поднимается заново на том же сокете; потеря самих
    UDP-датаграмм отбрасывается надёжным каналом (retransmit), как и раньше.
* **События мира видны клиентам.** Хост рассылает их двумя каналами: поток
  `environment` (F0-05, тот же, что уходит в Lua) и `coop.obs.event` для событий
  сессии (`player_rejoin`, `window_expired`). Клиентский бот получает их в свой
  `observations.jsonl`, поэтому `observer = peer` по сети выполняется.
  Ограничения:
  - `join_rejected` + `files` наблюдает сам отклонённый клиент (хост не может
    адресовать пира, которого нет в сессии) — в сетевом профиле у хоста это
    событие не публикуется вовсе;
  - `level_change` (F1-13) в наблюдения ботов не выведен — команда
    `debug.level_change` по-прежнему `debug_unsupported`;
  - в in-process профиле echo событий через loopback-пира отбрасывается, чтобы
    хост не считал их дважды.
* **GNS не линкуется.** `xnt` — самостоятельный транспорт: надёжная/ненадёжная
  доставка, фрагментация, RTT и keepalive реализованы в самом `xnt` поверх
  обычного UDP, а GameNetworkingSockets v1.6.1 остаётся его LAN-швом
  (`XNET_WITH_GNS`, требует protobuf/OpenSSL). В отчёте бэкендов это видно как
  `gns: unavailable — built without XNET_WITH_GNS`.
* **Компиляция движка — только CI (MSVC/VS2022, F0-01).** Локально проверены:
  адаптер против реального ядра: `tests/cpp/coop/run_core.sh` (g++ и clang++,
  in-process профиль + два процесса по UDP: подбор, событие хоста у клиента,
  дроп пира → возврат в окне и вне окна),
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
| `src/xrGame/coop/coop_transport.{h,cpp}` | F0-02: загрузка `xnt`, host/client эндпоинт, публикация эфемерного порта, отчёт о транспорте |
| `src/xrGame/coop/coop_core_script.{h,cpp}` | Lua-таблица `coop_runtime` |
| `src/xrGame/coop/coop_bot_json.{h,cpp}` | минимальный JSON протокола ботов (без движка) |
| `src/xrGame/coop/coop_bot_channel.{h,cpp}` | inbox/outbox/observations протокола ботов (без движка) |
| `gamedata/scripts/coop_core.script` | bootstrap Lua-слоя + события среды → `coop.events` |
| `gamedata/scripts/coop_sync.script` | `on_raw`/`off_raw` — канал ядра без кодека `coop_events` |
| `src/xrGame/Level.cpp` | одна строка в `CLevel::OnFrame` |
| `src/xrServerEntities/script_engine_export.cpp` | регистрация `coop_runtime` |
| `tests/cpp/coop/**`, `tests/lua/coop/test_core.lua` | локальные проверки |
