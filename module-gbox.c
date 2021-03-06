#define MODULE_LOG_PREFIX "gbox"

#include "globals.h"
#ifdef MODULE_GBOX

#include "module-gbox.h"
#include "module-gbox-helper.h"
#include "module-gbox-sms.h"
#include "module-cccam.h"
#include "module-cccam-data.h"
#include "oscam-failban.h"
#include "oscam-client.h"
#include "oscam-ecm.h"
#include "oscam-lock.h"
#include "oscam-net.h"
#include "oscam-chk.h"
#include "oscam-string.h"
#include "oscam-time.h"
#include "oscam-reader.h"
#include "oscam-garbage.h"
#include "oscam-files.h"


#define FILE_GBOX_VERSION	"gbox.ver"
#define FILE_SHARED_CARDS_INFO	"share.info"
#define FILE_ATTACK_INFO	"attack.txt"
#define FILE_GBOX_PEER_ONL	"share.onl"
#define FILE_STATS		"stats.info"
#define FILE_GOODNIGHT_OSD	"goodnight.osd"
#define FILE_Local_CARDS_INFO	"sc.info"

#define GBOX_STAT_HELLOL	0
#define GBOX_STAT_HELLOS	1
#define GBOX_STAT_HELLOR	2
#define GBOX_STAT_HELLO3	3
#define GBOX_STAT_HELLO4	4

#define RECEIVE_BUFFER_SIZE	1024
#define MIN_GBOX_MESSAGE_LENGTH	10 //CMD + pw + pw. TODO: Check if is really min
#define MIN_ECM_LENGTH		8
#define HELLO_KEEPALIVE_TIME	120 //send hello to peer every 2 min in case no ecm received
#define STATS_WRITE_TIME	300 //write stats file every 5 min

#define LOCAL_GBOX_MAJOR_VERSION	0x02

static struct gbox_data local_gbox;
static time_t last_stats_written;

//static void    gbox_send_boxinfo(struct s_client *cli);
static void	gbox_send_hello(struct s_client *cli);
static void	gbox_send_hello_packet(struct s_client *cli, int8_t number, uchar *outbuf, uchar *ptr, int32_t nbcards);
static void	gbox_send_checkcode(struct s_client *cli);
static void	gbox_local_cards(struct s_client *cli);
static void	gbox_init_ecm_request_ext(struct gbox_ecm_request_ext *ere); 	
static int32_t	gbox_client_init(struct s_client *cli);
static int8_t	gbox_check_header(struct s_client *cli, struct s_client *proxy, uchar *data, int32_t l);
static int8_t	gbox_incoming_ecm(struct s_client *cli, uchar *data, int32_t n);
static int8_t	gbox_cw_received(struct s_client *cli, uchar *data, int32_t n);
static int32_t	gbox_recv_chk(struct s_client *cli, uchar *dcw, int32_t *rc, uchar *data, int32_t n);
static int32_t	gbox_checkcode_recv(struct s_client *cli, uchar *checkcode);
static uint16_t	gbox_convert_password_to_id(uint32_t password);
static int32_t	gbox_send_ecm(struct s_client *cli, ECM_REQUEST *er, uchar *UNUSED(buf));
uint32_t	gbox_get_ecmchecksum(uchar *ecm, uint16_t ecmlen);
static void	init_local_gbox(void);

char *get_gbox_tmp_fname(char *fext)
{
	static char gbox_tmpfile_buf[64] = { 0 };	
	const char *slash = "/";
	if(!cfg.gbox_tmp_dir)
	{
		snprintf(gbox_tmpfile_buf, sizeof(gbox_tmpfile_buf), "%s%s%s",get_tmp_dir(), slash, fext);
	}
	else
	{ 
		if(cfg.gbox_tmp_dir[strlen(cfg.gbox_tmp_dir) - 1] == '/') { slash = ""; }
		snprintf(gbox_tmpfile_buf, sizeof(gbox_tmpfile_buf), "%s%s%s", cfg.gbox_tmp_dir, slash, fext);
	}
	return gbox_tmpfile_buf; 
}

uint16_t gbox_get_local_gbox_id(void)
{
	return local_gbox.id;
}

uint32_t gbox_get_local_gbox_password(void)
{
	return local_gbox.password;
}

static uint8_t gbox_get_my_vers (void)
{
	uint8_t gbx_vers = a2i(cfg.gbox_my_vers,1);

	return gbx_vers;
}

static uint8_t gbox_get_my_cpu_api (void)
{
/* For configurable later adapt according to these functions:
unsigned char *GboxAPI( unsigned char a ) {
	a = a & 7 ;
	switch ( a ) { 
		case 0 : strcpy ( s_24,"No API");
                	break;  
		case 1 : strcpy ( s_24,"API 1");
                        break;  
                case 2 : strcpy ( s_24,"API 2");
                        break;  
                case 3 : strcpy ( s_24,"API 3");
                        break;  
                case 4 : strcpy ( s_24,"IBM API");
                        break;  
                default : strcpy ( s_24," ");
	}
        return s_24 ;
}
                                                                                        
unsigned char *GboxCPU( unsigned char a ) {
	a = a & 112 ;
        a = a >> 4 ;
        switch ( a ) { 
	        case 1 : strcpy ( s_23,"80X86 compatible CPU");
        		break;  
        	case 2 : strcpy ( s_23,"Motorola PowerPC MPC823 CPU");
        		break;  
        	case 3 : strcpy ( s_23,"IBM PowerPC STB CPU");
        		break;  
		default : strcpy ( s_23," ");
	}
	return s_23:
}
*/
	return a2i(cfg.gbox_my_cpu_api,1);
}

static void write_goodnight_to_osd_file(struct s_client *cli)
{
	char *fext= FILE_GOODNIGHT_OSD; 
	char *fname = get_gbox_tmp_fname(fext); 
	if (file_exists(fname))
	{
	char buf[50];
	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%s %s %s", fname, username(cli), cli->reader->device);
	cs_log_dbg(D_READER, "found file %s - write goodnight info from %s %s to OSD", fname, username(cli),cli->reader->device);
	char *cmd = buf;
              FILE *p;
              if ((p = popen(cmd, "w")) == NULL)
		{	
			cs_log("Error %s",fname);
			return;
		}
              pclose(p);
	}
	return;
}

void gbox_write_peer_onl(void)
{
	char *fext= FILE_GBOX_PEER_ONL; 
	char *fname = get_gbox_tmp_fname(fext); 
	FILE *fhandle = fopen(fname, "w");
	if(!fhandle)
	{
		cs_log("Couldn't open %s: %s", fname, strerror(errno));
		return;
	}
	struct s_client *cl;
	for(cl = first_client; cl; cl = cl->next)
	{
		if(cl->gbox && cl->typ == 'p')
		{
			struct gbox_peer *peer = cl->gbox;
			if (peer->online)
				{ fprintf(fhandle, "1 %s  %s %04X 2.%02X\n",cl->reader->device, cs_inet_ntoa(cl->ip),peer->gbox.id, peer->gbox.minor_version); }
			else
				{ fprintf(fhandle, "0 %s  %s %04X 0.00\n",cl->reader->device, cs_inet_ntoa(cl->ip),peer->gbox.id); }
		}
	}
	fclose(fhandle);
	return;
}	

void gbox_write_version(void)
{
	char *fext= FILE_GBOX_VERSION; 
	char *fname = get_gbox_tmp_fname(fext); 

	FILE *fhandle = fopen(fname, "w");
	if(!fhandle)
	{
		cs_log("Couldn't open %s: %s", fname, strerror(errno));
		return;
	}
	fprintf(fhandle, "%02X.%02X\n", LOCAL_GBOX_MAJOR_VERSION, gbox_get_my_vers());
	fclose(fhandle);
}

void gbox_write_local_cards_info(void)
{
	int32_t card_count = 0;
	char *fext= FILE_Local_CARDS_INFO; 
	char *fname = get_gbox_tmp_fname(fext); 
	FILE *fhandle;
	fhandle = fopen(fname, "w");
	if(!fhandle)
	{
		cs_log("Couldn't open %s: %s", fname, strerror(errno));
		return;
	}
	LL_ITER it;
	struct gbox_card *card;
	it = ll_iter_create(local_gbox.cards);
	char *crdtype = NULL;
	while((card = ll_iter_next(&it)))
	{
		if(card->type == 1)
		{crdtype = "Local_Card";}
		if(card->type == 2)
		{crdtype = "Betun_Card";}		
		if(card->type == 3)
		{crdtype = "CCcam_Card";}		
		if(card->type == 4)
		{crdtype = "Proxy_Card";}
		fprintf(fhandle, "CardID:%2d %s %08X Sl:%2d id:%04X\n",
				card_count, crdtype, card->provid_1,card->id.slot, card->id.peer);
		card_count++;		
					
	}
	fclose(fhandle);
	return;
}

void gbox_write_shared_cards_info(void)
{
	int32_t card_count = 0;
	//int32_t i = 0;
	char *fext= FILE_SHARED_CARDS_INFO; 
	char *fname = get_gbox_tmp_fname(fext); 
	FILE *fhandle;
	fhandle = fopen(fname, "w");
	if(!fhandle)
	{
		cs_log("Couldn't open %s: %s", fname, strerror(errno));
		return;
	}
	LL_ITER it;
	struct gbox_card *card;
	struct s_client *cl;
	for(cl = first_client; cl; cl = cl->next)
	//for(i = 0, cl = first_client; cl; cl = cl->next, i++)
	{
		if(cl->gbox && cl->reader->card_status == CARD_INSERTED && cl->typ == 'p')
		{
			struct gbox_peer *peer = cl->gbox;

			it = ll_iter_create(peer->gbox.cards);
			while((card = ll_iter_next(&it)))
			{
				fprintf(fhandle, "CardID %2d at %s Card %08X Sl:%2d Lev:%1d dist:%1d id:%04X\n",
						card_count, cl->reader->device, card->provid_1,
						card->id.slot, card->lvl, card->dist, card->id.peer);
				card_count++;
			}
		}
	}
	fclose(fhandle);
	return;
}

void gbox_write_stats(void)
{
	int32_t card_count = 0;
	int32_t i = 0;
	struct gbox_good_srvid *srvid_good = NULL;
	struct gbox_bad_srvid *srvid_bad = NULL;	
	char *fext= FILE_STATS; 
	char *fname = get_gbox_tmp_fname(fext); 
	FILE *fhandle;
	fhandle = fopen(fname, "w");
	if(!fhandle)
	{
		cs_log("Couldn't open %s: %s", fname, strerror(errno));
		return;
	}

	LL_ITER it;
	struct gbox_card *card;

	struct s_client *cl;
	for(i = 0, cl = first_client; cl; cl = cl->next, i++)
	{
		if(cl->gbox && cl->reader->card_status == CARD_INSERTED && cl->typ == 'p')
		{
			struct gbox_peer *peer = cl->gbox;

			it = ll_iter_create(peer->gbox.cards);
			while((card = ll_iter_next(&it)))
			{
				fprintf(fhandle, "CardID %4d Card %08X id:%04X #CWs:%d AVGtime:%d ms\n",
						card_count, card->provid_1, card->id.peer, card->no_cws_returned, card->average_cw_time);
				fprintf(fhandle, "Good SIDs:\n");
				LL_ITER it2 = ll_iter_create(card->goodsids);
				while((srvid_good = ll_iter_next(&it2)))
					{ fprintf(fhandle, "%04X\n", srvid_good->srvid.sid); }
				fprintf(fhandle, "Bad SIDs:\n");				
				it2 = ll_iter_create(card->badsids);
				while((srvid_bad = ll_iter_next(&it2)))
					{ fprintf(fhandle, "%04X #%d\n", srvid_bad->srvid.sid, srvid_bad->bad_strikes); }				
				card_count++;
			} // end of while ll_iter_next
		} // end of if cl->gbox INSERTED && 'p'
	} // end of for cl->next
	fclose(fhandle);
	return;
}

