LUA_LSM_INT_GETSECURITY_DEFINE5(inode_getsecurity, struct mnt_idmap *, idmap,
		struct inode *, inode, const char *, name,
		void **, buffer, bool, alloc)
{
	int top = lua_gettop(L);
	int ret = LSM_RET_DEFAULT(inode_getsecurity);
	int nres, tt;

	*newmntidmap(L) = idmap;
	*newinode(L) = inode;
	lua_pushstring(L, name);

	lua_pushvalue(L, -3 - 4);       /* env */
	lua_setfenv(L, -3 - 6);         /* restore thread.env */

	lua_call(L, 3, LUA_MULTRET);

	/* stack: [thread, env, _MODULES, _M, res1, ...] */
	nres = lua_gettop(L) - top + 1;
	switch (nres) {
	case 0:
		break;
	case 1:
		tt = lua_type(L, top);
		if (tt == LUA_TBOOLEAN) {
			ret = lua_toboolean(L, top) ? 0 : -EPERM;
		} else if (tt == LUA_TSTRING) {
			size_t len;
			const char *v = lua_tolstring(L, top, &len);
			ret = (int)len;
			if (alloc) {
				*buffer = kmemdup(v, len, GFP_NOFS);
				if (*buffer == NULL)
					ret = -ENOMEM;
			}
		}
		break;
	default:
		if (!lua_toboolean(L, top)) {
			int errno = lua_tointeger(L, top + 1);
			if (errno > 0 && errno <= MAX_ERRNO)
				ret = -errno;
		}
		break;
	}
	return ret;
}
