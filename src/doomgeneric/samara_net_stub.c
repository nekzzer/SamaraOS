/* Networking stubs — DOOM uses these symbols even in single-player builds. */

#include "doomtype.h"
#include "d_ticcmd.h"
#include "net_defs.h"
#include "net_client.h"
#include "net_server.h"
#include "net_io.h"
#include "net_packet.h"
#include "net_query.h"
#include "net_dedicated.h"
#include "net_gui.h"
#include "net_loop.h"
#include "net_sdl.h"
#include "sha1.h"

/* Module instances referenced from d_loop.c */
net_module_t net_loop_server_module = { 0 };
net_module_t net_loop_client_module = { 0 };
net_module_t net_sdl_module         = { 0 };

/* Globals from net_client.h (drone / net_client_connected live in dummy.c) */
boolean net_client_received_wait_data = 0;
net_waitdata_t net_client_wait_data;
boolean net_waiting_for_launch = 0;
char* net_player_name = "samara";
sha1_digest_t net_server_wad_sha1sum;
sha1_digest_t net_server_deh_sha1sum;
unsigned int net_server_is_freedoom = 0;
sha1_digest_t net_local_wad_sha1sum;
sha1_digest_t net_local_deh_sha1sum;
unsigned int net_local_is_freedoom = 0;

/* CL */
boolean NET_CL_Connect(net_addr_t* a, net_connect_data_t* d) { (void)a; (void)d; return 0; }
void NET_CL_Disconnect(void) { }
void NET_CL_Run(void) { }
void NET_CL_Init(void) { }
void NET_CL_LaunchGame(void) { }
void NET_CL_StartGame(net_gamesettings_t* s) { (void)s; }
void NET_CL_SendTiccmd(ticcmd_t* t, int m) { (void)t; (void)m; }
boolean NET_CL_GetSettings(net_gamesettings_t* s) { (void)s; return 0; }
void NET_Init(void) { }
void NET_BindVariables(void) { }

/* SV */
void NET_SV_Init(void) { }
void NET_SV_Run(void) { }
void NET_SV_Shutdown(void) { }
void NET_SV_AddModule(net_module_t* m) { (void)m; }
void NET_SV_RegisterWithMaster(void) { }

/* IO */
net_context_t* NET_NewContext(void) { return 0; }
void NET_AddModule(net_context_t* c, net_module_t* m) { (void)c; (void)m; }
void NET_SendPacket(net_addr_t* a, net_packet_t* p) { (void)a; (void)p; }
void NET_SendBroadcast(net_context_t* c, net_packet_t* p) { (void)c; (void)p; }
boolean NET_RecvPacket(net_context_t* c, net_addr_t** a, net_packet_t** p) { (void)c; (void)a; (void)p; return 0; }
char* NET_AddrToString(net_addr_t* a) { (void)a; return "<no-net>"; }
void NET_FreeAddress(net_addr_t* a) { (void)a; }
net_addr_t* NET_ResolveAddress(net_context_t* c, char* a) { (void)c; (void)a; return 0; }

/* Query */
int NET_StartLANQuery(void) { return 0; }
int NET_StartMasterQuery(void) { return 0; }
void NET_LANQuery(void) { }
void NET_MasterQuery(void) { }
void NET_QueryAddress(char* a) { (void)a; }
net_addr_t* NET_FindLANServer(void) { return 0; }
int NET_Query_Poll(net_query_callback_t cb, void* u) { (void)cb; (void)u; return 0; }
net_addr_t* NET_Query_ResolveMaster(net_context_t* c) { (void)c; return 0; }
void NET_Query_AddToMaster(net_addr_t* a) { (void)a; }
boolean NET_Query_CheckAddedToMaster(boolean* r) { (void)r; return 0; }
void NET_Query_MasterResponse(net_packet_t* p) { (void)p; }

/* Packet */
net_packet_t* NET_NewPacket(int sz) { (void)sz; return 0; }
net_packet_t* NET_PacketDup(net_packet_t* p) { (void)p; return 0; }
void NET_FreePacket(net_packet_t* p) { (void)p; }
char* NET_ReadString(net_packet_t* p) { (void)p; return 0; }
void NET_WriteString(net_packet_t* p, char* s) { (void)p; (void)s; }

/* GUI / dedicated */
void NET_WaitForLaunch(void) { }
void NET_DedicatedServer(void) { }
