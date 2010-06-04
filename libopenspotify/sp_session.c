#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <libspotify/api.h>

#include "cache.h"
#include "debug.h"
#include "iothread.h"
#include "link.h"
#include "login.h"
#include "player.h"
#include "playlist.h"
#include "request.h"
#include "sp_opaque.h"
#include "user.h"


SP_LIBEXPORT(sp_error) sp_session_init (const sp_session_config *config, sp_session **psession) {
	sp_session *session;

	if(!config) // XXX - verify
		return SP_ERROR_INVALID_INDATA;

	/* Check if API version matches. */
	if(config->api_version < SPOTIFY_API_VERSION || config->api_version > SPOTIFY_API_VERSION)
		return SP_ERROR_BAD_API_VERSION;

	/* Maximum user-agent length is 4096 bytes (including null-terminator). */
	if(config->user_agent == NULL || strlen(config->user_agent) > 4095)
		return SP_ERROR_BAD_USER_AGENT;

	/* Application key needs to have 321 bytes with the first byte being 0x01. */
	if(config->application_key == NULL || config->application_key_size != 321 ||
		((char *)config->application_key)[0] != 0x01)
		return SP_ERROR_BAD_APPLICATION_KEY;

	/* Allocate memory for our session. */
	if((session = (sp_session *)malloc(sizeof(sp_session))) == NULL)
		return SP_ERROR_API_INITIALIZATION_FAILED;

	memset(session, 0, sizeof(sp_session));
	
	/* Allocate memory for callbacks and copy them to our session. */
	session->userdata = config->userdata;
	session->callbacks = (sp_session_callbacks *)malloc(sizeof(sp_session_callbacks));
	memcpy(session->callbacks, config->callbacks, sizeof(sp_session_callbacks));

	
	/* Connection state is undefined (We were never logged in).*/
	session->connectionstate = SP_CONNECTION_STATE_UNDEFINED;

	session->user = NULL;
	memset(session->country, 0, sizeof(session->country));
	
	/* Login context, needed by network.c and login.c */
	session->login = NULL;
	memset(session->username, 0, sizeof(session->username));
	memset(session->password, 0, sizeof(session->password));

	
	/* Playlist container object */
	playlistcontainer_create(session);

	/* Albums/artists/tracks memory management */
	session->hashtable_albums = hashtable_create(16);
	session->hashtable_artists = hashtable_create(16);
	session->hashtable_images = hashtable_create(20);
	session->hashtable_tracks = hashtable_create(16);
	session->hashtable_users = hashtable_create(256);

	/* Allocate memory for user info. */
	if((session->user = (sp_user *)malloc(sizeof(sp_user))) == NULL)
		return SP_ERROR_API_INITIALIZATION_FAILED;

	/* Low-level networking stuff. */
	session->sock = -1;

	/* Incoming packet buffer */
	session->packet = NULL;

	/* To allow main thread to communicate with network thread */
	session->requests = NULL;

	/* Channels */
	session->channels = NULL;
	session->next_channel_id = 0;
	session->num_channels = 0;


	/* Spawn networking thread. */
#ifdef _WIN32
	session->request_mutex = CreateMutex(NULL, FALSE, NULL);
	session->idle_wakeup = CreateEvent(NULL, FALSE, FALSE, NULL);
	session->thread_main = GetCurrentThread();
	session->thread_io = CreateThread(NULL, 0, iothread, session, 0, NULL);
#else
	pthread_mutex_init(&session->request_mutex, NULL);
	pthread_cond_init(&session->idle_wakeup, NULL);

	session->thread_main = pthread_self();
	if(pthread_create(&session->thread_io, NULL, iothread, session))
		return SP_ERROR_OTHER_TRANSIENT;
#endif

	/* Player thread */
	if(player_init(session))
		return SP_ERROR_OTHER_TRANSIENT;

	/* Helper function for sp_link_create_from_string() */
	libopenspotify_link_init(session);

	/* Load album, artist and track cache */
	cache_init(session);

	/* Run garbage collector and save metadata to disk periodically */
	request_post(session, REQ_TYPE_CACHE_PERIODIC, NULL);

	DSFYDEBUG("Session initialized at %p\n", session);
	
	*psession = session;

	return SP_ERROR_OK;
}


SP_LIBEXPORT(sp_error) sp_session_login (sp_session *session, const char *username, const char *password) {
	strncpy(session->username, username, sizeof(session->username) - 1);
	session->username[sizeof(session->username) - 1] = 0;

	strncpy(session->password, password, sizeof(session->password) - 1);
	session->password[sizeof(session->password) - 1] = 0;

	session->user = user_add(session, username);
	user_add_ref(session->user);
	
	DSFYDEBUG("Posting REQ_TYPE_LOGIN\n");
	request_post(session, REQ_TYPE_LOGIN, NULL);

	return SP_ERROR_OK;
}