void hostname2ip(char *hostname, IN_ADDR_T *ip)
{
	cs_resolve(hostname, ip, NULL, NULL);
}

static uint16_t gbox_convert_password_to_id(uint32_t password)
{
	return (((password >> 24) & 0xff) ^ ((password >> 8) & 0xff)) << 8 | (((password >> 16) & 0xff) ^ (password & 0xff));
}

static int8_t gbox_remove_all_bad_sids(struct gbox_peer *peer, ECM_REQUEST *er, uint16_t sid)
{
	if (!peer || !er) { return -1; }

	struct gbox_card *card = NULL;
	struct gbox_bad_srvid *srvid = NULL;
	struct gbox_card_pending *pending = NULL;
	LL_ITER it = ll_iter_create(er->gbox_cards_pending);
	while ((pending = ll_iter_next(&it)))
	{
		LL_ITER it2 = ll_iter_create(peer->gbox.cards);
		while((card = ll_iter_next(&it2)))
		{
			if(card->id.peer == pending->id.peer && card->id.slot == pending->id.slot)
			{
				LL_ITER it3 = ll_iter_create(card->badsids);
				while((srvid = ll_iter_next(&it3)))
				{
					if(srvid->srvid.sid == sid)
					{
						ll_iter_remove_data(&it3); // remove sid_ok from badsids
						break;
					}
				}
			}	
		}
	}
	return 0;
}

void gbox_add_good_card(struct s_client *cl, uint16_t id_card, uint16_t caid, uint8_t slot, uint16_t sid_ok, uint32_t cw_time)
{
	if (!cl || !cl->gbox) { return; }

	struct gbox_peer *peer = cl->gbox;
	struct gbox_card *card = NULL;
	struct gbox_good_srvid *srvid = NULL;
	uint8_t factor = 0;
	LL_ITER it = ll_iter_create(peer->gbox.cards);
	while((card = ll_iter_next(&it)))
	{
		if(card->id.peer == id_card && card->caid == caid && card->id.slot == slot)
		{
			card->no_cws_returned++;
			if (!card->no_cws_returned)
				{ card->no_cws_returned = 10; } //wrap around
			if (card->no_cws_returned < 10)
				{ factor = card->no_cws_returned; }
			else
				{ factor = 10; }	
				card->average_cw_time = ((card->average_cw_time * (factor-1)) + cw_time) / factor;				
			cl->reader->currenthops = card->dist;
			LL_ITER it2 = ll_iter_create(card->goodsids);
			while((srvid = ll_iter_next(&it2)))
			{
				if(srvid->srvid.sid == sid_ok)
				{
					srvid->last_cw_received = time(NULL);
					return; // sid_ok is already in the list of goodsids
				}
			}

			if(!cs_malloc(&srvid, sizeof(struct gbox_good_srvid)))
				{ return; }
			srvid->srvid.sid = sid_ok;
			srvid->srvid.provid_id = card->provid;
			srvid->last_cw_received = time(NULL);
			cs_log_dbg(D_READER, "Adding good SID: %04X for CAID: %04X Provider: %04X on CardID: %04X\n", sid_ok, caid, card->provid, id_card);
			ll_append(card->goodsids, srvid);
			break;
		}
	}//end of ll_iter_next
	//return dist_c;
}

void gbox_free_card(struct gbox_card *card)
{
	ll_destroy_data_NULL(card->badsids);
	ll_destroy_data_NULL(card->goodsids);
	add_garbage(card);
	return;
}

void gbox_free_cardlist(LLIST *card_list)
{
	if(card_list)
	{
		LL_ITER it = ll_iter_create(card_list);
		struct gbox_card *card;
		while((card = ll_iter_next_remove(&it)))
		{
			gbox_free_card(card);
		}
		ll_destroy_NULL(card_list);
	}
	return;
}

void gbox_init_ecm_request_ext(struct gbox_ecm_request_ext *ere)
{
/*	
	ere->gbox_crc = 0;
	ere->gbox_ecm_id = 0;
	ere->gbox_ecm_ok = 0;
*/	
	ere->gbox_hops = 0;
	ere->gbox_peer = 0;                		
	ere->gbox_mypeer = 0;
	ere->gbox_caid = 0;
	ere->gbox_prid = 0;
	ere->gbox_slot = 0;
	ere->gbox_version = 0;
	ere->gbox_unknown = 0;
	ere->gbox_type = 0;
}

struct s_client *get_gbox_proxy(uint16_t gbox_id)
{
	struct s_client *cl;
	for(cl = first_client; cl; cl = cl->next)
	{
		if(cl->typ == 'p' && cl->gbox && cl->gbox_peer_id == gbox_id)
			{ return cl; }
	}
	return NULL;
}

// if input client is typ proxy get client and vice versa
struct s_client *switch_client_proxy(struct s_client *cli, uint16_t gbox_id)
{
	if (!cli) { return cli; }
	
	struct s_client *cl;
	int8_t typ;
	if(cli->typ == 'c')
		{ typ = 'p'; }
	else
		{ typ = 'c'; }
	for(cl = first_client; cl; cl = cl->next)
	{
		if(cl->typ == typ && cl->gbox && cl->gbox_peer_id == gbox_id)
			{ return cl; }
	}
	return cli;
}

static int8_t gbox_reinit_peer(struct gbox_peer *peer)
{
	NULLFREE(peer->hostname);
	peer->online		= 0;
	peer->ecm_idx		= 0;
	peer->hello_stat	= GBOX_STAT_HELLOL;
	peer->next_hello	= 0;	
	gbox_free_cardlist(peer->gbox.cards);
	peer->gbox.cards	= ll_create("peer.cards");		
//	peer->my_user		= NULL;
	
	return 0;
}

static int8_t gbox_reinit_proxy(struct s_client *proxy)
{
	struct gbox_peer *peer = proxy->gbox;
	if (peer)
		{ gbox_reinit_peer(peer); }
	proxy->reader->tcp_connected	= 0;
	proxy->reader->card_status	= NO_CARD;
	proxy->reader->last_s		= proxy->reader->last_g = 0;

	return 0;
}

void gbox_reconnect_client(uint16_t gbox_id)
{
	struct s_client *cl;
	for(cl = first_client; cl; cl = cl->next)
	{
		if(cl->gbox && cl->typ == 'p' && cl->gbox_peer_id == gbox_id)
		{
			hostname2ip(cl->reader->device, &SIN_GET_ADDR(cl->udp_sa));
			SIN_GET_FAMILY(cl->udp_sa) = AF_INET;
			SIN_GET_PORT(cl->udp_sa) = htons((uint16_t)cl->reader->r_port);
			hostname2ip(cl->reader->device, &(cl->ip));
			gbox_reinit_proxy(cl);
			gbox_send_hello(cl);
		}
	}
}

static void *gbox_server(struct s_client *cli, uchar *UNUSED(b), int32_t l)
{
	if(l > 0)
	{
		cs_log("gbox_server %s/%d", cli->reader->label, cli->port);
//		gbox_check_header(cli, NULL, b, l);
	}
	return 0;
}

char *gbox_username(struct s_client *client)
{
	if(!client) { return "anonymous"; }
	if(client->reader)
		if(client->reader->r_usr[0])
		{
			return client->reader->r_usr;
		}
	return "anonymous";
}

static int8_t gbox_disconnect_double_peers(struct s_client *cli)
{
	struct s_client *cl;
	for(cl = first_client; cl; cl = cl->next)
	{
		if (cl->typ == 'c' && cl->gbox_peer_id == cli->gbox_peer_id && cl != cli)
		{
			cs_log_dbg(D_READER, "disconnected double client %s",username(cl));
			cs_disconnect_client(cl);		
		}
	}
	return 0;
}

static int8_t gbox_auth_client(struct s_client *cli, uint32_t gbox_password)
{
	if (!cli) { return -1; }

	uint16_t gbox_id = gbox_convert_password_to_id(gbox_password);
	struct s_client *cl = switch_client_proxy(cli, gbox_id);

	if(cl->typ == 'p' && cl->gbox && cl->reader)
	{
		struct gbox_peer *peer = cl->gbox;
		struct s_auth *account = get_account_by_name(gbox_username(cl));

		if ((peer->gbox.password == gbox_password) && account)
		{
			cli->crypted = 1; //display as crypted
			cli->gbox = cl->gbox; //point to the same gbox as proxy
			cli->reader = cl->reader; //point to the same reader as proxy
			cli->gbox_peer_id = cl->gbox_peer_id; //signal authenticated
			gbox_disconnect_double_peers(cli);
			cs_auth_client(cli, account, NULL);
			cli->account = account;
			cli->grp = account->grp;
			cli->lastecm = time(NULL);
			return 0;
		}
	}
	return -1;
}

static void gbox_server_init(struct s_client *cl)
{
	if(!cl->init_done)
	{
		if(IP_ISSET(cl->ip))
			{ cs_log("new connection from %s", cs_inet_ntoa(cl->ip)); }
		//We cannot authenticate here, because we don't know gbox pw
		cl->gbox_peer_id = NO_GBOX_ID;
		cl->init_done = 1;
	}
	return;
}

static uint16_t gbox_decode_cmd(uchar *buf)
{
	return buf[0] << 8 | buf[1];
}

int8_t gbox_message_header(uchar *buf, uint16_t cmd, uint32_t peer_password, uint32_t local_password)
{
	if (!buf) { return -1; }
	buf[0] = cmd >> 8;
	buf[1] = cmd & 0xff;
	if (cmd == MSG_GSMS_1) { return 0; }
	buf[2] = (peer_password >> 24) & 0xff;
	buf[3] = (peer_password >> 16) & 0xff;
	buf[4] = (peer_password >> 8) & 0xff;
	buf[5] = peer_password & 0xff;
	if (cmd == MSG_CW) { return 0; }	
	buf[6] = (local_password >> 24) & 0xff;
	buf[7] = (local_password >> 16) & 0xff;
	buf[8] = (local_password >> 8) & 0xff;
	buf[9] = local_password & 0xff;
	return 0;
}

int8_t get_card_action(struct gbox_card *card, uint32_t provid1, uint16_t peer_id, uint8_t slot, LLIST *cards)
{
	LL_ITER it;
	struct gbox_card *card_s;
	if (!card) { return 1; }	//insert
	if (card->provid_1 < provid1) { return -1; }	//remove
	if (card->id.peer == peer_id && card->provid_1 == provid1 && card->id.slot == slot)
		{ return 0; }	//keep
	else
	{ 
		it = ll_iter_create(cards);
		while ((card_s = ll_iter_next(&it)))
		{
			//card is still somewhere else we need to remove current
			if (card_s->id.peer == peer_id && card_s->provid_1 == provid1 && card_s->id.slot == slot)
				{ return -1; } //remove		
		}
		return 1; //insert
	}	
}

static int8_t gbox_init_card(struct gbox_card *card, uint16_t caid, uint32_t provid, uint32_t provid1, uint8_t *ptr)
{
	if (!card || !ptr) { return -1; }

	card->caid = caid;
	card->provid = provid;
	card->provid_1 = provid1;
	card->id.slot = ptr[0];
	card->dist = ptr[1] & 0xf;
	card->lvl = ptr[1] >> 4;
	card->id.peer = ptr[2] << 8 | ptr[3];
	card->badsids = ll_create("badsids");
	card->goodsids = ll_create("goodsids");
	card->no_cws_returned = 0;
	card->average_cw_time = 0;

	return 0;
}

