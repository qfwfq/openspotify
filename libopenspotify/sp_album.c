#include <stdio.h>
#include <string.h>

#include <spotify/api.h>

#include "debug.h"
#include "sp_opaque.h"


SP_LIBEXPORT(bool) sp_album_is_loaded(sp_album *album) {

	return album->is_loaded;
}


SP_LIBEXPORT(sp_artist *) sp_album_artist(sp_album *album) {

	return album->artist;
}


SP_LIBEXPORT(const byte *) sp_album_cover(sp_album *album) {

	return album->image_id;
}


SP_LIBEXPORT(const char *) sp_album_name(sp_album *album) {

	return album->name;
}


SP_LIBEXPORT(int) sp_album_year(sp_album *album) {

	return album->year;
}


SP_LIBEXPORT(void) sp_album_add_ref(sp_album *album) {

	album->ref_count++;
}


SP_LIBEXPORT(void) sp_album_release(sp_album *album) {
	if(album->ref_count)
		album->ref_count--;

	/* FIXME: Deallocate here when ref count reaches 0 */
}