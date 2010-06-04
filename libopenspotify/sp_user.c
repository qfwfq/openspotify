#include <libspotify/api.h>

#include "debug.h"
#include "sp_opaque.h"

SP_LIBEXPORT(const char *) sp_user_canonical_name(sp_user *user) {

	return user->canonical_name;
}


SP_LIBEXPORT(const char *) sp_user_display_name(sp_user *user) {
	if(user->display_name)
		return user->display_name;

	return user->canonical_name;
}


SP_LIBEXPORT(bool) sp_user_is_loaded(sp_user *user) {

	return user->is_loaded;
}
