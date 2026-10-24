/**
 * =============================================================================
 * MultiAddonManager
 * Copyright (C) 2024-2025 xen
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
#include "engine2/gamesessionconfiguration.h"
#include "engine2/igameeventsystem.h"
#include "engine2/inetworkgameserver.h"
#include "engine2/inetworkserverservice.h"
#include "engine2/isource2server.h"
#include "engine2/ivengineserver2.h"
#include "engine2/networkgameserverbase.h"
#include "engine2/serversideclient.h"
#include "khook.hpp"
#include "utlvector.h"
#include <deque>
#include "networksystem/inetworkserializer.h"
#include "steam/steam_api_common.h"
#include "steam/isteamugc.h"
#include "steam/isteamuser.h"
#include "imultiaddonmanager.h"
#include <string>
#include "tier0_strtools.h"

#ifdef AMBUILD
#include "version_gen.h"
#else
#include "version_gen_placeholder.h"
#endif

#ifdef _WIN32
#define ROOTBIN "/bin/win64/"
#define GAMEBIN "/csgo/bin/win64/"
#else
#define ROOTBIN "/bin/linuxsteamrt64/"
#define GAMEBIN "/csgo/bin/linuxsteamrt64/"
#endif

class AddonId
{
public:
	AddonId() = default;

	AddonId(const std::string &pszAddon) : m_pszAddon(pszAddon) {}
	AddonId(const char *pszAddon) : m_pszAddon(pszAddon ? pszAddon : "") {}
	AddonId(PublishedFileId_t iAddon) : m_pszAddon(std::to_string(iAddon)) {}

	// 0 means none or it's a community map
	operator PublishedFileId_t() const
	{
		return m_pszAddon.empty() ? 0 : V_StringToUint64(m_pszAddon.c_str(), 0);
	}

	operator const std::string &() const
	{
		return m_pszAddon;
	}

	operator const char *() const
	{
		return m_pszAddon.c_str();
	}

	const std::string &ToString() const { return m_pszAddon; }
	const char *c_str() const { return m_pszAddon.c_str(); }
	PublishedFileId_t ToSteamId() const { return static_cast<PublishedFileId_t>(*this); }
	bool empty() const { return m_pszAddon.empty(); }
	void clear() { m_pszAddon.clear(); }

	bool operator==(const AddonId &o) const { return m_pszAddon == o.m_pszAddon; }
	bool operator!=(const AddonId &o) const { return m_pszAddon != o.m_pszAddon; }

private:
	std::string m_pszAddon;
};

template <typename Container, typename T>
bool Contains(const Container &container, const T &value)
{
	return std::find(container.begin(), container.end(), value) != container.end();
}

// Returns true if an element was removed, false if value was not present.
template <typename Container, typename T>
bool RemoveValue(Container &container, const T &value)
{
	auto it = std::find(container.begin(), container.end(), value);
	if (it == container.end())
		return false;
	container.erase(it);
	return true;
}

class MultiAddonManager : public ISmmPlugin, public IMetamodListener, public IMultiAddonManager
{
public:
	MultiAddonManager();

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	void *OnMetamodQuery(const char *iface, int *ret);
public: //hooks
	KHook::Return<void> Hook_StartupServer_Post(INetworkServerService *pThis, const GameSessionConfiguration_t &config, ISource2WorldSession *, const char *);
	KHook::Return<bool> Hook_ClientConnect(IServerGameClients *pThis, CPlayerSlot slot, const char *pszName, uint64 steamID64, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason);
	KHook::Return<void> Hook_ClientDisconnect_Post(IServerGameClients *pThis, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 steamID64, const char *pszNetworkID);
	KHook::Return<void> Hook_ClientActive_Post(IServerGameClients *pThis, CPlayerSlot slot, bool bLoadGame, const char *pszName, uint64 steamID64);
	KHook::Return<void> Hook_GameFrame_Post(IServerGameDLL *pThis, bool simulating, bool bFirstTick, bool bLastTick);
	KHook::Return<bool> Hook_FireEvent(IGameEventManager2 *pThis, IGameEvent *event, bool bDontBroadcast = false);
	KHook::Return<bool> Hook_CanHLTVClientConnect(IServerGameClients *pThis, int index, const CSteamID &steamID, int *pRejectReason);
	KHook::Return<bool> Hook_SendNetMessage(CServerSideClientBase *pClient, const CNetMessage *pData, NetChannelBufType_t bufType);
	KHook::Return<void> Hook_SetPendingHostStateRequest(CHostStateMgr *pMgrDoNotUse, CHostStateRequest *pRequest);
	KHook::Return<void> Hook_ReplyConnection(CNetworkGameServerBase *pThis, CServerSideClient *pClient);
	KHook::Return<uint64> Hook_ScriptGetAddon();

	// Returns true if mm_extra_addons was touched
	void ParseCfg();
	void BuildAddonPath(const char *pszAddon, char *buf, size_t len, bool bLegacy);
	bool MountAddon(const char *pszAddon, bool bAddToTail);
	bool UnmountAddon(const char *pszAddon);
	bool AddAddon(const char *pszAddon, bool bRefresh = false);
	bool RemoveAddon(const char *pszAddon, bool bRefresh = false);
	bool IsAddonMounted(const char *pszAddon, bool bCheckWorkshopMap = false) { return Contains(m_MountedAddons, AddonId(pszAddon)) || (bCheckWorkshopMap && GetCurrentWorkshopMap() == AddonId(pszAddon));  }
	bool DownloadAddon(const char *pszAddon, bool bImportant = false, bool bForce = false);
	void PrintDownloadProgress();
	void RefreshAddons(bool bReloadMap = false);
	void ClearAddons();
	void ReloadMap();
	AddonId GetCurrentWorkshopMap() { return m_sCurrentWorkshopMap; }
	void SetCurrentWorkshopMap(const char *pszWorkshopID) { m_sCurrentWorkshopMap = pszWorkshopID; }
	void ClearCurrentWorkshopMap() { m_sCurrentWorkshopMap.clear(); }

	bool HasUGCConnection();
	void AddClientAddon(const char *pszAddon, uint64 steamID64 = 0, bool bRefresh = false);
	void RemoveClientAddon(const char *pszAddon, uint64 steamID64 = 0);
	void ClearClientAddons(uint64 steamID64 = 0);
	void GetClientAddons(std::vector<AddonId> &addons, uint64 steamID64 = 0);
	void CheckClientAddons(uint64 steamID64);
	void AddTimedOutClient(uint64 steamID64) { m_TimedOutClients.insert(steamID64); }

public:
	const char *GetAuthor() override		{ return PLUGIN_AUTHOR; }
	const char *GetName() override			{ return PLUGIN_DISPLAY_NAME; }
	const char *GetDescription() override	{ return PLUGIN_DESCRIPTION; }
	const char *GetURL() override			{ return PLUGIN_URL; }
	const char *GetLicense() override		{ return PLUGIN_LICENSE; }
	const char *GetVersion() override		{ return PLUGIN_FULL_VERSION; }
	const char *GetDate() override			{ return __DATE__; }
	const char *GetLogTag() override		{ return PLUGIN_LOGTAG; }

	std::vector<AddonId> m_ExtraAddons;

	// List of addons mounted by the plugin. Does not contain the original server mounted addon.
	std::vector<AddonId> m_MountedAddons;
	
	// List of addons to be mounted by the all clients.
	std::vector<AddonId> m_GlobalClientAddons;

private:
	KHook::Virtual<IServerGameDLL, void, bool, bool, bool> m_hookGameFrame;
	KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t &, ISource2WorldSession *, const char *> m_hookStartupServer;
	KHook::Virtual<IServerGameClients, bool, CPlayerSlot, const char *, uint64, const char *, bool, CBufferString *> m_hookClientConnect;
	KHook::Virtual<IServerGameClients, bool, int, const CSteamID &, int *> m_hookCanHLTVClientConnect;
	KHook::Virtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *> m_hookClientDisconnect;
	KHook::Virtual<IServerGameClients, void, CPlayerSlot, bool, const char *, uint64> m_hookClientActive;
	KHook::Virtual<IGameEventManager2, bool, IGameEvent *, bool> m_hookFireEvent;
	KHook::Virtual<CServerSideClientBase, bool, const CNetMessage *, NetChannelBufType_t> m_hookSendNetMessage_ServerSideClient;
	KHook::Virtual<CServerSideClientBase, bool, const CNetMessage *, NetChannelBufType_t> m_hookSendNetMessage_HLTVClient;
	KHook::Function<void, CHostStateMgr *, CHostStateRequest *> m_hookSetPendingHostStateRequest;
	KHook::Function<void, CNetworkGameServerBase *, CServerSideClient *> m_hookReplyConnection;
	KHook::Function<uint64> m_hookScriptGetAddon;

	std::vector<AddonId> m_ImportantDownloads; // Important addon downloads that will trigger a map reload when finished
	struct AddonDownloadInfo_t
	{
		bool operator==(const AddonDownloadInfo_t& other) const { return addon == other.addon; }
		bool operator==(const AddonId& other) const { return addon == other; }

		AddonId addon;
		bool bImportant;
		bool bForce;
	};
	std::vector<AddonDownloadInfo_t> m_PreSteamLoginDownloads; // Downloads that failed to start because the server wasn't logged on to Steam yet
	std::deque<AddonId> m_DownloadQueue; // Queue of all addon downloads to print progress

	STEAM_GAMESERVER_CALLBACK_MANUAL(MultiAddonManager, OnAddonDownloaded, DownloadItemResult_t, m_CallbackDownloadItemResult);
	STEAM_GAMESERVER_CALLBACK_MANUAL(MultiAddonManager, OnSteamServersConnected, SteamServersConnected_t, m_CallbackSteamServersConnected);

	bool m_bSteamConnected = false;

	// Used when reloading current map
	AddonId m_sCurrentWorkshopMap;

	std::set<uint64> m_TimedOutClients;
};

extern MultiAddonManager g_MultiAddonManager;

PLUGIN_GLOBALVARS();

// Taken from the comments in steamclientpublic.h and https://partner.steamgames.com/doc/api/steam_api
constexpr const char* g_SteamErrorMessages[] =
{
	"No result.",
	"Success.",
	"Generic failure.",
	"Your Steam client doesn't have a connection to the back-end.",
	"NoConnectionRetry: This should never appear unless Valve is trolling.",
	"Password/ticket is invalid.",
	"The user is logged in elsewhere.",
	"Protocol version is incorrect.",
	"A parameter is incorrect.",
	"File was not found.",
	"Called method is busy - action not taken.",
	"Called object was in an invalid state.",
	"The name was invalid.",
	"The email was invalid.",
	"The name is not unique.",
	"Access is denied.",
	"Operation timed out.",
	"The user is VAC2 banned.",
	"Account not found.",
	"The Steam ID was invalid.",
	"The requested service is currently unavailable.",
	"The user is not logged on.",
	"Request is pending, it may be in process or waiting on third party.",
	"Encryption or Decryption failed.",
	"Insufficient privilege.",
	"Too much of a good thing.",
	"Access has been revoked (used for revoked guest passes.)",
	"License/Guest pass the user is trying to access is expired.",
	"Guest pass has already been redeemed by account, cannot be used again.",
	"The request is a duplicate and the action has already occurred in the past, ignored this time.",
	"All the games in this guest pass redemption request are already owned by the user.",
	"IP address not found.",
	"Failed to write change to the data store.",
	"Failed to acquire access lock for this operation.",
	"The logon session has been replaced.",
	"Failed to connect.",
	"The authentication handshake has failed.",
	"There has been a generic IO failure.",
	"The remote server has disconnected.",
	"Failed to find the shopping cart requested.",
	"A user blocked the action.",
	"The target is ignoring sender.",
	"Nothing matching the request found.",
	"The account is disabled.",
	"This service is not accepting content changes right now.",
	"Account doesn't have value, so this feature isn't available.",
	"Allowed to take this action, but only because requester is admin.",
	"A Version mismatch in content transmitted within the Steam protocol.",
	"The current CM can't service the user making a request, user should try another.",
	"You are already logged in elsewhere, this cached credential login has failed.",
	"The user is logged in elsewhere. (Use instead!)",
	"Long running operation has suspended/paused. (eg. content download.)",
	"Operation has been canceled, typically by user. (eg. a content download.)",
	"Operation canceled because data is ill formed or unrecoverable.",
	"Operation canceled - not enough disk space.",
	"The remote or IPC call has failed.",
	"Password could not be verified as it's unset server side.",
	"External account (PSN, Facebook...) is not linked to a Steam account.",
	"PSN ticket was invalid.",
	"External account (PSN, Facebook...) is already linked to some other account, must explicitly request to replace/delete the link first.",
	"The sync cannot resume due to a conflict between the local and remote files.",
	"The requested new password is not allowed.",
	"New value is the same as the old one. This is used for secret question and answer.",
	"Account login denied due to 2nd factor authentication failure.",
	"The requested new password is not legal.",
	"Account login denied due to auth code invalid.",
	"Account login denied due to 2nd factor auth failure - and no mail has been sent.",
	"The users hardware does not support Intel's Identity Protection Technology (IPT).",
	"Intel's Identity Protection Technology (IPT) has failed to initialize.",
	"Operation failed due to parental control restrictions for current user.",
	"Facebook query returned an error.",
	"Account login denied due to an expired auth code.",
	"The login failed due to an IP restriction.",
	"The current users account is currently locked for use. This is likely due to a hijacking and pending ownership verification.",
	"The logon failed because the accounts email is not verified.",
	"There is no URL matching the provided values.",
	"Bad Response due to a Parse failure, missing field, etc.",
	"The user cannot complete the action until they re-enter their password.",
	"The value entered is outside the acceptable range.",
	"Something happened that we didn't expect to ever happen.",
	"The requested service has been configured to be unavailable.",
	"The files submitted to the CEG server are not valid.",
	"The device being used is not allowed to perform this action.",
	"The action could not be complete because it is region restricted.",
	"Temporary rate limit exceeded, try again later, different from which may be permanent.",
	"Need two-factor code to login.",
	"The thing we're trying to access has been deleted.",
	"Login attempt failed, try to throttle response to possible attacker.",
	"Two factor authentication (Steam Guard) code is incorrect.",
	"The activation code for two-factor authentication (Steam Guard) didn't match.",
	"The current account has been associated with multiple partners.",
	"The data has not been modified.",
	"The account does not have a mobile device associated with it.",
	"The time presented is out of range or tolerance.",
	"SMS code failure - no match, none pending, etc.",
	"Too many accounts access this resource.",
	"Too many changes to this account.",
	"Too many changes to this phone.",
	"Cannot refund to payment method, must use wallet.",
	"Cannot send an email.",
	"Can't perform operation until payment has settled.",
	"The user needs to provide a valid captcha.",
	"A game server login token owned by this token's owner has been banned.",
	"Game server owner is denied for some other reason such as account locked, community ban, vac ban, missing phone, etc.",
	"The type of thing we were requested to act on is invalid.",
	"The IP address has been banned from taking this action.",
	"This Game Server Login Token (GSLT) has expired from disuse; it can be reset for use.",
	"User doesn't have enough wallet funds to complete the action.",
	"There are too many of this thing pending already.",
	"No site licenses found",
	"The WG couldn't send a response because we exceeded max network send size",
	"The user is not mutually friends",
	"The user is limited",
	"Item can't be removed",
	"Account has been deleted",
	"A license for this already exists, but cancelled",
	"Access is denied because of a community cooldown (probably from support profile data resets)",
	"No launcher was specified, but a launcher was needed to choose correct realm for operation.",
	"User must agree to china SSA or global SSA before login",
	"The specified launcher type is no longer supported; the user should be directed elsewhere",
	"The user's realm does not match the realm of the requested resource",
	"Signature check did not match",
	"Failed to parse input",
	"Account does not have a verified phone number",
	"User device doesn't have enough battery charge currently to complete the action",
	"The operation requires a charger to be plugged in, which wasn't present",
	"Cached credential was invalid - user must reauthenticate",
	"The phone number provided is a Voice Over IP number",
	"The data being accessed is not supported by this API",
	"Reached the maximum size of the family",
	"The local data for the offline mode cache is insufficient to login",
	"Retry the operation later",
};
