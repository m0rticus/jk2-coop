/*
===========================================================================
Copyright (C) 2026 OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.
===========================================================================
*/

#include "../server/exe_headers.h"

#include "cl_steam.h"
#include "client.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifdef USE_STEAMWORKS
#include "steam/steam_api.h"
#endif

static cvar_t *cl_coopEnabled;
static cvar_t *cl_coopRole;
static cvar_t *cl_coopLobbyState;
static cvar_t *cl_coopLobbyId;
static cvar_t *cl_coopPeerSteamId;
static cvar_t *cl_coopLastError;
static cvar_t *cl_coopSteamAvailable;
static cvar_t *cl_coopAutoHost;
static cvar_t *cl_coopLobbySummary;
static cvar_t *cl_coopLobbyMembers;
static cvar_t *cl_coopSelectedMap;
static cvar_t *cl_coopSelectedLevel;
static cvar_t *cl_coopDifficultyName;

struct coopLevelDef_t
{
	const char *mapName;
	const char *displayName;
};

static const coopLevelDef_t s_coopLevels[] =
{
	{ "kejim_post", "Kejim Post" },
	{ "kejim_base", "Kejim Base" },
	{ "artus_mine", "Artus Mine" },
	{ "artus_detention", "Artus Detention" },
	{ "artus_topside", "Artus Topside" },
	{ "yavin_temple", "Yavin Temple" },
	{ "yavin_trial", "Yavin Trial" },
	{ "ns_streets", "Nar Shaddaa Streets" },
	{ "ns_hideout", "Nar Shaddaa Hideout" },
	{ "ns_starpad", "Nar Shaddaa Starpad" },
	{ "bespin_undercity", "Bespin Undercity" },
	{ "bespin_streets", "Bespin Streets" },
	{ "bespin_platform", "Bespin Platform" },
	{ "cairn_bay", "Cairn Bay" },
	{ "cairn_assembly", "Cairn Assembly" },
	{ "cairn_reactor", "Cairn Reactor" },
	{ "cairn_dock1", "Cairn Dock" },
	{ "doom_comm", "Doomgiver Communications" },
	{ "doom_detention", "Doomgiver Detention" },
	{ "doom_shields", "Doomgiver Shields" },
	{ "yavin_swamp", "Yavin Swamp" },
	{ "yavin_canyon", "Yavin Canyon" },
	{ "yavin_courtyard", "Yavin Courtyard" },
	{ "yavin_final", "Yavin Final" }
};

static const char *CL_Coop_DifficultyName( int skill )
{
	switch ( skill )
	{
	case 0:
		return "Padawan";
	case 2:
		return "Jedi Knight";
	case 3:
		return "Jedi Master";
	default:
		return "Jedi";
	}
}

static void CL_Steam_SetCvar( const char *name, const char *value )
{
	Cvar_Set( name, value ? value : "" );
}

static void CL_Steam_SetError( const char *message )
{
	CL_Steam_SetCvar( "cl_coopLastError", message );
	Com_Printf( S_COLOR_YELLOW "Steam co-op: %s\n", message );
}

static int CL_Coop_FindLevelByMap( const char *mapName )
{
	if ( !mapName || !mapName[0] )
	{
		return -1;
	}

	for ( int i = 0; i < ARRAY_LEN( s_coopLevels ); ++i )
	{
		if ( !Q_stricmp( mapName, s_coopLevels[i].mapName ) )
		{
			return i;
		}
	}

	return -1;
}

static void CL_Coop_SetSelectedLevel( int index )
{
	const int levelCount = ARRAY_LEN( s_coopLevels );
	if ( index < 0 )
	{
		index = levelCount - 1;
	}
	else if ( index >= levelCount )
	{
		index = 0;
	}

	CL_Steam_SetCvar( "cl_coopSelectedMap", s_coopLevels[index].mapName );
	CL_Steam_SetCvar( "cl_coopSelectedLevel", s_coopLevels[index].displayName );
}

static int CL_Coop_SelectedLevelIndex()
{
	int index = CL_Coop_FindLevelByMap( cl_coopSelectedMap ? cl_coopSelectedMap->string : "" );
	if ( index < 0 )
	{
		index = 0;
		CL_Coop_SetSelectedLevel( index );
	}
	return index;
}

