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
#include "../server/server.h"

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
static cvar_t *cl_coopLocalIdentity;
static cvar_t *cl_coopHostIdentity;
static cvar_t *cl_coopPeerIdentity;
static cvar_t *cl_coopDifficultyName;

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

static qboolean CL_Coop_IsSafeSaveName( const char *saveName )
{
	if ( !saveName || !saveName[0] )
	{
		return qfalse;
	}
	if ( strpbrk( saveName, "\\/:;\"'\n\r" ) )
	{
		return qfalse;
	}
	return qtrue;
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

static void CL_Coop_LaunchNewGame( int skill )
{
	CL_Coop_SetDifficulty( skill );
	CL_Steam_SetCvar( "cl_coopEnabled", "1" );
	Cbuf_ExecuteText( EXEC_APPEND, va( "set cl_coopEnabled 1\nset g_spskill %d\nmap kejim_post\n", skill ) );
}

static void CL_Coop_LoadSaveGame( const char *saveName )
{
	if ( !CL_Coop_IsSafeSaveName( saveName ) )
	{
		CL_Steam_SetError( va( "Invalid co-op save name '%s'.", saveName ? saveName : "" ) );
		return;
	}

	CL_Steam_SetCvar( "cl_coopEnabled", "1" );
	Cbuf_ExecuteText( EXEC_APPEND, va( "set cl_coopEnabled 1\nload %s\n", saveName ) );
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
	void StartSave( const char *saveName );
	void PrintStatus() const;
	void SendUsercmd( int sequence, const usercmd_t *cmd );
	void OnConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *info );

private:
	void SetState( const char *state );
	void SetRole( const char *role );
	void SetLobbyId( CSteamID lobbyId );
	void SetPeer( CSteamID peerId );
	void ClearRichPresence();
	void UpdateRichPresence( const char *status );
	void FormatIdentity( CSteamID steamId, char *buffer, int bufferSize ) const;
	void UpdateIdentityCvars();
	void UpdateLobbySummary();
	void StartListenSocket();
	void ConnectToLobbyOwner();
	void CloseNetworking();
	void SendPacket( const char *packet, bool reliable = true );
	void ReceivePackets();
	void HandlePacket( const char *packet );
	void SendPingIfNeeded();
	void OnLobbyCreated( LobbyCreated_t *result, bool ioFailure );
	void OnLobbyEnter( LobbyEnter_t *event );
	void OnLobbyChatUpdate( LobbyChatUpdate_t *event );
	void OnGameLobbyJoinRequested( GameLobbyJoinRequested_t *event );

	bool steamReady;
	bool host;
	bool joiningLobby;
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
	  joiningLobby( false ),
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
	joiningLobby = false;
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
	joiningLobby = true;
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
	joiningLobby = false;
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

	SteamMatchmaking()->SetLobbyData( lobby, "state", "starting" );
	SteamMatchmaking()->SetLobbyData( lobby, "start_mode", "new_game" );
	SteamMatchmaking()->SetLobbyData( lobby, "skill", va( "%d", Cvar_VariableIntegerValue( "g_spskill" ) ) );
	SetState( "starting-game" );
	UpdateRichPresence( "Starting new OpenJO co-op game" );
	SendPacket( va( "START_NEW_GAME %d", Cvar_VariableIntegerValue( "g_spskill" ) ) );
	Com_Printf( "Steam co-op: starting new game on %s difficulty\n", CL_Coop_DifficultyName( Cvar_VariableIntegerValue( "g_spskill" ) ) );
	CL_Coop_LaunchNewGame( Cvar_VariableIntegerValue( "g_spskill" ) );
}

void SteamCoopState::StartSave( const char *saveName )
{
	if ( !steamReady )
	{
		CL_Steam_SetError( "Steam is not available; cannot load a co-op save." );
		return;
	}
	if ( !host )
	{
		CL_Steam_SetError( "Only the lobby host can load a co-op save." );
		return;
	}
	if ( !lobby.IsValid() )
	{
		CL_Steam_SetError( "Steam co-op lobby is not ready yet; try again in a moment." );
		return;
	}
	if ( !CL_Coop_IsSafeSaveName( saveName ) )
	{
		CL_Steam_SetError( va( "Invalid co-op save name '%s'.", saveName ? saveName : "" ) );
		return;
	}

	SteamMatchmaking()->SetLobbyData( lobby, "state", "starting" );
	SteamMatchmaking()->SetLobbyData( lobby, "start_mode", "save_game" );
	SetState( "starting-save" );
	UpdateRichPresence( "Loading OpenJO co-op save" );
	SendPacket( va( "START_SAVE %s", saveName ) );
	Com_Printf( "Steam co-op: loading save %s\n", saveName );
	CL_Coop_LoadSaveGame( saveName );
}

