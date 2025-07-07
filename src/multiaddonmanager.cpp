/**
 * =============================================================================
 * MultiAddonManager
 * Copyright (C) 2024 xen
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "networkbasetypes.pb.h"
#include "gameevents.pb.h"

#include <stdio.h>
#include "multiaddonmanager.h"
#include "module.h"
#include "utils/plat.h"
#include "networksystem/inetworkserializer.h"
#include "networksystem/inetworkmessages.h"
#include "convar.h"
#include "hoststate.h"
#include "igameeventsystem.h"
#include "serversideclient.h"
#include "funchook.h"
#include "filesystem.h"
#include "steam/steam_gameserver.h"
#include <string>
#include <sstream>
#include "iserver.h"

#include "tier0/memdbgon.h"

CConVar<bool> mm_addon_mount_download("mm_addon_mount_download", FCVAR_NONE, "Whether to download an addon upon mounting even if it's installed", false);
CConVar<bool> mm_block_disconnect_messages("mm_block_disconnect_messages", FCVAR_NONE, "Whether to block \"loop shutdown\" disconnect messages", false);
CConVar<bool> mm_cache_clients_with_addons("mm_cache_clients_with_addons", FCVAR_NONE, "Whether to cache clients addon download list, this will prevent reconnects on mapchange/rejoin", false);
CConVar<float> mm_cache_clients_duration("mm_cache_clients_duration", FCVAR_NONE, "How long to cache clients' downloaded addons list in seconds, pass 0 for forever.", 0.0f);
CConVar<float> mm_extra_addons_timeout("mm_extra_addons_timeout", FCVAR_NONE, "How long until clients are timed out in between connects for extra addons in seconds, requires mm_extra_addons to be used", 10.f);

void Message(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	LoggingSystem_Log(0, LS_MESSAGE, Color(0, 255, 200), "[MultiAddonManager] %s", buf);

	va_end(args);
}

void Panic(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	Warning("[MultiAddonManager] %s", buf);

	va_end(args);
}

void StringToVector(const char *pszString, CUtlVector<std::string> &vector)
{
	std::stringstream stream(pszString);

	vector.RemoveAll();

	while (stream.good())
	{
		std::string substr;
		getline(stream, substr, ',');

		if (!substr.empty())
			vector.AddToTail(substr);
	}
}

std::string VectorToString(CUtlVector<std::string> &vector)
{
	std::string result;

	FOR_EACH_VEC(vector, i)
	{
		result += vector[i];

		if (i + 1 < vector.Count())
			result += ',';
	}

	return result;
}

typedef bool (FASTCALL *SendNetMessage_t)(CServerSideClient *, CNetMessage*, NetChannelBufType_t);
typedef void (FASTCALL *HostStateRequest_t)(CHostStateMgr*, CHostStateRequest*);
typedef void (FASTCALL *ReplyConnection_t)(INetworkGameServer *, CServerSideClient *);

bool FASTCALL Hook_SendNetMessage(CServerSideClient *pClient, CNetMessage *pData, NetChannelBufType_t bufType);
void FASTCALL Hook_SetPendingHostStateRequest(CHostStateMgr*, CHostStateRequest*);
void FASTCALL Hook_ReplyConnection(INetworkGameServer *, CServerSideClient *);

SendNetMessage_t g_pfnSendNetMessage = nullptr;
HostStateRequest_t g_pfnSetPendingHostStateRequest = nullptr;
ReplyConnection_t g_pfnReplyConnection = nullptr;

funchook_t *g_pSendNetMessageHook = nullptr;
funchook_t *g_pSetPendingHostStateRequest = nullptr;
funchook_t *g_pReplyConnectionHook = nullptr;

int g_iLoadEventsFromFileHookId = -1;

class GameSessionConfiguration_t { };

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);
SH_DECL_HOOK3_void(INetworkServerService, StartupServer, SH_NOATTRIB, 0, const GameSessionConfiguration_t &, ISource2WorldSession *, const char *);
SH_DECL_HOOK6(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, CPlayerSlot, const char*, uint64, const char *, bool, CBufferString *);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, false, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *);
SH_DECL_HOOK4_void(IServerGameClients, ClientActive, SH_NOATTRIB, false, CPlayerSlot, bool, const char *, uint64);
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK8_void(IGameEventSystem, PostEventAbstract, SH_NOATTRIB, 0, CSplitScreenSlot, bool, int, const uint64 *,
	INetworkMessageInternal *, const CNetMessage *, unsigned long, NetChannelBufType_t);
SH_DECL_HOOK2(IGameEventManager2, LoadEventsFromFile, SH_NOATTRIB, 0, int, const char *, bool);

// Signatures

// "Discarding pending request '%s, %u'\n"
// "Sending S2C_CONNECTION to %s [addons:'%s']\n"
#ifdef PLATFORM_WINDOWS
constexpr const byte g_HostStateRequest_Sig[] = "\x48\x89\x74\x24\x2A\x57\x48\x83\xEC\x2A\x33\xF6\x48\x8B\xFA\x48\x39\x35";
constexpr const byte g_ReplyConnection_Sig[] = "\x48\x8B\xC4\x55\x41\x54\x41\x55\x41\x57";
#else
constexpr const byte g_HostStateRequest_Sig[] = "\x55\x48\x89\xE5\x41\x56\x41\x55\x41\x54\x49\x89\xF4\x53\x48\x83\x7F";
constexpr const byte g_ReplyConnection_Sig[] = "\x55\xB9\x00\x01\x00\x00";
#endif


// Offsets
constexpr int g_iServerAddonsOffset = 328;

#ifdef PLATFORM_WINDOWS
constexpr int g_iSendNetMessageOffset = 15;
constexpr int g_iClientListOffset = 624;
#else
constexpr int g_iSendNetMessageOffset = 16;
constexpr int g_iClientListOffset = 640;
#endif

/* 
The general workflow is defined as follows:
0. The server defines a list of server side addons and global client side addons to mount.
1. Client connects and request for the list of addons through ReplyConnection. MAM get the full list of addons to load.
2. If there is at least one addon to load, client will be prompted to download the first addon.
3. Once done, client reconnects and ClientConnect fires. If connected within the timeout interval, MAM marks this first addon as downloaded, 
	then check if there are other addons to download. If there is at least one, it will send a signon message to the client through SendNetMessage.
4. Client will be prompted to download the next addon. Client reconnects and ClientConnect fires again. 
	MAM marks the previous downloading addon (the one sent in the previous signon message) as done, and keep sending signon messages until all addons are downloaded.
5. Once all addons are downloaded, MAM stops sending custom signon messages.
Note: 
The list of addons to download does not have to be in order, but the addon list that the client uses to load is. This order is somewhat arbitrarily defined as follows: 
	- Original server workshop map (if any)
	- Server side *mounted* addons (m_MountedAddons)
	- Client side global addons (m_GlobalClientAddons)
	- Client side client-specific addons (addonsToLoad).
While plugins using the interface can add/remove addons at any time between these steps, it should be fine since the list of addon to load is newly checked every time the client connects.
*/
struct ClientAddonInfo_t
{
	double lastActiveTime {};
	CUtlVector<std::string> addonsToLoad;
	CUtlVector<std::string> downloadedAddons;
	std::string currentPendingAddon;
};