static void CL_Coop_SetDifficulty( int skill )
{
	if ( skill < 0 )
	{
		skill = 0;
	}
	else if ( skill > 3 )
	{
		skill = 3;
	}

	Cvar_Set( "g_spskill", va( "%d", skill ) );
	CL_Steam_SetCvar( "cl_coopDifficultyName", CL_Coop_DifficultyName( skill ) );
}

static void CL_Coop_LaunchSelectedMap( const char *mapName, int skill )
{
	int index = CL_Coop_FindLevelByMap( mapName );
	if ( index < 0 )
	{
		CL_Steam_SetError( va( "Invalid co-op level '%s'.", mapName ? mapName : "" ) );
		return;
	}

	CL_Coop_SetDifficulty( skill );
	CL_Coop_SetSelectedLevel( index );
	CL_Steam_SetCvar( "cl_coopEnabled", "1" );
	Cbuf_ExecuteText( EXEC_APPEND, va( "set cl_coopEnabled 1\nset g_spskill %d\nmap %s\n", skill, s_coopLevels[index].mapName ) );
}

#ifdef USE_STEAMWORKS

static const int COOP_STEAM_VIRTUAL_PORT = 0;
static const int COOP_PING_INTERVAL_MSEC = 5000;

class SteamCoopState
{
public:
	SteamCoopState();
	~SteamCoopState();

	bool Init();
	void Shutdown();
	void Frame();
	void Host();
	void JoinLobby( uint64 lobbyId );
	void InviteFriend();
	void Leave();
	void StartGame();
	void PrintStatus() const;
	void OnConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *info );

private:
	void SetState( const char *state );
	void SetRole( const char *role );
	void SetLobbyId( CSteamID lobbyId );
	void SetPeer( CSteamID peerId );
	void ClearRichPresence();
	void UpdateRichPresence( const char *status );
	void UpdateLobbySummary();
	void StartListenSocket();
	void ConnectToLobbyOwner();
	void CloseNetworking();
	void SendPacket( const char *packet );
	void ReceivePackets();
	void HandlePacket( const char *packet );
	void SendPingIfNeeded();
	void OnLobbyCreated( LobbyCreated_t *result, bool ioFailure );
	void OnLobbyEnter( LobbyEnter_t *event );
	void OnLobbyChatUpdate( LobbyChatUpdate_t *event );
	void OnGameLobbyJoinRequested( GameLobbyJoinRequested_t *event );

	bool steamReady;
	bool host;
	bool connected;
	int lastPingTime;
	CSteamID lobby;
	CSteamID peer;
	HSteamListenSocket listenSocket;
	HSteamNetConnection connection;
	CCallResult<SteamCoopState, LobbyCreated_t> lobbyCreatedCall;
	CCallback<SteamCoopState, LobbyEnter_t> lobbyEnterCallback;
	CCallback<SteamCoopState, LobbyChatUpdate_t> lobbyChatUpdateCallback;
	CCallback<SteamCoopState, GameLobbyJoinRequested_t> lobbyJoinRequestedCallback;
};

static SteamCoopState *s_steamCoop;

static void CL_Steam_NetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *info )
{
	if ( s_steamCoop )
	{
		s_steamCoop->OnConnectionStatusChanged( info );
	}
}

SteamCoopState::SteamCoopState()
	: steamReady( false ),
	  host( false ),
	  connected( false ),
	  lastPingTime( 0 ),
	  listenSocket( k_HSteamListenSocket_Invalid ),
	  connection( k_HSteamNetConnection_Invalid ),
	  lobbyEnterCallback( this, &SteamCoopState::OnLobbyEnter ),
	  lobbyChatUpdateCallback( this, &SteamCoopState::OnLobbyChatUpdate ),
	  lobbyJoinRequestedCallback( this, &SteamCoopState::OnGameLobbyJoinRequested )
{
}

SteamCoopState::~SteamCoopState()
{
	Shutdown();
}

