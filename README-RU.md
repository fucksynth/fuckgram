# fuckgram

<img src="Telegram/Resources/art/icon256.png" width="96" align="right" alt="Иконка fuckgram">

[ [English](README.md) | Русский ]

Клиент Telegram для компьютера с ядром плагинов на Lua. Сделан на основе
[AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop), который, в свою
очередь, основан на [Telegram Desktop](https://github.com/telegramdesktop/tdesktop).

[![Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml/badge.svg?branch=dev)](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)

> **Ранняя разработка.** Релизов пока нет. Тестовые сборки под Windows
> собирает GitHub Actions на каждый push в `dev`.

## Возможности

Всё, что есть в AyuGram Desktop:

- Режим призрака (гибкий)
- История сообщений и защита от удаления
- Режим стримера
- Переводчик
- Настройка шрифтов и оформления
- Локальные функции Telegram Premium

И в работе:

- **Плагины на Lua 5.4.** Каждый плагин работает в своей виртуальной машине
  Lua с лимитами памяти и времени, без доступа к файлам, процессам и
  нативному коду.
- **Права проверяются до установки.** Плагин объявляет в манифесте, что ему
  нужно; код анализируется при импорте, и плагин, который использует больше
  объявленного, не устанавливается.
- **Пакеты `.flua`**: манифест, код и ресурсы в одном подписанном файле.
- **Небольшая и стабильная поверхность хуков.** Клиент общается с плагинами
  только через `Telegram/SourceFiles/plugins/plugins_hooks.h`, поэтому
  переход на новую версию AyuGram / Telegram Desktop обходится дёшево.

Устройство и список хуков: [docs/plugins.md](docs/plugins.md).

## Скачать

Релизов пока нет. Тестовая сборка лежит в последнем успешном
[запуске Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml),
в разделе Artifacts.

## Сборка

- [Windows](docs/building-win.md)
- [Linux](docs/building-linux.md)
- [macOS](docs/building-mac.md)

Нужны свои [ключи API](docs/api_credentials.md).

## Лицензия

GPL-3.0, см. [LICENSE](LICENSE). Это форк; копирайты Telegram Desktop и
AyuGram Desktop сохранены в их файлах.

## Благодарности

- [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) от Radolyn Labs —
  основа клиента
- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [exteraGram](https://github.com/exteraSquad/exteraGram) и
  [exDesktop](https://github.com/metawka/exDesktop) — по их хукам видно, куда
  встраивать наши
- Клиенты из благодарностей AyuGram: [Kotatogram](https://github.com/kotatogram/kotatogram-desktop),
  [64Gram](https://github.com/TDesktop-x64/tdesktop),
  [Forkgram](https://github.com/forkgram/tdesktop)
- Библиотеки: [Lua](https://www.lua.org), [JSON for Modern C++](https://github.com/nlohmann/json),
  [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