void SteamCoopState::PrintStatus() const
{
	Com_Printf( "Steam co-op status:\n" );
	Com_Printf( "  steam: %s\n", steamReady ? "available" : "unavailable" );
	Com_Printf( "  state: %s\n", cl_coopLobbyState ? cl_coopLobbyState->string : "" );
	Com_Printf( "  role: %s\n", cl_coopRole ? cl_coopRole->string : "" );
	Com_Printf( "  lobby: %s\n", cl_coopLobbyId ? cl_coopLobbyId->string : "" );
	Com_Printf( "  peer: %s\n", cl_coopPeerSteamId ? cl_coopPeerSteamId->string : "" );
	Com_Printf( "  local: %s\n", cl_coopLocalIdentity ? cl_coopLocalIdentity->string : "" );
	Com_Printf( "  host identity: %s\n", cl_coopHostIdentity ? cl_coopHostIdentity->string : "" );
	Com_Printf( "  peer identity: %s\n", cl_coopPeerIdentity ? cl_coopPeerIdentity->string : "" );
	Com_Printf( "  auto host: %s\n", cl_coopAutoHost && cl_coopAutoHost->integer ? "enabled" : "disabled" );
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
			CSteamID remote = info->m_info.m_identityRemote.GetSteamID();
			if ( remote.IsValid() )
			{
				peer = remote;
				SetPeer( peer );
			}
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
		{
			CSteamID remote = info->m_info.m_identityRemote.GetSteamID();
			if ( remote.IsValid() )
			{
				peer = remote;
				SetPeer( peer );
			}
		}
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
		CL_Steam_SetCvar( "cl_coopLocalIdentity", "You: Steam unavailable" );
		CL_Steam_SetCvar( "cl_coopHostIdentity", "Host: none" );
		CL_Steam_SetCvar( "cl_coopPeerIdentity", "Peer: none" );
		return;
	}

	if ( !lobby.IsValid() )
	{
		UpdateIdentityCvars();
		if ( host )
		{
			CL_Steam_SetCvar( "cl_coopLobbySummary", "Lobby: creating private invite-only lobby..." );
		}
		else if ( joiningLobby )
		{
			CL_Steam_SetCvar( "cl_coopLobbySummary", "Lobby: joining invite..." );
		}
		else
		{
			CL_Steam_SetCvar( "cl_coopLobbySummary", "Lobby: Steam ready; creating invite lobby..." );
		}
		CL_Steam_SetCvar( "cl_coopLobbyMembers", "Players: 1/2" );
		return;
	}

	char members[256] = {0};
	int memberCount = SteamMatchmaking()->GetNumLobbyMembers( lobby );
	CSteamID local = SteamUser()->GetSteamID();
	CSteamID peerCandidate;
	for ( int i = 0; i < memberCount; ++i )
	{
		CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex( lobby, i );
		if ( member.IsValid() && member != local && !peerCandidate.IsValid() )
		{
			peerCandidate = member;
		}
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

	peer = peerCandidate;
	SetPeer( peer );
	UpdateIdentityCvars();
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

void SteamCoopState::FormatIdentity( CSteamID steamId, char *buffer, int bufferSize ) const
{
	if ( !buffer || bufferSize <= 0 )
	{
		return;
	}
	buffer[0] = '\0';

	if ( !steamReady || !steamId.IsValid() )
	{
		Q_strncpyz( buffer, "none", bufferSize );
		return;
	}

	const char *name = ( steamId == SteamUser()->GetSteamID() ) ? SteamFriends()->GetPersonaName() : SteamFriends()->GetFriendPersonaName( steamId );
	if ( !name || !name[0] )
	{
		name = "Unknown";
	}

	Com_sprintf( buffer, bufferSize, "%s (%llu)", name, steamId.ConvertToUint64() );
}

void SteamCoopState::UpdateIdentityCvars()
{
	if ( !steamReady )
	{
		return;
	}

	CSteamID local = SteamUser()->GetSteamID();
	CSteamID hostId = lobby.IsValid() ? SteamMatchmaking()->GetLobbyOwner( lobby ) : ( host ? local : CSteamID() );
	CSteamID peerId = peer;

	if ( lobby.IsValid() )
	{
		for ( int i = 0; i < SteamMatchmaking()->GetNumLobbyMembers( lobby ); ++i )
		{
			CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex( lobby, i );
			if ( member.IsValid() && member != local )
			{
				peerId = member;
				break;
			}
		}
	}

	char localIdentity[128];
	char hostIdentity[128];
	char peerIdentity[128];
	FormatIdentity( local, localIdentity, sizeof( localIdentity ) );
	FormatIdentity( hostId, hostIdentity, sizeof( hostIdentity ) );
	FormatIdentity( peerId, peerIdentity, sizeof( peerIdentity ) );

	CL_Steam_SetCvar( "cl_coopLocalIdentity", va( "You: %s", localIdentity ) );
	CL_Steam_SetCvar( "cl_coopHostIdentity", va( "Host: %s", hostIdentity ) );
	CL_Steam_SetCvar( "cl_coopPeerIdentity", peerId.IsValid() ? va( "Peer: %s", peerIdentity ) : "Peer: waiting for invited friend" );
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

void SteamCoopState::SendPacket( const char *packet, bool reliable )
{
	if ( !connected || connection == k_HSteamNetConnection_Invalid || !packet )
	{
		return;
	}

	EResult result = SteamNetworkingSockets()->SendMessageToConnection(
		connection,
		packet,
		static_cast<uint32>( strlen( packet ) + 1 ),
		reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoDelay,
		NULL );
	if ( result != k_EResultOK )
	{
		CL_Steam_SetError( va( "Failed to send Steam P2P packet: %d", result ) );
	}
}

void SteamCoopState::SendUsercmd( int sequence, const usercmd_t *cmd )
{
	if ( !cmd || !connected )
	{
		return;
	}

	SendPacket(
		va(
			"USERCMD %d %d %d %d %d %d %d %d %d %d %d",
			sequence,
			cmd->serverTime,
			cmd->angles[0],
			cmd->angles[1],
			cmd->angles[2],
			cmd->forwardmove,
			cmd->rightmove,
			cmd->upmove,
			cmd->buttons,
			cmd->weapon,
			cmd->generic_cmd ),
		false );
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
	else if ( !Q_stricmpn( packet, "START_NEW_GAME ", 15 ) )
	{
		int skill = 1;
		if ( sscanf( packet + 15, "%d", &skill ) != 1 )
		{
			CL_Steam_SetError( "Received malformed START_NEW_GAME packet." );
			return;
		}
		Com_Printf( "Steam co-op: received START_NEW_GAME %d\n", skill );
		CL_Coop_LaunchNewGame( skill );
	}
	else if ( !Q_stricmpn( packet, "START_SAVE ", 11 ) )
	{
		char saveName[MAX_QPATH] = {0};
		if ( sscanf( packet + 11, "%63s", saveName ) != 1 )
		{
			CL_Steam_SetError( "Received malformed START_SAVE packet." );
			return;
		}
		Com_Printf( "Steam co-op: received START_SAVE %s\n", saveName );
		CL_Coop_LoadSaveGame( saveName );
	}
	else if ( !Q_stricmpn( packet, "USERCMD ", 8 ) )
	{
		int sequence = 0;
		int serverTime = 0;
		int angles[3] = {0, 0, 0};
		int forwardmove = 0;
		int rightmove = 0;
		int upmove = 0;
		int buttons = 0;
		int weapon = 0;
		int genericCmd = 0;
		if ( sscanf(
				packet + 8,
				"%d %d %d %d %d %d %d %d %d %d %d",
				&sequence,
				&serverTime,
				&angles[0],
				&angles[1],
				&angles[2],
				&forwardmove,
				&rightmove,
				&upmove,
				&buttons,
				&weapon,
				&genericCmd ) != 11 )
		{
			CL_Steam_SetError( "Received malformed USERCMD packet." );
			return;
		}

		(void)sequence;
		usercmd_t cmd;
		memset( &cmd, 0, sizeof( cmd ) );
		cmd.serverTime = serverTime;
		cmd.angles[0] = angles[0];
		cmd.angles[1] = angles[1];
		cmd.angles[2] = angles[2];
		cmd.forwardmove = ClampChar( forwardmove );
		cmd.rightmove = ClampChar( rightmove );
		cmd.upmove = ClampChar( upmove );
		cmd.buttons = buttons;
		cmd.weapon = static_cast<byte>( Com_Clampi( 0, 255, weapon ) );
		cmd.generic_cmd = static_cast<byte>( Com_Clampi( 0, 255, genericCmd ) );
		SV_Coop_ApplyRemoteUsercmd( &cmd );
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
	SteamMatchmaking()->SetLobbyData( lobby, "skill", va( "%d", Cvar_VariableIntegerValue( "g_spskill" ) ) );
	SteamMatchmaking()->SetLobbyData( lobby, "host_name", SteamFriends()->GetPersonaName() );
	SteamMatchmaking()->SetLobbyData( lobby, "host_id", va( "%llu", SteamUser()->GetSteamID().ConvertToUint64() ) );
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
	joiningLobby = false;

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

static void CL_Coop_StartSave_f( void )
{
	if ( Cmd_Argc() < 2 )
	{
		CL_Steam_SetError( "Usage: coop_start_save <save name>" );
		return;
	}
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->StartSave( Cmd_Argv( 1 ) );
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
	cl_coopAutoHost = Cvar_Get( "cl_coopAutoHost", "1", CVAR_TEMP );
	if ( cl_coopAutoHost->integer == 0 && ( cl_coopAutoHost->flags & CVAR_ARCHIVE ) )
	{
		Cvar_Set( "cl_coopAutoHost", "1" );
	}
	cl_coopAutoHost->flags &= ~CVAR_ARCHIVE;
	cl_coopLobbySummary = Cvar_Get( "cl_coopLobbySummary", "Lobby: starting Steam co-op...", CVAR_TEMP );
	cl_coopLobbyMembers = Cvar_Get( "cl_coopLobbyMembers", "Players: 1/2", CVAR_TEMP );
	cl_coopLocalIdentity = Cvar_Get( "cl_coopLocalIdentity", "You: Steam unavailable", CVAR_TEMP );
	cl_coopHostIdentity = Cvar_Get( "cl_coopHostIdentity", "Host: none", CVAR_TEMP );
	cl_coopPeerIdentity = Cvar_Get( "cl_coopPeerIdentity", "Peer: waiting for invited friend", CVAR_TEMP );
	Cvar_Get( "g_spskill", "1", CVAR_ARCHIVE );
	cl_coopDifficultyName = Cvar_Get( "cl_coopDifficultyName", CL_Coop_DifficultyName( Cvar_VariableIntegerValue( "g_spskill" ) ), CVAR_TEMP );
	CL_Coop_SetDifficulty( Cvar_VariableIntegerValue( "g_spskill" ) );

	Cmd_AddCommand( "coop_host", CL_Steam_Host_f );
	Cmd_AddCommand( "coop_invite_friend", CL_Steam_InviteFriend_f );
	Cmd_AddCommand( "coop_difficulty", CL_Coop_Difficulty_f );
	Cmd_AddCommand( "coop_start_game", CL_Coop_StartGame_f );
	Cmd_AddCommand( "coop_start_save", CL_Coop_StartSave_f );
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
	Cmd_RemoveCommand( "coop_start_game" );
	Cmd_RemoveCommand( "coop_start_save" );
	Cmd_RemoveCommand( "coop_leave" );
	Cmd_RemoveCommand( "coop_status" );
}

void CL_Steam_SendUsercmd( int sequence, const usercmd_t *cmd )
{
#ifdef USE_STEAMWORKS
	if ( s_steamCoop )
	{
		s_steamCoop->SendUsercmd( sequence, cmd );
	}
#endif
}