bool SteamCoopState::Init()
{
	if ( steamReady )
	{
		return true;
	}

	if ( !SteamAPI_Init() )
	{
		CL_Steam_SetCvar( "cl_coopSteamAvailable", "0" );
		CL_Steam_SetError( "SteamAPI_Init failed. Make sure Steam is running and steam_appid.txt is present." );
		return false;
	}

	steamReady = true;
	CL_Steam_SetCvar( "cl_coopSteamAvailable", "1" );
	SetState( "steam-ready" );
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( CL_Steam_NetConnectionStatusChanged );
	SteamNetworkingUtils()->InitRelayNetworkAccess();
	UpdateRichPresence( "In menus" );
	UpdateLobbySummary();
	Com_Printf( "Steam co-op: initialized as %s\n", SteamFriends()->GetPersonaName() );
	return true;
}

void SteamCoopState::Shutdown()
{
	if ( !steamReady )
	{
		return;
	}

	Leave();
	ClearRichPresence();
	SteamAPI_Shutdown();
	steamReady = false;
	CL_Steam_SetCvar( "cl_coopSteamAvailable", "0" );
}

void SteamCoopState::Frame()
{
	if ( !steamReady )
	{
		return;
	}

	SteamAPI_RunCallbacks();
	ReceivePackets();
	SendPingIfNeeded();
}

void SteamCoopState::Host()
{
	if ( !steamReady )
	{
		CL_Steam_SetError( "Steam is not available; cannot host a co-op lobby." );
		return;
	}

	Leave();
	host = true;
	CL_Steam_SetCvar( "cl_coopEnabled", "1" );
	SetRole( "host" );
	SetState( "creating-lobby" );
	UpdateLobbySummary();
	SteamAPICall_t call = SteamMatchmaking()->CreateLobby( k_ELobbyTypePrivate, 2 );
	lobbyCreatedCall.Set( call, this, &SteamCoopState::OnLobbyCreated );
}

void SteamCoopState::JoinLobby( uint64 lobbyId )
{
	if ( !steamReady )
	{
		CL_Steam_SetError( "Steam is not available; cannot join a co-op lobby." );
		return;
	}
	if ( lobbyId == 0 )
	{
		CL_Steam_SetError( "Invalid lobby id." );
		return;
	}

	Leave();
	host = false;
	CL_Steam_SetCvar( "cl_coopEnabled", "1" );
	SetRole( "client" );
	SetState( "joining-lobby" );
	UpdateLobbySummary();
	SteamMatchmaking()->JoinLobby( CSteamID( lobbyId ) );
}

void SteamCoopState::InviteFriend()
{
	if ( !steamReady )
	{
		CL_Steam_SetError( "Steam is not available; cannot invite a friend." );
		return;
	}
	if ( !lobby.IsValid() )
	{
		CL_Steam_SetError( "Steam co-op lobby is not ready yet; try again in a moment." );
		return;
	}

	SteamFriends()->ActivateGameOverlayInviteDialog( lobby );
}

void SteamCoopState::Leave()
{
	CloseNetworking();
	if ( steamReady && lobby.IsValid() )
	{
		SteamMatchmaking()->LeaveLobby( lobby );
	}

	lobby.Clear();
	peer.Clear();
	host = false;
	connected = false;
	CL_Steam_SetCvar( "cl_coopEnabled", "0" );
	SetRole( "" );
	SetLobbyId( lobby );
	SetPeer( peer );
	SetState( steamReady ? "steam-ready" : "offline" );
	UpdateRichPresence( steamReady ? "In menus" : "" );
	UpdateLobbySummary();
}