CUtlVector<CServerSideClient *> *GetClientList()
{
	if (!g_pNetworkServerService)
		return nullptr;

	return (CUtlVector<CServerSideClient *> *)((char *)g_pNetworkServerService->GetIGameServer() + g_iClientListOffset);
}
std::unordered_map<uint64, ClientAddonInfo_t> g_ClientAddons; 

CConVar<CUtlString> mm_extra_addons("mm_extra_addons", FCVAR_NONE, "The workshop IDs of extra addons separated by commas, addons will be downloaded (if not present) and mounted", CUtlString(""),
	[](CConVar<CUtlString> *cvar, CSplitScreenSlot slot, const CUtlString *new_val, const CUtlString *old_val)
	{
		StringToVector(new_val->Get(), g_MultiAddonManager.m_ExtraAddons);

		g_MultiAddonManager.RefreshAddons();
	});


CConVar<CUtlString> mm_client_extra_addons("mm_client_extra_addons", FCVAR_NONE, "The workshop IDs of extra client addons that will be applied to all clients, separated by commas", CUtlString(""),
	[](CConVar<CUtlString> *cvar, CSplitScreenSlot slot, const CUtlString *new_val, const CUtlString *old_val)
	{
		StringToVector(new_val->Get(), g_MultiAddonManager.m_GlobalClientAddons);
	});

MultiAddonManager g_MultiAddonManager;
INetworkGameServer *g_pNetworkGameServer = nullptr;
CSteamGameServerAPIContext g_SteamAPI;
CGlobalVars *gpGlobals = nullptr;
IGameEventSystem *g_pGameEventSystem = nullptr;
IGameEventManager2 *g_pGameEventManager = nullptr;

