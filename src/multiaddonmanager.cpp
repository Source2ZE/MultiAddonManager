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

#include <stdio.h>
#include "multiaddonmanager.h"
#include "module.h"
#include "utils/plat.h"
#include "networksystem/inetworkserializer.h"
#include "serversideclient.h"
#include "funchook.h"
#include "filesystem.h"
#include "steam/steam_gameserver.h"
#include <string>
#include "iserver.h"

#include "tier0/memdbgon.h"

void Message(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	ConColorMsg(Color(0, 255, 200), "[MultiAddonManager] %s", buf);

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

std::string g_sExtraAddons;
CUtlVector<char *> g_vecExtraAddons;

typedef void (FASTCALL *SendNetMessage_t)(INetChannel *pNetChan, INetworkSerializable *pNetMessage, void *pData, int a4);
typedef void* (FASTCALL *HostStateRequest_t)(void *a1, void **pRequest);

void FASTCALL Hook_SendNetMessage(INetChannel *pNetChan, INetworkSerializable *pNetMessage, void *pData, int a4);
void* FASTCALL Hook_HostStateRequest(void *a1, void **pRequest);

SendNetMessage_t g_pfnSendNetMessage = nullptr;
HostStateRequest_t g_pfnHostStateRequest = nullptr;

funchook_t *g_pSendNetMessageHook = nullptr;
funchook_t *g_pHostStateRequestHook = nullptr;

class GameSessionConfiguration_t { };

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);
SH_DECL_HOOK3_void(INetworkServerService, StartupServer, SH_NOATTRIB, 0, const GameSessionConfiguration_t &, ISource2WorldSession *, const char *);
SH_DECL_HOOK6(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, CPlayerSlot, const char*, uint64, const char *, bool, CBufferString *);

MultiAddonManager g_MultiAddonManager;
IServerGameClients *g_pServerGameClients = nullptr;
IVEngineServer *g_pEngineServer = nullptr;
INetworkGameServer *g_pNetworkGameServer = nullptr;
CSteamGameServerAPIContext g_SteamAPI;

PLUGIN_EXPOSE(MultiAddonManager, g_MultiAddonManager);
bool MultiAddonManager::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pServerGameClients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	// Required to get the IMetamodListener events
	g_SMAPI->AddListener( this, this );

	CModule *pEngineModule = new CModule(ROOTBIN, "engine2");
	CModule *pNetworkSystemModule = new CModule(ROOTBIN, "networksystem");

#ifdef PLATFORM_WINDOWS
	const byte SendNetMessage_Sig[] = "\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20\x57\x41\x56\x41\x57\x48\x83\xEC\x40\x49\x8B\xE8";
	const byte HostStateRequest_Sig[] = "\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x30\x33\xF6\x48\x8B\xFA";
#else
	const byte SendNetMessage_Sig[] = "\x55\x48\x89\xE5\x41\x57\x41\x89\xCF\x41\x56\x4C\x8D\xB7\x90\x76\x00\x00";
	const byte HostStateRequest_Sig[] = "\x55\x48\x89\xE5\x41\x56\x41\x55\x41\x54\x49\x89\xF4\x53\x48\x83\x7F\x30\x00";
#endif

	int sig_error;

	g_pfnSendNetMessage = (SendNetMessage_t)pNetworkSystemModule->FindSignature(SendNetMessage_Sig, sizeof(SendNetMessage_Sig) - 1, sig_error);

	if (!g_pfnSendNetMessage)
	{
		V_snprintf(error, maxlen, "Could not find the signature for SendNetMessage\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Warning("Signature for SendNetMessage occurs multiple times! Using first match but this might end up crashing!\n");
	}

	g_pfnHostStateRequest = (HostStateRequest_t)pEngineModule->FindSignature(HostStateRequest_Sig, sizeof(HostStateRequest_Sig) - 1, sig_error);

	if (!g_pfnHostStateRequest)
	{
		V_snprintf(error, maxlen, "Could not find the signature for HostStateRequest\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Panic("Signature for HostStateRequest occurs multiple times! Using first match but this might end up crashing!\n");
	}

	delete pEngineModule;
	delete pNetworkSystemModule;

	g_pSendNetMessageHook = funchook_create();
	funchook_prepare(g_pSendNetMessageHook, (void**)&g_pfnSendNetMessage, (void*)Hook_SendNetMessage);
	funchook_install(g_pSendNetMessageHook, 0);

	g_pHostStateRequestHook = funchook_create();
	funchook_prepare(g_pHostStateRequestHook, (void **)&g_pfnHostStateRequest, (void*)Hook_HostStateRequest);
	funchook_install(g_pHostStateRequestHook, 0);

	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, this, &MultiAddonManager::Hook_GameServerSteamAPIActivated, false);
	SH_ADD_HOOK_MEMFUNC(INetworkServerService, StartupServer, g_pNetworkServerService, this, &MultiAddonManager::Hook_StartupServer, true);
	SH_ADD_HOOK(IServerGameClients, ClientConnect, g_pServerGameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientConnect), false);

	if (late)
		g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();

	ConVar_Register(FCVAR_LINKED_CONCOMMAND);

	g_pEngineServer->ServerCommand("exec multiaddonmanager/multiaddonmanager");

	return true;
}