void SteamCoopState::StartGame()
{
	if ( !steamReady )
	{
		CL_Steam_SetError( "Steam is not available; cannot start a co-op game." );
		return;
	}
	if ( !host )
	{
		CL_Steam_SetError( "Only the lobby host can start a co-op game." );
		return;
	}
	if ( !lobby.IsValid() )
	{
		CL_Steam_SetError( "Steam co-op lobby is not ready yet; try again in a moment." );
		return;
	}

	int skill = Cvar_VariableIntegerValue( "g_spskill" );
	int index = CL_Coop_SelectedLevelIndex();
	const char *mapName = s_coopLevels[index].mapName;

	SteamMatchmaking()->SetLobbyData( lobby, "state", "starting" );
	SteamMatchmaking()->SetLobbyData( lobby, "map", mapName );
	SteamMatchmaking()->SetLobbyData( lobby, "skill", va( "%d", skill ) );
	SetState( "starting-game" );
	UpdateRichPresence( va( "Starting %s co-op", s_coopLevels[index].displayName ) );
	SendPacket( va( "START_MAP %s %d", mapName, skill ) );
	Com_Printf( "Steam co-op: starting %s on %s difficulty\n", s_coopLevels[index].displayName, CL_Coop_DifficultyName( skill ) );
	CL_Coop_LaunchSelectedMap( mapName, skill );
}

void SteamCoopState::PrintStatus() const
{
	Com_Printf( "Steam co-op status:\n" );
	Com_Printf( "  steam: %s\n", steamReady ? "available" : "unavailable" );
	Com_Printf( "  state: %s\n", cl_coopLobbyState ? cl_coopLobbyState->string : "" );
	Com_Printf( "  role: %s\n", cl_coopRole ? cl_coopRole->string : "" );
	Com_Printf( "  lobby: %s\n", cl_coopLobbyId ? cl_coopLobbyId->string : "" );
	Com_Printf( "  peer: %s\n", cl_coopPeerSteamId ? cl_coopPeerSteamId->string : "" );
	Com_Printf( "  auto host: %s\n", cl_coopAutoHost && cl_coopAutoHost->integer ? "enabled" : "disabled" );
	Com_Printf( "  level: %s (%s)\n", cl_coopSelectedLevel ? cl_coopSelectedLevel->string : "", cl_coopSelectedMap ? cl_coopSelectedMap->string : "" );
	Com_Printf( "  difficulty: %s\n", cl_coopDifficultyName ? cl_coopDifficultyName->string : "" );
	Com_Printf( "  p2p: %s\n", connected ? "connected" : "not connected" );
	if ( cl_coopLastError && cl_coopLastError->string[0] )
	{
		Com_Printf( "  last error: %s\n", cl_coopLastError->string );
	}
}

void SteamCoopState::OnConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *info )
{
	if ( !steamReady || !info )
	{
		return;
	}

	switch ( info->m_info.m_eState )
	{
	case k_ESteamNetworkingConnectionState_Connecting:
		if ( host && connection == k_HSteamNetConnection_Invalid )
		{
			EResult result = SteamNetworkingSockets()->AcceptConnection( info->m_hConn );
			if ( result == k_EResultOK )
			{
				connection = info->m_hConn;
				SetState( "p2p-accepting" );
				UpdateLobbySummary();
			}
			else
			{
				CL_Steam_SetError( va( "Failed to accept P2P connection: %d", result ) );
				SteamNetworkingSockets()->CloseConnection( info->m_hConn, 0, "accept failed", false );
			}
		}
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		connection = info->m_hConn;
		connected = true;
		SetState( "p2p-connected" );
		UpdateLobbySummary();
		if ( host )
		{
			SendPacket( "WELCOME" );
		}
		else
		{
			SendPacket( "HELLO" );
		}
		break;

	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		if ( info->m_hConn == connection )
		{
			connected = false;
			connection = k_HSteamNetConnection_Invalid;
			SetState( "p2p-disconnected" );
			UpdateLobbySummary();
			CL_Steam_SetError( info->m_info.m_szEndDebug );
		}
		SteamNetworkingSockets()->CloseConnection( info->m_hConn, 0, "connection closed", false );
		break;

	default:
		break;
	}
}

