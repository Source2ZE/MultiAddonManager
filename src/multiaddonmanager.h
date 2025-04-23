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

#pragma once

#include <ISmmPlugin.h>
#include <igameevents.h>
#include <sh_vector.h>
#include "utlqueue.h"
#include "utlvector.h"
#include "networksystem/inetworkserializer.h"
#include "steam/steam_api_common.h"
#include "steam/isteamugc.h"
#include "imultiaddonmanager.h"

#ifdef _WIN32
#define ROOTBIN "/bin/win64/"
#define GAMEBIN "/csgo/bin/win64/"
#else
#define ROOTBIN "/bin/linuxsteamrt64/"
#define GAMEBIN "/csgo/bin/linuxsteamrt64/"
#endif

class MultiAddonManager : public ISmmPlugin, public IMetamodListener, public IMultiAddonManager
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	void *OnMetamodQuery(const char *iface, int *ret);
public: //hooks
	void Hook_GameServerSteamAPIActivated();
	void Hook_StartupServer(const GameSessionConfiguration_t &config, ISource2WorldSession *, const char *);
	bool Hook_ClientConnect(CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 xuid, const char *pszNetworkID);
	void Hook_ClientActive(CPlayerSlot slot, bool bLoadGame, const char *pszName, uint64 xuid);
	void Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void Hook_PostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64 *clients,
		INetworkMessageInternal *pEvent, const CNetMessage *pData, unsigned long nSize, NetChannelBufType_t bufType);
	int Hook_LoadEventsFromFile(const char *filename, bool bSearchAll);

	void BuildAddonPath(const char *pszAddon, char *buf, size_t len, bool bLegacy);
	bool MountAddon(const char *pszAddon, bool bAddToTail);
	bool UnmountAddon(const char *pszAddon);
	bool AddAddon(const char *pszAddon, bool bRefresh = false);
	bool RemoveAddon(const char *pszAddon, bool bRefresh = false);
	bool IsAddonMounted(const char *pszAddon, bool bCheckWorkshopMap = false) { return m_MountedAddons.Find(pszAddon) != -1 || (bCheckWorkshopMap && GetCurrentWorkshopMap() == pszAddon);  }
	bool DownloadAddon(const char *pszAddon, bool bImportant = false, bool bForce = false);
	void PrintDownloadProgress();
	void RefreshAddons(bool bReloadMap = false);
	void ClearAddons();
	void ReloadMap();
	std::string GetCurrentWorkshopMap() { return m_sCurrentWorkshopMap; }
	void SetCurrentWorkshopMap(const char *pszWorkshopID) { m_sCurrentWorkshopMap = pszWorkshopID; }
	void ClearCurrentWorkshopMap() { m_sCurrentWorkshopMap.clear(); }

	bool HasGCConnection();
	void AddClientAddon(const char *pszAddon, uint64 xuid = 0, bool bRefresh = false);
	void RemoveClientAddon(const char *pszAddon, uint64 xuid = 0);
	void ClearClientAddons(uint64 xuid = 0);
	void GetClientAddons(CUtlVector<std::string> &addons, uint64 xuid = 0);

public:
	const char *GetAuthor();
	const char *GetName();
	const char *GetDescription();
	const char *GetURL();
	const char *GetLicense();
	const char *GetVersion();
	const char *GetDate();
	const char *GetLogTag();

	CUtlVector<std::string> m_ExtraAddons;

	// List of addons mounted by the plugin. Does not contain the original server mounted addon.
	CUtlVector<std::string> m_MountedAddons;
	
	// List of addons to be mounted by the all clients.
	CUtlVector<std::string> m_GlobalClientAddons;

private:
	CUtlVector<PublishedFileId_t> m_ImportantDownloads; // Important addon downloads that will trigger a map reload when finished
	CUtlQueue<PublishedFileId_t> m_DownloadQueue; // Queue of all addon downloads to print progress

	STEAM_GAMESERVER_CALLBACK_MANUAL(MultiAddonManager, OnAddonDownloaded, DownloadItemResult_t, m_CallbackDownloadItemResult);
	// Used when reloading current map
	std::string m_sCurrentWorkshopMap;
};

extern MultiAddonManager g_MultiAddonManager;

PLUGIN_GLOBALVARS();
