---@meta
-- Types of the fuckgram plugin API (version 1) for lua-language-server.
-- Add this folder to "Lua.workspace.library" in .luarc.json:
--   { "workspace.library": ["<path to fuckgram>/docs/sdk"] }
-- The client never loads this file: it only gives completion and checks.

---@alias PeerId integer Bot API style: users > 0, groups < 0, channels -100...

---@class Message
---@field id integer
---@field peer PeerId
---@field from PeerId
---@field text string
---@field out boolean
---@field date integer unixtime

---@class Peer
---@field id PeerId
---@field type "user"|"bot"|"group"|"supergroup"|"channel"
---@field name string
---@field username? string
---@field is_self boolean
---@field premium boolean
---@field verified boolean

---@class Draft
---@field peer PeerId
---@field text string

---@class Context
---@field peer? PeerId
---@field message? Message

---@class Link
---@field url string
---@field local boolean tg:// and other client links

---@class Action: Context
---@field type "reply"|"reaction"|"call"|"transcribe"|"notification"|"player.next"|"player.previous"|"player.stop"|"play"|"file.open"|"select"|"message.menu"|"header.click"
---@field emoji? string reaction
---@field custom_emoji_id? integer reaction
---@field remove? boolean reaction toggled off
---@field video? boolean call
---@field reaction? boolean notification about a reaction
---@field path? string file.open

---@class Shown: Context
---@field place "message"|"chat.list"|"chat.title"|"chat.status"|"copy"
---@field text string

---@class Screen: Context
---@field screen "chat.list"|"chat"|"profile"

---@class ProfileRow
---@field label string
---@field value string

---@class HttpRequest
---@field url string https:// only, to a host from the "net:" permissions
---@field method? string GET by default
---@field headers? table<string, string>
---@field body? string

---@class HttpResponse
---@field status integer
---@field body string
---@field headers table<string, string> lower-case names
---@field error? string

---@class TlObject A TL object or method call: constructor name in "_".
---@field _ string

---@class TlError
---@field code integer
---@field type string e.g. "FLOOD_WAIT_5", "PEER_ID_INVALID"
---@field description string

---@class SendOptions
---@field reply_to? integer message id in the same chat
---@field silent? boolean
---@field schedule? integer unixtime
---@field parse_mode? "markdown"|"html"

---@class ConfirmOptions
---@field title? string
---@field text string
---@field ok? string
---@field cancel? string

---@class InputOptions
---@field title? string
---@field text? string
---@field value? string
---@field placeholder? string
---@field ok? string
---@field cancel? string

---@class plugin
---@field id string
---@field name string
---@field version string
plugin = {}

---@overload fun(event: "message.new", handler: fun(message: Message))
---@overload fun(event: "message.send", handler: fun(draft: Draft): (false|string|nil))
---@overload fun(event: "app.state", handler: fun(state: "started"|"active"|"inactive"|"stopping"))
---@overload fun(event: "url.open", handler: fun(link: Link): boolean?)
---@overload fun(event: "action", handler: fun(action: Action): false?)
---@overload fun(event: "display", handler: fun(shown: Shown): string?)
---@overload fun(event: "screen", handler: fun(screen: Screen))
---@overload fun(event: "profile.rows", handler: fun(peer: PeerId): ProfileRow[]?)
---@overload fun(event: "mtproto.request", handler: fun(call: TlObject): (false|TlObject|nil), methods?: string|string[])
---@overload fun(event: "mtproto.response", handler: fun(result: any, method?: string): any, methods?: string|string[])
---@overload fun(event: "mtproto.updates", handler: fun(updates: TlObject): (false|TlObject|nil))
function plugin.on(event, handler, methods) end

---A menu item or a settings row; returns an id for plugin.menu_remove.
---@param place "message"|"main"|"chat"|"profile"|"send"|MenuItem
---@param text? string
---@param handler? fun(context: Context)
---@return integer id
---@overload fun(item: MenuItem): integer
function plugin.menu(place, text, handler) end

