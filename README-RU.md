# fuckgram

<img src="Telegram/Resources/art/icon256.png" width="96" align="right" alt="Иконка fuckgram">

[ [English](README.md) | Русский ]

Клиент Telegram для компьютера с плагинами на Lua. Сделан на основе
[AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) 7.2.9, который,
в свою очередь, основан на [Telegram Desktop](https://github.com/telegramdesktop/tdesktop).

[![Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml/badge.svg?branch=release)](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)

Канал: [@fuckgramdesktop](https://t.me/fuckgramdesktop) ·
Плагины: [@fuckgramplugins](https://t.me/fuckgramplugins)

> **Состояние.** Сборки под Windows работают. `release` — основная ветка,
> из неё собирается Release-клиент; разработка идёт в `dev`, из неё
> собирается Debug. Релизов на GitHub пока нет, Linux и macOS пока не
> собираются.

## Возможности

Всё, что есть в AyuGram Desktop: режим призрака, история сообщений и защита
от удаления, режим стримера, переводчик, шрифты и оформление, локальные
функции Telegram Premium. И сверх того:

### Плагины на Lua 5.4

- **Песочница.** Каждый плагин работает в своей виртуальной машине Lua: без
  файлов, процессов, `load`, `debug` и байткода. Лимиты времени на вызов
  (50 мс на фильтр отправки, 200 мс на события, 1 с на пункт меню), 64 МБ
  памяти, после 5 ошибок плагин останавливается.
- **Проверка кода до установки.** Права перечислены в манифесте; код
  анализируется при импорте, и плагин, который использует больше — или
  меньше — объявленного, не устанавливается.
- **43 хука в клиенте:** новые и отправляемые сообщения, меню сообщений,
  чатов, профилей и главное меню, текст в чатах и списке чатов, ссылки,
  ответы, реакции, звонки, плеер, строки профиля, экраны, а также сырые
  запросы, ответы и обновления MTProto.
- **API:** отправка, правка и удаление сообщений; чаты и пользователи; поле
  ввода; окна, уведомления и буфер обмена; настройки и файлы плагина;
  таймеры, JSON, HTTPS к объявленным адресам, корутины; любой метод API
  через `tg.invoke` с кодеком TL слоя клиента.
- **Безопасность:** платежи, звёзды, подарки, вход, сеансы, пароль,
  Telegram Passport и удаление аккаунта по запросу плагина ждут «Разрешить»
  от пользователя. Падение во время запуска плагинов включает безопасный
  режим.
- **Менеджер плагинов** (Настройки → Плагины): карточки с иконками,
  версиями и правами, включение, журнал, «поделиться», удаление; настройки
  движка с режимом разработчика, который перезагружает папки плагинов при
  сохранении.
- **Установка** — открыть файл `.flua` в любом чате или из **каталога
  плагинов**: проверенные и свои каналы, категории, поиск и обновления.

### Ещё

- 12 иконок приложения (Настройки → fuckgram → Оформление → Иконка
  приложения).

## Плагины

Готовые, в [examples/plugins](examples/plugins):

| Плагин | Что делает |
|---|---|
| Чистые ссылки | убирает метки слежки (`utm_*`, `fbclid`, `si`…) из ссылок, которые вы отправляете |
| Калькулятор | `2+2*2=` уходит как `2+2*2 = 6` |
| Курсы валют | `/cur 100 usd rub` отправляет сумму по сегодняшнему курсу |
| Раскладка | исправляет текст, набранный не в той раскладке |
| Автоответчик | `/away текст` отвечает в личных сообщениях, пока вас нет |
| hello, weather, raw, selftest | примеры API |

Плагин — это папка с `manifest.json` и `init.lua`:

```lua
plugin.on("message.send", function(draft)
    if draft.text == "/shrug" then
        return "¯\\_(ツ)_/¯"
    end
end)
```

Упакуйте его командой `python tools/flua_pack.py <папка>` и выложите `.flua`
в канал. API, права и каталог описаны в [docs/plugins.md](docs/plugins.md),
типы для редакторов — в [docs/sdk/fuckgram.lua](docs/sdk/fuckgram.lua).

## Скачать

Последняя сборка — в [Releases](https://github.com/fucksynth/fuckgram/releases/latest):
`fuckgram-win-x64.zip` с `fuckgram.exe`. Положите exe в отдельную папку:
данные он хранит рядом с собой. Дальше fuckgram обновляется сам: раз в сутки
проверяет релизы, скачивает новую сборку в фоне, сверяет SHA-256 и
предлагает «Обновить fuckgram» под списком чатов (настройки fuckgram →
Обновления).

Debug-сборки ветки `dev` — в [GitHub Actions](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)
(скачивание после входа в GitHub, хранятся 2 дня); сами они не обновляются.

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
  [exDesktop](https://github.com/metawka/exDesktop) — их хуки и каталог
  показали, куда идти
- Клиенты из благодарностей AyuGram: [Kotatogram](https://github.com/kotatogram/kotatogram-desktop),
  [64Gram](https://github.com/TDesktop-x64/tdesktop),
  [Forkgram](https://github.com/forkgram/tdesktop)
- Библиотеки: [Lua](https://www.lua.org), [JSON for Modern C++](https://github.com/nlohmann/json),
  [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
