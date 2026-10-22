# Coop Lua bridge и хуки событий (F0-05)

> **Задача:** `F0-05` — Lua-мост и хуки событий (движковый поток).
> **Статус:** реализация, компиляция проверяется CI (`F0-01`, windows-latest/MSVC).

## 1. Слои

```
gamedata/scripts/coop*.script   Lua gameplay layer (события, sync-примитивы, debug)
        │  coop_bridge.*  (luabind, стабильный интерфейс)
        ▼
src/xrGame/coop/coop_bridge.*   тонкий движковый адаптер (C++, без игровой логики)
        │  coop::IBridgeSink
        ▼
транспорт F0-02 / ядро репликации F0-06
```

- **NFR-ARCH-9.** Движок спрятан за `coop_bridge`; coop-ядро и моды зависят только от
  этого интерфейса. Правка upstream-файлов минимальна: одна проверка в
  `CALifeUpdateManager::update()` (debug-фриз A-Life) и одна регистрация модуля в
  `script_engine_export.cpp`.
- **NFR-ARCH-10.** Через границу C++↔Lua идут только события/решения. Перемещение,
  HP, время и погода опрашиваются одним throttle-тиком Lua (по умолчанию 250 мс) и
  отправляются в C++ только при изменении значения. Покадрового маршаллинга нет.

## 2. C++: `coop_bridge` (стабильный интерфейс)

Сессия и роль:

| Функция | Назначение |
|---|---|
| `coop_bridge.session()` | снимок сессии: роль, локальный игрок, слоты, версия API |
| `coop_bridge.set_role(role, local_player)` | `offline` / `host` / `client` (ставит менеджер соединений F0-08) |
| `coop_bridge.role()`, `is_host()`, `is_client()`, `is_offline()` | host/peer-проверки |
| `coop_bridge.local_player()`, `peer_count()`, `set_peer_count(n)` | слоты (максимум 4) |

Исходящие/входящие:

| Функция | Назначение |
|---|---|
| `coop_bridge.emit_event(name, payload)` | событие наружу, возвращает sequence (0 — отказ) |
| `coop_bridge.sync(channel, payload)` | sync-вызов наружу, возвращает sequence |
| `coop_bridge.pump()` | забрать входящие из установленного sink |
| `coop_bridge.inbound(max)` | забрать входящие из локальной очереди |
| `coop_bridge.enable_dispatch()` | доставлять входящие в `coop_sync.on_inbound` |
| `coop_bridge.set_loopback(bool)` | debug: собственные сообщения возвращаются во входящие |

Паритет и диагностика:

| Функция | Назначение |
|---|---|
| `coop_bridge.api_manifest()` | список экспортов C++-половины (`name`, `argc`, `since`) |
| `coop_bridge.api_fingerprint()` | FNV-1a 64 по манифесту (hex-строка) |
| `coop_bridge.api_version()` | версия контракта |
| `coop_bridge.stats()`, `last_error()`, `clear()` | счётчики и состояние |

Debug (PLAN §8.11), по умолчанию выключено:

| Функция | Назначение |
|---|---|
| `coop_bridge.set_debug_enabled(enabled, launcher_allows)` | включение поверхности; `launcher_allows` = `-coop_debug` в командной строке |
| `coop_bridge.set_alife_enabled(bool)` / `alife_enabled()` | фриз A-Life (читается в `CALifeUpdateManager::update()`) |
| `coop_bridge.set_god_mode(bool)` / `god_mode()` | флаг бессмертия (применяется Lua-слоем к актору) |

Транспортный шов `coop::IBridgeSink` (`on_outbound` / `pull_inbound`) реализует
`F0-02`+`F0-06`; без sink мост работает как счётчик и не теряет данные.

## 3. Lua: публичный фасад `coop.*`

### 3.1. События — `coop.events`

Минимальный набор Фазы 0: `player_move`, `player_health`, `player_inventory`,
`item_pickup`, `player_death`, `npc_spawn`, `npc_death`, `world_time`, `weather`,
`actor_spawn`.