plugin.settings = {}

---@param key string
---@param default? boolean|number|string
---@return boolean|number|string|nil
function plugin.settings.get(key, default) end

---@param key string
---@param value boolean|number|string|nil
function plugin.settings.set(key, value) end

---Every setting of the plugin.
---@return table<string, boolean|number|string>
function plugin.settings.all() end

---Clears every setting of the plugin.
function plugin.settings.reset() end

---@alias PageRowType "header"|"divider"|"switch"|"selector"|"input"|"text"|"slider"|"custom"

---@class PageRow
---@field type? PageRowType "text" by default
---@field key? string the setting a switch, selector, input or slider changes
---@field text? string
---@field subtext? string
---@field icon? IconName
---@field default? boolean|number|string the value while the setting is not set
---@field items? string[] selector: the choices, the value is 1-based
---@field placeholder? string input
---@field min? number slider
---@field max? number slider
---@field step? number slider
---@field height? integer custom: 1..1000 px
---@field accent? boolean text: an accent color title
---@field red? boolean text: a red title
---@field on_change? fun(value: boolean|number|string)
---@field on_click? fun()
---@field page? fun(): PageRow[] text: opens a nested page
---@field paint? fun(c: Canvas) custom

---@class Page: PageRow[]
---@field title? string

---Gives the plugin a settings page: a gear on its card in Settings > Plugins.
---`builder` runs every time the page opens and after refresh().
---@param builder fun(): Page
function plugin.settings.page(builder) end

---Builds open pages of the plugin again.
function plugin.settings.refresh() end

---@alias IconName "settings"|"palette"|"customize"|"font"|"chat"|"chats"|"reply"|"forward"|"edit"|"draw"|"copy"|"delete"|"pin"|"unpin"|"select"|"send"|"schedule"|"silent"|"mute"|"unmute"|"link"|"share"|"download"|"folder"|"file"|"photo"|"gif"|"sticker"|"emoji"|"reactions"|"search"|"translate"|"info"|"faq"|"user"|"users"|"add_user"|"admin"|"channel"|"bot"|"plugins"|"lock"|"key"|"shield"|"privacy"|"block"|"eye"|"eye_off"|"ghost"|"bell"|"sound"|"sound_off"|"phone"|"video"|"star"|"fave"|"like"|"premium"|"gift"|"shop"|"stats"|"timer"|"hourglass"|"archive"|"saved"|"night"|"network"|"storage"|"devices"|"experimental"|"power"|"restore"|"refresh"|"add"|"remove"|"cancel"|"expand"|"collapse"|"new_window"|"qr"|"location"|"ip"|"username"|"topics"|"poll"|"article"|"spoiler"|"tag"|"payment"|"code"

---@alias MenuPlace "message"|"main"|"chat"|"profile"|"send"|"settings.main"|"settings.account"|"settings.notifications"|"settings.privacy"|"settings.chats"|"settings.folders"|"settings.advanced"|"settings.calls"|"settings.sessions"|"settings.blocked"|"settings.shortcuts"|"settings.fuckgram"|"settings.fuckgram.ghost"|"settings.fuckgram.filters"|"settings.fuckgram.general"|"settings.fuckgram.appearance"|"settings.fuckgram.chats"|"settings.fuckgram.other"|"settings.plugins"

---@class MenuItem
---@field place MenuPlace
---@field text string
---@field icon? IconName
---@field on_click? fun(ctx: Context)
---@field page? fun(): Page opens a page instead

---Removes a menu item or settings row by the id plugin.menu() gave.
---@param id integer
---@return boolean removed
function plugin.menu_remove(id) end

---Bytes of resources/<path> from the package, nil if there is none.
---@param path string
---@return string?
function plugin.resource(path) end

---The plugin's own files in plugins_data/<id>/files/.
plugin.files = {}