bool MultiAddonManager::Unload(char *error, size_t maxlen)
{
	g_vecExtraAddons.PurgeAndDeleteElements();

	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, this, &MultiAddonManager::Hook_GameServerSteamAPIActivated, false);
	SH_REMOVE_HOOK_MEMFUNC(INetworkServerService, StartupServer, g_pNetworkServerService, this, &MultiAddonManager::Hook_StartupServer, true);
	SH_REMOVE_HOOK(IServerGameClients, ClientConnect, g_pServerGameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientConnect), false);

	if (g_pSendNetMessageHook)
	{
		funchook_uninstall(g_pSendNetMessageHook, 0);
		funchook_destroy(g_pSendNetMessageHook);
	}

	if (g_pHostStateRequestHook)
	{
		funchook_uninstall(g_pHostStateRequestHook, 0);
		funchook_destroy(g_pHostStateRequestHook);
	}

	return true;
}

void DownloadAddon(const char *pszAddon)
{
	if (!g_SteamAPI.SteamUGC())
	{
		Panic("Cannot download addons as the Steam API is not initialized\n");
		return;
	}

	PublishedFileId_t addon = V_StringToUint64(pszAddon, 0);

	if (addon == 0)
	{
		Panic("Invalid addon %s\n", pszAddon);
		return;
	}

	uint32 nItemState = g_SteamAPI.SteamUGC()->GetItemState(addon);

	if ((nItemState & k_EItemStateInstalled) && !(nItemState & k_EItemStateNeedsUpdate))
	{
		Message("Addon %lli is already installed and up to date\n", addon);
		return;
	}

	if (!g_SteamAPI.SteamUGC()->DownloadItem(addon, true))
	{
		Panic("Addon download for %lli failed to start, addon ID is invalid or server is not logged on Steam\n", addon);
		return;
	}

	Message("Addon download started for %lli\n", addon);
}

void MultiAddonManager::Hook_GameServerSteamAPIActivated()
{
	Message("Steam API Activated\n");

	g_SteamAPI.Init();
	
	// If there are any extra addons set in the cfg, download them immediately
	FOR_EACH_VEC(g_vecExtraAddons, i)
		DownloadAddon(g_vecExtraAddons[i]);

	RETURN_META(MRES_IGNORED);
}

CON_COMMAND_F(mm_extra_addons, "The workshop IDs of extra addons, separated by commas", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("%s %s\n", args[0], g_sExtraAddons.c_str());
	}
	else
	{
		g_sExtraAddons = args[1];

		g_vecExtraAddons.PurgeAndDeleteElements();
		V_SplitString(g_sExtraAddons.c_str(), ",", g_vecExtraAddons);

		// Download extra addons immediately
		FOR_EACH_VEC(g_vecExtraAddons, i)
			DownloadAddon(g_vecExtraAddons[i]);
	}
}

CON_COMMAND_F(mm_download_addon, "Download an addon manually", FCVAR_GAMEDLL | FCVAR_RELEASE | FCVAR_SPONLY)
{
	if (args.ArgC() != 2)
	{
		Message("Usage: mm_download_addon <ID>\n");
		return;
	}

	DownloadAddon(args[1]);
}

CON_COMMAND_F(mm_print_searchpaths, "Print search paths", FCVAR_GAMEDLL | FCVAR_RELEASE | FCVAR_SPONLY)
{
	g_pFullFileSystem->PrintSearchPaths();
}

