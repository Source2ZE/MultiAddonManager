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
#include <sstream>
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
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);

MultiAddonManager g_MultiAddonManager;
INetworkGameServer *g_pNetworkGameServer = nullptr;
CSteamGameServerAPIContext g_SteamAPI;
CGlobalVars *gpGlobals = nullptr;

// Interface to other plugins
CAddonManagerInterface g_AddonManagerInterface;

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
		Panic("Signature for SendNetMessage occurs multiple times! Using first match but this might end up crashing!\n");
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

	SH_ADD_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameServerSteamAPIActivated), false);
	SH_ADD_HOOK(INetworkServerService, StartupServer, g_pNetworkServerService, SH_MEMBER(this, &MultiAddonManager::Hook_StartupServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientConnect, g_pSource2GameClients, SH_MEMBER(this, &MultiAddonManager::Hook_ClientConnect), false);
	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameFrame), true);

	if (late)
	{
		g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();
		gpGlobals = g_pEngineServer->GetServerGlobals();
		g_SteamAPI.Init();
		m_CallbackDownloadItemResult.Register(this, &MultiAddonManager::OnAddonDownloaded);
	}

	ConVar_Register(FCVAR_LINKED_CONCOMMAND);

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
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &MultiAddonManager::Hook_GameFrame), true);

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

	return &g_AddonManagerInterface;
}

void MultiAddonManager::BuildAddonPath(const char *pszAddon, char *buf, size_t len)
{
	// The workshop on a dedicated server is stored relative to the working directory for whatever reason
	static CBufferStringGrowable<MAX_PATH> s_sWorkingDir;
	ExecuteOnce(g_pFullFileSystem->GetSearchPath("EXECUTABLE_PATH", GET_SEARCH_PATH_ALL, s_sWorkingDir, 1));

	V_snprintf(buf, len, "%ssteamapps/workshop/content/730/%s/%s.vpk", s_sWorkingDir.Get(), pszAddon, pszAddon);
}