void SteamCoopState::UpdateLobbySummary()
{
	if ( !steamReady )
	{
		CL_Steam_SetCvar( "cl_coopLobbySummary", "Lobby: Steam unavailable" );
		CL_Steam_SetCvar( "cl_coopLobbyMembers", "Players: 0/2" );
		return;
	}

	if ( !lobby.IsValid() )
	{
		CL_Steam_SetCvar( "cl_coopLobbySummary", host ? "Lobby: creating private invite-only lobby..." : "Lobby: joining invite..." );
		CL_Steam_SetCvar( "cl_coopLobbyMembers", "Players: 1/2" );
		return;
	}

	char members[256] = {0};
	int memberCount = SteamMatchmaking()->GetNumLobbyMembers( lobby );
	for ( int i = 0; i < memberCount; ++i )
	{
		CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex( lobby, i );
		const char *name = SteamFriends()->GetFriendPersonaName( member );
		if ( !name || !name[0] )
		{
			name = ( member == SteamUser()->GetSteamID() ) ? SteamFriends()->GetPersonaName() : "Unknown";
		}
		if ( members[0] )
		{
			Q_strcat( members, sizeof( members ), ", " );
		}
		Q_strcat( members, sizeof( members ), name );
	}

	if ( !members[0] )
	{
		Q_strncpyz( members, SteamFriends()->GetPersonaName(), sizeof( members ) );
		memberCount = 1;
	}

	CL_Steam_SetCvar( "cl_coopLobbyMembers", va( "Players: %d/2 - %s", memberCount, members ) );
	CL_Steam_SetCvar(
		"cl_coopLobbySummary",
		memberCount > 1 ? va( "Lobby ready: %s", members ) : va( "Lobby open: waiting for invited friend (%s)", members ) );
}

void SteamCoopState::SetState( const char *state )
{
	CL_Steam_SetCvar( "cl_coopLobbyState", state );
}

void SteamCoopState::SetRole( const char *role )
{
	CL_Steam_SetCvar( "cl_coopRole", role );
}

void SteamCoopState::SetLobbyId( CSteamID lobbyId )
{
	CL_Steam_SetCvar( "cl_coopLobbyId", lobbyId.IsValid() ? va( "%llu", lobbyId.ConvertToUint64() ) : "" );
}

void SteamCoopState::SetPeer( CSteamID peerId )
{
	CL_Steam_SetCvar( "cl_coopPeerSteamId", peerId.IsValid() ? va( "%llu", peerId.ConvertToUint64() ) : "" );
}

void SteamCoopState::ClearRichPresence()
{
	if ( steamReady )
	{
		SteamFriends()->ClearRichPresence();
	}
}

void SteamCoopState::UpdateRichPresence( const char *status )
{
	if ( !steamReady )
	{
		return;
	}

	SteamFriends()->SetRichPresence( "status", status ? status : "" );
	if ( lobby.IsValid() )
	{
		SteamFriends()->SetRichPresence( "steam_player_group", va( "%llu", lobby.ConvertToUint64() ) );
		SteamFriends()->SetRichPresence( "steam_player_group_size", va( "%d", SteamMatchmaking()->GetNumLobbyMembers( lobby ) ) );
	}
	else
	{
		SteamFriends()->SetRichPresence( "steam_player_group", "" );
		SteamFriends()->SetRichPresence( "steam_player_group_size", "" );
	}
}

void SteamCoopState::StartListenSocket()
{
	CloseNetworking();
	listenSocket = SteamNetworkingSockets()->CreateListenSocketP2P( COOP_STEAM_VIRTUAL_PORT, 0, NULL );
	if ( listenSocket == k_HSteamListenSocket_Invalid )
	{
		CL_Steam_SetError( "Failed to create Steam P2P listen socket." );
		SetState( "p2p-listen-failed" );
		return;
	}

	SetState( "p2p-listening" );
}

void SteamCoopState::ConnectToLobbyOwner()
{
	if ( !lobby.IsValid() )
	{
		return;
	}

	CSteamID owner = SteamMatchmaking()->GetLobbyOwner( lobby );
	if ( !owner.IsValid() || owner == SteamUser()->GetSteamID() )
	{
		return;
	}

	CloseNetworking();
	peer = owner;
	SetPeer( peer );

	SteamNetworkingIdentity identity;
	identity.Clear();
	identity.SetSteamID( owner );
	connection = SteamNetworkingSockets()->ConnectP2P( identity, COOP_STEAM_VIRTUAL_PORT, 0, NULL );
	if ( connection == k_HSteamNetConnection_Invalid )
	{
		CL_Steam_SetError( "Failed to create Steam P2P client connection." );
		SetState( "p2p-connect-failed" );
		return;
	}

	SetState( "p2p-connecting" );
}