SP_LIBEXPORT(sp_connectionstate) sp_session_connectionstate (sp_session *session) {
	DSFYDEBUG("Returning connection state %d\n", session->connectionstate);

	return session->connectionstate;
}


SP_LIBEXPORT(sp_error) sp_session_logout (sp_session *session) {

	DSFYDEBUG("Posting REQ_TYPE_LOGOUT\n");
	request_post(session, REQ_TYPE_LOGOUT, NULL);

	return SP_ERROR_OK;
}


SP_LIBEXPORT(sp_user *) sp_session_user(sp_session *session) {
	
	return session->user;
}


SP_LIBEXPORT(void *) sp_session_userdata(sp_session *session) {
	return session->userdata;
}


SP_LIBEXPORT(void) sp_session_process_events(sp_session *session, int *next_timeout) {
	struct request *request;
	sp_albumbrowse *alb;
	sp_artistbrowse *arb;
	sp_toplistbrowse *toplistbrowse;
	sp_search *search;
	sp_image *image;
	sp_playlist *playlist;
	sp_playlistcontainer *pc;
	int i, value;

	while((request = request_fetch_next_result(session, next_timeout)) != NULL) {
		DSFYDEBUG("Event processing for request <type %s, state %s, input %p, timeout %d>"
				" with output <error %d, output %p>\n",
				REQUEST_TYPE_STR(request->type),
				REQUEST_STATE_STR(request->state),
				request->input, request->next_timeout,
				request->error, request->output);


		/* FIXME: Verify that these callbacks are indeed called from the main thread! */
		switch(request->type) {
		case REQ_TYPE_LOGIN:
			if(session->callbacks->logged_in == NULL)
				break;

			session->callbacks->logged_in(session, request->error);
			break;
			
		case REQ_TYPE_LOGOUT:
			if(session->callbacks->logged_out == NULL)
				break;

			session->callbacks->logged_out(session);
			break;

		case REQ_TYPE_PLAY_TOKEN_LOST:
			if(session->callbacks->play_token_lost == NULL)
				break;

			session->callbacks->play_token_lost(session);
			break;

		case REQ_TYPE_NOTIFY:
			if(session->callbacks->message_to_user == NULL)
				break;

			/* We'll leak memory here for each login made :( */
			session->callbacks->message_to_user(session, request->output);
			break;
				
		case REQ_TYPE_PC_LOAD:
			pc = session->playlistcontainer;
			for(i = 0; i < pc->num_callbacks; i++)
				if(pc->callbacks[i]->container_loaded)
					pc->callbacks[i]->container_loaded(pc, pc->userdata[i]);
			break;

		case REQ_TYPE_PC_PLAYLIST_ADD:
			value = *(int *)request->output; /* position */
			pc = session->playlistcontainer;
			playlist = pc->playlists[value];
			for(i = 0; i < pc->num_callbacks; i++)
				if(pc->callbacks[i]->playlist_added)
					pc->callbacks[i]->playlist_added(pc, playlist, value, pc->userdata[i]);

			break;
				
		case REQ_TYPE_PLAYLIST_RENAME:
			pc = session->playlistcontainer;
			playlist = (sp_playlist *)request->output;
			for(i = 0; i < playlist->num_callbacks; i++)
				if(playlist->callbacks[i]->playlist_renamed)
					playlist->callbacks[i]->playlist_renamed(playlist, playlist->userdata[i]);
			
			break;

		case REQ_TYPE_PLAYLIST_STATE_CHANGED:
			pc = session->playlistcontainer;
			playlist = NULL;
			for(i = 0; i < pc->num_playlists; i++) {
				if(memcmp(pc->playlists[i]->id, request->output, 17))
					continue;
				playlist = pc->playlists[i];
				break;
			}

			if(!playlist)
				break;

			for(i = 0; i < playlist->num_callbacks; i++)
				if(playlist->callbacks[i]->playlist_state_changed)
					playlist->callbacks[i]->playlist_state_changed(playlist, playlist->userdata[i]);
			break;
				
		case REQ_TYPE_PLAYLIST_LOAD:
			pc = session->playlistcontainer;
			playlist = (sp_playlist *)request->output;
			for(i = 0; i < playlist->num_callbacks; i++)
				if(playlist->callbacks[i]->tracks_added)
					playlist->callbacks[i]->tracks_added(playlist, (sp_track *const *)playlist->tracks, playlist->num_tracks, 0, playlist->userdata[i]);
			
			break;
				
		case REQ_TYPE_ALBUMBROWSE:
			alb = (sp_albumbrowse *)request->output;
			if(alb->callback)
				alb->callback(alb, alb->userdata);

			break;

		case REQ_TYPE_ARTISTBROWSE:
	                arb = (sp_artistbrowse *)request->output;
	                if(arb->callback)
	                        arb->callback(arb, arb->userdata);

			break;

		case REQ_TYPE_BROWSE_ALBUM:
		case REQ_TYPE_BROWSE_ARTIST:
		case REQ_TYPE_BROWSE_TRACK:
		case REQ_TYPE_BROWSE_PLAYLIST_TRACKS:
			DSFYDEBUG("Metadata updated for request <type %s, state %s, input %p> in main thread\n",
				  REQUEST_TYPE_STR(request->type), REQUEST_STATE_STR(request->type), request->input);
				
			if(session->callbacks->metadata_updated != NULL)
				session->callbacks->metadata_updated(session);
			break;

		case REQ_TYPE_TOPLISTBROWSE:
	                toplistbrowse = (sp_toplistbrowse *)request->output;
	                if(toplistbrowse->callback)
	                        toplistbrowse->callback(toplistbrowse, toplistbrowse->userdata);

			break;

		case REQ_TYPE_SEARCH:
	                search = (sp_search *)request->output;
	                if(search->callback)
	                        search->callback(search, search->userdata);

			break;

		case REQ_TYPE_IMAGE:
			image = (sp_image *)request->output;
			if(image->callback)
				image->callback(image, image->userdata);
			break;

		default:
			break;
		}


		/* Now that we've delievered the result, mark it for deletion */
		request_mark_processed(session, request);
	}

}


