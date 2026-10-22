# Lua-тесты coop-моста (F0-05)

Движок для этих тестов не нужен: движковые глобалы (`db`, `level`, `alife`,
`vector`, `time_global`, `RegisterScriptCallback`) и C++-таблица `coop_bridge`
подменяются моками (`harness.lua`). Скрипты загружаются как есть из
`gamedata/scripts/` тем же Lua 5.1 (LuaJIT), что и в движке.

```bash
# из корня клона движка
luajit tests/lua/coop/run.lua           # юнит-тесты Lua-слоя
luajit tests/lua/coop/e2e_loopback.lua  # сквозной прогон хост -> клиент
```

Код возврата `0` — зелёно, `1` — есть падения (печатаются как `FAIL ...`).

| Файл | Что проверяет |
|---|---|
| `test_codec.lua` | детерминизм и типы кодирования состояния, экранирование |
| `test_events.lua` | подписки, throttle/дедуп, движковые колбэки, offline-режим |
| `test_sync.lua` | single-writer, отказ клиенту, request/response, таймаут |
| `test_parity.lua` | манифест/отпечаток, missing/extra/arity, сериализация |
| `test_debug.lua` | гейтинг debug-поверхности и debug-команды |
| `e2e_loopback.lua` | реальные скрипты: хост пишет состояние/событие, клиент применяет |

Форматирование скриптов — stylua с конфигом CI
(`.github/ci/conf/stylua.toml` в проектном репозитории).
