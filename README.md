# MultiAddonManager

A MetaMod plugin that allows you to use multiple workshop addons at once and have clients download them.

## ConVars
- `mm_extra_addons <ids>` The workshop IDs of extra addons separated by commas, addons will be downloaded (if not present) and mounted (e.g. "3090239773,3070231528").
  Once downloads are done, the map is automatically reloaded so content can be precached.
- `mm_client_extra_addons <ids>` The workshop IDs of extra client-side only addons that will be loaded by all clients, separated by commas. These addons are not loaded or downloaded by the server.
  Changes will only apply to future clients.

- `mm_extra_addons_timeout <seconds> (default 10)` How long until clients are timed out in between connects for extra addons, timed out clients will reconnect for their current pending download.
- `mm_print_searchpaths` Print all the search paths currently mounted by the server.
- `mm_addon_mount_download <0/1> (default 0)` If enabled, the plugin will initiate an addon download every time even if it's already installed, this will guarantee that updates are applied immediately.
- `mm_cache_clients_with_addons <0/1> (default 0)` If enabled, the plugin will keep track of which addons client SteamIDs have downloaded to prevent sending them addons when they already have them (i.e. when they rejoin or the map changes).
- `mm_cache_clients_duration <0/seconds> (default 0)` How long to cache clients' downloaded addons list, pass 0 for forever.
- `mm_block_disconnect_messages <0/1> (default 0)` If enabled, the plugin will block *ALL* disconnect events with the "loop shutdown" reason. This will prevent disconnect chat messsages whenever someone reconnects because they're getting an addon.
- `mm_addon_debug <0/1> (default 0)` Whether to print some extra debug information (mainly when clients are joining)

## Commands
- `mm_download_addon <id>` Download an addon manually.

 Both of these commands require a map reload to apply changes.
- `mm_add_addon <id>` Add an addon to the list, but don't mount.
- `mm_remove_addon <id>` Remove an addon from the list, but don't unmount.

 These following commands will only affect future clients:
- `mm_add_client_addon <id>` Add a workshop ID to the global client-only addon list.
- `mm_remove_client_addon <id>` Remove a workshop ID from the global client-only addon list.

## Usage in other MetaMod plugins
- Include the [public header](https://github.com/Source2ZE/MultiAddonManager/blob/main/public/imultiaddonmanager.h).
- Query the interface in `AllPluginsLoaded` like this:
```cpp
IMultiAddonManager *pInterface = (IMultiAddonManager*)g_SMAPI->MetaFactory(MULTIADDONMANAGER_INTERFACE, nullptr, nullptr);
```

## Installation

- Install [Metamod](https://cs2.poggu.me/metamod/installation/)
- Download the [latest release package](https://github.com/Source2ZE/MultiAddonManager/releases/latest) for your OS
- Extract the package contents into `game/csgo` on your server
- Edit the config file at `game/csgo/cfg/multiaddonmanager/multiaddonmanager.cfg`
