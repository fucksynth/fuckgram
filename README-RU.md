# fuckgram

<img src="Telegram/Resources/art/icon256.png" width="96" align="right" alt="Иконка fuckgram">

[ [English](README.md) | Русский ]

Клиент Telegram для компьютера с плагинами на Lua. Сделан на основе
[Telegram Desktop](https://github.com/telegramdesktop/tdesktop) 7.4.6 и
включает всё из [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop).

[![Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml/badge.svg?branch=release)](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)
[![Release](https://img.shields.io/github/v/release/fucksynth/fuckgram?label=release)](https://github.com/fucksynth/fuckgram/releases/latest)

Канал: [@fuckgramdesktop](https://t.me/fuckgramdesktop) ·
Плагины: [@fuckgramplugins](https://t.me/fuckgramplugins) ·
[Официальные плагины](https://github.com/fucksynth/fuckgram-plugins)

> **Состояние.** Windows 10 и новее, x64 (Qt 6). Текущая версия —
> [7.4.6](https://github.com/fucksynth/fuckgram/releases/latest), обновляется
> сама. Linux и macOS пока не собираются.

## Скачать

Возьмите zip `win-x64` в
[Releases](https://github.com/fucksynth/fuckgram/releases/latest) и положите
`fuckgram.exe` в отдельную папку: данные он хранит рядом с собой.

Дальше fuckgram сам обновляется до каждого стабильного релиза. Раз в сутки
он проверяет, скачивает новую версию в фоне, сверяет её SHA-256 и
показывает «Обновить fuckgram» под списком чатов, а после перезапуска —
что нового. В настройках fuckgram → Обновления есть состояние, кнопка
проверки и переключатель. Пре-релизы не предлагаются.

## Возможности

Всё, что есть в AyuGram Desktop: режим призрака, история сообщений и защита
от удаления, режим стримера, переводчик, шрифты и оформление, локальные
функции Telegram Premium. И сверх того:

### Плагины на Lua 5.4

- **Песочница.** Каждый плагин работает в своём состоянии Lua: без файлов,
  процессов, `load`, `debug` и байткода. Ограничения по времени на вызов
  (50 мс для фильтра отправки, 200 мс для событий, 1 с для пунктов меню),
  64 МБ памяти; плагин, упавший 5 раз, останавливается.
- **Проверка кода перед установкой.** В манифесте перечислены права; код
  разбирается при импорте, и плагин, который использует больше — или
  меньше, — чем заявил, не устанавливается.
- **45 точек встраивания в клиент:** новые и отправляемые сообщения; меню
  сообщений, чатов, профилей, кнопки отправки и главное меню; любой раздел
  настроек; текст в чатах и списке чатов; ссылки, ответы, реакции, звонки,
  плеер, строки профиля, экраны; запросы, ответы и обновления MTProto.
- **Интерфейс:** у каждого плагина своя страница настроек (переключатели,
  выбор, поля ввода, ползунки, вложенные страницы), пункты меню с иконками
  где угодно, окна и уведомления и собственная отрисовка — холст для строк
  и плавающие панели поверх окна.
- **API:** отправка, правка и удаление сообщений (Markdown и HTML); чаты и
  пользователи; поле ввода; музыкальный плеер; настройки и файлы плагина;
  таймеры, JSON, HTTPS к заявленным адресам, корутины; любой метод API
  через `tg.invoke` с кодеком TL слоя клиента.
- **Переводы:** плагин говорит на языке интерфейса — `locales/<язык>.json`
  и `plugin.t()` с формами множественного числа, переведённые имя и
  описание в манифесте.
- **Безопасность:** платежи, звёзды, подарки, входы, сеансы, пароль,
  Telegram Passport и удаление аккаунта, вызванные плагином, ждут
  «Разрешить» от пользователя. Сбой при запуске плагинов включает
  безопасный режим.
- **Менеджер плагинов** (Настройки → Плагины): карточки с иконками,
  версиями и правами, включение, настройки, журнал, отправка, удаление;
  режим разработчика перезагружает папки плагинов при сохранении.
- **Каталог плагинов:** проверенный канал и официальный репозиторий, свои
  каналы, категории, поиск и обновления. Или откройте `.flua` в любом чате.

### Ещё

- 12 иконок приложения (настройки fuckgram → Оформление → Иконка приложения).

## Плагины

Официальные лежат в
[fucksynth/fuckgram-plugins](https://github.com/fucksynth/fuckgram-plugins)
и в канале [@fuckgramplugins](https://t.me/fuckgramplugins); каталог в
клиенте показывает их.

| Плагин | Что делает |
|---|---|
| Плеер Material | музыкальный плеер карточкой Material 3 поверх окна |
| Пиксельный котик | по окну гуляет пиксельный котик 8 пород, его можно погладить и перенести мышью |
| Чистые ссылки | убирает метки (`utm_*`, `fbclid`, `si`…) из отправляемых ссылок |
| Калькулятор | `2+2*2=` уходит как `2+2*2 = 6` |
| Курсы валют | `/cur 100 usd rub` отправляет сумму по сегодняшнему курсу |
| Раскладка | исправляет текст, набранный не в той раскладке |
| Автоответчик | отвечает в личных чатах, пока вас нет |
| Витрина API | все виды страниц, меню и отрисовки, доступные плагину |

Примеры для авторов (hello, weather, raw, selftest) — в
[examples/plugins](examples/plugins).

## Как написать плагин

Плагин — это папка с `manifest.json`, `init.lua` и, для переводов,
`locales/`:

```lua
plugin.on("message.send", function(draft)
    if draft.text == "/shrug" then
        return "¯\\_(ツ)_/¯"
    end
end)

plugin.menu{
    place = "main",
    text = plugin.t("menu.hello"),
    icon = "plugins",
    on_click = function() tg.toast(plugin.t("hello")) end,
}
```

Упакуйте его командой `python tools/flua_pack.py <папка>`. API, права,
страницы, переводы и каталог описаны в [docs/plugins.md](docs/plugins.md);
типы для редакторов — в [docs/sdk/fuckgram.lua](docs/sdk/fuckgram.lua).

## Сборка

- [Windows](docs/building-win.md)
- [Linux](docs/building-linux.md)
- [macOS](docs/building-mac.md)
- [Как выпускается релиз](docs/releasing.md)

Нужны свои [ключи API](docs/api_credentials.md). Ветки: `release` —
основная, из неё собирается Release-клиент; разработка идёт в `dev`, из неё
собирается Debug.

## Лицензия

GPL-3.0, см. [LICENSE](LICENSE). Это форк; исходные уведомления об авторских
правах Telegram Desktop и AyuGram Desktop сохранены в их файлах.

## Благодарности

- [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) от Radolyn Labs —
  основа этого клиента
- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [exteraGram](https://github.com/exteraSquad/exteraGram) и
  [exDesktop](https://github.com/metawka/exDesktop) — их точки встраивания
  плагинов и каталог подсказали путь
- Клиенты, которых благодарит AyuGram: [Kotatogram](https://github.com/kotatogram/kotatogram-desktop),
  [64Gram](https://github.com/TDesktop-x64/tdesktop),
  [Forkgram](https://github.com/forkgram/tdesktop)
- Библиотеки: [Lua](https://www.lua.org), [JSON for Modern C++](https://github.com/nlohmann/json),
  [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
