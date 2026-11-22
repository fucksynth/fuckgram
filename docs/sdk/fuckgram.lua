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

---@param place "message"|"main"|"chat"|"profile"
---@param text string
---@param handler fun(context: Context)
function plugin.menu(place, text, handler) end

plugin.settings = {}

---@param key string
---@param default? boolean|number|string
---@return boolean|number|string|nil
function plugin.settings.get(key, default) end

---@param key string
---@param value boolean|number|string|nil
function plugin.settings.set(key, value) end

tg = {}

---Writes to the client log.
---@param ... any
function tg.log(...) end

---@param text string
function tg.toast(text) end

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
---@return boolean queued
function tg.messages.edit(peer, id, text) end

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