//returns number of cards in a hello packet or -1 in case of error
int16_t read_cards_from_hello(uint8_t *ptr, uint8_t *len, CAIDTAB *ctab, uint8_t maxdist, LL_ITER *it, LLIST *cards, uint8_t last_packet)
{	
	uint8_t *current_ptr = 0;
	uint16_t caid;
	uint32_t provid;
	uint32_t provid1;
	struct gbox_card *card_s;
	struct gbox_card *card;
	int16_t ncards_in_msg = 0;
	LL_ITER *previous_it;

	while(ptr < len)
	{
		switch(ptr[0])
		{
			//Viaccess
		case 0x05:
			caid = ptr[0] << 8;
			provid =  ptr[1] << 16 | ptr[2] << 8 | ptr[3];
			break;
			//Cryptoworks
		case 0x0D:
			caid = ptr[0] << 8 | ptr[1];
			provid =  ptr[2];
			break;
		default:
			caid = ptr[0] << 8 | ptr[1];
			provid =  ptr[2] << 8 | ptr[3];
			break;
		}

		ncards_in_msg += ptr[4];
		//caid check
		if(chk_ctab(caid, ctab))
		{
			provid1 =  ptr[0] << 24 | ptr[1] << 16 | ptr[2] << 8 | ptr[3];
	
			current_ptr = ptr; 
			ptr += 5;

			// for all cards of current caid/provid,
			while (ptr < current_ptr + 5 + current_ptr[4] * 4)
			{
				if ((ptr[1] & 0xf) <= maxdist)
				{
					previous_it = it;
					card_s = ll_iter_next(it);
					switch (get_card_action(card_s, provid1, ptr[2] << 8 | ptr[3], ptr[0], cards))
					{
					case -1:
						//IDEA: Later put card to a list of temporary not available cards
						//reason: not loose good/bad sids
						//can be later removed by daily garbage collector for example
						cs_log_dbg(D_READER, "delete card: caid=%04X, provid=%06X, slot=%d, level=%d, dist=%d, peer=%04X",
									  card_s->caid, card_s->provid, card_s->id.slot, card_s->lvl, card_s->dist, card_s->id.peer);
						//delete card because not send anymore 
						ll_iter_remove(it);
						gbox_free_card(card_s);				
						break;
					case 0:
						ptr += 4;	
						break;
					case 1:	
						// create card info from data and add card to peer.cards
						if(!cs_malloc(&card, sizeof(struct gbox_card)))
						{ 
							cs_log("Can't allocate gbox card");
							continue;	
						}
						gbox_init_card(card,caid,provid,provid1,ptr);
						if (!card_s)
							{ ll_append(cards, card); }
						else
						{ 
							ll_iter_insert(previous_it, card); 
							it = previous_it;
						}
						ll_iter_next(it);
						cs_log_dbg(D_READER, "new card: caid=%04X, provid=%06X, slot=%d, level=%d, dist=%d, peer=%04X",
									  card->caid, card->provid, card->id.slot, card->lvl, card->dist, card->id.peer);			
						ptr += 4;
						break;
					default:
						ptr += 4;
						break;	
					}	//switch
				} // if <= maxdist
				else
					{ ptr += 4; }	//ignore because of maxdist
			} // end while cards for provider
		}
		else
			{ ptr += 5 + ptr[4] * 4; } //skip cards because caid
	} // end while caid/provid

	if (last_packet)
	{
		//delete cards at the end of the list if there are some
		while ((card_s = ll_iter_next(it)))
		{
			cs_log_dbg(D_READER, "delete card: caid=%04X, provid=%06X, slot=%d, level=%d, dist=%d, peer=%04X",
						  card_s->caid, card_s->provid, card_s->id.slot, card_s->lvl, card_s->dist, card_s->id.peer);
			//delete card because not send anymore 
			ll_iter_remove(it);
			gbox_free_card(card_s);									
		}
	}

	return ncards_in_msg;
}

int32_t gbox_cmd_hello(struct s_client *cli, uchar *data, int32_t n)
{
	if (!cli || !cli->gbox || !cli->reader || !data) { return -1; };

	struct gbox_peer *peer = cli->gbox;
	int16_t cards_number = 0;
	int32_t payload_len = n;
	int32_t hostname_len = 0;
	int32_t footer_len = 0;
	uint8_t *ptr = 0;
	LL_ITER it;

	if(!(gbox_decode_cmd(data) == MSG_HELLO1)) 
	{
		gbox_decompress(data, &payload_len);
		ptr = data + 12;
	}
	else
		{ ptr = data + 11; }		
	cs_log_dump_dbg(D_READER, data, payload_len, "decompressed data (%d bytes):", payload_len);

	if ((data[11] & 0xf) != peer->next_hello) //out of sync hellos
	{
		cs_log("-> out of sync hello from %s %s, expected: %02X, received: %02X"
			,username(cli), cli->reader->device, peer->next_hello, data[11] & 0xf);
		peer->next_hello = 0;
		peer->hello_stat = GBOX_STAT_HELLOL;
		gbox_send_hello(cli);
		return 0;
	}
	
	if (!(data[11] & 0xf)) //is first packet 
	{
		if(!peer->gbox.cards)
			{ peer->gbox.cards = ll_create("peer.cards"); }
		it = ll_iter_create(peer->gbox.cards);	
		hostname_len = data[payload_len - 1];
		footer_len = hostname_len + 2 + 7;
		if(!peer->hostname || memcmp(peer->hostname, data + payload_len - 1 - hostname_len, hostname_len))
		{	
			NULLFREE(peer->hostname);
			if(!cs_malloc(&peer->hostname, hostname_len + 1))
			{
				cs_writeunlock(&peer->lock);
				return -1;
			}
			memcpy(peer->hostname, data + payload_len - 1 - hostname_len, hostname_len);
			peer->hostname[hostname_len] = '\0';
		}
		gbox_checkcode_recv(cli, data + payload_len - footer_len - 1);
		peer->gbox.minor_version = data[payload_len - footer_len - 1 + 7];
		peer->gbox.cpu_api = data[payload_len - footer_len + 7];
		peer->total_cards = 0;
	}
	else
		{ it = peer->last_it; }

	cs_log_dbg(D_READER, "-> Hello packet no. %d received", (data[11] & 0xF) + 1);

	// read cards from hello
	cards_number = read_cards_from_hello(ptr, data + payload_len - footer_len - 1, &cli->reader->ctab, cli->reader->gbox_maxdist, &it, peer->gbox.cards, data[11] & 0x80);
	if (cards_number < 0)
		{ return -1; }
	else
		{ peer->total_cards += cards_number; }

	if(data[11] & 0x80)   //last packet
	{
		uchar tmpbuf[8];
		memset(&tmpbuf[0], 0xff, 7);		
		if(data[10] == 0x01 && !memcmp(data+12,tmpbuf,7)) //good night message
		{
			//This is a good night / reset packet (good night data[0xA] / reset !data[0xA] 
			cs_log("-> Good Night from %s %s",username(cli), cli->reader->device);
			write_goodnight_to_osd_file(cli);
			gbox_reinit_proxy(cli);
		}
		else	//last packet of Hello
		{
			peer->online = 1;
			if(!data[0xA])
			{
				cs_log("-> HelloS in %d packets from %s (%s:%d) V2.%02X with %d cards filtered to %d cards", (data[0x0B] & 0x0f)+1, cli->reader->label, cs_inet_ntoa(cli->ip), cli->reader->r_port, peer->gbox.minor_version, peer->total_cards, ll_count(peer->gbox.cards));
				peer->hello_stat = GBOX_STAT_HELLOR;
				gbox_send_hello(cli);
			}
			else
			{
				cs_log("-> HelloR in %d packets from %s (%s:%d) V2.%02X with %d cards filtered to %d cards", (data[0x0B] & 0x0f)+1, cli->reader->label, cs_inet_ntoa(cli->ip), cli->reader->r_port, peer->gbox.minor_version, peer->total_cards, ll_count(peer->gbox.cards));
				gbox_send_checkcode(cli);
			}
			if(peer->hello_stat == GBOX_STAT_HELLOS)
			{
				gbox_send_hello(cli);
			}
			cli->reader->tcp_connected = 2; //we have card
			if(ll_count(peer->gbox.cards) == 0)
				{ cli->reader->card_status = NO_CARD; }
			else	
				{ cli->reader->card_status = CARD_INSERTED; }
			
			peer->next_hello = 0;
			gbox_write_local_cards_info();
			gbox_write_shared_cards_info();
			gbox_write_peer_onl();
			cli->last = time((time_t *)0); //hello is activity on proxy
		}		
	}
	else { peer->next_hello++; }
	peer->last_it = it; //save position for next hello
	return 0;
}

static int8_t is_blocked_peer(uint16_t peer)
{
	if (peer == NO_GBOX_ID) { return 1; }
	else { return 0; }
}

static int8_t gbox_incoming_ecm(struct s_client *cli, uchar *data, int32_t n)
{
	if (!cli || !cli->gbox || !data || !cli->reader) { return -1; }

	struct gbox_peer *peer;
	struct s_client *cl;
	int32_t diffcheck = 0;

	peer = cli->gbox;
	if (!peer || !peer->my_user) { return -1; }
	cl = peer->my_user;

	// No ECMs with length < MIN_LENGTH expected
	if ((((data[19] & 0x0f) << 8) | data[20]) < MIN_ECM_LENGTH) { return -1; }

	// GBOX_MAX_HOPS not violated
	if (data[n - 15] + 1 > GBOX_MAXHOPS) { return -1; }

	// ECM must not take more hops than allowed by gbox_reshare
	if (data[n - 15] + 1 > cli->reader->gbox_reshare) 
	{
		cs_log("-> ECM took more hops than allowed from gbox_reshare. Hops stealing detected!");	
		return -1;
	}

	//Check for blocked peers
	uint16_t requesting_peer = data[(((data[19] & 0x0f) << 8) | data[20]) + 21] << 8 | 
				   data[(((data[19] & 0x0f) << 8) | data[20]) + 22];
	if (is_blocked_peer(requesting_peer)) 
	{ 
		cs_log_dbg(D_READER, "ECM from peer %04X blocked", requesting_peer);
		return -1;		
	}			   

	ECM_REQUEST *er;
	if(!(er = get_ecmtask())) { return -1; }

	struct gbox_ecm_request_ext *ere;
	if(!cs_malloc(&ere, sizeof(struct gbox_ecm_request_ext)))
	{
        	cs_writeunlock(&peer->lock);
              	return -1;
	}

	uchar *ecm = data + 18; //offset of ECM in gbx message

	er->src_data = ere;                
	gbox_init_ecm_request_ext(ere);

	er->gbox_ecm_id = peer->gbox.id;

	if(peer->ecm_idx == 100) { peer->ecm_idx = 0; }

	er->idx = peer->ecm_idx++;
	er->ecmlen = (((ecm[1] & 0x0f) << 8) | ecm[2]) + 3;

	er->pid = data[10] << 8 | data[11];
	er->srvid = data[12] << 8 | data[13];

	if(ecm[er->ecmlen + 5] == 0x05)
		{ er->caid = (ecm[er->ecmlen + 5] << 8); }
	else
		{ er->caid = (ecm[er->ecmlen + 5] << 8 | ecm[er->ecmlen + 6]); }

//	ei->extra = data[14] << 8 | data[15];
	memcpy(er->ecm, data + 18, er->ecmlen);
	ere->gbox_peer = ecm[er->ecmlen] << 8 | ecm[er->ecmlen + 1];
	ere->gbox_version = ecm[er->ecmlen + 2];
	ere->gbox_unknown = ecm[er->ecmlen + 3];
	ere->gbox_type = ecm[er->ecmlen + 4];
	ere->gbox_caid = ecm[er->ecmlen + 5] << 8 | ecm[er->ecmlen + 6];
	ere->gbox_prid = ecm[er->ecmlen + 7] << 8 | ecm[er->ecmlen + 8];
	ere->gbox_mypeer = ecm[er->ecmlen + 10] << 8 | ecm[er->ecmlen + 11];
	ere->gbox_slot = ecm[er->ecmlen + 12];

	diffcheck = gbox_checkcode_recv(cl, data + n - 14);
	//TODO: What do we do with our own checkcode @-7?
	er->gbox_crc = gbox_get_ecmchecksum(&er->ecm[0], er->ecmlen);
	ere->gbox_hops = data[n - 15] + 1;
	memcpy(&ere->gbox_routing_info[0], &data[n - 15 - ere->gbox_hops + 1], ere->gbox_hops - 1);

	er->prid = chk_provid(er->ecm, er->caid);
	cs_log_dbg(D_READER, "<- ECM (%d<-) from server (%s:%d) to cardserver (%04X) SID %04X", ere->gbox_hops, peer->hostname, cli->port, ere->gbox_peer, er->srvid);
	get_cw(cl, er);

	//checkcode did not match gbox->peer checkcode
	if(diffcheck)
	{
		//        TODO: Send HelloS here?
		//        gbox->peer.hello_stat = GBOX_STAT_HELLOS;
		//                gbox_send_hello(cli);
	}
	return 0;
}