PLUGIN_EXPOSE(MultiAddonManager, g_MultiAddonManager);
bool MultiAddonManager::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	// Required to get the IMetamodListener events
	g_SMAPI->AddListener( this, this );

	CModule engineModule(ROOTBIN, "engine2");
	CModule serverModule(GAMEBIN, "server");

	int sig_error;

	g_pfnSetPendingHostStateRequest = (HostStateRequest_t)engineModule.FindSignature(g_HostStateRequest_Sig, sizeof(g_HostStateRequest_Sig) - 1, sig_error);

	if (!g_pfnSetPendingHostStateRequest)
	{
		V_snprintf(error, maxlen, "Could not find the signature for HostStateRequest\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Panic("Signature for HostStateRequest occurs multiple times! Using first match but this might end up crashing!\n");
	}

	g_pSetPendingHostStateRequest = funchook_create();
	funchook_prepare(g_pSetPendingHostStateRequest, (void**)&g_pfnSetPendingHostStateRequest, (void*)Hook_SetPendingHostStateRequest);
	funchook_install(g_pSetPendingHostStateRequest, 0);

	// We're using funchook even though it's a virtual function because it can be called on a different thread and SourceHook isn't thread-safe
	void **pServerSideClientVTable = (void **)engineModule.FindVirtualTable("CServerSideClient");
	g_pfnSendNetMessage = (SendNetMessage_t)pServerSideClientVTable[g_iSendNetMessageOffset];

	g_pSendNetMessageHook = funchook_create();
	funchook_prepare(g_pSendNetMessageHook, (void**)&g_pfnSendNetMessage, (void*)Hook_SendNetMessage);
	funchook_install(g_pSendNetMessageHook, 0);

	g_pfnReplyConnection = (ReplyConnection_t)engineModule.FindSignature(g_ReplyConnection_Sig, sizeof(g_ReplyConnection_Sig) - 1, sig_error);

	if (!g_pfnReplyConnection)
	{
		V_snprintf(error, maxlen, "Could not find the signature for ReplyConnection\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Panic("Signature for ReplyConnection occurs multiple times! Using first match but this might end up crashing!\n");
	}

	g_pReplyConnectionHook = funchook_create();
	funchook_prepare(g_pReplyConnectionHook, (void**)&g_pfnReplyConnection, (void*)Hook_ReplyConnection);
	funchook_install(g_pReplyConnectionHook, 0);
	
	SH_ADD_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameServerSteamAPIActivated), false);
	SH_ADD_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &MultiAddonManager::Hook_StartupServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientConnect, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientConnect), false);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientDisconnect), true);
	SH_ADD_HOOK(IServerGameClients, ClientActive, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientActive), true);
	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameFrame), true);
	SH_ADD_HOOK(IGameEventSystem, PostEventAbstract, g_pGameEventSystem, SH_MEMBER(this, &MultiAddonManager::Hook_PostEvent), false);

	auto pCGameEventManagerVTable = (IGameEventManager2*)serverModule.FindVirtualTable("CGameEventManager");

	if (!pCGameEventManagerVTable)
		return false;

	g_iLoadEventsFromFileHookId = SH_ADD_DVPHOOK(IGameEventManager2, LoadEventsFromFile, pCGameEventManagerVTable, SH_MEMBER(this, &MultiAddonManager::Hook_LoadEventsFromFile), false);

	if (late)
	{
		g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();
		gpGlobals = g_pEngineServer->GetServerGlobals();
		if (!CommandLine()->HasParm("-dedicated"))
		{
			g_SteamAPI.Init();
			m_CallbackDownloadItemResult.Register(this, &MultiAddonManager::OnAddonDownloaded);
		}
	}

	META_CONVAR_REGISTER(FCVAR_RELEASE);

	g_pEngineServer->ServerCommand("exec multiaddonmanager/multiaddonmanager");

	Message("Plugin loaded successfully!\n");

	return true;
}

bool MultiAddonManager::Unload(char *error, size_t maxlen)
{
	ClearAddons();

	SH_REMOVE_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameServerSteamAPIActivated), false);
	SH_REMOVE_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &MultiAddonManager::Hook_StartupServer), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientConnect, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientConnect), false);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientDisconnect), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientActive, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientActive), true);
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameFrame), true);
	SH_REMOVE_HOOK(IGameEventSystem, PostEventAbstract, g_pGameEventSystem, SH_MEMBER(this, &MultiAddonManager::Hook_PostEvent), false);
	SH_REMOVE_HOOK_ID(g_iLoadEventsFromFileHookId);

	if (g_pSetPendingHostStateRequest)
	{
		funchook_uninstall(g_pSetPendingHostStateRequest, 0);
		funchook_destroy(g_pSetPendingHostStateRequest);
	}

	if (g_pSendNetMessageHook)
	{
		funchook_uninstall(g_pSendNetMessageHook, 0);
		funchook_destroy(g_pSendNetMessageHook);
	}
	
	return true;
}

void *MultiAddonManager::OnMetamodQuery(const char *iface, int *ret)
{
	if (V_strcmp(iface, MULTIADDONMANAGER_INTERFACE))
	{
		if (ret)
			*ret = META_IFACE_FAILED;

		return nullptr;
	}

	if (ret)
		*ret = META_IFACE_OK;

	return static_cast<IMultiAddonManager*>(&g_MultiAddonManager);
}

void MultiAddonManager::BuildAddonPath(const char *pszAddon, char *buf, size_t len, bool bLegacy = false)
{
	// The workshop on a dedicated server is stored relative to the working directory for whatever reason
	static CBufferStringGrowable<MAX_PATH> s_sWorkingDir;
	ExecuteOnce(g_pFullFileSystem->GetSearchPath("EXECUTABLE_PATH", GET_SEARCH_PATH_ALL, s_sWorkingDir, 1));

	V_snprintf(buf, len, "%ssteamapps/workshop/content/730/%s/%s%s.vpk", s_sWorkingDir.Get(), pszAddon, pszAddon, bLegacy ? "" : "_dir");
}

