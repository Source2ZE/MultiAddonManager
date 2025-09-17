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
	bool Hook_ClientConnect(CPlayerSlot slot, const char *pszName, uint64 steamID64, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 steamID64, const char *pszNetworkID);
	void Hook_ClientActive(CPlayerSlot slot, bool bLoadGame, const char *pszName, uint64 steamID64);
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

	bool HasUGCConnection();
	void AddClientAddon(const char *pszAddon, uint64 steamID64 = 0, bool bRefresh = false);
	void RemoveClientAddon(const char *pszAddon, uint64 steamID64 = 0);
	void ClearClientAddons(uint64 steamID64 = 0);
	void GetClientAddons(CUtlVector<std::string> &addons, uint64 steamID64 = 0);

public:
	const char *GetAuthor() override		{ return "xen"; }
	const char *GetName() override			{ return "MultiAddonManager"; }
	const char *GetDescription() override	{ return "Multi Addon Manager"; }
	const char *GetURL() override			{ return "https://github.com/Source2ZE/MultiAddonManager"; }
	const char *GetLicense() override		{ return "GPL v3 License"; }
	const char *GetVersion() override		{ return MULTIADDONMANAGER_VERSION; }  // defined by the build script
	const char *GetDate() override			{ return __DATE__; }
	const char *GetLogTag() override		{ return "MultiAddonManager"; }

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
	"The phone number provided is a Voice Over IP number"
};
