#pragma once

#include <inetchannel.h>
#include <playerslot.h>
#include "circularbuffer.h"
#include "networksystem/inetworksystem.h"
#include "threadtools.h"
#include "tier1/netadr.h"
#include <entity2/entityidentity.h>
#include <networksystem/inetworksystem.h>
#include <steam/steamclientpublic.h>
#include <tier1/utlstring.h>

#include <network_connection.pb.h>

class INetMessage;
class CNetworkGameServerBase;
class CNetworkGameServer;
class CUtlSlot;

enum CopiedLockState_t : int32
{
	CLS_NOCOPY = 0,
	CLS_UNLOCKED = 1,
	CLS_LOCKED_BY_COPYING_THREAD = 2,
};

template <class MUTEX, CopiedLockState_t L = CLS_UNLOCKED>
class CCopyableLock : public MUTEX
{
	typedef MUTEX BaseClass;

public:
	// ...
};

class CUtlSignaller_Base
{
public:
	using Delegate_t = CUtlDelegate<void(CUtlSlot*)>;

	CUtlSignaller_Base(const Delegate_t& other) :
		m_SlotDeletionDelegate(other) {}
	CUtlSignaller_Base(Delegate_t&& other) :
		m_SlotDeletionDelegate(Move(other)) {}

private:
	Delegate_t m_SlotDeletionDelegate;
};

class CUtlSlot
{
public:
	using MTElement_t = CUtlSignaller_Base*;

	CUtlSlot() :
		m_ConnectedSignallers(0, 1) {}

private:
	CCopyableLock<CThreadFastMutex> m_Mutex;
	CUtlVector<MTElement_t> m_ConnectedSignallers;
};

class CServerSideClientBase : public CUtlSlot, public INetworkChannelNotify, public INetworkMessageProcessingPreFilter
{
public:
	virtual ~CServerSideClientBase() = 0;

public:
	CPlayerSlot GetPlayerSlot() const { return m_nClientSlot; }
	CPlayerUserId GetUserID() const { return m_UserID; }
	CEntityIndex GetEntityIndex() const { return m_nEntityIndex; }
	CSteamID GetClientSteamID() const { return m_SteamID; }
	const char* GetClientName() const { return m_Name; }
	INetChannel* GetNetChannel() const { return m_NetChannel; }
	const netadr_t* GetRemoteAddress() const { return &m_nAddr.GetAddress(); }
	CNetworkGameServerBase* GetServer() const { return m_Server; }

	virtual void Connect(int socket, const char* pszName, int nUserID, INetChannel* pNetChannel, bool bFakePlayer, bool bSplitClient, int iClientPlatform) = 0;
	virtual void Inactivate() = 0;
	virtual void Reactivate(CPlayerSlot nSlot) = 0;
	virtual void SetServer(CNetworkGameServer* pNetServer) = 0;
	virtual void Reconnect() = 0;
	virtual void Disconnect(ENetworkDisconnectionReason reason) = 0;
	virtual bool CheckConnect() = 0;

private:
	virtual void unk_10() = 0;

public:
	virtual void SetRate(int nRate) = 0;
	virtual void SetUpdateRate(float fUpdateRate) = 0;
	virtual int GetRate() = 0;

	virtual void Clear() = 0;

	virtual bool ExecuteStringCommand(const CNetMessagePB<CNETMsg_StringCmd>& msg) = 0;
	virtual void SendNetMessage(const CNetMessage* pData, NetChannelBufType_t bufType) = 0;

#ifdef LINUX
private:
	virtual void unk_17() = 0;
#endif

public:
	virtual void ClientPrintf(PRINTF_FORMAT_STRING const char*, ...) = 0;

	bool IsConnected() const { return m_nSignonState >= SIGNONSTATE_CONNECTED; }
	bool IsInGame() const { return m_nSignonState == SIGNONSTATE_FULL; }
	bool IsSpawned() const { return m_nSignonState >= SIGNONSTATE_NEW; }
	bool IsActive() const { return m_nSignonState == SIGNONSTATE_FULL; }
	int GetSignonState() const { return m_nSignonState; }
	virtual bool IsFakeClient() const { return m_bFakePlayer; }
	virtual bool IsHLTV() = 0;

public:
	CUtlString m_UserIDString;
	CUtlString m_Name;
	CPlayerSlot m_nClientSlot;
	CEntityIndex m_nEntityIndex;
	CNetworkGameServerBase* m_Server;
	INetChannel* m_NetChannel;
	uint8 m_nUnkVariable;
	bool m_bMarkedToKick;
	SignonState_t m_nSignonState;
	bool m_bSplitScreenUser;
	bool m_bSplitAllowFastDisconnect;
	int m_nSplitScreenPlayerSlot;
	CServerSideClientBase* m_SplitScreenUsers[4];
	CServerSideClientBase* m_pAttachedTo;
	bool m_bSplitPlayerDisconnecting;
	int m_UnkVariable172;
	bool m_bFakePlayer;
	bool m_bSendingSnapshot;
	[[maybe_unused]] char pad6[0x5];
	CPlayerUserId m_UserID = -1;
	bool m_bReceivedPacket;
	CSteamID m_SteamID;
	CSteamID m_UnkSteamID;
	CSteamID m_UnkSteamID2;
	CSteamID m_nFriendsID;
	ns_address m_nAddr;
	ns_address m_nAddr2;
	KeyValues* m_ConVars;
	bool m_bUnk0;

private:
	[[maybe_unused]] char pad273[0x28];

public:
	bool m_bConVarsChanged;
	bool m_bIsHLTV;

private:
	[[maybe_unused]] char pad29[0xB];

public:
	uint32 m_nSendtableCRC;
	uint32 m_uChallengeNumber;
	int m_nSignonTick;
	int m_nDeltaTick;
	int m_UnkVariable3;
	int m_nStringTableAckTick;
};

class CServerSideClient : public CServerSideClientBase
{
};