int32_t gbox_cmd_switch(struct s_client *proxy, uchar *data, int32_t n)
{
	if (!data || !proxy) { return -1; }
	uint16_t cmd = gbox_decode_cmd(data);
	switch(cmd)
	{
	case MSG_BOXINFO:
		gbox_send_hello(proxy);
		break;
	case MSG_GOODBYE:
		cs_log("-> goodbye message from %s",username(proxy));	
		//needfix what to do after Goodbye?
		//suspect: we get goodbye as signal of SID not found
		break;
	case MSG_UNKNWN:
		cs_log("-> MSG_UNKNWN 48F9 from %s", username(proxy));	  
		break;
	case MSG_GSMS_1:
		if (!cfg.gsms_dis)
		{
			cs_log("-> MSG_GSMS_1 from %s", username(proxy));
			gbox_send_gsms_ack(proxy,1);
			write_gsms_msg(proxy, data +4, data[3], data[2]);
		}
		else
		{
			gsms_unavail();
		}
 		break;
	case MSG_GSMS_2:
		if (!cfg.gsms_dis)
		{
			cs_log("-> MSG_GSMS_2 from %s", username(proxy));
			gbox_send_gsms_ack(proxy,2);
			write_gsms_msg(proxy, data +16, data[14], data[15]);
		}
		else
		{
			gsms_unavail();
		}
		break;
	case MSG_GSMS_ACK_1:
		if (!cfg.gsms_dis)
		{
			cs_log("-> MSG_GSMS_ACK_1 from %s", username(proxy));
			write_gsms_ack(proxy,1);
		}
		else
		{
			gsms_unavail();
		}
		break;
	case MSG_GSMS_ACK_2:
		if (!cfg.gsms_dis)
		{
			cs_log("-> MSG_GSMS_ACK_2 from %s", username(proxy));
			write_gsms_ack(proxy,2);
		} 
		else
		{
			gsms_unavail();
		}
		break;
	case MSG_HELLO1:
	case MSG_HELLO:
		if (gbox_cmd_hello(proxy, data, n) < 0)
			{ return -1; }
		break;
	case MSG_CW:
		gbox_cw_received(proxy, data, n);
		break;
	case MSG_CHECKCODE:
		gbox_checkcode_recv(proxy, data + 10);
		break;
	case MSG_ECM:
		gbox_incoming_ecm(proxy, data, n);
		break;
	default:
		cs_log("-> unknown command %04X received from %s", cmd, username(proxy));
		cs_log_dump_dbg(D_READER, data, n, "unknown data received (%d bytes):", n);
	} // end switch
	if ((time(NULL) - last_stats_written) > STATS_WRITE_TIME)
	{ 
		gbox_write_stats();
		last_stats_written = time(NULL);
	}
	return 0;
}

static uint32_t gbox_get_pw(uchar *buf)
{
	return buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
}

//returns -1 in case of error, 1 if authentication was performed, 0 else
static int8_t gbox_check_header(struct s_client *cli, struct s_client *proxy, uchar *data, int32_t l)
{
	struct gbox_peer *peer = NULL;
	
	if (proxy) 
		{ peer = proxy->gbox; }

	char tmp[0x50];
	int32_t n = l;
	uint8_t authentication_done = 0;
	uint32_t my_received_pw = 0;
	uint32_t peer_received_pw = 0;
	cs_log_dump_dbg(D_READER, data, n, "-> encrypted data (%d bytes):", n);

	if(gbox_decode_cmd(data) == MSG_HELLO1)
		{ cs_log("test cs2gbox"); }
	else
		{ gbox_decrypt(data, n, local_gbox.password); }

	cs_log_dump_dbg(D_READER, data, n, "-> decrypted data (%d bytes):", n);
	//verify my pass received
	my_received_pw = gbox_get_pw(&data[2]);
	if (my_received_pw == local_gbox.password)
	{
		cs_log_dbg(D_READER, "-> data, peer : %04x   data: %s", cli->gbox_peer_id, cs_hexdump(0, data, l, tmp, sizeof(tmp)));

		if (gbox_decode_cmd(data) != MSG_CW)
		{
			if (cli->gbox_peer_id == NO_GBOX_ID)
			{
				if (gbox_auth_client(cli, gbox_get_pw(&data[6])) < 0)
				{ 
					cs_log_dbg(D_READER, "Authentication failed. Please check user in oscam.server and oscam.user");
					return -1;
				}
				//NEEDFIX: Pretty sure this should not be done here
				authentication_done = 1;
				gbox_local_cards(cli);	
				proxy = get_gbox_proxy(cli->gbox_peer_id);
				if (proxy) { peer = proxy->gbox; }
				if (peer) { peer->my_user = cli; }
			}
			if (!peer) { return -1; }
			peer_received_pw = gbox_get_pw(&data[6]);
			if (peer_received_pw != peer->gbox.password)
			{
				cs_log("gbox peer: %04X sends wrong password", peer->gbox.id);
				return -1;
				//continue; // next client
			}
		} else 
		{
			// if my pass ok verify CW | pass to peer
			if((data[39] != ((local_gbox.id >> 8) & 0xff)) || (data[40] != (local_gbox.id & 0xff))) 	
			{
				cs_log("gbox peer: %04X sends CW for other than my id: %04X", cli->gbox_peer_id, local_gbox.id);
				return -1;
				//continue; // next client
			}
		}
	}  // error my pass
	else if (gbox_decode_cmd(data) == MSG_GSMS_1 || gbox_decode_cmd(data) == MSG_GSMS_ACK_1 ) 
	{
		// MSG_GSMS_1 dont have passw and would fail. Just let them pass through for processing later
	}
	else
	{
		cs_log("ATTACK ALERT from IP %s", cs_inet_ntoa(cli->ip));
		cs_log("received data, data: %s", cs_hexdump(0, data, n, tmp, sizeof(tmp)));
		return -1;
		//continue; // next client
	}
	if (!proxy) { return -1; }

	if (!IP_EQUAL(cli->ip, proxy->ip))
	{ 
		cs_log("Received IP %s did not match previous IP %s. Try to reconnect.", cs_inet_ntoa(cli->ip), cs_inet_ntoa(proxy->ip));
		gbox_reconnect_client(cli->gbox_peer_id); 
		return -1;	
	}
	if(!peer) { return -1; }

	return authentication_done;
}

static void gbox_calc_checkcode(void)
{
	int32_t i = 0;
	struct s_client *cl;
	
	local_gbox.checkcode[0] = 0x15;
	local_gbox.checkcode[1] = 0x30;
	local_gbox.checkcode[2] = 0x02;
	local_gbox.checkcode[3] = 0x04;
	local_gbox.checkcode[4] = 0x19;
	local_gbox.checkcode[5] = 0x19;
	local_gbox.checkcode[6] = 0x66;

	LL_ITER it = ll_iter_create(local_gbox.cards);
	struct gbox_card *card;
	while((card = ll_iter_next(&it)))
	{
		local_gbox.checkcode[0] ^= (0xFF & (card->provid_1 >> 24));
		local_gbox.checkcode[1] ^= (0xFF & (card->provid_1 >> 16));
		local_gbox.checkcode[2] ^= (0xFF & (card->provid_1 >> 8));
		local_gbox.checkcode[3] ^= (0xFF & (card->provid_1));
		local_gbox.checkcode[4] ^= (0xFF & (card->id.slot));
		local_gbox.checkcode[5] ^= (0xFF & (card->id.peer >> 8));
		local_gbox.checkcode[6] ^= (0xFF & (card->id.peer));
	}
	for(i = 0, cl = first_client; cl; cl = cl->next, i++)
	{
		if (cl->gbox && cl->typ == 'p')
		{
			struct gbox_peer *peer = cl->gbox;
			it = ll_iter_create(peer->gbox.cards);
			while((card = ll_iter_next(&it)))
			{
				local_gbox.checkcode[0] ^= (0xFF & (card->provid_1 >> 24));
				local_gbox.checkcode[1] ^= (0xFF & (card->provid_1 >> 16));
				local_gbox.checkcode[2] ^= (0xFF & (card->provid_1 >> 8));
				local_gbox.checkcode[3] ^= (0xFF & (card->provid_1));
				local_gbox.checkcode[4] ^= (0xFF & (card->id.slot));
				local_gbox.checkcode[5] ^= (0xFF & (card->id.peer >> 8));
				local_gbox.checkcode[6] ^= (0xFF & (card->id.peer));
			}
		}	
	}
}

//returns 1 if checkcode changed / 0 if not
static int32_t gbox_checkcode_recv(struct s_client *cli, uchar *checkcode)
{
	struct gbox_peer *peer = cli->gbox;
	char tmp[14];

	if(memcmp(peer->gbox.checkcode, checkcode, 7))
	{
		memcpy(peer->gbox.checkcode, checkcode, 7);
		cs_log_dbg(D_READER, "-> new checkcode=%s",  cs_hexdump(0, peer->gbox.checkcode, 14, tmp, sizeof(tmp)));
		return 1;
	}
	return 0;
}

uint32_t gbox_get_ecmchecksum(uchar *ecm, uint16_t ecmlen)
{
	uint8_t checksum[4];
	int32_t counter;

	checksum[3] = ecm[0];
	checksum[2] = ecm[1];
	checksum[1] = ecm[2];
	checksum[0] = ecm[3];

	for(counter = 1; counter < (ecmlen / 4) - 4; counter++)
	{
		checksum[3] ^= ecm[counter * 4];
		checksum[2] ^= ecm[counter * 4 + 1];
		checksum[1] ^= ecm[counter * 4 + 2];
		checksum[0] ^= ecm[counter * 4 + 3];
	}

	return checksum[3] << 24 | checksum[2] << 16 | checksum[1] << 8 | checksum[0];
}