void SteamCoopState::CloseNetworking()
{
	if ( steamReady )
	{
		if ( connection != k_HSteamNetConnection_Invalid )
		{
			SteamNetworkingSockets()->CloseConnection( connection, 0, "closing co-op", false );
		}
		if ( listenSocket != k_HSteamListenSocket_Invalid )
		{
			SteamNetworkingSockets()->CloseListenSocket( listenSocket );
		}
	}

	connection = k_HSteamNetConnection_Invalid;
	listenSocket = k_HSteamListenSocket_Invalid;
	connected = false;
	lastPingTime = 0;
}

void SteamCoopState::SendPacket( const char *packet )
{
	if ( !connected || connection == k_HSteamNetConnection_Invalid || !packet )
	{
		return;
	}

	EResult result = SteamNetworkingSockets()->SendMessageToConnection(
		connection,
		packet,
		static_cast<uint32>( strlen( packet ) + 1 ),
		k_nSteamNetworkingSend_Reliable,
		NULL );
	if ( result != k_EResultOK )
	{
		CL_Steam_SetError( va( "Failed to send Steam P2P packet: %d", result ) );
	}
}

void SteamCoopState::ReceivePackets()
{
	if ( connection == k_HSteamNetConnection_Invalid )
	{
		return;
	}

	SteamNetworkingMessage_t *messages[16];
	int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection( connection, messages, ARRAY_LEN( messages ) );
	for ( int i = 0; i < count; ++i )
	{
		const char *packet = static_cast<const char *>( messages[i]->m_pData );
		if ( packet )
		{
			HandlePacket( packet );
		}
		messages[i]->Release();
	}
}

void SteamCoopState::HandlePacket( const char *packet )
{
	if ( !packet )
	{
		return;
	}

	if ( !Q_stricmp( packet, "HELLO" ) )
	{
		Com_Printf( "Steam co-op: received HELLO\n" );
		SendPacket( "WELCOME" );
	}
	else if ( !Q_stricmp( packet, "WELCOME" ) )
	{
		Com_Printf( "Steam co-op: received WELCOME\n" );
	}
	else if ( !Q_stricmp( packet, "PING" ) )
	{
		SendPacket( "PONG" );
	}
	else if ( !Q_stricmp( packet, "PONG" ) )
	{
		Com_DPrintf( "Steam co-op: received PONG\n" );
	}
	else if ( !Q_stricmpn( packet, "START_MAP ", 10 ) )
	{
		char mapName[MAX_QPATH] = {0};
		int skill = 1;
		if ( sscanf( packet + 10, "%63s %d", mapName, &skill ) != 2 )
		{
			CL_Steam_SetError( "Received malformed START_MAP packet." );
			return;
		}
		Com_Printf( "Steam co-op: received START_MAP %s %d\n", mapName, skill );
		CL_Coop_LaunchSelectedMap( mapName, skill );
	}
	else
	{
		Com_DPrintf( "Steam co-op: received packet '%s'\n", packet );
	}
}

void SteamCoopState::SendPingIfNeeded()
{
	if ( !connected )
	{
		return;
	}

	int now = Sys_Milliseconds();
	if ( lastPingTime == 0 || now - lastPingTime >= COOP_PING_INTERVAL_MSEC )
	{
		SendPacket( "PING" );
		lastPingTime = now;
	}
}