bool MultiAddonManager::MountAddon(const char *pszAddon, bool bAddToTail = false)
{
	if (!pszAddon || !*pszAddon)
		return false;

	CUtlVector<std::string> serverMountedAddons;
	StringToVector(this->m_sCurrentWorkshopMap.c_str(), serverMountedAddons);
	if (serverMountedAddons.Find(pszAddon) != -1)
	{
		Message("%s: Addon %s is already mounted by the server\n", __func__, pszAddon);
		return false;
	}

	PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
	uint32 iAddonState = g_SteamAPI.SteamUGC()->GetItemState(iAddon);

	if (iAddonState & k_EItemStateLegacyItem)
	{
		Message("%s: Addon %s is not compatible with Source 2, skipping\n", __func__, pszAddon);
		return false;
	}

	if (!(iAddonState & k_EItemStateInstalled))
	{
		Message("%s: Addon %s is not installed, queuing a download\n", __func__, pszAddon);
		DownloadAddon(pszAddon, true, true);
		return false;
	}
	else if (mm_addon_mount_download.Get())
	{
		// Queue a download anyway in case the addon got an update and the server desires this, but don't reload the map when done
		DownloadAddon(pszAddon, false, true);
	}

	char pszPath[MAX_PATH];
	BuildAddonPath(pszAddon, pszPath, sizeof(pszPath));

	if (!g_pFullFileSystem->FileExists(pszPath))
	{
		// This might be a legacy addon (before mutli-chunk was introduced), try again without the _dir
		BuildAddonPath(pszAddon, pszPath, sizeof(pszPath), true);

		if (!g_pFullFileSystem->FileExists(pszPath))
		{
			Panic("%s: Addon %s not found at %s\n", __func__, pszAddon, pszPath);
			return false;
		}
	}
	else
	{
		// We still need it without _dir anyway because the filesystem will append suffixes if needed
		BuildAddonPath(pszAddon, pszPath, sizeof(pszPath), true);
	}

	if (m_MountedAddons.Find(pszAddon) != -1)
	{
		Panic("%s: Addon %s is already mounted\n", __func__, pszAddon);
		return false;
	}

	Message("Adding search path: %s\n", pszPath);

	g_pFullFileSystem->AddSearchPath(pszPath, "GAME", bAddToTail ? PATH_ADD_TO_TAIL : PATH_ADD_TO_HEAD, SEARCH_PATH_PRIORITY_VPK);
	m_MountedAddons.AddToTail(pszAddon);

	return true;
}

bool MultiAddonManager::UnmountAddon(const char *pszAddon)
{
	if (!pszAddon || !*pszAddon)
		return false;

	char path[MAX_PATH];
	BuildAddonPath(pszAddon, path, sizeof(path));

	if (!g_pFullFileSystem->RemoveSearchPath(path, "GAME"))
		return false;

	m_MountedAddons.FindAndFastRemove(pszAddon);

	Message("Removing search path: %s\n", path);

	return true;
}

void MultiAddonManager::PrintDownloadProgress()
{
	if (m_DownloadQueue.Count() == 0)
		return;

	uint64 iBytesDownloaded = 0;
	uint64 iTotalBytes = 0;

	if (!g_SteamAPI.SteamUGC()->GetItemDownloadInfo(m_DownloadQueue.Head(), &iBytesDownloaded, &iTotalBytes) || !iTotalBytes)
		return;

	double flMBDownloaded = (double)iBytesDownloaded / 1024 / 1024;
	double flTotalMB = (double)iTotalBytes / 1024 / 1024;

	double flProgress = (double)iBytesDownloaded / (double)iTotalBytes;
	flProgress *= 100.f;

	Message("Downloading addon %lli: %.2f/%.2f MB (%.2f%%)\n", m_DownloadQueue.Head(), flMBDownloaded, flTotalMB, flProgress);
}

// bImportant adds downloads to the pending list, which will reload the current map once the list is exhausted
// bForce will initiate a download even if the addon already exists and is updated
// Internally, downloads are queued up and processed one at a time
bool MultiAddonManager::DownloadAddon(const char *pszAddon, bool bImportant, bool bForce)
{
	if (!g_SteamAPI.SteamUGC())
	{
		Panic("%s: Cannot download addons as the Steam API is not initialized\n", __func__);
		return false;
	}

	PublishedFileId_t addon = V_StringToUint64(pszAddon, 0);

	if (addon == 0)
	{
		Panic("%s: Invalid addon %s\n", __func__, pszAddon);
		return false;
	}

	if (m_DownloadQueue.Check(addon))
	{
		Panic("%s: Addon %s is already queued for download!\n", __func__, pszAddon);
		return false;
	}

	uint32 nItemState = g_SteamAPI.SteamUGC()->GetItemState(addon);

	if (!bForce && (nItemState & k_EItemStateInstalled))
	{
		Message("Addon %lli is already installed\n", addon);
		return true;
	}

	if (!g_SteamAPI.SteamUGC()->DownloadItem(addon, false))
	{
		Panic("%s: Addon download for %lli failed to start, addon ID is invalid or server is not logged on Steam\n", __func__, addon);
		return false;
	}
	
	if (bImportant && m_ImportantDownloads.Find(addon) == -1)
		m_ImportantDownloads.AddToTail(addon);

	m_DownloadQueue.Insert(addon);

	Message("Addon download started for %lli\n", addon);

	return true;
}

void MultiAddonManager::RefreshAddons(bool bReloadMap)
{
	if (!g_SteamAPI.SteamUGC())
		return;

	Message("Refreshing addons (%s)\n", VectorToString(m_ExtraAddons).c_str());

	// Remove our paths first in case addons were switched
	FOR_EACH_VEC_BACK(m_MountedAddons, i)
		UnmountAddon(m_MountedAddons[i].c_str());

	bool bAllAddonsMounted = true;

	FOR_EACH_VEC(m_ExtraAddons, i)
	{
		if (!MountAddon(m_ExtraAddons[i].c_str()))
			bAllAddonsMounted = false;
	}

	if (bAllAddonsMounted && bReloadMap)
		ReloadMap();
}

void MultiAddonManager::ClearAddons()
{
	m_ExtraAddons.RemoveAll();

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = "";
	
	FOR_EACH_VEC_BACK(m_MountedAddons, i)
		UnmountAddon(m_MountedAddons[i].c_str());
}