void gbox_send(struct s_client *cli, uchar *buf, int32_t l)
{
	struct gbox_peer *peer = cli->gbox;

	cs_log_dump_dbg(D_READER, buf, l, "<- decrypted data (%d bytes):", l);

	hostname2ip(cli->reader->device, &SIN_GET_ADDR(cli->udp_sa));
	SIN_GET_FAMILY(cli->udp_sa) = AF_INET;
	SIN_GET_PORT(cli->udp_sa) = htons((uint16_t)cli->reader->r_port);

	gbox_encrypt(buf, l, peer->gbox.password);
	sendto(cli->udp_fd, buf, l, 0, (struct sockaddr *)&cli->udp_sa, cli->udp_sa_len);
	cs_log_dump_dbg(D_READER, buf, l, "<- encrypted data (%d bytes):", l);
}

static void gbox_send_hello_packet(struct s_client *cli, int8_t number, uchar *outbuf, uchar *ptr, int32_t nbcards)
{
	struct gbox_peer *peer = cli->gbox;
	int32_t hostname_len = strlen(cfg.gbox_hostname);
	int32_t len;
	gbox_message_header(outbuf, MSG_HELLO, peer->gbox.password, local_gbox.password);
	// initial HELLO = 0, subsequent = 1
	if(peer->hello_stat > GBOX_STAT_HELLOS)
		{ outbuf[10] = 1; }
	else
		{ outbuf[10] = 0; }
	outbuf[11] = number;    // 0x80 (if last packet) else 0x00 | packet number

	if((number & 0x0F) == 0)
	{
		gbox_calc_checkcode();
		if(peer->hello_stat != GBOX_STAT_HELLOL)
			{ memcpy(++ptr, local_gbox.checkcode, 7); }
		else	
			{ memset(++ptr, 0, 7); }		
		ptr += 7;
		*ptr = local_gbox.minor_version;
		*(++ptr) = local_gbox.cpu_api;
		memcpy(++ptr, cfg.gbox_hostname, hostname_len);
		ptr += hostname_len;
		*ptr = hostname_len;
	}
	len = ptr - outbuf + 1;
	switch(peer->hello_stat)
	{
	case GBOX_STAT_HELLOL:
		cs_log("<- HelloL to %s", cli->reader->label);
		if((number & 0x80) == 0x80)
			{ peer->hello_stat = GBOX_STAT_HELLOS; }
		break;
	case GBOX_STAT_HELLOS:
		cs_log("<- HelloS total cards  %d to %s", nbcards, cli->reader->label);
		if((number & 0x80) == 0x80)
			{ peer->hello_stat = GBOX_STAT_HELLO3; }
		break;
	case GBOX_STAT_HELLOR:
		cs_log("<- HelloR total cards  %d to %s", nbcards, cli->reader->label);
		if((number & 0x80) == 0x80)
			{ peer->hello_stat = GBOX_STAT_HELLO3; }
		break;
	default:
		cs_log("<- hello total cards  %d to %s", nbcards, cli->reader->label);
		break;
	}
	cs_log_dump_dbg(D_READER, outbuf, len, "<- hello, (len=%d):", len);

	gbox_compress(outbuf, len, &len);

	gbox_send(cli, outbuf, len);
}

static void gbox_send_hello(struct s_client *cli)
{
	struct gbox_peer *peer = cli->gbox;

	int32_t nbcards = 0;
	int32_t packet;
	uchar buf[2048];
	/*
	  int32_t ok = 0;
	#ifdef WEBIF
	  ok = check_ip(cfg.http_allowed, cli->ip) ? 1 : 0;
	#endif
	*/
	packet = 0;
	uchar *ptr = buf + 11;
	if(ll_count(local_gbox.cards) != 0 && peer->hello_stat > GBOX_STAT_HELLOL)
	{
		memset(buf, 0, sizeof(buf));

		LL_ITER it = ll_iter_create(local_gbox.cards);
		struct gbox_card *card;
		while((card = ll_iter_next(&it)))
		{
			//send to user only cards which matching CAID from account and lvl > 0
			if(chk_ctab(card->caid, &peer->my_user->account->ctab) && card->lvl > 0)
			{
				*(++ptr) = card->provid_1 >> 24;
				*(++ptr) = card->provid_1 >> 16;
				*(++ptr) = card->provid_1 >> 8;
				*(++ptr) = card->provid_1 & 0xff;
				*(++ptr) = 1;       //note: original gbx is more efficient and sends all cards of one caid as package
				*(++ptr) = card->id.slot;
				//If you modify the next line you are going to destroy the community
				//It will be recognized by original gbx and you will get banned
				*(++ptr) = ((card->lvl - 1) << 4) + card->dist + 1;
				*(++ptr) = card->id.peer >> 8;
				*(++ptr) = card->id.peer & 0xff;
				nbcards++;
				if(nbcards == 100)    //check if 100 is good or we need more sophisticated algorithm
				{
					gbox_send_hello_packet(cli, packet, buf, ptr, nbcards);
					packet++;
					nbcards = 0;
					ptr = buf + 11;
					memset(buf, 0, sizeof(buf));
				}
			}
		}
	} // end if local card exists

	//last packet has bit 0x80 set
	gbox_send_hello_packet(cli, 0x80 | packet, buf, ptr, nbcards);
}

static void gbox_send_checkcode(struct s_client *cli)
{
	struct gbox_peer *peer = cli->gbox;
	uchar outbuf[20];

	gbox_calc_checkcode();
	gbox_message_header(outbuf, MSG_CHECKCODE, peer->gbox.password, local_gbox.password);
	memcpy(outbuf + 10, local_gbox.checkcode, 7);

	gbox_send(cli, outbuf, 17);
}
/*
static void gbox_send_boxinfo(struct s_client *cli)
{
	struct gbox_peer *peer = cli->gbox;
	uchar outbuf[256];
	int32_t hostname_len = strlen(cfg.gbox_hostname);

	gbox_message_header(outbuf, MSG_BOXINFO, peer);
	outbuf[0xA] = local_gbox.minor_version;
	outbuf[0xB] = local_gbox.type;
	memcpy(&outbuf[0xC], cfg.gbox_hostname, hostname_len);
	gbox_send(cli, outbuf, hostname_len + 0xC);
}
*/
static int32_t gbox_recv(struct s_client *cli, uchar *buf, int32_t l)
{
	uchar data[RECEIVE_BUFFER_SIZE];
	int32_t n = l;
	int8_t ret = 0;

	if(!cli->udp_fd || !cli->is_udp || cli->typ != 'c')
		{ return -1; }
	
	n = recv_from_udpipe(buf);		
	if (n < MIN_GBOX_MESSAGE_LENGTH || n >= RECEIVE_BUFFER_SIZE) //protect against too short or too long messages
		{ return -1; }
			
	struct s_client *proxy = get_gbox_proxy(cli->gbox_peer_id);
			
	memcpy(&data[0], buf, n);
	ret = gbox_check_header(cli, proxy, &data[0], n);
	if (ret < 0)
		{ return -1; }
	
	//in case of new authentication the proxy gbox can now be found 
	if (ret)
		{ proxy = get_gbox_proxy(cli->gbox_peer_id); } 	

	if (!proxy)
		{ return -1; }	
		
	cli->last = time((time_t *)0);
	//clients may timeout - attach to peer's gbox/reader
	cli->gbox = proxy->gbox; //point to the same gbox as proxy
	cli->reader = proxy->reader; //point to the same reader as proxy
	struct gbox_peer *peer = proxy->gbox;
				
	cs_writelock(&peer->lock);
	if(gbox_cmd_switch(proxy, data, n) < 0)
		{ return -1; }
	cs_writeunlock(&peer->lock);
				
	//clients may timeout - dettach from peer's gbox/reader
	cli->gbox = NULL;
	cli->reader = NULL;
	return 0;	
}

static void gbox_send_dcw(struct s_client *cl, ECM_REQUEST *er)
{
	if (!cl || !er) { return; }

	struct s_client *cli = switch_client_proxy(cl, cl->gbox_peer_id);
	if (!cli->gbox) { return; }
	struct gbox_peer *peer = cli->gbox;

	if(er->rc >= E_NOTFOUND)
	{
		cs_log_dbg(D_READER, "unable to decode!");
		return;
	}

	uchar buf[60];
	memset(buf, 0, sizeof(buf));

	struct gbox_ecm_request_ext *ere = er->src_data;

	gbox_message_header(buf, MSG_CW , peer->gbox.password, 0);
	buf[6] = er->pid >> 8;			//PID
	buf[7] = er->pid & 0xff;		//PID
	buf[8] = er->srvid >> 8;		//SrvID
	buf[9] = er->srvid & 0xff;		//SrvID
	buf[10] = ere->gbox_mypeer >> 8;	//From peer
	buf[11] = ere->gbox_mypeer & 0xff;	//From peer
	buf[12] = (ere->gbox_slot << 4) | (er->ecm[0] & 0x0f); //slot << 4 | even/odd
	buf[13] = ere->gbox_caid >> 8;		//CAID first byte
	memcpy(buf + 14, er->cw, 16);		//CW
	buf[30] = er->gbox_crc >> 24;		//CRC
	buf[31] = er->gbox_crc >> 16;		//CRC
	buf[32] = er->gbox_crc >> 8;		//CRC
	buf[33] = er->gbox_crc & 0xff;		//CRC
	buf[34] = ere->gbox_caid >> 8;		//CAID
	buf[35] = ere->gbox_caid & 0xff;	//CAID
	buf[36] = ere->gbox_slot;  		//Slot
	if (buf[34] == 0x06)			//if irdeto
	{
		buf[37] = er->chid >> 8;	//CHID
		buf[38] = er->chid & 0xff;	//CHID
	}
	else
	{
		if (local_gbox.minor_version == 0x2A)
		{
			buf[37] = 0xff;		//gbox.net sends 0xff
			buf[38] = 0xff;		//gbox.net sends 0xff
		}
		else
		{
			buf[37] = 0;		//gbox sends 0
			buf[38] = 0;		//gbox sends 0
		}	
	}
	buf[39] = ere->gbox_peer >> 8;		//Target peer
	buf[40] = ere->gbox_peer & 0xff;	//Target peer
	if (er->rc == E_CACHE1 || er->rc == E_CACHE2 || er->rc == E_CACHEEX)
		{ buf[41] = 0x03; }		//cache
	else
		{ buf[41] = 0x01; }		//card, emu, needs probably further investigation
	buf[42] = 0x30;				//1st nibble unknown / 2nd nibble distance
	buf[43] = ere->gbox_unknown;		//meaning unknown, copied from ECM request

	//This copies the routing info from ECM to answer.
	//Each hop adds one byte and number of hops is in er->gbox_hops.
	memcpy(&buf[44], &ere->gbox_routing_info, ere->gbox_hops - 1);
	buf[44 + ere->gbox_hops - 1] = ere->gbox_hops - 1;	//Hops 
	/*
	char tmp[0x50];
	cs_log("sending dcw to peer : %04x   data: %s", er->gbox_peer, cs_hexdump(0, buf, er->gbox_hops + 44, tmp, sizeof(tmp)));
	*/
	gbox_send(cli, buf, ere->gbox_hops + 44);

	cs_log_dbg(D_READER, "<- CW (distance: %d) from %s/%d (%04X) ", ere->gbox_hops, cli->reader->label, cli->port, ere->gbox_peer);
}

static uint8_t gbox_next_free_slot(uint16_t id)
{
	LL_ITER it = ll_iter_create(local_gbox.cards);
	struct gbox_card *c;
	uint8_t lastslot = 0;

	while((c = ll_iter_next(&it)))
	{
		if(id == c->id.peer && c->id.slot > lastslot)
			{ lastslot = c->id.slot; }
	}
	return ++lastslot;
}