---@param name string 1-64 of A-Z a-z 0-9 _ . -
---@return string? bytes
function plugin.files.read(name) end

---@param name string
---@param bytes string up to 16 MB, 64 MB for all files
---@return boolean written
function plugin.files.write(name, bytes) end

---@param name string
---@return boolean removed
function plugin.files.remove(name) end

---@return string[] names
function plugin.files.list() end

tg = {}

---Writes to the client log.
---@param ... any
function tg.log(...) end

---@class ToastOptions
---@field text string
---@field title? string
---@field duration? integer ms, 500..60000

---@param text string|ToastOptions
function tg.toast(text) end

---Readable text of a value, tables with their fields.
---@param value any
---@return string
function tg.dump(value) end

---Runs `fn` as a coroutine: inside it tg.http.request, tg.invoke and the
---tg.ui dialogs wait for their answer without blocking the client.
---@param fn function
---@param ... any
function tg.async(fn, ...) end

---Any API method of the client's layer. Inside tg.async returns
---result, error; with a callback returns nothing.
---@param call TlObject
---@param callback? fun(result: any, error: TlError?)
---@return any result
---@return TlError? error
function tg.invoke(call, callback) end

---@return Peer?
function tg.me() end

tg.messages = {}

---@param peer PeerId
---@param text string
---@param options? SendOptions
---@return boolean queued
function tg.messages.send(peer, text, options) end

---Your own message, or one you may edit.
---@param peer PeerId
---@param id integer
---@param text string
---@param options? { parse_mode: "markdown"|"html"|nil }
---@return boolean queued
function tg.messages.edit(peer, id, text, options) end

---@class SendFileOptions
---@field bytes string the file, up to 16 MB
---@field name string the file name
---@field caption? string
---@field photo? boolean send a picture as a compressed photo
---@field silent? boolean

---@param peer PeerId
---@param file SendFileOptions
---@return boolean queued
function tg.messages.send_file(peer, file) end

---@param peer PeerId
---@param ids integer|integer[] up to 100
---@param revoke? boolean for everyone, when allowed for all of them
---@return integer count of messages being deleted
function tg.messages.delete(peer, ids, revoke) end

---A message the client has loaded, nil otherwise.
---@param peer PeerId
---@param id integer
---@return Message?
function tg.messages.get(peer, id) end

tg.peers = {}

---A chat or user the client knows, nil otherwise.
---@param id PeerId
---@return Peer?
function tg.peers.get(id) end

tg.chats = {}

---@return PeerId? peer of the chat open in the active window
function tg.chats.current() end

tg.compose = {}

---@return string? text of the message field in the open chat
function tg.compose.get() end

---@param text string
---@return boolean
function tg.compose.set(text) end

tg.ui = {}

---Shows `display` handlers again on what is already on the screen.
function tg.ui.refresh() end

---@param text string
---@param title? string
function tg.ui.alert(text, title) end

---@param options string|ConfirmOptions
---@param callback? fun(confirmed: boolean)
---@return boolean confirmed inside tg.async
function tg.ui.confirm(options, callback) end

---@param options InputOptions
---@param callback? fun(text: string?)
---@return string? text inside tg.async, nil when cancelled
function tg.ui.input(options, callback) end

---@param text string
function tg.ui.copy(text) end

---@class DialogOptions
---@field title? string
---@field text? string
---@field items? string[] a list to choose from
---@field buttons? string[] up to 3
---@field danger? integer the number of a button to make red

---@param options DialogOptions
---@param callback? fun(chosen: integer?)
---@return integer? chosen the item or button, nil if closed (inside tg.async)
function tg.ui.dialog(options, callback) end

---@class Current
---@field screen "chat"|"chats"|"none"
---@field peer? PeerId
---@field title? string the chat name
---@field window? string the window title

---@return Current
function tg.ui.current() end