void MultiAddonManager::Hook_GameServerSteamAPIActivated()
{
	// This is only intended for dedicated servers
	// Also if this is somehow called again don't do anything
	if (!CommandLine()->HasParm("-dedicated") || g_SteamAPI.SteamUGC())
		return;

	Message("Steam API Activated\n");

	g_SteamAPI.Init();
	m_CallbackDownloadItemResult.Register(this, &MultiAddonManager::OnAddonDownloaded);

	Message("Refreshing addons to check for updates\n");
	RefreshAddons(true);

	RETURN_META(MRES_IGNORED);
}

void MultiAddonManager::ReloadMap()
{
	char cmd[MAX_PATH];

	// Using the concommand here as g_pEngineServer->ChangeLevel doesn't unmount workshop maps and we wanna be clean.
	// See Hook_SetPendingHostStateRequest's comment for more details.
	if (m_sCurrentWorkshopMap.empty())
		V_snprintf(cmd, sizeof(cmd), "changelevel %s", gpGlobals->mapname.ToCStr());
	else
		V_snprintf(cmd, sizeof(cmd), "host_workshop_map %s", m_sCurrentWorkshopMap.c_str());

	g_pEngineServer->ServerCommand(cmd);
}

void MultiAddonManager::OnAddonDownloaded(DownloadItemResult_t *pResult)
{
	if (pResult->m_eResult == k_EResultOK)
		Message("Addon %lli downloaded successfully\n", pResult->m_nPublishedFileId);
	else
		Panic("Addon %lli download failed with reason \"%s\" (%i)\n", pResult->m_nPublishedFileId, g_SteamErrorMessages[pResult->m_eResult], pResult->m_eResult);

	// This download isn't triggered by us, don't do anything
	if (!m_DownloadQueue.Check(pResult->m_nPublishedFileId))
		return;

	m_DownloadQueue.RemoveAtHead();
	
	bool bFound = m_ImportantDownloads.FindAndRemove(pResult->m_nPublishedFileId);
	
	// That was the last important download, now reload the map
	if (bFound && m_ImportantDownloads.Count() == 0)
	{
		Message("All addon downloads finished, reloading map %s\n", gpGlobals->mapname);
		ReloadMap();
	}
}

bool MultiAddonManager::AddAddon(const char *pszAddon, bool bRefresh)
{
	if (m_ExtraAddons.Find(pszAddon) != -1)
	{
		Panic("Addon %s is already in the list!\n", pszAddon);
		return false;
	}

	Message("Adding %s to addon list\n", pszAddon);

	m_ExtraAddons.AddToTail(pszAddon);

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_ExtraAddons).c_str();

	Message("Clearing client cache due to addons changing");

	if (bRefresh)
		RefreshAddons();

	return true;
}

bool MultiAddonManager::RemoveAddon(const char *pszAddon, bool bRefresh)
{
	int index = m_ExtraAddons.Find(pszAddon);

	if (index == -1)
	{
		Panic("Addon %s is not in the list!\n", pszAddon);
		return false;
	}

	Message("Removing %s from addon list\n", pszAddon);

	m_ExtraAddons.Remove(index);

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_ExtraAddons).c_str();

	if (bRefresh)
		RefreshAddons();

	return true;
}

CNetMessagePB<CNETMsg_SignonState> *GetAddonSignonStateMessage(const char *pszAddon)
{
	if (!gpGlobals)
		return nullptr;

	INetworkMessageInternal *pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("SignonState");
	CNetMessagePB<CNETMsg_SignonState> *pMsg = pNetMsg->AllocateMessage()->ToPB<CNETMsg_SignonState>();
	pMsg->set_spawn_count(gpGlobals->serverCount);
	pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);
	pMsg->set_addons(pszAddon ? pszAddon : "");
	pMsg->set_num_server_players(GetClientList()->Count());
	for (int i = 0; i < GetClientList()->Count(); i++)
	{
		auto client = GetClientList()->Element(i);

		char const *szNetworkId = g_pEngineServer->GetPlayerNetworkIDString(client->GetPlayerSlot());

		pMsg->add_players_networkids(szNetworkId);
	}

	return pMsg;
}

bool MultiAddonManager::HasUGCConnection()
{
	return g_SteamAPI.SteamUGC() != nullptr;
}

void MultiAddonManager::AddClientAddon(const char *pszAddon, uint64 steamID64, bool bRefresh)
{
	if (!steamID64)
	{
		if (m_GlobalClientAddons.Find(pszAddon) != -1)
		{
			Panic("Addon %s is already in the list!\n", pszAddon);
			return;
		}
	
		m_GlobalClientAddons.AddToTail(pszAddon);
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		if (g_ClientAddons[steamID64].addonsToLoad.Find(pszAddon) != -1)
		{
			Panic("Addon %s is already in the list!\n", pszAddon);
			return;
		}

		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.AddToTail(pszAddon);
	}
	
	if (bRefresh)
	{
		CUtlVector<CServerSideClient*> &clients = *GetClientList();
		auto pMsg = GetAddonSignonStateMessage(pszAddon);
		if (!pMsg)
		{
			Panic("Failed to create signon state message for %s\n", pszAddon);
			return;
		}
		FOR_EACH_VEC(clients, i)
		{
			CServerSideClient *pClient = clients[i];
			if (steamID64 == 0 || pClient->GetClientSteamID()->ConvertToUint64() == steamID64)
			{
				// Client is already loading, telling them to reload now will actually just disconnect them. ("Received signon %i when at %i\n" in client console)
				if (pClient->GetSignonState() == SIGNONSTATE_CHANGELEVEL)
					break;
				// Client still has addons to load anyway, they don't need to be told to reload
				if (!g_ClientAddons[steamID64].currentPendingAddon.empty())
					break;
				ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];

				CUtlVector<std::string> addons;
				g_MultiAddonManager.GetClientAddons(addons, steamID64);
				
				FOR_EACH_VEC(clientInfo.downloadedAddons, j)
				{
					addons.FindAndRemove(clientInfo.downloadedAddons[j]);
				}

				if (!addons.Count())
				{
					break;
				}
				clientInfo.currentPendingAddon = addons.Head();
				
				pClient->GetNetChannel()->SendNetMessage(pMsg, BUF_RELIABLE);

				if (steamID64)
				{
					break;
				}
			}
		}
		delete pMsg;
	}
}