static void gbox_add_local_card(uint16_t id, uint16_t caid, uint32_t prid, uint8_t slot, uint8_t card_reshare, uint8_t dist, uint8_t type)
{
	struct gbox_card *c;

	//don't insert 0100:000000
	if((caid >> 8 == 0x01) && (!prid))
	{
		return;
	}
	//skip CAID 18XX providers
	if((caid >> 8 == 0x18) && (prid))
	{
		return;
	}
	if(!cs_malloc(&c, sizeof(struct gbox_card)))
	{
		return;
	}
	c->caid = caid;
	switch(caid >> 8)
	{
		// Viaccess
	case 0x05:
		c->provid_1 = (caid >> 8) << 24 | (prid & 0xFFFFFF);
		break;
		// Cryptoworks
	case 0x0D:
		c->provid_1 = (caid >> 8) << 24 | (caid & 0xFF) << 16 |
					  ((prid << 8) & 0xFF00);
		break;
	default:
		c->provid_1 = (caid >> 8) << 24 | (caid & 0xFF) << 16 |
					  (prid & 0xFFFF);
		break;
	}
	c->provid = prid;
	c->id.peer = id;
	c->id.slot = slot;
	c->lvl = card_reshare;
	c->dist = dist;
	c->type = type;
	ll_append(local_gbox.cards, c);
}

static void gbox_local_cards(struct s_client *cli)
{
	int32_t i;
	uint32_t prid = 0;
	int8_t slot = 0;
#ifdef MODULE_CCCAM
	LL_ITER it, it2;
	struct cc_card *card = NULL;
	struct cc_data *cc;
	uint32_t checksum = 0;
	uint16_t cc_peer_id = 0;
	struct cc_provider *provider;
	uint8_t *node1 = NULL;
	uint8_t min_reshare = 0;
#endif

	if(!local_gbox.cards)
		{ gbox_free_cardlist(local_gbox.cards); }
	local_gbox.cards = ll_create("local_cards");

	struct s_client *cl;
	for(cl = first_client; cl; cl = cl->next)
	{
		if(cl->typ == 'r' && cl->reader && cl->reader->card_status == 2)
		{
			slot = gbox_next_free_slot(local_gbox.id);
			//SECA, Viaccess and Cryptoworks have multiple providers
			if((cl->reader->caid >> 8 == 0x01) || (cl->reader->caid >> 8 == 0x05) ||
					(cl->reader->caid >> 8 == 0x0D))
			{
				for(i = 0; i < cl->reader->nprov; i++)
				{
					prid = cl->reader->prid[i][1] << 16 |
						   cl->reader->prid[i][2] << 8 | cl->reader->prid[i][3];
					gbox_add_local_card(local_gbox.id, cl->reader->caid, prid, slot, cli->reader->gbox_reshare, 0, 1);
				}
			}
			else
			{ 
				gbox_add_local_card(local_gbox.id, cl->reader->caid, 0, slot, cli->reader->gbox_reshare, 0, 1); 
				
				//Check for Betatunnel on gbox account in oscam.user
				if (chk_is_betatunnel_caid(cl->reader->caid) == 1 && cli->ttab.n && cl->reader->caid == cli->ttab.bt_caidto[0])
					{
						//For now only first entry in tunnel tab. No sense in iteration?
						//Add betatunnel card to transmitted list
						gbox_add_local_card(local_gbox.id, cli->ttab.bt_caidfrom[0], 0, slot, cli->reader->gbox_reshare, 0, 2);
						cs_log_dbg(D_READER, "gbox created betatunnel card for caid: %04X->%04X",cli->ttab.bt_caidfrom[0],cl->reader->caid);
					}
			}
		}   //end local readers
#ifdef MODULE_CCCAM
		if((cfg.cc_reshare > -1) && cl->typ == 'p' && cl->reader && cl->reader->typ == R_CCCAM && cl->cc)
		{
			cc = cl->cc;
			it = ll_iter_create(cc->cards);
			while((card = ll_iter_next(&it)))
			{
				//calculate gbox id from cc node
				//1st node is orgin, shorten to 32Bit by CRC, the GBX-ID like from PW
				node1 = ll_has_elements(card->remote_nodes);
				checksum = (uint32_t)crc32(0L, node1, 8);
				cc_peer_id = ((((checksum >> 24) & 0xFF) ^((checksum >> 8) & 0xFF)) << 8 |
							  (((checksum >> 16) & 0xFF) ^(checksum & 0xFF)));
				slot = gbox_next_free_slot(cc_peer_id);
				min_reshare = cfg.cc_reshare;
				if (card->reshare < min_reshare)
					{ min_reshare = card->reshare; }
				min_reshare++; //strange CCCam logic. 0 means direct peers
				if((card->caid >> 8 == 0x01) || (card->caid >> 8 == 0x05) ||
						(card->caid >> 8 == 0x0D))
				{
					it2 = ll_iter_create(card->providers);
					while((provider = ll_iter_next(&it2)))
						{ gbox_add_local_card(cc_peer_id, card->caid, provider->prov, slot, min_reshare, card->hop, 3); }
				}
				else
					{ gbox_add_local_card(cc_peer_id, card->caid, 0, slot, min_reshare, card->hop, 3); }
			}
		}   //end cccam
#endif
	} //end for clients

	if (cfg.gbox_proxy_cards_num > 0) 
	{ 
		for (i = 0; i < cfg.gbox_proxy_cards_num; i++) 
		{
			if ((cfg.gbox_proxy_card[i] >> 24) == 0x05)
			{ 
				slot = gbox_next_free_slot(local_gbox.id);
				gbox_add_local_card(local_gbox.id, (cfg.gbox_proxy_card[i] >> 16) & 0xFFF0, cfg.gbox_proxy_card[i] & 0xFFFFF, slot, cli->reader->gbox_reshare, 0, 4);
				cs_log_dbg(D_READER,"add proxy card:  slot %d %04lX:%06lX",slot, (cfg.gbox_proxy_card[i] >> 16) & 0xFFF0, cfg.gbox_proxy_card[i] & 0xFFFFF);
			}	
			else 
			{	
				slot = gbox_next_free_slot(local_gbox.id);
				gbox_add_local_card(local_gbox.id, cfg.gbox_proxy_card[i] >> 16, cfg.gbox_proxy_card[i] & 0xFFFF, slot, cli->reader->gbox_reshare, 0, 4);
				cs_log_dbg(D_READER,"add proxy card:  slot %d %04lX:%06lX",slot, cfg.gbox_proxy_card[i] >> 16, cfg.gbox_proxy_card[i] & 0xFFFF);
			}
		}
	}  // end add proxy reader cards 
} //end add local gbox cards