void SteamCoopState::OnLobbyCreated( LobbyCreated_t *result, bool ioFailure )
{
	if ( ioFailure || !result || result->m_eResult != k_EResultOK )
	{
		SetState( "lobby-create-failed" );
		CL_Steam_SetError( va( "Failed to create Steam lobby: %d", result ? result->m_eResult : k_EResultFail ) );
		return;
	}

	lobby = CSteamID( result->m_ulSteamIDLobby );
	SetLobbyId( lobby );
	SteamMatchmaking()->SetLobbyData( lobby, "openjo_coop", "1" );
	SteamMatchmaking()->SetLobbyData( lobby, "version", "1" );
	SteamMatchmaking()->SetLobbyData( lobby, "state", "lobby" );
	SteamMatchmaking()->SetLobbyData( lobby, "join_policy", "invite-only" );
	SteamMatchmaking()->SetLobbyData( lobby, "map", cl_coopSelectedMap ? cl_coopSelectedMap->string : s_coopLevels[0].mapName );
	SteamMatchmaking()->SetLobbyData( lobby, "skill", va( "%d", Cvar_VariableIntegerValue( "g_spskill" ) ) );
	UpdateRichPresence( "Hosting invite-only OpenJO co-op" );
	StartListenSocket();
	UpdateLobbySummary();
	Com_Printf( "Steam co-op: invite-only lobby created %llu\n", lobby.ConvertToUint64() );
}

void SteamCoopState::OnLobbyEnter( LobbyEnter_t *event )
{
	if ( !event )
	{
		return;
	}
	if ( event->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess )
	{
		SetState( "lobby-enter-failed" );
		CL_Steam_SetError( va( "Failed to enter Steam lobby: %u", event->m_EChatRoomEnterResponse ) );
		return;
	}

	lobby = CSteamID( event->m_ulSteamIDLobby );
	SetLobbyId( lobby );

	CSteamID owner = SteamMatchmaking()->GetLobbyOwner( lobby );
	host = ( owner == SteamUser()->GetSteamID() );
	SetRole( host ? "host" : "client" );
	SetState( host ? "lobby-host" : "lobby-client" );
	UpdateRichPresence( host ? "Hosting invite-only OpenJO co-op" : "Joined OpenJO co-op" );
	UpdateLobbySummary();

	if ( host )
	{
		StartListenSocket();
	}
	else
	{
		ConnectToLobbyOwner();
	}
}

void SteamCoopState::OnLobbyChatUpdate( LobbyChatUpdate_t *event )
{
	if ( !event || !lobby.IsValid() || event->m_ulSteamIDLobby != lobby.ConvertToUint64() )
	{
		return;
	}

	UpdateRichPresence( host ? "Hosting invite-only OpenJO co-op" : "Joined OpenJO co-op" );
	UpdateLobbySummary();
}

void SteamCoopState::OnGameLobbyJoinRequested( GameLobbyJoinRequested_t *event )
{
	if ( !event )
	{
		return;
	}

	JoinLobby( event->m_steamIDLobby.ConvertToUint64() );
}

#endif

static bool CL_Steam_IsReady()
{
#ifdef USE_STEAMWORKS
	return s_steamCoop && cl_coopSteamAvailable && cl_coopSteamAvailable->integer;
#else
	return false;
#endif
}

static void CL_Steam_Host_f( void )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->Host();
	}
#else
	CL_Steam_SetError( "This OpenJK build was not compiled with Steamworks support." );
#endif
}

static void CL_Steam_InviteFriend_f( void )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->InviteFriend();
	}
#else
	CL_Steam_SetError( "This OpenJK build was not compiled with Steamworks support." );
#endif
}

static void CL_Coop_Difficulty_f( void )
{
	if ( Cmd_Argc() < 2 )
	{
		CL_Steam_SetError( "Usage: coop_difficulty <0-3>" );
		return;
	}

	CL_Coop_SetDifficulty( atoi( Cmd_Argv( 1 ) ) );
}

static void CL_Coop_LevelNext_f( void )
{
	CL_Coop_SetSelectedLevel( CL_Coop_SelectedLevelIndex() + 1 );
}

static void CL_Coop_LevelPrev_f( void )
{
	CL_Coop_SetSelectedLevel( CL_Coop_SelectedLevelIndex() - 1 );
}

static void CL_Coop_StartGame_f( void )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->StartGame();
	}
#else
	CL_Steam_SetError( "This OpenJK build was not compiled with Steamworks support." );
#endif
}

static void CL_Steam_Leave_f( void )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->Leave();
	}
#else
	CL_Steam_SetError( "This OpenJK build was not compiled with Steamworks support." );
#endif
}

static void CL_Steam_Status_f( void )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->PrintStatus();
		return;
	}