void MultiAddonManager::RemoveClientAddon(const char *pszAddon, uint64 steamID64)
{
	if (!steamID64)
	{
		m_GlobalClientAddons.FindAndRemove(pszAddon);
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.FindAndRemove(pszAddon);
	}
}

void MultiAddonManager::ClearClientAddons(uint64 steamID64)
{
	if (!steamID64)
	{
		m_GlobalClientAddons.RemoveAll();
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.RemoveAll();
	}
}

void MultiAddonManager::GetClientAddons(CUtlVector<std::string> &addons, uint64 steamID64)
{
	addons.RemoveAll();
	
	if (!GetCurrentWorkshopMap().empty())
		addons.AddToTail(GetCurrentWorkshopMap().c_str());
	// The list of mounted addons should never contain the workshop map.
	addons.AddVectorToTail(m_MountedAddons);
	// Also make sure we don't have duplicates.
	FOR_EACH_VEC(m_GlobalClientAddons, i)
	{
		if (addons.Find(m_GlobalClientAddons[i].c_str()) == -1)
			addons.AddToTail(m_GlobalClientAddons[i].c_str());
	}
	// If we specify a client steamID64, check for the addons exclusive to this client as well.
	if (steamID64)
	{
		FOR_EACH_VEC(g_ClientAddons[steamID64].addonsToLoad, i)
		{
			if (addons.Find(g_ClientAddons[steamID64].addonsToLoad[i].c_str()) == -1)
				addons.AddToTail(g_ClientAddons[steamID64].addonsToLoad[i].c_str());
		}
	}
}

CON_COMMAND_F(mm_add_client_addon, "Add a workshop ID to the global client-only addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <ID>\n", args[0]);
		return;
	}
	g_MultiAddonManager.AddClientAddon(args[1]);
}

CON_COMMAND_F(mm_remove_client_addon, "Remove a workshop ID from the global client-only addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <ID>\n", args[0]);
		return;
	}
	g_MultiAddonManager.RemoveClientAddon(args[1]);
}

CON_COMMAND_F(mm_add_addon, "Add a workshop ID to the extra addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.AddAddon(args[1]);
}

CON_COMMAND_F(mm_remove_addon, "Remove a workshop ID from the extra addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.RemoveAddon(args[1]);
}

CON_COMMAND_F(mm_download_addon, "Download an addon manually", FCVAR_SPONLY)
{
	if (args.ArgC() != 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.DownloadAddon(args[1], false, true);
}

CON_COMMAND_F(mm_print_searchpaths, "Print search paths", FCVAR_SPONLY)
{
	g_pFullFileSystem->PrintSearchPaths();
}

CON_COMMAND_F(mm_print_searchpaths_client, "Print search paths client-side, only usable if you're running the plugin on a listenserver", FCVAR_CLIENTDLL)
{
	g_pFullFileSystem->PrintSearchPaths();
}

void MultiAddonManager::Hook_StartupServer(const GameSessionConfiguration_t &config, ISource2WorldSession *session, const char *mapname)
{
	Message("Hook_StartupServer: %s\n", mapname);

	gpGlobals = g_pEngineServer->GetServerGlobals();
	g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();

	// Remove empty paths added when there are 2+ addons, they screw up file writes
	g_pFullFileSystem->RemoveSearchPath("", "GAME");
	g_pFullFileSystem->RemoveSearchPath("", "DEFAULT_WRITE_PATH");

	// This has to be done here to replicate the behavior on clients, where they mount addons in the string order
	// So if the current map is ID 1 and extra addons are IDs 2 and 3, they would be mounted in that order with ID 3 at the top
	// Note that the actual map VPK(s) and any sub-maps like team_select will be even higher, but those usually don't contain any assets that concern us
	RefreshAddons();
}

bool FASTCALL Hook_SendNetMessage(CServerSideClient *pClient, CNetMessage *pData, NetChannelBufType_t bufType)
{
	NetMessageInfo_t *info = pData->GetNetMessage()->GetNetMessageInfo();
	
	uint64 steamID64 = pClient->GetClientSteamID()->ConvertToUint64();
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	
	// If we are sending a message to the client, that means the client is still active.
	clientInfo.lastActiveTime = Plat_FloatTime();

	if (info->m_MessageId != net_SignonState || !CommandLine()->HasParm("-dedicated"))
		return g_pfnSendNetMessage(pClient, pData, bufType);

	auto pMsg = pData->ToPB<CNETMsg_SignonState>();

	CUtlVector<std::string> addons;
	g_MultiAddonManager.GetClientAddons(addons, steamID64);
	
	
	if (pMsg->signon_state() == SIGNONSTATE_CHANGELEVEL)
	{
		// When switching to another map, the signon message might contain more than 1 addon.
		// This puts the client in limbo because client doesn't know how to handle multiple addons at the same time.
		CUtlVector<std::string> addonsList;
		StringToVector(pMsg->addons().c_str(), addonsList);
		if (addonsList.Count() > 1)
		{
			// If there's more than one addon, ensure that it takes the first addon (which should be the workshop map or the first custom addon)
			pMsg->set_addons(addonsList.Head());
			// Since the client will download the addon contained inside this messsage, we might as well add it to the list of client's downloaded addons.
			clientInfo.currentPendingAddon = addonsList.Head();
		}
		else if (addonsList.Count() == 1)
		{
			// Nothing to do here, the rest of the required addons can be sent later.
			clientInfo.currentPendingAddon = pMsg->addons();
		}
		
		return g_pfnSendNetMessage(pClient, pData, bufType);
	}
	FOR_EACH_VEC(clientInfo.downloadedAddons, i)
	{
		addons.FindAndRemove(clientInfo.downloadedAddons[i]);
	}
	
	// Check if client has downloaded everything.
	if (addons.Count() == 0)
	{
		return g_pfnSendNetMessage(pClient, pData, bufType);
	}

	// Otherwise, send the next addon to the client.
	Message("%s: Number of addons remaining to download for %lli: %d\n", __func__, steamID64, addons.Count());
	clientInfo.currentPendingAddon = addons.Head();
	pMsg->set_addons(addons.Head().c_str());
	pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);

	return g_pfnSendNetMessage(pClient, pData, bufType);
}