static int32_t gbox_client_init(struct s_client *cli)
{
	if(!cfg.gbx_port[0] || cfg.gbx_port[0] > 65535)
	{
		cs_log("error, no/invalid port=%d configured in oscam.conf!",
			   cfg.gbx_port[0] ? cfg.gbx_port[0] : 0);
		return -1;
	}
	
	if(!cfg.gbox_hostname || strlen(cfg.gbox_hostname) > 128)
	{
		cs_log("error, no/invalid hostname '%s' configured in oscam.conf!",
			   cfg.gbox_hostname ? cfg.gbox_hostname : "");
		return -1;
	}

	if(!local_gbox.id)
	{
		cs_log("error, no/invalid password '%s' configured in oscam.conf!",
			   cfg.gbox_my_password ? cfg.gbox_my_password : "");
		return -1;
	}

	if(!cs_malloc(&cli->gbox, sizeof(struct gbox_peer)))
		{ return -1; }

	struct gbox_peer *peer = cli->gbox;
	struct s_reader *rdr = cli->reader;

	rdr->card_status = CARD_NEED_INIT;
	rdr->tcp_connected = 0;

	memset(peer, 0, sizeof(struct gbox_peer));

	peer->gbox.password = a2i(rdr->r_pwd, 4);
	cs_log_dbg(D_READER, "gbox peer password: %s:", rdr->r_pwd);

	peer->gbox.id = gbox_convert_password_to_id(peer->gbox.password);	
	if (get_gbox_proxy(peer->gbox.id) || peer->gbox.id == NO_GBOX_ID || peer->gbox.id == local_gbox.id)
	{
		cs_log("error, double/invalid gbox id: %04X", peer->gbox.id);	
		return -1;
	}
	cli->gbox_peer_id = peer->gbox.id;	

	cli->pfd = 0;
	cli->crypted = 1;

	set_null_ip(&cli->ip);

	if((cli->udp_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	{
		cs_log("socket creation failed (errno=%d %s)", errno, strerror(errno));
		cs_disconnect_client(cli);
	}

	int32_t opt = 1;
	setsockopt(cli->udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	set_so_reuseport(cli->udp_fd);

	set_socket_priority(cli->udp_fd, cfg.netprio);

	memset((char *)&cli->udp_sa, 0, sizeof(cli->udp_sa));

	if(!hostResolve(rdr))
		{ return 0; }

	cli->port = rdr->r_port;
	SIN_GET_FAMILY(cli->udp_sa) = AF_INET;
	SIN_GET_PORT(cli->udp_sa) = htons((uint16_t)rdr->r_port);
	hostname2ip(cli->reader->device, &SIN_GET_ADDR(cli->udp_sa));

	cs_log("proxy %s (fd=%d, peer id=%04X, my id=%04X, my hostname=%s, peer's listen port=%d)",
		   rdr->device, cli->udp_fd, peer->gbox.id, local_gbox.id, cfg.gbox_hostname, rdr->r_port);

	cli->pfd = cli->udp_fd;

	cs_lock_create(&peer->lock, "gbox_lock", 5000);

	gbox_reinit_peer(peer);

	cli->reader->card_status = CARD_NEED_INIT;
	gbox_send_hello(cli);

	if(!cli->reader->gbox_maxecmsend)
		{ cli->reader->gbox_maxecmsend = DEFAULT_GBOX_MAX_ECM_SEND; }

	if(!cli->reader->gbox_maxdist)
		{ cli->reader->gbox_maxdist = DEFAULT_GBOX_MAX_DIST; }

	//value > 5 not allowed in gbox network
	if(!cli->reader->gbox_reshare || cli->reader->gbox_reshare > 5)
		{ cli->reader->gbox_reshare = 5; }

	return 0;
}

static uint32_t gbox_get_pending_time(ECM_REQUEST *er, uint16_t peer_id, uint8_t slot)
{
	if (!er) { return 0; }
	
	struct gbox_card_pending *pending = NULL;
	LL_ITER it = ll_iter_create(er->gbox_cards_pending);
	while ((pending = ll_iter_next(&it)))
	{
		if ((pending->id.peer == peer_id) && (pending->id.slot == slot))
			{ return pending->pending_time; }
	}
	
	return 0;
}

static int32_t gbox_recv_chk(struct s_client *cli, uchar *dcw, int32_t *rc, uchar *data, int32_t n)
{
    if(gbox_decode_cmd(data) == MSG_CW && n > 43)
    {
        int i;
        uint16_t id_card = 0;
        struct s_client *proxy;
        if(cli->typ != 'p')
        	{ proxy = switch_client_proxy(cli, cli->gbox_peer_id); }
        else
        	{ proxy = cli; }
        proxy->last = time((time_t *)0);
        *rc = 1;
        memcpy(dcw, data + 14, 16);
        uint32_t crc = data[30] << 24 | data[31] << 16 | data[32] << 8 | data[33];
        char tmp[32];
        cs_log_dbg(D_READER, "-> cws=%s, peer=%04X, ecm_pid=%04X, sid=%04X, crc=%08X, type=%02X, dist=%01X, unkn1=%01X, unkn2=%02X, chid/0x0000/0xffff=%04X",
                      cs_hexdump(0, dcw, 32, tmp, sizeof(tmp)), 
                      data[10] << 8 | data[11], data[6] << 8 | data[7], data[8] << 8 | data[9], crc, data[41], data[42] & 0x0f, data[42] >> 4, data[43], data[37] << 8 | data[38]);
        struct timeb t_now;             
        cs_ftime(&t_now);
        int64_t cw_time = GBOX_DEFAULT_CW_TIME;
        for(i = 0; i < cfg.max_pending; i++)
        {
        	if(proxy->ecmtask[i].gbox_crc == crc)
        	{
        		id_card = data[10] << 8 | data[11];
        		cw_time = comp_timeb(&t_now, &proxy->ecmtask[i].tps) - gbox_get_pending_time(&proxy->ecmtask[i], id_card, data[36]);
        		gbox_add_good_card(proxy, id_card, proxy->ecmtask[i].caid, data[36], proxy->ecmtask[i].srvid, cw_time);
        		gbox_remove_all_bad_sids(proxy->gbox, &proxy->ecmtask[i], proxy->ecmtask[i].srvid);
        		if(proxy->ecmtask[i].gbox_ecm_status == GBOX_ECM_NOT_ASKED || proxy->ecmtask[i].gbox_ecm_status == GBOX_ECM_ANSWERED)
        			{ return -1; }
			proxy->ecmtask[i].gbox_ecm_status = GBOX_ECM_ANSWERED;
			proxy->ecmtask[i].gbox_ecm_id = id_card;
			*rc = 1;
			return proxy->ecmtask[i].idx;
		}
	}
        //late answers from other peers,timing not possible
        gbox_add_good_card(proxy, id_card, data[34] << 8 | data[35], data[36], data[8] << 8 | data[9], GBOX_DEFAULT_CW_TIME);
        cs_log_dbg(D_READER, "no task found for crc=%08x", crc);
    }
    return -1;
}

static int8_t gbox_cw_received(struct s_client *cli, uchar *data, int32_t n)
{
	int32_t rc = 0, i = 0, idx = 0;
	uchar dcw[16];
	
	idx = gbox_recv_chk(cli, dcw, &rc, data, n);
	if(idx < 0) { return -1; }  // no dcw received
	if(!idx) { idx = cli->last_idx; }
	cli->reader->last_g = time((time_t *)0); // for reconnect timeout
	for(i = 0; i < cfg.max_pending; i++)
	{
		if(cli->ecmtask[i].idx == idx)
		{
                	cli->pending--;
                        casc_check_dcw(cli->reader, i, rc, dcw);
                        return 0;
		}
	}
	return -1;
}

void *gbox_rebroadcast_thread(struct gbox_rbc_thread_args *args)
{
	if (!args) { return NULL; }
	
	struct s_client *cli = args->cli;
	ECM_REQUEST *er = args->er;
	uint32_t waittime = args->waittime;

	if (!cli) { return NULL; }
	pthread_setspecific(getclient, cli);
	set_thread_name(__func__);

	cs_sleepms(waittime);
	if (!cli || !cli->gbox || !er) { return NULL; }

	struct gbox_peer *peer = cli->gbox;

	//ecm is not answered yet
	if (er->rc >= E_NOTFOUND)
	{
 		cs_writelock(&peer->lock);
		gbox_send_ecm(cli, er, NULL);
 		cs_writeunlock(&peer->lock);
	}

	return NULL;
}

static int8_t is_already_pending(ECM_REQUEST *er, struct gbox_card_id *searched_id)
{
	if (!er || !searched_id)
		{ return -1; }
		
	LL_ITER it = ll_iter_create(er->gbox_cards_pending);
	struct gbox_card_id *current_id;
	while ((current_id = ll_iter_next(&it)))
	{
		if (current_id->peer == searched_id->peer &&
			current_id->slot == searched_id->slot)
			{ return 1; }
	}
	
	return 0;	
}

static int32_t gbox_send_ecm(struct s_client *cli, ECM_REQUEST *er, uchar *UNUSED(buf))
{
	if(!cli || !er || !cli->reader)
		{ return -1; }

	if(!cli->gbox || !cli->reader->tcp_connected)
	{
		cs_log_dbg(D_READER, "%s server not init!", cli->reader->label);
		write_ecm_answer(cli->reader, er, E_NOTFOUND, 0x27, NULL, NULL);
		return -1;
	}

	struct gbox_peer *peer = cli->gbox;
	int32_t cont_1;
	uint32_t sid_verified = 0;

	if(!ll_count(peer->gbox.cards))
	{
		cs_log_dbg(D_READER, "%s NO CARDS!", cli->reader->label);
		write_ecm_answer(cli->reader, er, E_NOTFOUND, E2_CCCAM_NOCARD, NULL, NULL);
		return -1;
	}

	if(!peer->online)
	{
		cs_log_dbg(D_READER, "peer is OFFLINE!");
		write_ecm_answer(cli->reader, er, E_NOTFOUND, 0x27, NULL, NULL);
		//      gbox_send_hello(cli,0);
		return -1;
	}

	if(er->gbox_ecm_status == GBOX_ECM_ANSWERED)
		{ cs_log_dbg(D_READER, "%s replied to this ecm already", cli->reader->label); }

	if(er->gbox_ecm_status == GBOX_ECM_NOT_ASKED)
		{ er->gbox_cards_pending = ll_create("pending_gbox_cards"); }

	if(er->gbox_ecm_id == peer->gbox.id)
	{
		cs_log_dbg(D_READER, "%s provided ecm", cli->reader->label);
		write_ecm_answer(cli->reader, er, E_NOTFOUND, 0x27, NULL, NULL);
		return 0;
	}

	uint16_t ercaid = er->caid;
	uint32_t erprid = er->prid;

	switch(ercaid >> 8)
	{
		//Viaccess
	case 0x05:
		ercaid = (ercaid & 0xFF00) | ((erprid >> 16) & 0xFF);
		erprid = erprid & 0xFFFF;
		break;
		//Cryptoworks
	case 0x0D:
		erprid = erprid << 8;
		break;
		//Nagra
	case 0x18:
		erprid = 0;
		break;
	}

	uchar send_buf_1[1024];
	int32_t len2;

	if(!er->ecmlen) { return 0; }

	len2 = er->ecmlen + 18;
	er->gbox_crc = gbox_get_ecmchecksum(&er->ecm[0], er->ecmlen);

	memset(send_buf_1, 0, sizeof(send_buf_1));

	LL_ITER it = ll_iter_create(peer->gbox.cards);
	LL_ITER it2;
	struct gbox_card *card;

	int32_t cont_send = 0;
	uint32_t cont_card_1 = 0;
	uint8_t max_ecm_reached = 0;

	gbox_message_header(send_buf_1, MSG_ECM , peer->gbox.password, local_gbox.password);

	send_buf_1[10] = (er->pid >> 8) & 0xFF;
	send_buf_1[11] = er->pid & 0xFF;

	send_buf_1[12] = (er->srvid >> 8) & 0xFF;
	send_buf_1[13] = er->srvid & 0xFF;
	send_buf_1[14] = 0x00;
	send_buf_1[15] = 0x00;

	send_buf_1[16] = cont_card_1;
	send_buf_1[17] = 0x00;

	memcpy(send_buf_1 + 18, er->ecm, er->ecmlen);

	send_buf_1[len2]   = (local_gbox.id >> 8) & 0xff;
	send_buf_1[len2 + 1] = local_gbox.id & 0xff;
	send_buf_1[len2 + 2] = gbox_get_my_vers();
	send_buf_1[len2 + 3] = 0x00;
	send_buf_1[len2 + 4] = gbox_get_my_cpu_api();

	send_buf_1[len2 + 5] = ercaid >> 8;
	send_buf_1[len2 + 6] = ercaid & 0xFF;

	send_buf_1[len2 + 7] = (erprid >> 8) & 0xFF;
	send_buf_1[len2 + 8] = erprid & 0xFF;
	send_buf_1[len2 + 9] = 0x00;
	cont_1 = len2 + 10;

	struct gbox_good_srvid *srvid_good = NULL;
	struct gbox_bad_srvid *srvid_bad = NULL;
	struct gbox_card_id current_id;
	uint8_t enough = 0;
	time_t time_since_lastcw;
	uint32_t current_avg_card_time = 0;
	//loop over good only
	while((card = ll_iter_next(&it)))
	{
		current_id.peer = card->id.peer;
		current_id.slot = card->id.slot;					
		if(card->caid == er->caid && card->provid == er->prid && !is_already_pending(er, &current_id))
		{
			sid_verified = 0;
			
			//check if sid is good
			it2 = ll_iter_create(card->goodsids);
			while((srvid_good = ll_iter_next(&it2)))
			{
				if(srvid_good->srvid.provid_id == er->prid && srvid_good->srvid.sid == er->srvid)
				{
					if (!enough || current_avg_card_time > card->average_cw_time)
					{
						time_since_lastcw = abs(srvid_good->last_cw_received - time(NULL));
						current_avg_card_time = card->average_cw_time;
				
						if (enough)
							{ cont_1 = cont_1 - 3; }
						else
						{
							cont_card_1++;
							cont_send++;
							if (time_since_lastcw < GBOX_SID_CONFIRM_TIME && er->gbox_ecm_status == GBOX_ECM_NOT_ASKED)
								{ enough = 1; }						
						}	
						send_buf_1[cont_1] = card->id.peer >> 8;
						send_buf_1[cont_1 + 1] = card->id.peer;
						send_buf_1[cont_1 + 2] = card->id.slot;
						cont_1 = cont_1 + 3;
						sid_verified = 1;
						break;						
					}	
				}
			}

			if(cont_send == cli->reader->gbox_maxecmsend)
			{ 
				max_ecm_reached = 1;
				break;
			}
		}
	}
	
	//loop over bad and unknown cards
	it = ll_iter_create(peer->gbox.cards);	
	while((card = ll_iter_next(&it)))
	{
		current_id.peer = card->id.peer;
		current_id.slot = card->id.slot;					
		if(card->caid == er->caid && card->provid == er->prid && !is_already_pending(er, &current_id) && !enough)
		{
			sid_verified = 0;
			
			//check if sid is good
			it2 = ll_iter_create(card->goodsids);
			while((srvid_good = ll_iter_next(&it2)))
			{
				if(srvid_good->srvid.provid_id == er->prid && srvid_good->srvid.sid == er->srvid)
				{
					sid_verified = 1; 
					cs_log_dbg(D_READER, "ID: %04X SL: %02X SID: %04X is good", card->id.peer, card->id.slot, srvid_good->srvid.sid); 
				}
			}
			if(!sid_verified)
			{
				//check if sid is bad
				LL_ITER itt = ll_iter_create(card->badsids);
				while((srvid_bad = ll_iter_next(&itt)))
				{
					if(srvid_bad->srvid.provid_id == er->prid && srvid_bad->srvid.sid == er->srvid)
					{
						if (srvid_bad->bad_strikes < 3)
						{ 
							sid_verified = 2;
							srvid_bad->bad_strikes++;
						}
						else	
							{ sid_verified = 1; }
						cs_log_dbg(D_READER, "ID: %04X SL: %02X SID: %04X is bad %d", card->id.peer, card->id.slot, srvid_bad->srvid.sid, srvid_bad->bad_strikes); 
						break;
					}
				}
				
				//sid is neither good nor bad
				if(sid_verified != 1)
				{
					send_buf_1[cont_1] = card->id.peer >> 8;
					send_buf_1[cont_1 + 1] = card->id.peer;
					send_buf_1[cont_1 + 2] = card->id.slot;
					cont_1 = cont_1 + 3;
					cont_card_1++;
					cont_send++;
					
					if (!sid_verified)
					{ 
						if(!cs_malloc(&srvid_bad, sizeof(struct gbox_bad_srvid)))
							{ return 0; }

						srvid_bad->srvid.sid = er->srvid;
						srvid_bad->srvid.provid_id = card->provid;
						srvid_bad->bad_strikes = 1;
						ll_append(card->badsids, srvid_bad);
						cs_log_dbg(D_READER, "ID: %04X SL: %02X SID: %04X is not checked", card->id.peer, card->id.slot, srvid_bad->srvid.sid);
					}	 
				}
			}

			if(cont_send == cli->reader->gbox_maxecmsend)
			{ 
				max_ecm_reached = 1;
				break;
			}
		}
	}

	if(!cont_card_1 && er->gbox_ecm_status == GBOX_ECM_NOT_ASKED)
	{
		cs_log_dbg(D_READER, "no valid card found for CAID: %04X PROVID: %04X", er->caid, er->prid);
		write_ecm_answer(cli->reader, er, E_NOTFOUND, E2_CCCAM_NOCARD, NULL, NULL);
		return -1;
	}
	if(cont_card_1)
	{
		send_buf_1[16] = cont_card_1;

		//Hops
		send_buf_1[cont_1] = 0;
		cont_1++;		

		memcpy(&send_buf_1[cont_1], local_gbox.checkcode, 7);
		cont_1 = cont_1 + 7;
		memcpy(&send_buf_1[cont_1], peer->gbox.checkcode, 7);
		cont_1 = cont_1 + 7;

		cs_log_dbg(D_READER, "gbox sending ecm for %04X:%06X:%04X to %d cards -> %s", er->caid, er->prid , er->srvid, cont_card_1, cli->reader->label);
		uint32_t i = 0;
		struct gbox_card_pending *pending = NULL;
		struct timeb t_now;             
		cs_ftime(&t_now);
		for (i = 0; i < cont_card_1; i++)
		{	 			
			if(!cs_malloc(&pending, sizeof(struct gbox_card_pending)))
			{
				cs_log("Can't allocate gbox card pending");
				return -1;
			}
			pending->id.peer = (send_buf_1[len2+10+i*3] << 8) | send_buf_1[len2+11+i*3];
			pending->id.slot = send_buf_1[len2+12+i*3];
			pending->pending_time = comp_timeb(&t_now, &er->tps);
			ll_append(er->gbox_cards_pending, pending);		
			cs_log_dbg(D_READER, "gbox card %d: ID: %04X, Slot: %02X", i+1, (send_buf_1[len2+10+i*3] << 8) | send_buf_1[len2+11+i*3], send_buf_1[len2+12+i*3]); 
		}
	
		it = ll_iter_create(er->gbox_cards_pending);
		while ((pending = ll_iter_next(&it)))
			{ cs_log_dbg(D_READER, "Pending Card ID: %04X Slot: %02X Time: %d", pending->id.peer, pending->id.slot, pending->pending_time); }
	
		if(er->gbox_ecm_status > GBOX_ECM_NOT_ASKED)
			{ er->gbox_ecm_status++; }
		else 
		{
			if(max_ecm_reached)
				{ er->gbox_ecm_status = GBOX_ECM_SENT; }
			else
				{ er->gbox_ecm_status = GBOX_ECM_SENT_ALL; }
			cli->pending++;		
		}	  	
		gbox_send(cli, send_buf_1, cont_1);
		cli->reader->last_s = time((time_t *) 0);
	
		if(er->gbox_ecm_status < GBOX_ECM_ANSWERED)
		{ 
			//Create thread to rebroacast ecm after time
			pthread_t rbc_thread;
			struct gbox_rbc_thread_args args;
			args.cli = cli;
			args.er = er;
			if ((current_avg_card_time > 0) && (cont_card_1 == 1))
				{ args.waittime = current_avg_card_time + (current_avg_card_time / 2); }
			else
				{ args.waittime = GBOX_REBROADCAST_TIMEOUT; }
			cs_log_dbg(D_READER, "Creating rebroadcast thread with waittime: %d", args.waittime);
			int32_t ret = pthread_create(&rbc_thread, NULL, (void *)gbox_rebroadcast_thread, &args);
			if(ret)
			{
				cs_log("Can't create gbox rebroadcast thread (errno=%d %s)", ret, strerror(ret));
				return -1;
			}
			else
				{ pthread_detach(rbc_thread); }
		}
		else
			{ er->gbox_ecm_status--; }
	}	
	return 0;
}

static int32_t gbox_send_emm(EMM_PACKET *UNUSED(ep))
{
	// emms not yet supported

	return 0;
}

//init my gbox with id, password and cards crc
static void init_local_gbox(void)
{
	local_gbox.id = 0;
	local_gbox.password = 0;
	memset(&local_gbox.checkcode[0], 0, 7);
	local_gbox.minor_version = gbox_get_my_vers();
	local_gbox.cpu_api = gbox_get_my_cpu_api();

	if(!cfg.gbox_my_password || strlen(cfg.gbox_my_password) != 8) { return; }

	local_gbox.password = a2i(cfg.gbox_my_password, 4);
	cs_log_dbg(D_READER, "gbox my password: %s:", cfg.gbox_my_password);

	local_gbox.id = gbox_convert_password_to_id(local_gbox.password);
	if (local_gbox.id == NO_GBOX_ID)
	{
		cs_log("invalid local gbox id: %04X", local_gbox.id);
	}
	last_stats_written = time(NULL);
	gbox_write_version();
}

static void gbox_s_idle(struct s_client *cl)
{
	uint32_t time_since_last;
	struct s_client *proxy = get_gbox_proxy(cl->gbox_peer_id);
	struct gbox_peer *peer;

	if (proxy && proxy->gbox)
	{ 
		if (abs(proxy->last - time(NULL)) > abs(cl->lastecm - time(NULL)))
			{ time_since_last = abs(cl->lastecm - time(NULL)); } 
		else { time_since_last = abs(proxy->last - time(NULL)); }
		if (time_since_last > (HELLO_KEEPALIVE_TIME*3) && cl->gbox_peer_id != NO_GBOX_ID)	
		{
			//gbox peer apparently died without saying goodbye
			peer = proxy->gbox;
			cs_log_dbg(D_READER, "time since last proxy activity in sec: %d => taking gbox peer offline",time_since_last);
			gbox_reinit_proxy(proxy);
		}
	
		time_since_last = abs(cl->lastecm - time(NULL));
		if (time_since_last > HELLO_KEEPALIVE_TIME && cl->gbox_peer_id != NO_GBOX_ID)
		{
			peer = proxy->gbox;
			cs_log_dbg(D_READER, "time since last ecm in sec: %d => trigger keepalive hello",time_since_last);
			if (!peer->online)
				{ peer->hello_stat = GBOX_STAT_HELLOL; }
			else
				{ peer->hello_stat = GBOX_STAT_HELLOS; }
			gbox_send_hello(proxy);
			peer->hello_stat = GBOX_STAT_HELLOR;
		}	
	}	
	//prevent users from timing out
	cs_log_dbg(D_READER, "client idle prevented: %s", username(cl));
	cl->last = time((time_t *)0);
}

static int8_t gbox_send_peer_good_night(struct s_client *proxy)
{
	uchar outbuf[64];
	int32_t hostname_len = 0;
	if (cfg.gbox_hostname)
		hostname_len = strlen(cfg.gbox_hostname);
	int32_t len = hostname_len + 22;
	if(proxy->gbox && proxy->typ == 'p')
	{
		struct gbox_peer *peer = proxy->gbox;
		struct s_reader *rdr = proxy->reader;
		if (peer->online)
		{
			gbox_message_header(outbuf, MSG_HELLO, peer->gbox.password, local_gbox.password);
			outbuf[10] = 0x01;
			outbuf[11] = 0x80;
			memset(&outbuf[12], 0xff, 7);
			outbuf[19] = gbox_get_my_vers();
			outbuf[20] = gbox_get_my_cpu_api();
			memcpy(&outbuf[21], cfg.gbox_hostname, hostname_len);
			outbuf[21 + hostname_len] = hostname_len;
			cs_log("<- good night to %s:%d id: %04X", rdr->device, rdr->r_port, peer->gbox.id);
			gbox_compress(outbuf, len, &len);
			gbox_send(proxy, outbuf, len);
			gbox_reinit_proxy(proxy);
		}
	}
	return 0;	
}

void gbox_cleanup(struct s_client *cl)
{
	 if(cl && cl->gbox && cl->typ == 'p')
	 	{ gbox_send_peer_good_night(cl); }
}

/*
void gbox_send_goodbye(uint16_t boxid) //to implement later
{
	uchar outbuf[15];
	struct s_client *cli;
	for (cli = first_client; cli; cli = cli->next)
	{
		if(cli->gbox && cli->typ == 'p')
		{
			struct gbox_peer *peer = cli->gbox;
			if (peer->online && boxid == peer->gbox.id)
			{	
				gbox_message_header(outbuf, MSG_GOODBYE, peer->gbox.password, local_gbox.password);
				cs_log("gbox send goodbye to boxid: %04X", peer->gbox.id);
				gbox_send(cli, outbuf, 0xA);
			}
		}
	}
}
*/
/*
void gbox_send_HERE_query (uint16_t boxid)	//gbox.net send this cmd
{
	uchar outbuf[30];
	int32_t hostname_len = strlen(cfg.gbox_hostname);
	struct s_client *cli;
	for (cli = first_client; cli; cli = cli->next)
	{
		if(cli->gbox && cli->typ == 'p')
		{
			struct gbox_peer *peer = cli->gbox;
			if (peer->online && boxid == peer->gbox.id)
			{	
				gbox_message_header(outbuf, MSG_HERE, peer->gbox.password, local_gbox.password);
				outbuf[0xA] = gbox_get_my_vers();
				outbuf[0xB] = gbox_get_my_cpu_api();
				memcpy(&outbuf[0xC], cfg.gbox_hostname, hostname_len);
				cs_log("gbox send 'HERE?' to boxid: %04X", peer->gbox.id);
				gbox_send(cli, outbuf, hostname_len + 0xC);
			}
		}
	}
}
*/
void module_gbox(struct s_module *ph)
{
	init_local_gbox();
	int32_t i;
	for(i = 0; i < CS_MAXPORTS; i++)
	{
		if(!cfg.gbx_port[i]) { break; }
		ph->ptab.nports++;
		ph->ptab.ports[i].s_port = cfg.gbx_port[i];
	}
	ph->desc = "gbox";
	ph->num = R_GBOX;
	ph->type = MOD_CONN_UDP;
	ph->large_ecm_support = 1;
	ph->listenertype = LIS_GBOX;

	ph->s_handler = gbox_server;
	ph->s_init = gbox_server_init;

	ph->send_dcw = gbox_send_dcw;

	ph->recv = gbox_recv;
	ph->cleanup = gbox_cleanup;
	ph->c_init = gbox_client_init;
	ph->c_recv_chk = gbox_recv_chk;
	ph->c_send_ecm = gbox_send_ecm;
	ph->c_send_emm = gbox_send_emm;

	ph->s_idle = gbox_s_idle;
}
#endif