CON_COMMAND_F(mm_print_searchpaths_client, "Print search paths client-side, only usable if you're running the plugin on a listenserver", FCVAR_CLIENTDLL)
{
	g_pFullFileSystem->PrintSearchPaths();
}

CUtlVector<CServerSideClient *> *GetClientList()
{
	if (!g_pNetworkGameServer)
		return nullptr;

#ifdef PLATFORM_WINDOWS
	static constexpr int offset = 77;
#else
	static constexpr int offset = 79;
#endif

	return (CUtlVector<CServerSideClient *> *)(&g_pNetworkGameServer[offset]);
}

CServerSideClient *GetClientBySlot(CPlayerSlot slot)
{
	CUtlVector<CServerSideClient *> *pClients = GetClientList();

	if (!pClients)
		return nullptr;

	return pClients->Element(slot.Get());
}

struct ClientJoinInfo_t
{
	uint64 steamid;
	double signon_timestamp;
	int addon;
};

CUtlVector<ClientJoinInfo_t> g_ClientsPendingAddon;

void AddPendingClient(uint64 steamid)
{
	ClientJoinInfo_t PendingCLient{steamid, 0.f, 0};
	g_ClientsPendingAddon.AddToTail(PendingCLient);
}

ClientJoinInfo_t *GetPendingClient(uint64 steamid, int &index)
{
	index = 0;

	FOR_EACH_VEC(g_ClientsPendingAddon, i)
	{
		if (g_ClientsPendingAddon[i].steamid == steamid)
		{
			index = i;
			return &g_ClientsPendingAddon[i];
		}
	}

	return nullptr;
}

ClientJoinInfo_t *GetPendingClient(INetChannel *pNetChan)
{
	CUtlVector<CServerSideClient *> *pClients = GetClientList();

	if (!pClients)
		return nullptr;

	FOR_EACH_VEC(*pClients, i)
	{
		CServerSideClient *pClient = pClients->Element(i);

		if (pClient && pClient->GetNetChannel() == pNetChan)
			return GetPendingClient(pClient->GetClientSteamID()->ConvertToUint64(), i); // just pass i here, it's discarded anyway
	}

	return nullptr;
}

void MultiAddonManager::Hook_StartupServer(const GameSessionConfiguration_t &config, ISource2WorldSession *, const char *)
{
	g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();
	g_ClientsPendingAddon.RemoveAll();

	Message("Hook_StartupServer: %s\n", g_pNetworkGameServer->GetGlobals()->mapname);

	// Remove empty paths added when there are 2+ addons, they screw up file writes
	g_pFullFileSystem->RemoveSearchPath("", "GAME");
	g_pFullFileSystem->RemoveSearchPath("", "DEFAULT_WRITE_PATH");

	// Add extra addons to the search paths
	// This has to be done here to replicate the behavior on clients, where they mount addons in the string order
	// So if the current map is ID 1 and extra addons are IDs 2 and 3, they would be mounted in that order with ID 3 at the top
	// Note that the actual map VPK(s) and any sub-maps like team_select will be even higher, but those usually don't contain any assets that concern us

	// Remove our paths first in case addons were switched
	static CUtlVector<std::string> s_vecAddonPaths;
	FOR_EACH_VEC_BACK(s_vecAddonPaths, i)
	{
		Message("Removing search path: %s\n", s_vecAddonPaths[i].c_str());
		g_pFullFileSystem->RemoveSearchPath(s_vecAddonPaths[i].c_str(), "GAME");
		s_vecAddonPaths.FastRemove(i);
	}

	// The workshop is stored relative to the working directory for whatever reason
	CBufferStringGrowable<MAX_PATH> sWorkingDir;
	g_pFullFileSystem->GetSearchPath("EXECUTABLE_PATH", GET_SEARCH_PATH_ALL, sWorkingDir, 1);

	char path[MAX_PATH];
	FOR_EACH_VEC(g_vecExtraAddons, i)
	{
		V_snprintf(path, sizeof(path), "%ssteamapps/workshop/content/730/%s/%s.vpk", sWorkingDir.Get(), g_vecExtraAddons[i], g_vecExtraAddons[i]);
		Message("Adding search path: %s\n", path);
		g_pFullFileSystem->AddSearchPath(path, "GAME", PATH_ADD_TO_HEAD, SEARCH_PATH_PRIORITY_VPK);
		s_vecAddonPaths.AddToTail(path);
	}
}