// pMgrDoNotUse is named as such because the variable is optimized out in Windows builds and will not be passed to the function.
// The original Windows function just uses the global singleton instead.
void FASTCALL Hook_SetPendingHostStateRequest(CHostStateMgr* pMgrDoNotUse, CHostStateRequest *pRequest)
{
	// When IVEngineServer::ChangeLevel is called by the plugin or the server code,
	// (which happens at the end of a map), the server-defined addon does not change.
	// Also, host state requests coming from that function will always have "ChangeLevel" in its KV's name.
	// We can use this information to always be aware of what the original addon is.
	
	if (!pRequest->m_pKV)
	{
		g_MultiAddonManager.ClearCurrentWorkshopMap();
	}
	else if (V_stricmp(pRequest->m_pKV->GetName(), "ChangeLevel"))
	{
		if (!V_stricmp(pRequest->m_pKV->GetName(), "map_workshop"))
			g_MultiAddonManager.SetCurrentWorkshopMap(pRequest->m_pKV->GetString("customgamemode", ""));
		else
			g_MultiAddonManager.ClearCurrentWorkshopMap();
	}

	// Valve changed the way community maps (like de_dogtown) are loaded
	// Now their content lives in addons and they're mounted internally somehow (m_Addons is already set to it by this point)
	// So check if the addon is indeed one of the community maps and keep it, otherwise clients would error out due to missing assets
	// Each map has its own folder under game/csgo_community_addons which is mounted as "OFFICIAL_ADDONS"
	if (!pRequest->m_Addons.IsEmpty() && g_pFullFileSystem->IsDirectory(pRequest->m_Addons.String(), "OFFICIAL_ADDONS"))
		g_MultiAddonManager.SetCurrentWorkshopMap(pRequest->m_Addons);

	if (g_MultiAddonManager.m_ExtraAddons.Count() == 0)
		return g_pfnSetPendingHostStateRequest(pMgrDoNotUse, pRequest);

	// Rebuild the addon list. We always start with the original addon.
	if (g_MultiAddonManager.GetCurrentWorkshopMap().empty())
	{
		pRequest->m_Addons = VectorToString(g_MultiAddonManager.m_ExtraAddons).c_str();
	}
	else
	{
		// Don't add the same addon twice. Hopefully no server owner is diabolical enough to do things like `map de_dust2 customgamemode=1234,5678`.
		CUtlVector<std::string> newAddons;
		newAddons.CopyArray(g_MultiAddonManager.m_ExtraAddons.Base(), g_MultiAddonManager.m_ExtraAddons.Count());
		newAddons.FindAndRemove(g_MultiAddonManager.GetCurrentWorkshopMap().c_str());
		newAddons.AddToHead(g_MultiAddonManager.GetCurrentWorkshopMap().c_str());
		pRequest->m_Addons = VectorToString(newAddons).c_str();
	}

	g_pfnSetPendingHostStateRequest(pMgrDoNotUse, pRequest);
}

bool MultiAddonManager::Hook_ClientConnect( CPlayerSlot slot, const char *pszName, uint64 steamID64, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason )
{
	CUtlVector<std::string> addons;
	GetClientAddons(addons, steamID64);
	// We don't have an extra addon set so do nothing here, also don't do anything if we're a listenserver
	if (addons.Count() == 0 || !CommandLine()->HasParm("-dedicated"))
		RETURN_META_VALUE(MRES_IGNORED, true);
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];

	if (!clientInfo.currentPendingAddon.empty())
	{
		if (Plat_FloatTime() - clientInfo.lastActiveTime > mm_extra_addons_timeout.Get())
		{
			Message("%s: Client %lli has reconnected after the timeout or did not receive the addon message, will not add addon %s to the downloaded list\n", __func__, steamID64, clientInfo.currentPendingAddon.c_str());
		}
		else
		{
			Message("%s: Client %lli has connected within the interval with the pending addon %s, will send next addon in SendNetMessage hook\n", __func__, steamID64, clientInfo.currentPendingAddon.c_str());
			clientInfo.downloadedAddons.AddToTail(clientInfo.currentPendingAddon);
		}
		// Reset the current pending addon anyway, SendNetMessage tells us which addon to download next.
		clientInfo.currentPendingAddon.clear();
	}
	g_ClientAddons[steamID64].lastActiveTime = Plat_FloatTime();
	RETURN_META_VALUE(MRES_IGNORED, true);
}

