# tests/cpp/coop — движковый адаптер ядра без Windows

Движок собирается только в CI (MSVC/VS2022, `F0-01`), но два слоя проводки
написаны так, что проверяются на хосте обычным компилятором:

| Скрипт | Что проверяет | Зависимости |
|---|---|---|
| `run.sh` | JSON и файловый канал ботов (`coop_bot_json`, `coop_bot_channel`): порядок/`seq`/`token`, ack, поток наблюдений, `join_rejected` с путями | g++ (C++17) |
| `run_core.sh` | 1) весь адаптер (`coop_core_runtime`) против **реального** ядра в in-process профиле: тик, события среды, debug-команды, вердикт `HostSession` для интента бота, `<instance>.jsonl` (валидируется `obs::read_jsonl`) и метрики; 2) **два процесса по реальному UDP** (loopback): join, снапшот, интент подбора и ack, debug-интент клиента → вердикт хоста по сети, наблюдения клиента из зеркала | g++/clang++ + чекаут `coop` (`COOP_SOURCE_ROOT`) |

```bash
tests/cpp/coop/run.sh
COOP_SOURCE_ROOT=../coop tests/cpp/coop/run_core.sh
# или, если чекаут ядра лежит рядом с движком:
tests/cpp/coop/run_core.sh
```

`run_core.sh` подставляет `stub/` вместо движковых заголовков: адаптер трогает
ровно три движковые поверхности — `pch_script.h`, `ai().script_engine().functor`
и `luabind`. Всё остальное — реальный код ядра, скомпилированный из
`$(COOP_SOURCE_ROOT)/src/{core,replication,cfgdiff,transport}`, включая
`transport_link.cpp`: это единственный файл, привязанный к C ABI транспорта
`F0-02`, и с этой задачей он входит в проводку (в `xrGame.vcxproj` транспорт
собирается статически, `XNT_STATIC`).

`test_cross_process.cpp` — единственная локальная проверка, которая не может
быть in-process: она поднимает host и client1 как отдельные процессы и
обменивается пакетами по UDP на `127.0.0.1` (эфемерный порт хоста публикуется
в `<run-dir>/transport.port`).

Эти тесты не заменяют CI: MSVC-специфика (`/W4 /WX /analyze`, PCH, MSBuild
glob-источники в `xrGame.vcxproj`) проверяется только сборкой.
