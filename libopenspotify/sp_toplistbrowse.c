#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libspotify/api.h>

#include "album.h"
#include "artist.h"
#include "browse.h"
#include "debug.h"
#include "ezxml.h"
#include "request.h"
#include "sp_opaque.h"
#include "track.h"
#include "util.h"
#include "toplistbrowse.h"


SP_LIBEXPORT(sp_toplistbrowse *) sp_toplistbrowse_create (sp_session *session, sp_toplisttype type, sp_toplistregion region, toplistbrowse_complete_cb *callback, void *userdata) {
	sp_toplistbrowse *toplistbrowse;
	void **container;
	struct toplistbrowse_ctx *toplistbrowse_ctx;


	toplistbrowse = malloc(sizeof(sp_toplistbrowse));
	DSFYDEBUG("Allocated toplistbrowse at %p\n", toplistbrowse);
	if(toplistbrowse == NULL)
		return NULL;


	toplistbrowse->callback = callback;
	toplistbrowse->userdata = userdata;

	toplistbrowse->type = type;
	toplistbrowse->region = region;

	toplistbrowse->tracks = NULL;
	toplistbrowse->num_tracks = 0;

	toplistbrowse->artists = NULL;
	toplistbrowse->num_artists = 0;

	toplistbrowse->albums = NULL;
	toplistbrowse->num_albums = 0;

	toplistbrowse->error = SP_ERROR_IS_LOADING;
	toplistbrowse->is_loaded = 0;
	toplistbrowse->ref_count = 1;


	/*
	 * Temporarily increase ref count for the toplistbrowse so it's not free'd
	 * accidentily. It will be decreaed by the chanel callback.
	 *
	 */
	sp_toplistbrowse_add_ref(toplistbrowse);


	/* The album callback context */
	toplistbrowse_ctx = (struct toplistbrowse_ctx *)malloc(sizeof(struct toplistbrowse_ctx));


	toplistbrowse_ctx->session = session;
	toplistbrowse_ctx->req = NULL; /* Filled in by the request processor */
	toplistbrowse_ctx->buf = buf_new();
	toplistbrowse_ctx->toplistbrowse = toplistbrowse;

	/* Request input container. Will be free'd when the request is finished. */
	container = (void **)malloc(sizeof(void *));
	*container = toplistbrowse_ctx;


	request_post(session, REQ_TYPE_TOPLISTBROWSE, container);

	return toplistbrowse;
}


SP_LIBEXPORT(bool) sp_toplistbrowse_is_loaded(sp_toplistbrowse *toplistbrowse) {

	return toplistbrowse->is_loaded;
}


SP_LIBEXPORT(sp_error) sp_toplistbrowse_error(sp_toplistbrowse *toplistbrowse) {

	return toplistbrowse->error;
}


SP_LIBEXPORT(int) sp_toplistbrowse_num_tracks(sp_toplistbrowse *toplistbrowse) {

	return toplistbrowse->num_tracks;
}


SP_LIBEXPORT(sp_track *) sp_toplistbrowse_track(sp_toplistbrowse *toplistbrowse, int index) {
	if(index < 0 || index >= toplistbrowse->num_tracks)
		return NULL;

	return toplistbrowse->tracks[index];
}


SP_LIBEXPORT(int) sp_toplistbrowse_num_artists(sp_toplistbrowse *toplistbrowse) {

	return toplistbrowse->num_artists;
}


SP_LIBEXPORT(sp_artist *) sp_toplistbrowse_artist(sp_toplistbrowse *toplistbrowse, int index) {
	if(index < 0 || index >= toplistbrowse->num_artists)
		return NULL;

	return toplistbrowse->artists[index];
}


SP_LIBEXPORT(int) sp_toplistbrowse_num_albums(sp_toplistbrowse *toplistbrowse) {

	return toplistbrowse->num_albums;
}


SP_LIBEXPORT(sp_album *) sp_toplistbrowse_album(sp_toplistbrowse *toplistbrowse, int index) {
	if(index < 0 || index >= toplistbrowse->num_albums)
		return NULL;

	return toplistbrowse->albums[index];
}


SP_LIBEXPORT(void) sp_toplistbrowse_add_ref(sp_toplistbrowse *toplistbrowse) {

	toplistbrowse->ref_count++;
}


SP_LIBEXPORT(void) sp_toplistbrowse_release(sp_toplistbrowse *toplistbrowse) {
	int i;

	assert(toplistbrowse->ref_count > 0);
	toplistbrowse->ref_count--;

	if(toplistbrowse->ref_count)
		return;


	for(i = 0; i < toplistbrowse->num_tracks; i++)
		sp_track_release(toplistbrowse->tracks[i]);

	if(toplistbrowse->num_tracks)
		free(toplistbrowse->tracks);


	for(i = 0; i < toplistbrowse->num_artists; i++)
		sp_artist_release(toplistbrowse->artists[i]);

	if(toplistbrowse->num_artists)
		free(toplistbrowse->artists);


	for(i = 0; i < toplistbrowse->num_albums; i++)
		sp_album_release(toplistbrowse->albums[i]);

	if(toplistbrowse->num_albums)
		free(toplistbrowse->albums);


	DSFYDEBUG("Deallocating toplistbrowse at %p\n", toplistbrowse);
	free(toplistbrowse);
}