bool MultiAddonManager::MountAddon(const char *pszAddon, bool bAddToTail = false)
{
	if (!pszAddon || !*pszAddon)
		return false;

	PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
	uint32 iAddonState = g_SteamAPI.SteamUGC()->GetItemState(iAddon);

	if (!(iAddonState & k_EItemStateInstalled) || (iAddonState & k_EItemStateNeedsUpdate))
	{
		Message("%s: Addon %s is not installed or outdated, queuing a download\n", __func__, pszAddon);
		DownloadAddon(pszAddon, true, true);
		return false;
	}

	char path[MAX_PATH];
	BuildAddonPath(pszAddon, path, sizeof(path));

	if (!g_pFullFileSystem->FileExists(path))
	{
		Panic("%s: Addon %s not found at %s\n", __func__, pszAddon, path);
		return false;
	}

	if (m_MountedAddons.Find(pszAddon) != -1)
	{
		Panic("%s: Addon %s is already mounted\n", __func__, pszAddon);
		return false;
	}

	Message("Adding search path: %s\n", path);

	g_pFullFileSystem->AddSearchPath(path, "GAME", bAddToTail ? PATH_ADD_TO_TAIL : PATH_ADD_TO_HEAD, SEARCH_PATH_PRIORITY_VPK);
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
bool MultiAddonManager::DownloadAddon(const char *pszAddon, bool bImportant = false, bool bForce = false)
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

	if (!bForce && (nItemState & k_EItemStateInstalled) && !(nItemState & k_EItemStateNeedsUpdate))
	{
		Message("Addon %lli is already installed and up to date\n", addon);
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

void MultiAddonManager::RefreshAddons(bool bReloadMap = false)
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

	FOR_EACH_VEC_BACK(m_MountedAddons, i)
		UnmountAddon(m_MountedAddons[i].c_str());
}

void MultiAddonManager::Hook_GameServerSteamAPIActivated()
{
	// This is only intended for dedicated servers
	if (!CommandLine()->HasParm("-dedicated"))
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

	// Using the concommand here as g_pEngineServer->ChangeLevel somehow doesn't unmount workshop maps and we wanna be clean
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
		Panic("Addon %lli download failed with status %i\n", pResult->m_nPublishedFileId, pResult->m_eResult);

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

bool MultiAddonManager::AddAddon(const char *pszAddon, bool bRefresh = false)
{
	if (m_ExtraAddons.Find(pszAddon) != -1)
	{
		Panic("Addon %s is already in the list!\n", pszAddon);
		return false;
	}

	Message("Adding %s to addon list\n", pszAddon);

	m_ExtraAddons.AddToTail(pszAddon);

	if (bRefresh)
		RefreshAddons();

	return true;
}

bool MultiAddonManager::RemoveAddon(const char *pszAddon, bool bRefresh = false)
{
	int index = m_ExtraAddons.Find(pszAddon);

	if (index == -1)
	{
		Panic("Addon %s is not in the list!\n", pszAddon);
		return false;
	}

	Message("Removing %s from addon list\n", pszAddon);

	m_ExtraAddons.Remove(index);

	if (bRefresh)
		RefreshAddons();

	return true;
}

CON_COMMAND_F(mm_extra_addons, "The workshop IDs of extra addons separated by commas, addons will be downloaded (if not present) and mounted", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("%s %s\n", args[0], VectorToString(g_MultiAddonManager.m_ExtraAddons).c_str());
		return;
	}

	StringToVector(args[1], g_MultiAddonManager.m_ExtraAddons);

	g_MultiAddonManager.RefreshAddons();
}

CON_COMMAND_F(mm_add_addon, "Add a workshop ID to the extra addon list", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.AddAddon(args[1]);
}

CON_COMMAND_F(mm_remove_addon, "Remove a workshop ID from the extra addon list", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.RemoveAddon(args[1]);
}

CON_COMMAND_F(mm_download_addon, "Download an addon manually", FCVAR_GAMEDLL | FCVAR_RELEASE | FCVAR_SPONLY)
{
	if (args.ArgC() != 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.DownloadAddon(args[1], false, true);
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

void MultiAddonManager::Hook_StartupServer(const GameSessionConfiguration_t &config, ISource2WorldSession *session, const char *mapname)
{
	Message("Hook_StartupServer: %s\n", mapname);

	gpGlobals = g_pEngineServer->GetServerGlobals();
	g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();
	g_ClientsPendingAddon.RemoveAll();

	// Remove empty paths added when there are 2+ addons, they screw up file writes
	g_pFullFileSystem->RemoveSearchPath("", "GAME");
	g_pFullFileSystem->RemoveSearchPath("", "DEFAULT_WRITE_PATH");

	// This has to be done here to replicate the behavior on clients, where they mount addons in the string order
	// So if the current map is ID 1 and extra addons are IDs 2 and 3, they would be mounted in that order with ID 3 at the top
	// Note that the actual map VPK(s) and any sub-maps like team_select will be even higher, but those usually don't contain any assets that concern us
	RefreshAddons();
}

void FASTCALL Hook_SendNetMessage(INetChannel *pNetChan, INetworkSerializable *pNetMessage, void *pData, int a4)
{
	NetMessageInfo_t *info = pNetMessage->GetNetMessageInfo();

	// 7 for signon messages
	if (info->m_MessageId != 7 || g_MultiAddonManager.m_ExtraAddons.Count() == 0 || !CommandLine()->HasParm("-dedicated"))
		return g_pfnSendNetMessage(pNetChan, pNetMessage, pData, a4);

	ClientJoinInfo_t *pPendingClient = GetPendingClient(pNetChan);

	if (pPendingClient)
	{
		Message("%s: Sending addon %s to client %lli\n", __func__, g_MultiAddonManager.m_ExtraAddons[pPendingClient->addon].c_str(), pPendingClient->steamid);

		CNETMsg_SignonState *pMsg = (CNETMsg_SignonState *)pData;
		pMsg->set_addons(g_MultiAddonManager.m_ExtraAddons[pPendingClient->addon]);
		pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);

		pPendingClient->signon_timestamp = Plat_FloatTime();
	}

	g_pfnSendNetMessage(pNetChan, pNetMessage, pData, a4);
}

void* FASTCALL Hook_HostStateRequest(void *a1, void **pRequest)
{
	if (g_MultiAddonManager.m_ExtraAddons.Count() == 0)
		return g_pfnHostStateRequest(a1, pRequest);

	// This offset hasn't changed in 6 years so it should be safe
	CUtlString *psAddonString = (CUtlString *)(pRequest + 11);

	CUtlVector<std::string> vecAddons;
	StringToVector(psAddonString->Get(), vecAddons);

	// Clear the string just in case it wasn't somehow, like when reloading the map
	psAddonString->Clear();

	std::string sExtraAddonString = VectorToString(g_MultiAddonManager.m_ExtraAddons);

	// If it's empty or the first addon in the string is ours, it means we're on a default map
	if (vecAddons.Count() == 0 || g_MultiAddonManager.m_ExtraAddons.HasElement(vecAddons[0]))
	{
		Message("%s: setting addon string to \"%s\"\n", __func__, sExtraAddonString.c_str());
		psAddonString->Set(sExtraAddonString.c_str());
		g_MultiAddonManager.ClearCurrentWorkshopMap();
	}
	else
	{
		Message("%s: appending \"%s\" to addon string \"%s\"\n", __func__, sExtraAddonString.c_str(), vecAddons[0].c_str());
		psAddonString->Format("%s,%s", vecAddons[0].c_str(), sExtraAddonString.c_str());
		g_MultiAddonManager.SetCurrentWorkshopMap(vecAddons[0].c_str());
	}

	return g_pfnHostStateRequest(a1, pRequest);
}

float g_flRejoinTimeout = 10.f;
FAKE_FLOAT_CVAR(mm_extra_addons_timeout, "How long until clients are timed out in between connects for extra addons, requires mm_extra_addons to be used", g_flRejoinTimeout, 10.f, false);

bool MultiAddonManager::Hook_ClientConnect( CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason )
{
	// We don't have an extra addon set so do nothing here, also don't do anything if we're a listenserver
	if (m_ExtraAddons.Count() == 0 || !CommandLine()->HasParm("-dedicated"))
		RETURN_META_VALUE(MRES_IGNORED, true);

	// If we're on a default map and we have only 1 addon, no need to do any of this
	// The client will be prompted to download upon receiving C2S_CONNECTION with an addon
	if (m_ExtraAddons.Count() == 1 && m_sCurrentWorkshopMap.empty())
		RETURN_META_VALUE(MRES_IGNORED, true);

	Message("Client %s (%lli) connected:\n", pszName, xuid);

	// Store the client's ID temporarily as they will get reconnected once an extra addon is sent
	// This gets checked for in SendNetMessage so we don't repeatedly send the changelevel signon state for the same addon
	// The only caveat to this is that there's no way for us to verify if the client has actually downloaded the extra addon
	// as they're fully disconnected while downloading it, so the best we can do is use a timeout interval
	int index;
	ClientJoinInfo_t *pPendingClient = GetPendingClient(xuid, index);

	if (!pPendingClient)
	{
		// Client joined for the first time or after a timeout
		Message("first connection, sending addon %s\n", m_ExtraAddons[0].c_str());
		AddPendingClient(xuid);
	}
	else if ((Plat_FloatTime() - pPendingClient->signon_timestamp) < g_flRejoinTimeout)
	{
		// Client reconnected within the timeout interval
		// If they already have the addon this happens almost instantly after receiving the signon message with the addon
		pPendingClient->addon++;

		if (pPendingClient->addon < m_ExtraAddons.Count())
		{
			Message("reconnected within the interval, sending next addon %s\n", m_ExtraAddons[pPendingClient->addon].c_str());
		}
		else
		{
			Message("reconnected within the interval and has all addons, allowing\n");
			g_ClientsPendingAddon.FastRemove(index);
		}
	}
	else
	{
		Message("reconnected after the timeout or did not receive the addon message, will resend addon %s\n", m_ExtraAddons[pPendingClient->addon].c_str());
	}

	RETURN_META_VALUE(MRES_IGNORED, true);
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

bool CAddonManagerInterface::AddAddon(const char *pszAddon)
{
	return g_MultiAddonManager.AddAddon(pszAddon);
}

bool CAddonManagerInterface::RemoveAddon(const char *pszAddon)
{
	return g_MultiAddonManager.RemoveAddon(pszAddon);
}

bool CAddonManagerInterface::IsAddonMounted(const char *pszAddon)
{
	return g_MultiAddonManager.m_MountedAddons.Find(pszAddon) != -1;
}

bool CAddonManagerInterface::DownloadAddon(const char *pszAddon, bool bImportant, bool bForce)
{
	return g_MultiAddonManager.DownloadAddon(pszAddon, bImportant, bForce);
}

void CAddonManagerInterface::RefreshAddons()
{
	g_MultiAddonManager.RefreshAddons(true);
}

void CAddonManagerInterface::ClearAddons()
{
	g_MultiAddonManager.ClearAddons();
}