void Hook_SendNetMessage(INetChannel *pNetChan, INetworkSerializable *pNetMessage, void *pData, int a4)
{
	NetMessageInfo_t *info = pNetMessage->GetNetMessageInfo();

	// 7 for signon messages
	if (info->m_MessageId != 7 || g_vecExtraAddons.Count() == 0)
		return g_pfnSendNetMessage(pNetChan, pNetMessage, pData, a4);

	ClientJoinInfo_t *pPendingClient = GetPendingClient(pNetChan);

	if (pPendingClient)
	{
		Message("Detour_SendNetMessage: Sending addon %s to client %lli\n", g_vecExtraAddons[pPendingClient->addon], pPendingClient->steamid);

		CNETMsg_SignonState *pMsg = (CNETMsg_SignonState *)pData;
		pMsg->set_addons(g_vecExtraAddons[pPendingClient->addon]);
		pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);

		pPendingClient->signon_timestamp = Plat_FloatTime();
	}

	g_pfnSendNetMessage(pNetChan, pNetMessage, pData, a4);
}

void* FASTCALL Hook_HostStateRequest(void *a1, void **pRequest)
{
	if (g_sExtraAddons.empty())
		return g_pfnHostStateRequest(a1, pRequest);

	// This offset hasn't changed in 6 years so it should be safe
	CUtlString *sAddonString = (CUtlString *)(pRequest + 11);

	Message("Hook_HostStateRequest: appending \"%s\" to addon string \"%s\"\n", g_sExtraAddons.c_str(), sAddonString->Get());

	// addons are simply comma-delimited, can have any number of them
	if (!sAddonString->IsEmpty())
		sAddonString->Format("%s,%s", sAddonString->Get(), g_sExtraAddons.c_str());
	else
		sAddonString->Set(g_sExtraAddons.c_str());

	return g_pfnHostStateRequest(a1, pRequest);
}

float g_flRejoinTimeout = 10.f;
FAKE_FLOAT_CVAR(mm_extra_addons_timeout, "How long until clients are timed out in between connects for extra addons, requires mm_extra_addons to be used", g_flRejoinTimeout, 10.f, false);

bool MultiAddonManager::Hook_ClientConnect( CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason )
{
	CServerSideClient *pClient = GetClientBySlot(slot);

	// We don't have an extra addon set so do nothing here
	if (g_vecExtraAddons.Count() == 0)
		RETURN_META_VALUE(MRES_IGNORED, true);

	Message("Client %lli ", xuid);

	// Store the client's ID temporarily as they will get reconnected once an extra addon is sent
	// This gets checked for in SendNetMessage so we don't repeatedly send the changelevel signon state for the same addon
	// The only caveat to this is that there's no way for us to verify if the client has actually downloaded the extra addon
	// as they're fully disconnected while downloading it, so the best we can do is use a timeout interval
	int index;
	ClientJoinInfo_t *pPendingClient = GetPendingClient(xuid, index);

	if (!pPendingClient)
	{
		// Client joined for the first time or after a timeout
		ConColorMsg(Color(0, 255, 200), "connected for the first time, sending addon %s\n", g_vecExtraAddons[0]);
		AddPendingClient(xuid);
	}
	else if ((Plat_FloatTime() - pPendingClient->signon_timestamp) < g_flRejoinTimeout)
	{
		// Client reconnected within the timeout interval
		// If they already have the addon this happens almost instantly after receiving the signon message with the addon
		pPendingClient->addon++;

		if (pPendingClient->addon < g_vecExtraAddons.Count())
		{
			ConColorMsg(Color(0, 255, 200), "has reconnected within the interval, sending next addon %s\n", g_vecExtraAddons[pPendingClient->addon]);
		}
		else
		{
			ConColorMsg(Color(0, 255, 200), "has reconnected within the interval and has all addons, allowing\n");
			g_ClientsPendingAddon.FastRemove(index);
		}
	}
	else
	{
		ConColorMsg(Color(0, 255, 200), "has reconnected after the timeout or did not receive the addon message, will resend addon %s\n", g_vecExtraAddons[pPendingClient->addon]);
	}

	RETURN_META_VALUE(MRES_IGNORED, true);
}

const char *MultiAddonManager::GetLicense()
{
	return "GPL v3 License";
}

const char *MultiAddonManager::GetVersion()
{
	return "1.0";
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