---Opens a chat id, "@username" or a link.
---@param target PeerId|string
function tg.ui.open(target) end

---@alias Color string|{ r: integer, g: integer, b: integer, a: integer? } "#rrggbb", "#aarrggbb" or a theme color name

---The current value of a theme color as "#aarrggbb".
---@param name string
---@return string?
function tg.ui.color(name) end

---Decodes PNG, JPEG, WebP or GIF bytes for c:image().
---@param bytes string
---@return integer? id
---@return integer width
---@return integer height
function tg.ui.image(bytes) end

---@param id integer
function tg.ui.image_free(id) end

---@class TextOptions
---@field size? number px, 13 by default
---@field bold? boolean
---@field italic? boolean
---@field color? Color
---@field align? "left"|"center"|"right"
---@field width? number wraps the text
---@field height? number cuts it

---A canvas of one paint call. Sizes are px at 100 % interface scale.
---@class Canvas
local Canvas = {}
---@return integer width
---@return integer height
function Canvas:size() end
---@param color Color
function Canvas:fill(color) end
function Canvas:rect(x, y, w, h, color, radius) end
function Canvas:stroke(x, y, w, h, color, width, radius) end
function Canvas:circle(x, y, radius, color) end
function Canvas:line(x1, y1, x2, y2, color, width) end
---@param options? TextOptions
---@return number width
---@return number height
function Canvas:text(x, y, text, options) end
---@param options? TextOptions
---@return number width
---@return number height
function Canvas:measure(text, options) end
function Canvas:image(id, x, y, w, h, radius) end
---@param value number 0..1
function Canvas:opacity(value) end
---@param points number[] x1, y1, x2, y2, ...
---@param color Color
function Canvas:polygon(points, color) end

---@class OverlayOptions
---@field x? integer
---@field y? integer
---@field width? integer
---@field height? integer
---@field corner? "top-left"|"top-right"|"bottom-left"|"bottom-right"
---@field paint fun(c: Canvas)
---@field on_click? fun(x: integer, y: integer) without it clicks go through

---@class Overlay
---@field id integer
local Overlay = {}
function Overlay:update() end
---@param geometry { x: integer?, y: integer?, width: integer?, height: integer?, corner: string? }
function Overlay:move(geometry) end
function Overlay:close() end

---Width and height of the window content at 100 % scale, nil without a window.
---@return integer? width
---@return integer? height
function tg.ui.window_size() end

---Milliseconds of a steady clock.
---@return integer
function tg.now() end

---@class PlayerState
---@field title string
---@field performer string
---@field playing boolean
---@field position number seconds
---@field duration number seconds
---@field voice boolean a voice message, not music
---@field has_next boolean
---@field has_previous boolean
---@field peer? PeerId
---@field message? integer

---The client's music player (permission "player").
tg.player = {}
---@return PlayerState?
function tg.player.state() end
function tg.player.toggle() end
function tg.player.play() end
function tg.player.pause() end
function tg.player.stop() end
function tg.player.next() end
function tg.player.previous() end
---@param fraction number 0..1
function tg.player.seek(fraction) end

---A widget over the window that the plugin paints.
---@param options OverlayOptions
---@return Overlay?
function tg.ui.overlay(options) end

tg.timer = {}

---@param ms integer
---@param fn function
---@return integer id
function tg.timer.after(ms, fn) end

---@param ms integer at least 100
---@param fn function
---@return integer id
function tg.timer.every(ms, fn) end

---@param id integer
function tg.timer.cancel(id) end

tg.http = {}

---@param request HttpRequest
---@param callback? fun(response: HttpResponse)
---@return HttpResponse response inside tg.async
function tg.http.request(request, callback) end

tg.json = {}

---@param value any
---@return string
function tg.json.encode(value) end

---@param text string
---@return any value
---@return string? error
function tg.json.decode(text) end

---Called after the entry file ran.
function on_enable() end

---Called before the plugin stops.
function on_disable() end