SP_LIBEXPORT(sp_error) sp_session_player_load(sp_session *session, sp_track *track) {
	void **container;

	if(session == NULL || track == NULL) {
		return SP_ERROR_INVALID_INDATA;
	}
	else if(!sp_track_is_loaded(track)) {
		return SP_ERROR_RESOURCE_NOT_LOADED;
	}
	else if(!sp_track_is_available(track)) {
		return SP_ERROR_TRACK_NOT_PLAYABLE;
	}


	/* Unload any previously loaded track */
	player_push(session, PLAYER_UNLOAD, NULL, 0);


	/* The track will released in player.c when PLAYER_UNLOAD is called */
	container = malloc(sizeof(sp_track *));
	*container = track;
	sp_track_add_ref(track);
	player_push(session, PLAYER_LOAD, container, sizeof(sp_track *));
	

	return SP_ERROR_OK;
}


SP_LIBEXPORT(sp_error) sp_session_player_seek(sp_session *session, int offset) {
	/* FIXME: We should not dereference session->player->track as it could be racy wrt PLAYER_LOAD */
	if(session->player->track == NULL || offset < 0 || offset > session->player->track->duration) {
		return SP_ERROR_INVALID_INDATA;
	}

	player_push(session, PLAYER_SEEK, NULL, offset);

	return SP_ERROR_OK;
}


SP_LIBEXPORT(sp_error) sp_session_player_play(sp_session *session, bool play) {
	player_push(session, play? PLAYER_PLAY: PLAYER_PAUSE, NULL, 0);

	return SP_ERROR_OK;
}


SP_LIBEXPORT(void) sp_session_player_unload(sp_session *session) {
	if(session == NULL) {
		return;
	}

	player_push(session, PLAYER_UNLOAD, NULL, 0);
}


SP_LIBEXPORT(sp_playlistcontainer *) sp_session_playlistcontainer(sp_session *session) {

	/* FIXME: Docs says "return ... for the currently logged in user. What if not logged in? */
	return session->playlistcontainer;
}


/*
 * Not present in the official library
 * XXX - Might not be thread safe?
 *
 */
SP_LIBEXPORT(sp_error) sp_session_release (sp_session *session) {

	/* Unregister channels */
	DSFYDEBUG("Unregistering any active channels\n");
	channel_fail_and_unregister_all(session);

	/* Kill player thread */
	player_free(session);

	/* Kill networking thread */
	DSFYDEBUG("Terminating network thread\n");
#ifdef _WIN32
	TerminateThread(session->thread_io, 0);
	session->thread_io = (HANDLE)0;
	CloseHandle(session->idle_wakeup);
	CloseHandle(session->request_mutex);
#else

	pthread_cancel(session->thread_io);
	pthread_join(session->thread_io, NULL);
	session->thread_io = (pthread_t)0;
	pthread_mutex_destroy(&session->request_mutex);
	pthread_cond_destroy(&session->idle_wakeup);
#endif

	if(session->packet)
		buf_free(session->packet);

	if(session->login)
		login_release(session->login);

	playlistcontainer_release(session);

	if(session->hashtable_albums)
		hashtable_free(session->hashtable_albums);

	if(session->hashtable_artists)
		hashtable_free(session->hashtable_artists);

	if(session->hashtable_images)
		hashtable_free(session->hashtable_images);
	
	if(session->hashtable_tracks)
		hashtable_free(session->hashtable_tracks);

	if(session->user)
		user_release(session->user);
	
	if(session->hashtable_users)
		hashtable_free(session->hashtable_users);
	
	free(session->callbacks);

	/* Helper function for sp_link_create_from_string() */
	libopenspotify_link_release();

	free(session);

	DSFYDEBUG("Session released\n");

	return SP_ERROR_OK;
}