#endif
	Com_Printf( "Steam co-op status:\n" );
	Com_Printf( "  steam: %s\n", CL_Steam_IsReady() ? "available" : "unavailable" );
	if ( cl_coopLastError && cl_coopLastError->string[0] )
	{
		Com_Printf( "  last error: %s\n", cl_coopLastError->string );
	}
}

void CL_Steam_Init( void )
{
	cl_coopEnabled = Cvar_Get( "cl_coopEnabled", "0", CVAR_TEMP );
	cl_coopRole = Cvar_Get( "cl_coopRole", "", CVAR_TEMP );
	cl_coopLobbyState = Cvar_Get( "cl_coopLobbyState", "offline", CVAR_TEMP );
	cl_coopLobbyId = Cvar_Get( "cl_coopLobbyId", "", CVAR_TEMP );
	cl_coopPeerSteamId = Cvar_Get( "cl_coopPeerSteamId", "", CVAR_TEMP );
	cl_coopLastError = Cvar_Get( "cl_coopLastError", "", CVAR_TEMP );
	cl_coopSteamAvailable = Cvar_Get( "cl_coopSteamAvailable", "0", CVAR_TEMP );
	cl_coopAutoHost = Cvar_Get( "cl_coopAutoHost", "1", CVAR_ARCHIVE );
	cl_coopLobbySummary = Cvar_Get( "cl_coopLobbySummary", "Lobby: starting Steam co-op...", CVAR_TEMP );
	cl_coopLobbyMembers = Cvar_Get( "cl_coopLobbyMembers", "Players: 1/2", CVAR_TEMP );
	cl_coopSelectedMap = Cvar_Get( "cl_coopSelectedMap", s_coopLevels[0].mapName, CVAR_ARCHIVE );
	cl_coopSelectedLevel = Cvar_Get( "cl_coopSelectedLevel", s_coopLevels[0].displayName, CVAR_TEMP );
	Cvar_Get( "g_spskill", "1", CVAR_ARCHIVE );
	cl_coopDifficultyName = Cvar_Get( "cl_coopDifficultyName", CL_Coop_DifficultyName( Cvar_VariableIntegerValue( "g_spskill" ) ), CVAR_TEMP );
	CL_Coop_SetSelectedLevel( CL_Coop_SelectedLevelIndex() );
	CL_Coop_SetDifficulty( Cvar_VariableIntegerValue( "g_spskill" ) );

	Cmd_AddCommand( "coop_host", CL_Steam_Host_f );
	Cmd_AddCommand( "coop_invite_friend", CL_Steam_InviteFriend_f );
	Cmd_AddCommand( "coop_difficulty", CL_Coop_Difficulty_f );
	Cmd_AddCommand( "coop_level_next", CL_Coop_LevelNext_f );
	Cmd_AddCommand( "coop_level_prev", CL_Coop_LevelPrev_f );
	Cmd_AddCommand( "coop_start_game", CL_Coop_StartGame_f );
	Cmd_AddCommand( "coop_leave", CL_Steam_Leave_f );
	Cmd_AddCommand( "coop_status", CL_Steam_Status_f );

#ifdef USE_STEAMWORKS
	s_steamCoop = new SteamCoopState();
	if ( s_steamCoop->Init() && cl_coopAutoHost->integer )
	{
		s_steamCoop->Host();
	}
#else
	CL_Steam_SetCvar( "cl_coopLastError", "Steamworks support is not compiled into this build." );
#endif
}

void CL_Steam_Frame( void )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->Frame();
	}
#endif
}

void CL_Steam_Shutdown( void )
{
#ifdef USE_STEAMWORKS
	delete s_steamCoop;
	s_steamCoop = NULL;
#endif

	Cmd_RemoveCommand( "coop_host" );
	Cmd_RemoveCommand( "coop_invite_friend" );
	Cmd_RemoveCommand( "coop_difficulty" );
	Cmd_RemoveCommand( "coop_level_next" );
	Cmd_RemoveCommand( "coop_level_prev" );
	Cmd_RemoveCommand( "coop_start_game" );
	Cmd_RemoveCommand( "coop_leave" );
	Cmd_RemoveCommand( "coop_status" );
}