```lua
coop.events.on("npc_death", function(payload)
    -- payload.id, payload.section, payload.killer_id
end)
coop.events.emit("custom_event", { value = 1 })  -- локально + наружу
```

- Источник — штатные колбэки Anomaly (`actor_on_*`, `npc_on_*`, `monster_on_*`),
  регистрируются в `on_game_start`.
- Перемещение/HP/время/погода идут через throttle-тик `coop.events.pulse()`
  (`event_pulse_ms`, `move_threshold` в `gamedata/configs/coop/coop.ltx`).
- Сетевые события, пришедшие через `coop_sync.on_inbound`, диспатчатся в тот же
  поток: подписка одна и та же на хосте и клиенте.
- Кодирование состояния — детерминированное (`key=type:value`, ключи сортируются),
  вложенные таблицы не поддерживаются.

### 3.2. Синхронизация — `coop.sync`

```lua
coop.sync.register("loot.chan", { writer = "host" })   -- канал, single-writer
coop.sync.set_state("loot.chan", "count", 3)           -- пишет только владелец
coop.sync.get_state("loot.chan", "count", 0)           -- читают все
coop.sync.publish("input.chan", { action = "jump" })   -- writer = "any"
coop.sync.request("loot.chan", { item = "artifact" }, function(result, err) ... end)
coop.sync.on("loot.chan", function(payload) ... end)   -- inbound-подписка
```

- Клиент не пишет канал с `writer = "host"`: вызов отклоняется (`not_authority`),
  ошибка логируется, состояние не меняется (AC-2.10.2).
- `request/response` имеет таймаут (`request_timeout_ms`); ответ доставляется
  callback'ом.
- Offline: локальный кэш каналов работает, `publish/request` возвращают
  `not_connected` (одиночная игра не ломается).

### 3.3. Паритет экспортов — `coop.parity`

```lua
local payload = coop.parity.serialize(coop.parity.local_manifest())
local ok, result = coop.parity.handshake(remote_payload)  -- на подключении (F0-08)
-- result.missing / result.extra / result.mismatched, result.local_fp / remote_fp
```

Манифест = C++-половина (`coop_bridge.api_manifest`) + Lua-половина (`coop.api`),
формат строки: `layer\tname\targc\tsince`, отпечаток FNV-1a 32 (без bit-ops,
Lua 5.1/LuaJIT). `coop.parity.validate_local()` проверяет, что реестр `coop.api`
не разошёлся с фактическими функциями.

### 3.4. Debug-поверхность — `coop.debug`

```lua
coop.debug.enable(true)          -- только если движок запущен с -coop_debug
coop.debug.set_alife(false)      -- фриз A-Life
coop.debug.god_mode(true)        -- бессмертие актора
coop.debug.teleport(x, y, z)
coop.debug.teleport_to_object(id)
coop.debug.spawn("stalker_soldier", x, y, z)
coop.debug.command("alife", false)  -- диспетчер для бот-сценариев (F0-12)
coop.debug.status()
```

Без `-coop_debug` каждая команда отклоняется с записью в лог и не меняет мир
(прод-путь безопасен, Zero Dead Config). Включается только параметром запуска,
не конфигом и не скриптом.

## 4. Проверки

```bash
# юнит-тесты Lua-слоя (движок не нужен, моки движковых глобалов)
luajit tests/lua/coop/run.lua

# сквозной прогон «хост -> клиент» на реальных скриптах и протоколе сообщений
luajit tests/lua/coop/e2e_loopback.lua
```

Покрыто: кодирование состояния, throttle/дедуп событий, движковые колбэки,
single-writer и отказ клиенту, request/response и таймаут, паритет (match/missing/
extra/arity), debug-гейтинг и команды.

**Не покрыто локально:** компиляция C++ и запуск в движке — только CI (`F0-01`,
windows-latest/MSVC). Транспорт (F0-02) и сетевая доставка (F0-06) подключаются
через `IBridgeSink`; сейчас проверен loopback и ручная доставка сообщений.
