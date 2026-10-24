# deko3d в libglass

Upstream: https://github.com/devkitPro/deko3d, снимок `350f2b00a3e76ecd4f00191f8c5d6544ffbcb9db`. Лицензия и
авторские строки upstream сохранены (`LICENSE`).

Собирается как обычные исходники оверлея (`ryazhahand.mk`), без своего Makefile:

- `generated/` — заголовки, которые upstream порождает при сборке (`dekodef`, `dekomme`) из `source/maxwell/*.def`
  и `*.mme`; лежат готовыми.
- `source/dk_private.h` включает `__DK_INTERNAL__` — только для собственных файлов deko3d.

Добавлено для стекла:

- `MemBlock::initializeRemote`, `ryzDkImportNvMapId`, `ryzDkMemBlockId` (`dk_memblock.{h,cpp}`): чужой буфер nvmap
  по номеру — своя ссылка, без отображения в процессор, отображение в адресное пространство видеочипа этого
  устройства (кэшируемое: пишущий видеочип пишет через тот же L2). Деструктор снимает отображение и отпускает только
  свою ссылку.
- `ryzNvMapLoadRemote`: номер → дескриптор и размер. `nvMapLoadRemote` из libnx ещё спрашивает `NvMapParam_Kind`,
  а служба отвечает `BadValue` — и ни один чужой буфер не открывался.
- `ryzDkMessage` (`dk_device.cpp`): ошибки очереди видеочипа в журнал стекла; ожидание в `~Queue` ограничено.