void MultiAddonManager::Hook_ClientDisconnect( CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 steamID64, const char *pszNetworkID )
{
	// Mark the disconnection time for caching purposes.
	g_ClientAddons[steamID64].lastActiveTime = Plat_FloatTime();
}

void MultiAddonManager::Hook_ClientActive(CPlayerSlot slot, bool bLoadGame, const char * pszName, uint64 steamID64)
{
	// When the client reaches this stage, they should already have all the necessary addons downloaded, so we can safely remove the downloaded addons list here.
	if (!mm_cache_clients_with_addons.Get())
	{
		g_ClientAddons[steamID64].downloadedAddons.RemoveAll();
	}
}

void MultiAddonManager::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	static double s_flTime = 0.0f;

	// Print download progress every second
	if (Plat_FloatTime() - s_flTime > 1.f)
	{
		s_flTime = Plat_FloatTime();
		PrintDownloadProgress();
	}
}

void MultiAddonManager::Hook_PostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64 *clients,
	INetworkMessageInternal *pEvent, const CNetMessage *pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	NetMessageInfo_t *info = pEvent->GetNetMessageInfo();

	if (mm_block_disconnect_messages.Get() && info->m_MessageId == GE_Source1LegacyGameEvent)
	{
		auto pMsg = pData->ToPB<CMsgSource1LegacyGameEvent>();

		static int sDisconnectId = g_pGameEventManager->LookupEventId("player_disconnect");

		if (pMsg->eventid() == sDisconnectId)
		{
			IGameEvent *pEvent = g_pGameEventManager->UnserializeEvent(*pMsg);

			// This will prevent "loop shutdown" messages in the chat when clients reconnect
			// As far as we're aware, there are no other cases where this reason is used
			if (pEvent->GetInt("reason") == NETWORK_DISCONNECT_LOOPSHUTDOWN)
				*(uint64*)clients = 0;
		}
	}

	RETURN_META(MRES_IGNORED);
}

int MultiAddonManager::Hook_LoadEventsFromFile(const char *filename, bool bSearchAll)
{
	if (!g_pGameEventManager)
		g_pGameEventManager = META_IFACEPTR(IGameEventManager2);

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

void FASTCALL Hook_ReplyConnection(INetworkGameServer *server, CServerSideClient *client)
{
	uint64 steamID64 = client->GetClientSteamID()->ConvertToUint64();
	// Clear cache if necessary.
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	if (mm_cache_clients_with_addons.Get() && mm_cache_clients_duration.Get() != 0 && Plat_FloatTime() - clientInfo.lastActiveTime > mm_cache_clients_duration.Get())
	{
		Message("%s: Client %lli has not connected for a while, clearing the cache\n", __func__, steamID64);
		clientInfo.currentPendingAddon.clear();
		clientInfo.downloadedAddons.RemoveAll();
	}
	clientInfo.lastActiveTime = Plat_FloatTime();

	// Server copies the CUtlString from CNetworkGameServer to this client.
	CUtlString *addons = (CUtlString *)((uintptr_t)server + g_iServerAddonsOffset);
	CUtlString originalAddons = *addons;

	// Figure out which addons the client should be loading.
	CUtlVector<std::string> clientAddons;
	g_MultiAddonManager.GetClientAddons(clientAddons, steamID64);
	if (clientAddons.Count() == 0)
	{
		// No addons to send. This means the list of original addons is empty as well.
		assert(originalAddons.IsEmpty());
		g_pfnReplyConnection(server, client);
		return;
	}

	// Handle the first addon here. The rest should be handled in the SendNetMessage hook.
	if (g_ClientAddons[steamID64].downloadedAddons.Find(clientAddons[0]) == -1)
		g_ClientAddons[steamID64].currentPendingAddon = clientAddons[0];
	
	*addons = VectorToString(clientAddons).c_str();

	Message("%s: Sending addons %s to steamID64 %lli\n", __func__, addons->Get(), steamID64);
	g_pfnReplyConnection(server, client);

	*addons = originalAddons;
}

const char *MultiAddonManager::GetLicense()
{
	return "GPL v3 License";
}

const char *MultiAddonManager::GetVersion()
{
	return MULTIADDONMANAGER_VERSION; // defined by the build script
}

const char *MultiAddonManager::GetDate()
{
	return __DATE__;
}

const char *MultiAddonManager::GetLogTag()
{
	return "MultiAddonManager";
}

const char *MultiAddonManager::GetAuthor()
{
	return "xen";
}

const char *MultiAddonManager::GetDescription()
{
	return "Multi Addon Manager";
}

const char *MultiAddonManager::GetName()
{
	return "MultiAddonManager";
}

const char *MultiAddonManager::GetURL()
{
	return "https://github.com/Source2ZE/MultiAddonManager";
}
