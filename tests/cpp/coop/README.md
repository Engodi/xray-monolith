# tests/cpp/coop — движковый адаптер ядра без Windows

Движок собирается только в CI (MSVC/VS2022, `F0-01`), но два слоя проводки
написаны так, что проверяются на хосте обычным компилятором:

| Скрипт | Что проверяет | Зависимости |
|---|---|---|
| `run.sh` | JSON и файловый канал ботов (`coop_bot_json`, `coop_bot_channel`): порядок/`seq`/`token`, ack, поток наблюдений, `join_rejected` с путями | g++ (C++17) |
| `run_core.sh` | весь адаптер (`coop_core_runtime`) против **реального** ядра: тик, события среды, debug-команды, вердикт `HostSession` для интента бота, `<instance>.jsonl` (валидируется `obs::read_jsonl`) и метрики | g++ + чекаут `coop` (`COOP_SOURCE_ROOT`) |

```bash
tests/cpp/coop/run.sh
COOP_SOURCE_ROOT=../coop tests/cpp/coop/run_core.sh
# или, если чекаут ядра лежит рядом с движком:
tests/cpp/coop/run_core.sh
```

`run_core.sh` подставляет `stub/` вместо движковых заголовков: адаптер трогает
ровно три движковые поверхности — `pch_script.h`, `ai().script_engine().functor`
и `luabind`. Всё остальное — реальный код ядра, скомпилированный из
`$(COOP_SOURCE_ROOT)/src/{core,replication,cfgdiff}`. `transport_link.cpp`
исключён: это единственный файл, привязанный к C ABI транспорта `F0-02`
(подключается отдельной задачей).

Эти тесты не заменяют CI: MSVC-специфика (`/W4 /WX /analyze`, PCH, MSBuild
glob-источники в `xrGame.vcxproj`) проверяется только сборкой.
